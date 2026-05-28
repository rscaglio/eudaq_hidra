"""GridPanel — the default panel: a fixed list of histograms in an N-column grid.

This is the panel type you reference with `type: grid` in
`config.yaml`. It expects:

    - type: grid
      cols: 2                                # optional, default 2
      histograms: [hist_name_1, hist_name_2, ...]

It's a thin object: declare which histograms it wants, declare its
Dash layout (one `dcc.Graph` per histogram, arranged in rows of
`cols`), and on each poll receive a `figs` dict and return the figures
in the same order as the graph slots.

For anything fancier (channel selector, event display, multi-row
custom layout) write a new Panel subclass — see `panels/base.py` and
the README section "Custom panels".
"""

from __future__ import annotations

from dash import dcc, html

from .. import theme
from .base import Panel


class GridPanel(Panel):
    def histogram_names(self) -> list[str]:
        return list(self.params["histograms"])

    def layout(self) -> html.Div:
        names = self.histogram_names()
        cols = int(self.params.get("cols", 2))

        # One Graph slot per histogram. The dict ID is what the poll
        # callback addresses via pattern matching — see poll.py.
        graph_slots = [
            dcc.Graph(
                id={"type": "panel-graph", "panel": self.panel_id, "index": i},
                # placeholder_figure keeps the dashboard's dark theme
                # visible before the first poll arrives (no white
                # flash).
                figure=theme.placeholder_figure(name),
                style={"flex": "1", "minWidth": "0"},
                config={"displayModeBar": False},
            )
            for i, name in enumerate(names)
        ]

        # Arrange the slots in rows of `cols` items.
        rows = []
        for start in range(0, len(graph_slots), cols):
            row = html.Div(
                style={"display": "flex", "gap": "12px", "marginBottom": "12px"},
                children=graph_slots[start:start + cols],
            )
            rows.append(row)
        return html.Div(rows)

    def render(self, figs, payloads, client_state):
        # We ignore `payloads`: the standard bar-chart figure built by
        # the framework is exactly what we want to show. If a histogram
        # is missing from `figs` we fall back to the styled placeholder
        # so the slot doesn't go blank-white.
        return [figs.get(n, theme.placeholder_figure(n)) for n in self.histogram_names()]
