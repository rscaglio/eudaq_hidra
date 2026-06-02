"""Build a Plotly figure from a decoded histogram.

This module is the bridge between two worlds:

  * the **decoders** (in `decoders/`) — they take the raw JSON payload
    from the backend and produce a `DecodedHist` (counts, edges, errors).
  * **Plotly** — to render those numbers in the browser we wrap them in
    a `plotly.graph_objects.Figure`.

`to_figure()` is what the poll callback calls once per histogram.
"""

from __future__ import annotations

import logging
import re
from typing import Optional

import numpy as np
import plotly.graph_objects as go

from . import theme
from .config import HistogramDisplayCfg
from .decoders import Decoder, DecodedHist, DecoderError
from .perf import Phase

logger = logging.getLogger(__name__)

# Minimal ROOT TLatex -> Unicode map for axis titles. ROOT stores axis
# titles like "#Delta t [#mus]"; Plotly doesn't speak TLatex, so we
# translate the handful of tokens the HiDRA histograms actually use.
# Unknown "#tokens" are left as-is rather than guessed at.
_ROOT_TLATEX = {
    "#Delta": "Δ",
    "#delta": "δ",
    "#sigma": "σ",
    "#Sigma": "Σ",
    "#sum": "Σ",
    "#pi": "π",
    "#times": "×",
    "#sqrt": "√",
    "#mu": "µ",  # leave last: also turns "#mus" into "µs"
}


def _root_latex_to_unicode(s: str) -> str:
    for token, glyph in _ROOT_TLATEX.items():
        s = s.replace(token, glyph)
    return s


def _axis_unit(axis_title: str) -> Optional[str]:
    """Pull the unit out of an axis title like "Δt [µs]" -> "µs"."""
    m = re.search(r"\[([^\]]+)\]", axis_title)
    return m.group(1).strip() if m else None


def to_figure(
    decoder: Decoder,
    obj_dict: Optional[dict],
    name: str,
    overlay_hist: Optional[DecodedHist] = None,
    options: Optional[HistogramDisplayCfg] = None,
) -> go.Figure:
    """Render one histogram as a Plotly figure.

    * `obj_dict` is the raw payload the backend returned (or `None` if
      the name is missing on the server).
    * `overlay_hist` is the optional reference histogram from a local
      `.root` file (loaded by `OverlayStore`).
    * `options` are the per-histogram display options from config.yaml
      (`logx`, `density`); `None` means defaults (linear, raw counts).
    * Errors during decoding/rendering produce a figure with an
      explanatory annotation instead of crashing the callback.
    """

    logx = bool(options.logx) if options else False
    density = bool(options.density) if options else False

    # Decode early so we can use the histogram's own title, if available.
    # A failed decode must NOT silently fall through to an empty plot
    # (which looks like valid-but-blank data): capture the error here and
    # render it as an annotation below. DecoderError = a payload shape we
    # don't support yet (expected, warn); anything else is unexpected and
    # gets a full stack trace in the log.
    decoded = None
    decode_error: Optional[tuple[str, str]] = None  # (message, colour)
    if obj_dict is not None and "_typename" in obj_dict:
        try:
            with Phase("decode.live"):
                decoded = decoder.decode(obj_dict)
        except DecoderError as exc:
            logger.warning("decoding %s unsupported: %s", name, exc)
            decode_error = (f"unsupported: {exc}", theme.WARN)
        except Exception as exc:
            logger.exception("decoding %s failed", name)
            decode_error = (f"decode error: {exc}", theme.ERR)

    # Pick the title: the decoded histogram's own title when valid, else the name.
    plot_title = decoded.title if decoded and hasattr(decoded, "title") and decoded.title else name

    # Accumulate traces + layout tweaks as plain data, then build the
    # go.Figure in a single shot at the end. Constructing once with
    # data=/layout= is ~4x cheaper than go.Figure()+update_layout()+
    # add_trace()+update_layout(), which dominates the poll (validation).
    layout = theme.base_figure_layout(plot_title)
    traces: list = []
    annotations: list = []

    # No payload for this name: show a "missing on server" placeholder
    # instead of an empty plot, so the user notices that something is off.
    if obj_dict is None or "_typename" not in obj_dict:
        layout["title"] = f"{plot_title} (missing)"
        annotations.append(dict(text="missing on server", showarrow=False, font=dict(color=theme.WARN, size=14)))
        layout["annotations"] = annotations
        return go.Figure(layout=layout)

    # Decoding failed (above): show the captured message instead of an
    # empty plot, so a broken payload is visibly broken rather than blank.
    if decode_error is not None:
        text, colour = decode_error
        annotations.append(dict(text=text, showarrow=False, font=dict(color=colour, size=12)))
        layout["annotations"] = annotations
        return go.Figure(layout=layout)

    # A log x-axis only makes sense when every bin edge is positive; if
    # not (e.g. a histogram that starts at 0), fall back to linear so the
    # plot doesn't silently drop bins.
    apply_logx = logx and decoded is not None and decoded.edges.size > 0 and bool(np.all(decoded.edges > 0))

    # Build the live trace from the (successfully) decoded histogram.
    if decoded is not None:
        try:
            with Phase(f"trace_build.{decoded.typename[:3]}"):
                trace, extra_layout = _build_trace(decoded, color=theme.PRIMARY, density=density, logx=apply_logx)
            if trace is not None:
                traces.append(trace)
                layout.update(extra_layout)
            else:
                annotations.append(dict(text=f"Unknown type: {decoded.typename}", showarrow=False, font=dict(size=14)))
        except Exception as exc:
            # The payload decoded but we couldn't turn it into a trace
            # (e.g. malformed edges): render it rather than going blank.
            logger.exception("building trace for %s failed", name)
            annotations.append(dict(text=f"render error: {exc}", showarrow=False, font=dict(color=theme.ERR, size=12)))
            layout["annotations"] = annotations
            return go.Figure(layout=layout)

    # Optional reference overlay (already decoded — comes from
    # OverlayStore which uses uproot). Match the live trace's density/logx
    # so the two are directly comparable.
    if overlay_hist is not None:
        with Phase("trace_build.overlay"):
            otrace, _ = _build_trace(
                overlay_hist, color=theme.REFERENCE, dashed=True, label_suffix=" (ref)",
                density=density, logx=apply_logx,
            )
        if otrace is not None:
            traces.append(otrace)

    # A log x-axis is a layout property; the trace already places its bars
    # at geometric centres with log-width to match.
    if apply_logx:
        layout["xaxis"]["type"] = "log"

    # Axis titles (with units) come straight from the ROOT histogram, which
    # stores them as "Title;x-title;y-title" — already split into
    # fXaxis.fTitle / fYaxis.fTitle by ROOT at construction. Histograms with
    # no axis title (most ADC/TDC ones) leave these empty and are unchanged.
    x_title = _root_latex_to_unicode((obj_dict.get("fXaxis") or {}).get("fTitle", "") or "")
    y_title = _root_latex_to_unicode((obj_dict.get("fYaxis") or {}).get("fTitle", "") or "")
    if x_title:
        layout["xaxis"]["title"] = x_title

    if density:
        # Density rescales the y content (per unit x). Label it with the real
        # x unit when the x title carries one (e.g. "events / µs"), else fall
        # back to a generic label so the bars aren't read as raw counts.
        base_y = y_title or "entries"
        unit = _axis_unit(x_title)
        layout["yaxis"]["title"] = f"{base_y} / {unit}" if unit else f"{base_y} / bin width"
    elif y_title:
        layout["yaxis"]["title"] = y_title

    if annotations:
        layout["annotations"] = annotations
    return go.Figure(data=traces, layout=layout)


def _build_trace(
    decoded: DecodedHist,
    color: str,
    dashed: bool = False,
    label_suffix: str = "",
    density: bool = False,
    logx: bool = False,
) -> tuple[Optional[go.BaseTraceType], dict]:
    """Build one trace for a decoded histogram.

    Returns ``(trace, extra_layout)``. ``trace`` is None for unknown
    types (TH2 / TProfile2D / …), which the caller turns into an
    annotation. ``extra_layout`` carries any layout tweak the trace
    needs (e.g. ``bargap=0`` for the bar chart).

    `density` divides each bin content by its (linear) bin width.
    `logx` lays the bars out for a logarithmic x-axis: bars are centred
    at the bin's geometric mean and their `width` is given in log10 (the
    axis' own units when `xaxis.type == "log"`), so a non-uniform binning
    renders correctly instead of being squashed against the left edge.
    """
    label = decoded.name + label_suffix

    # Bin centres and widths come from the bin edges. Same shape for
    # TH1 and TProfile — the difference between the two is only in
    # what `decoded.counts` means (raw count vs. mean), which we don't
    # need to know here.
    if decoded.typename == "TProfile" or decoded.typename.startswith("TH1"):
        edges = decoded.edges
        lin_widths = edges[1:] - edges[:-1]

        y = decoded.counts.astype(float)
        if density:
            # Per unit x. Guard against zero-width bins (shouldn't happen
            # for valid edges, but never divide by zero).
            with np.errstate(divide="ignore", invalid="ignore"):
                y = np.where(lin_widths > 0, y / lin_widths, 0.0)

        if logx:
            # On a log axis Plotly interprets x positions and bar widths
            # in log10 units: centre on the geometric mean, width = decade
            # span of the bin.
            centers = np.sqrt(edges[:-1] * edges[1:])
            bar_widths = np.log10(edges[1:]) - np.log10(edges[:-1])
        else:
            centers = 0.5 * (edges[:-1] + edges[1:])
            bar_widths = lin_widths

        if dashed:
            # A "dashed bar chart" is unreadable, so the overlay trace
            # always uses a dashed line on top of the live bars.
            return (
                go.Scatter(
                    x=centers, y=y,
                    mode="lines",
                    line=dict(color=color, dash="dash"),
                    name=label,
                ),
                {},
            )
        return (
            go.Bar(
                x=centers, y=y, width=bar_widths,
                marker=dict(color=color, line=dict(width=0)),
                name=label,
            ),
            {"bargap": 0},
        )

    # TH2 / TProfile2D and anything else: not implemented yet.
    return None, {}
