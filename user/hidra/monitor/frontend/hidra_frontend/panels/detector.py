"""DetectorPanel — a 2D map of the calorimeter modules.

Unlike the other panels (which draw one histogram per graph slot),
this one reads a single per-channel histogram (`ADC_mean` by default)
and lays its values out *spatially*: each module is drawn as a
rectangle at its physical (row, column) position, coloured by the mean
ADC of its PMT.

A module has two PMTs — one "S" and one "C" — so we emit **two**
figures, one per type, sharing the same geometry.

Config (in `config.yaml`):

    - type: detector
      histogram: ADC_mean          # optional, default "ADC_mean"

The channel -> (module, row, column, type) map comes from
`hidra_frontend.mapping.get_pmt_channel_info()`. Channels that are not
PMTs (e.g. the muon counter) are simply absent from that map and never
drawn.

The source histogram is a `TProfile` filled with `Fill(channel, adc)`,
so the value for channel `c` is the mean stored in fArray bin `c + 1`
(bin 0 is underflow). We read the buffers directly here — same trick
`MetricPanel` uses — instead of going through the full decoder, since
all we need is one number per channel.
"""

from __future__ import annotations

from typing import Optional

import plotly.colors
import plotly.graph_objects as go
from dash import dcc, html

from .. import theme
from ..mapping import get_pmt_channel_info
from .base import Panel

# Physical module dimensions (mm). Used only for the aspect ratio of
# the drawn rectangles, so the map looks like the real detector.
MODULE_WIDTH_MM = 128.0
MODULE_HEIGHT_MM = 28.3

# PMT types, in the order their figures appear (S first, then C).
PMT_TYPES = ("S", "C")

COLORSCALE = "Viridis"
# Fill colour for a module that exists in the map but has no data this
# poll (channel missing on the server, or zero entries in the profile).
NO_DATA_COLOR = theme.SURFACE


class DetectorPanel(Panel):
    def _hist_name(self) -> str:
        return self.params.get("histogram", "ADC_mean")

    def histogram_names(self) -> list[str]:
        return [self._hist_name()]

    def layout(self) -> html.Div:
        slots = [
            dcc.Graph(
                id={"type": "panel-graph", "panel": self.panel_id, "index": i},
                figure=theme.placeholder_figure(f"Detector — {ptype} channels"),
                style={"flex": "1", "minWidth": "0", "height": "320px"},
                config={"displayModeBar": False},
            )
            for i, ptype in enumerate(PMT_TYPES)
        ]
        return html.Div(
            style={"display": "flex", "gap": "12px", "marginBottom": "12px"},
            children=slots,
        )

    def render(self, figs, payloads, client_state):
        payload = payloads.get(self._hist_name())
        means = _channel_means(payload)
        info = get_pmt_channel_info()
        return [_detector_figure(ptype, means, info) for ptype in PMT_TYPES]


def _channel_means(payload: Optional[dict]) -> Optional[dict[int, float]]:
    """channel index -> mean value, read straight from the TProfile buffers.

    Returns None when the payload is missing/unusable (so the figure can
    show a "missing" placeholder). For a channel with zero entries the
    mean is undefined and the channel is simply absent from the dict.
    """
    if not payload or "_typename" not in payload:
        return None

    nbins = payload.get("fXaxis", {}).get("fNbins", 0)
    sumw = payload.get("fArray") or []
    if nbins < 1 or not sumw:
        return None

    # fArray / fBinEntries layout: [underflow, bin_1, ..., bin_N, overflow].
    # Channel c was filled at x = c, which lands in bin c + 1.
    entries = payload.get("fBinEntries") or []

    means: dict[int, float] = {}
    for channel in range(nbins):
        idx = channel + 1
        if idx >= len(sumw):
            break
        if entries:
            # TProfile: mean = sum(weight*y) / sum(weight).
            if idx < len(entries) and entries[idx] > 0:
                means[channel] = sumw[idx] / entries[idx]
        elif sumw[idx]:
            # Plain TH1 fallback: the bin content is already the value.
            means[channel] = float(sumw[idx])
    return means


def _detector_figure(
    pmt_type: str,
    means: Optional[dict[int, float]],
    info: dict[int, dict],
) -> go.Figure:
    """One figure: every module drawn as a rectangle, coloured by its
    `pmt_type` PMT's mean ADC."""
    title = f"Detector — {pmt_type} channels"
    fig = go.Figure()
    fig.update_layout(**theme.base_figure_layout(title))

    modules = [d for d in info.values() if d["type"] == pmt_type]

    # Geometry (axis ranges + aspect) is derived from *all* PMT modules
    # so the S and C maps line up, even if one type misses a module.
    if not info:
        fig.add_annotation(text="no module mapping", showarrow=False, font=dict(color=theme.WARN, size=14))
        return fig

    cols = [d["column"] for d in info.values()]
    rows = [d["row"] for d in info.values()]
    x_lo, x_hi = min(cols) * MODULE_WIDTH_MM, (max(cols) + 1) * MODULE_WIDTH_MM
    y_lo, y_hi = min(rows) * MODULE_HEIGHT_MM, (max(rows) + 1) * MODULE_HEIGHT_MM

    if means is None:
        fig.update_layout(title=f"{title} (missing)")
        fig.add_annotation(text="missing on server", showarrow=False, font=dict(color=theme.WARN, size=14))
        return fig

    # Colour range from the values actually present for this type.
    values = [means[d["channel"]] for d in modules if d["channel"] in means]
    cmin, cmax = (min(values), max(values)) if values else (0.0, 1.0)
    if cmin == cmax:
        cmax = cmin + 1.0

    for d in modules:
        col, row = d["column"], d["row"]
        x0, x1 = col * MODULE_WIDTH_MM, (col + 1) * MODULE_WIDTH_MM
        y0, y1 = row * MODULE_HEIGHT_MM, (row + 1) * MODULE_HEIGHT_MM

        value = means.get(d["channel"])
        if value is None:
            fill = NO_DATA_COLOR
            label = f"{d['module']}<br>—"
        else:
            t = (value - cmin) / (cmax - cmin)
            fill = plotly.colors.sample_colorscale(COLORSCALE, [t])[0]
            label = f"{d['module']}<br>{value:.0f}"

        fig.add_shape(
            type="rect",
            x0=x0, x1=x1, y0=y0, y1=y1,
            line=dict(color=theme.BG, width=2),
            fillcolor=fill,
            layer="below",
        )
        fig.add_annotation(
            x=0.5 * (x0 + x1), y=0.5 * (y0 + y1),
            text=label,
            showarrow=False,
            font=dict(color=theme.FG, size=11),
        )

    # Invisible trace carrying the colourscale so Plotly draws a colorbar.
    fig.add_trace(
        go.Scatter(
            x=[None], y=[None],
            mode="markers",
            marker=dict(
                colorscale=COLORSCALE,
                cmin=cmin, cmax=cmax,
                color=[cmin],
                showscale=True,
                colorbar=dict(title="mean ADC", thickness=12),
            ),
            hoverinfo="skip",
            showlegend=False,
        )
    )

    pad_x = 0.05 * (x_hi - x_lo)
    pad_y = 0.20 * (y_hi - y_lo)
    fig.update_layout(
        xaxis=dict(
            title="x [mm]",
            range=[x_lo - pad_x, x_hi + pad_x],
            showgrid=False, zeroline=False,
        ),
        # Reversed so row 1 sits at the top, matching a front view.
        # scaleanchor keeps mm isotropic, so each module keeps its real
        # 128 x 28.3 proportions.
        yaxis=dict(
            title="y [mm]",
            range=[y_hi + pad_y, y_lo - pad_y],
            showgrid=False, zeroline=False,
            scaleanchor="x", scaleratio=1.0,
        ),
    )
    return fig
