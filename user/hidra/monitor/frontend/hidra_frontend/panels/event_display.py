"""Selected-event ADC + FERS front face, using the existing channel maps.

Complete batches and event selection live in browser Stores. Browsing is local:
the backend is contacted only on tab open, Refresh events or Latest. A changing
DAQ buffer cannot invalidate an event already listed in the browser.
"""

from __future__ import annotations

from collections import defaultdict
import math
import uuid

import plotly.graph_objects as go
from dash import ClientsideFunction, Dash, Input, Output, State, ctx, dcc, html, no_update
from dash.exceptions import PreventUpdate

from .. import theme
from ..mapping import get_pmt_channel_info, get_sipm_channel_info
from .base import Panel
from .detector import MODULE_HEIGHT_MM, MODULE_WIDTH_MM, _build_geometry, _detector_figure


def _channel_values(values) -> dict[int, float]:
    """Null, negative decoder sentinels and non-finite values mean no data."""
    return {
        ch: float(value) for ch, value in enumerate(values or [])
        if isinstance(value, (float, int)) and math.isfinite(value) and value >= 0
    }


def _sipm_geometry(info: dict[int, dict]) -> dict:
    """Register the fibre map inside its named calorimeter modules.

    The module naming convention M<column><two-digit row> is the same as
    modules.json (e.g. M308 is column 3, row 8). Fibre positions and orientation
    come from sipm_channels.json: columns increase rightward, rows upward. Each
    module's full fibre grid fills one module footprint, including both types.
    """
    modules = defaultdict(list)
    for channel, cell in info.items():
        modules[str(cell["module"]).removeprefix("M")].append((channel, cell))
    cells = []
    outlines = []
    for module, entries in modules.items():
        column, row = divmod(int(module), 100)
        cols = [cell["column"] for _, cell in entries]
        rows = [cell["row"] for _, cell in entries]
        col_min, row_min = min(cols), min(rows)
        ncols, nrows = max(cols) - col_min + 1, max(rows) - row_min + 1
        outlines.append((column, row, f"M{module}"))
        for channel, cell in entries:
            x = column - 0.5 + (cell["column"] - col_min + 0.5) / ncols
            y = row - 0.5 + (cell["row"] - row_min + 0.5) / nrows
            cells.append((x, y, channel, cell))
    return {"cells": cells, "outlines": outlines}


def front_face_figure(pmt_type, snapshot, gain, scale, pmt_geom, sipm_geom, channels_per_board=64):
    """One physical face: large PMT modules around the fine central SiPM cells."""
    adc = _channel_values(snapshot.get("adc"))
    fers = _channel_values(snapshot.get(f"fers_{gain.lower()}"))
    title = f"Front face · {pmt_type} channels · ADC + FERS {gain}"
    fig = _detector_figure(pmt_type, adc, pmt_geom, "ADC", title)
    # Fixed scales allow comparisons between events. Event scaling is useful
    # for inspecting small signals; both S/C views still share each source's range.
    pmt_channels = {
        ch for group in (pmt_geom or {}).get("per_type", {}).values() for _, _, ch in group["cells"]
    }
    sipm_channels = {ch for _, _, ch, _ in sipm_geom["cells"]}
    adc_max = max([1.0, *(adc[ch] for ch in pmt_channels if ch in adc)]) if scale == "event" else 4095
    fers_max = max([1.0, *(fers[ch] for ch in sipm_channels if ch in fers)]) if scale == "event" else 4095
    for trace in fig.data:
        if trace.showscale is not False:
            trace.zmin, trace.zmax = 0, adc_max
            trace.colorbar = dict(title="PMT ADC", thickness=12, len=0.42, y=0.78)

    cells = sipm_geom["cells"]
    if cells:
        xs = sorted({x for x, _, _, _ in cells})
        ys = sorted({y for _, y, _, _ in cells})
        xi, yi = {x: i for i, x in enumerate(xs)}, {y: i for i, y in enumerate(ys)}
        z = [[None] * len(xs) for _ in ys]
        empty = [[None] * len(xs) for _ in ys]
        hit = [[None] * len(xs) for _ in ys]
        hover = [[None] * len(xs) for _ in ys]
        for x, y, channel, cell in cells:
            if cell["type"] != pmt_type:
                continue
            r, c = yi[y], xi[x]
            value = fers.get(channel)
            z[r][c] = value
            empty[r][c] = 0 if value is None else None
            hit[r][c] = 0
            board, local = divmod(channel, channels_per_board)
            signal = f"{gain} ADC {value:g}" if value is not None else "no data"
            hover[r][c] = (
                f"M{str(cell['module']).removeprefix('M')} · {pmt_type} · FERS ch {channel}"
                f"<br>board {board} · ch {local} · fibre ({cell['column']}, {cell['row']})<br>{signal}"
            )
        fig.add_traces([
            go.Heatmap(x=xs, y=ys, z=empty, showscale=False, hoverinfo="skip",
                       colorscale=[[0, theme.EMPTY], [1, theme.EMPTY]], zmin=0, zmax=1),
            go.Heatmap(x=xs, y=ys, z=z, colorscale="Viridis", zmin=0, zmax=fers_max,
                       hoverinfo="skip", colorbar=dict(title=f"FERS {gain}", thickness=12, len=0.42, y=0.25)),
            go.Heatmap(x=xs, y=ys, z=hit, text=hover, showscale=False, hoverongaps=False,
                       colorscale=[[0, "rgba(0,0,0,0)"], [1, "rgba(0,0,0,0)"]],
                       hovertemplate="%{text}<extra></extra>", zmin=0, zmax=1),
        ])
        for col, row, module in sipm_geom["outlines"]:
            fig.add_shape(type="rect", x0=col-0.5, x1=col+0.5, y0=row-0.5, y1=row+0.5,
                          line=dict(color=theme.FG, width=1), fillcolor="rgba(0,0,0,0)")
            fig.add_annotation(x=col, y=row+0.5, text=module, showarrow=False, yanchor="top",
                               font=dict(size=9, color=theme.FG), bgcolor=theme.BG_ALT)

    fig.update_layout(
        uirevision=f"event-face-{pmt_type}",
        xaxis=dict(title="module column", range=[0.5, 5.5], constrain="domain"),
        yaxis=dict(title="module row", range=[0.5, 20.5], autorange=False,
                   scaleanchor="x", scaleratio=MODULE_HEIGHT_MM / MODULE_WIDTH_MM),
    )
    return fig


class EventDisplayPanel(Panel):
    def __init__(self, panel_id, params):
        super().__init__(panel_id, params)
        self._client = params.get("_client")
        self._pmt_geom = _build_geometry(get_pmt_channel_info())
        self._sipm_geom = _sipm_geometry(get_sipm_channel_info())

    def histogram_names(self):
        return []

    def render(self, figs, payloads, client_state):
        # Its own callbacks update event-display-graph, outside histogram polling.
        return []

    def _id(self, name):
        return f"{self.panel_id}-event-{name}"

    def layout(self):
        return html.Div([
            html.Div([
                html.Button("← Previous", id=self._id("previous"), n_clicks=0, className="btn"),
                dcc.Dropdown(id=self._id("select"), options=[], placeholder="Select a buffered event",
                             clearable=False, persistence=True, persistence_type="session",
                             style={"minWidth": "340px", "flex": "1"}),
                html.Button("Next →", id=self._id("next"), n_clicks=0, className="btn"),
                html.Button("Latest", id=self._id("latest"), n_clicks=0, className="btn"),
                html.Button("Refresh events", id=self._id("refresh"), n_clicks=0, className="btn"),
                html.Span("FERS gain:"),
                dcc.RadioItems(id=self._id("gain"), options=["HG", "LG"], value="HG", inline=True,
                               persistence=True, persistence_type="session", labelStyle={"marginRight": "10px"}),
                dcc.RadioItems(id=self._id("scale"), options=[
                    {"label": "Full scale", "value": "full"}, {"label": "Event scale", "value": "event"}],
                    value="full", inline=True, labelStyle={"marginRight": "10px"}),
            ], style={"display": "flex", "alignItems": "center", "gap": "10px", "flexWrap": "wrap"}),
            html.Div(id=self._id("feed-status"), style={"marginTop": "10px", "fontSize": "12px"}),
            html.Div(id=self._id("metadata"), style={"marginTop": "8px", "color": theme.PRIMARY}),
            html.P("Loaded events stay available until you refresh. Latest loads newer events. "
                   "Raw ADC counts; grey cells have no data.",
                   style={"fontSize": "12px", "color": theme.FG}),
            html.Div([
                dcc.Graph(id=self._id(f"face-{typ}"), figure=theme.placeholder_figure(f"Front face · {typ}"),
                          style={"height": self.params.get("height", "680px"), "minWidth": "360px", "flex": "1"},
                          config={"displayModeBar": True, "displaylogo": False})
                for typ in ("S", "C")
            ], style={"display": "flex", "flexWrap": "wrap", "gap": "12px"}),
            dcc.Store(id=self._id("batch")),
            dcc.Store(id=self._id("collection")),
            dcc.Store(id=self._id("snapshot"), storage_type="session"),
        ])

    def register_callbacks(self, app: Dash):
        @app.callback(
            Output(self._id("batch"), "data"),
            Output(self._id("feed-status"), "children"),
            Input("tab-mount-state", "data"), Input(self._id("refresh"), "n_clicks"),
            Input(self._id("latest"), "n_clicks"),
        )
        def load_batch(mount, _refresh_clicks, _latest_clicks):
            if not isinstance(mount, dict) or mount.get("tab") != self.panel_id.rsplit("__p", 1)[0]:
                raise PreventUpdate
            batch = self._client.event_batch() if self._client else None
            if not isinstance(batch, dict) or not isinstance(batch.get("events"), list):
                return no_update, "Event feed unavailable; loaded events remain browsable. Rebuild/restart the monitor if needed."
            events = batch["events"]
            if any(not isinstance(ev, dict) or not isinstance(ev.get("id"), str) or not ev["id"] or
                   any(not isinstance(ev.get(key), list) for key in ("adc", "fers_hg", "fers_lg")) or
                   "run_number" not in ev or "event_number" not in ev for ev in events):
                return no_update, "Invalid event batch; keeping the loaded events."
            capacity = batch.get("capacity", 0)
            status = (f"Loaded {len(events)} events · buffer {batch.get('available', len(events))}/{capacity} · "
                      f"display ≤ {batch.get('capture_max_hz', '—')} events/s · "
                      f"monitor prescale {batch.get('event_prescale', 1)}")
            if not capacity:
                status += " · capture disabled (EVENT_DISPLAY_BUFFER_SIZE=0)"
            elif not events:
                status += " · no events yet; press Refresh events after acquisition starts"
            latest = bool(_latest_clicks) and ctx.triggered_id == self._id("latest")
            return {"events": events, "action": "latest" if latest else "refresh",
                    "revision": uuid.uuid4().hex}, status

        # No backend lookup and no full batch round-trip on navigation. Only the
        # selected snapshot is sent to Python for plotting, independent of DAQ rate.
        app.clientside_callback(
            ClientsideFunction(namespace="hidra_event_display", function_name="select_event"),
            Output(self._id("collection"), "data"),
            Output(self._id("select"), "options"),
            Output(self._id("select"), "value"),
            Output(self._id("snapshot"), "data"), Output(self._id("metadata"), "children"),
            Input(self._id("batch"), "data"), Input(self._id("previous"), "n_clicks"),
            Input(self._id("next"), "n_clicks"), Input(self._id("select"), "value"),
            State(self._id("collection"), "data"), State(self._id("snapshot"), "data"),
        )

        @app.callback(
            Output(self._id("face-S"), "figure"), Output(self._id("face-C"), "figure"),
            Input(self._id("snapshot"), "data"), Input(self._id("gain"), "value"),
            Input(self._id("scale"), "value"),
        )
        def draw(snapshot, gain, scale):
            return [front_face_figure(typ, snapshot or {}, gain or "HG", scale,
                                      self._pmt_geom, self._sipm_geom,
                                      self.params.get("channels_per_board", 64)) for typ in ("S", "C")]
