"""DetectorPanel — a 2D map of the calorimeter modules.

Unlike the other panels (which draw one histogram per graph slot),
this one reads a single per-channel histogram (`ADC_mean` by default)
and lays its values out *spatially*: each module sits at its (row,
column) position, coloured by the mean ADC of its PMT.

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

Each figure is a **single `Heatmap` trace**, deliberately: the axes,
colorbar and geometry are identical from poll to poll, so only the `z`
array changes. That lets Plotly's client-side `react` diff update just
the colours instead of rebuilding the whole scene — which it would
have to do with per-module layout shapes/annotations.
"""

from __future__ import annotations

from typing import Optional

import plotly.graph_objects as go
from dash import dcc, html

from .. import theme
from ..mapping import get_pmt_channel_info
from .base import Panel

# Physical module dimensions (mm). Used only for the cell aspect ratio,
# so each module keeps its real 128 x 28.3 proportions on screen.
MODULE_WIDTH_MM = 128.0
MODULE_HEIGHT_MM = 28.3

# PMT types, in the order their figures appear (S first, then C).
PMT_TYPES = ("S", "C")

COLORSCALE = "Viridis"


class DetectorPanel(Panel):
    def _hist_name(self) -> str:
        return self.params.get("histogram", "ADC_mean")

    def histogram_names(self) -> list[str]:
        return [self._hist_name()]

    def layout(self) -> html.Div:
        height = self.params.get("height", "320px")
        slots = [
            dcc.Graph(
                id={"type": "panel-graph", "panel": self.panel_id, "index": i},
                figure=theme.placeholder_figure(f"Detector — {ptype} channels"),
                style={"flex": "1", "minWidth": "0", "height": height},
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
    """One figure for a PMT type: a single Heatmap over the (row, column)
    grid, each cell coloured by that module's mean ADC."""
    title = f"Detector — {pmt_type} channels"
    fig = go.Figure()
    fig.update_layout(**theme.base_figure_layout(title))

    if not info:
        fig.add_annotation(text="no module mapping", showarrow=False, font=dict(color=theme.WARN, size=14))
        return fig

    if means is None:
        fig.update_layout(title=f"{title} (missing)")
        fig.add_annotation(text="missing on server", showarrow=False, font=dict(color=theme.WARN, size=14))
        return fig

    # The grid spans the full integer row/column range of *all* PMT
    # modules (not just the present ones) so the S and C maps line up
    # and any missing position is left as an empty cell rather than
    # collapsed away.
    all_cols = [d["column"] for d in info.values()]
    all_rows = [d["row"] for d in info.values()]
    columns = list(range(min(all_cols), max(all_cols) + 1))
    rows = list(range(min(all_rows), max(all_rows) + 1))
    col_idx = {c: i for i, c in enumerate(columns)}
    row_idx = {r: i for i, r in enumerate(rows)}

    # z = value per cell (None where there is no module / no data this
    # poll); text = the module label shown in the cell.
    z: list[list[Optional[float]]] = [[None] * len(columns) for _ in rows]
    text: list[list[str]] = [[""] * len(columns) for _ in rows]
    for d in info.values():
        if d["type"] != pmt_type:
            continue
        ri, ci = row_idx[d["row"]], col_idx[d["column"]]
        value = means.get(d["channel"])
        z[ri][ci] = value
        text[ri][ci] = d["module"]

    values = [v for rowvals in z for v in rowvals if v is not None]
    cmin, cmax = (min(values), max(values)) if values else (0.0, 1.0)
    if cmin == cmax:
        cmax = cmin + 1.0

    fig.add_trace(
        go.Heatmap(
            x=columns, y=rows, z=z,
            text=text, texttemplate="%{text}",
            textfont=dict(size=11),
            colorscale=COLORSCALE,
            zmin=cmin, zmax=cmax,
            xgap=2, ygap=2,
            hoverongaps=False,
            hovertemplate="%{text}<br>mean ADC %{z:.1f}<extra></extra>",
            colorbar=dict(title="mean ADC", thickness=12),
        )
    )

    fig.update_layout(
        xaxis=dict(
            title="column",
            tickmode="array", tickvals=columns,
            showgrid=False, zeroline=False,
        ),
        # autorange reversed -> row 1 at the top (front view). scaleanchor
        # with scaleratio = height/width makes each 1x1 cell display with
        # the real 128 x 28.3 module proportions.
        yaxis=dict(
            title="row",
            tickmode="array", tickvals=rows,
            autorange="reversed",
            showgrid=False, zeroline=False,
            scaleanchor="x", scaleratio=MODULE_HEIGHT_MM / MODULE_WIDTH_MM,
        ),
    )
    return fig
