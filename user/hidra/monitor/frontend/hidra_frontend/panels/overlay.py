"""OverlayPanel — several histograms superimposed in one plot.

Unlike `histograms` (one graph slot per histogram), this panel fetches a
fixed list of histograms and draws them as overlaid line traces in a
single graph, with a legend whose entries toggle each series on/off
(Plotly native; all visible by default).

Built for comparing two estimators of the same quantity — e.g. the
per-channel pedestal noise as `IQR/1.349` vs the standard deviation
(`ADC_noise_pedestal` and `ADC_noise_std_pedestal`).

Config (in `config.yaml`)::

    - type: overlay
      histograms: [ADC_noise_pedestal, ADC_noise_std_pedestal]
      labels: ["IQR/1.349", "std"]   # optional, default = histogram names
      title: "Pedestal noise per channel"   # optional
      per_channel: true              # optional; x is a channel index ->
                                     #   "ch N" hover and markers per point

It builds its own figure from the raw payloads (like `detector`), so it
needs its own decoder.
"""

from __future__ import annotations

from dash import dcc, html

from .. import theme
from ..decoders import get_decoder
from ..figure_builder import overlay_figure
from .base import Panel
from .graph_controls import controls_overlay

# Colour cycle for the overlaid series. The first (primary/robust) series
# gets the accent blue; the rest are visually distinct.
_COLORS = [theme.PRIMARY, theme.REFERENCE, theme.SECONDARY, theme.ACCENT, theme.ERR]


class OverlayPanel(Panel):
    def __init__(self, panel_id, params):
        super().__init__(panel_id, params)
        self._decoder = get_decoder(params.get("decoder", "pure"))
        self._histos = list(params.get("histograms", []))
        labels = params.get("labels")
        self._labels = list(labels) if labels else list(self._histos)
        self._per_channel = bool(params.get("per_channel", False))

    def _title(self) -> str:
        return self.params.get("title", self._histos[0] if self._histos else "overlay")

    def histogram_names(self) -> list[str]:
        return list(self._histos)

    def figure_names(self) -> list[str]:
        # Builds its own overlaid figure from the raw payloads.
        return []

    def control_indices(self) -> list[int]:
        return [0]

    def layout(self) -> html.Div:
        height = self.params.get("height", "320px")
        return html.Div(
            className="plot-cell",
            style={"flex": "1", "minWidth": "0"},
            children=[
                dcc.Graph(
                    id={"type": "panel-graph", "panel": self.panel_id, "index": 0},
                    figure=theme.placeholder_figure(self._title()),
                    style={"minWidth": "0", "height": height},
                    config={"displayModeBar": False},
                ),
                controls_overlay(self.panel_id, 0),
            ],
        )

    def render(self, figs, payloads, client_state):
        specs = [
            (payloads.get(name), _COLORS[i % len(_COLORS)], label)
            for i, (name, label) in enumerate(zip(self._histos, self._labels))
        ]
        if self._per_channel:
            fig = overlay_figure(
                self._decoder, specs, self._title(),
                per_channel=True, line_shape="linear", mode="lines+markers",
            )
        else:
            fig = overlay_figure(self._decoder, specs, self._title())
        return [fig]
