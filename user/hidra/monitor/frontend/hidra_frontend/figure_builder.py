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
from typing import Optional

import plotly.graph_objects as go

from . import theme
from .decoders import Decoder, DecodedHist, DecoderError
from .perf import Phase

logger = logging.getLogger(__name__)


def to_figure(
    decoder: Decoder,
    obj_dict: Optional[dict],
    name: str,
    overlay_hist: Optional[DecodedHist] = None,
) -> go.Figure:
    """Render one histogram as a Plotly figure.

    * `obj_dict` is the raw payload the backend returned (or `None` if
      the name is missing on the server).
    * `overlay_hist` is the optional reference histogram from a local
      `.root` file (loaded by `OverlayStore`).
    * Errors during decoding/rendering produce a figure with an
      explanatory annotation instead of crashing the callback.
    """

    # Decode early so we can use the histogram's own title, if available.
    decoded = None
    if obj_dict is not None and "_typename" in obj_dict:
        try:
            with Phase("decode.live"):
                decoded = decoder.decode(obj_dict)
        except Exception:
            decoded = None

    # Pick the title: the decoded histogram's own title when valid, else the name.
    plot_title = decoded.title if decoded and hasattr(decoded, "title") and decoded.title else name
    fig = go.Figure()
    fig.update_layout(**theme.base_figure_layout(plot_title))

    # No payload for this name: show a "missing on server" placeholder
    # instead of an empty plot, so the user notices that something is
    # off.

    if obj_dict is None or "_typename" not in obj_dict:
        fig.update_layout(title=f"{plot_title} (missing)")
        fig.add_annotation(text="missing on server", showarrow=False, font=dict(color=theme.WARN, size=14))
        return fig

    # Decode the payload (TBufferJSON dict -> DecodedHist) and add a
    # trace. We catch DecoderError separately so unknown types render
    # an explanatory message instead of a stack trace.

    if decoded is not None:
        try:
            with Phase(f"trace_build.{decoded.typename[:3]}"):
                _add_trace(fig, decoded, color=theme.PRIMARY)
        except DecoderError as exc:
            fig.add_annotation(text=f"unsupported: {exc}", showarrow=False, font=dict(color=theme.WARN, size=12))
            return fig
        except Exception as exc:
            logger.exception("decoding %s failed", name)
            fig.add_annotation(text=f"decode error: {exc}", showarrow=False, font=dict(color=theme.ERR, size=12))
            return fig

    # Optional reference overlay (already decoded — comes from
    # OverlayStore which uses uproot).
    if overlay_hist is not None:
        with Phase("trace_build.overlay"):
            _add_trace(fig, overlay_hist, color=theme.REFERENCE, dashed=True, label_suffix=" (ref)")

    return fig


def _add_trace(
    fig: go.Figure,
    decoded: DecodedHist,
    color: str,
    dashed: bool = False,
    label_suffix: str = "",
) -> None:
    """Append one trace to `fig`.

    * For TH1 / TProfile we emit a bar chart (the "live" trace) or a
      dashed line trace (the "overlay" trace, when `dashed=True`).
    * Unknown types produce an annotation in the figure so the user
      sees what happened.
    """
    label = decoded.name + label_suffix

    # Bin centres and widths come from the bin edges. Same shape for
    # TH1 and TProfile — the difference between the two is only in
    # what `decoded.counts` means (raw count vs. mean), which we don't
    # need to know here.
    if decoded.typename == "TProfile" or decoded.typename.startswith("TH1"):
        centers = 0.5 * (decoded.edges[:-1] + decoded.edges[1:])
        widths = decoded.edges[1:] - decoded.edges[:-1]
        if dashed:
            # A "dashed bar chart" is unreadable, so the overlay
            # trace always uses a dashed line on top of the live bars.
            fig.add_trace(
                go.Scatter(
                    x=centers, y=decoded.counts,
                    mode="lines",
                    line=dict(color=color, dash="dash"),
                    name=label,
                )
            )
        else:
            fig.add_trace(
                go.Bar(
                    x=centers, y=decoded.counts, width=widths,
                    marker=dict(color=color, line=dict(width=0)),
                    name=label,
                )
            )
            fig.update_layout(bargap=0)
        return

    # TH2 / TProfile2D and anything else: not implemented yet.
    fig.add_annotation(text=f"Unknown type: {decoded.typename}", showarrow=False, font=dict(size=14))
