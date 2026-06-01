"""Cross-tab navigation: click a detector module -> open its ADC channel.

The detector map (`DetectorPanel`) draws one cell per PMT module, each
carrying its ADC channel index as Plotly ``customdata``. A detector panel
configured with ``link_tab: <tab id>`` becomes clickable: clicking a cell

  1. tells the `ChannelSelectorPanel` in the linked tab to select that
     channel (`select_channel`), and
  2. switches the active tab to the linked tab.

Switching the tab makes `update_tab_content` rebuild that tab's DOM; the
selector's dropdown is laid out with the channel we just set, and the
poll callback then fetches and draws that channel's histogram.

All detector graphs share the generic ``panel-graph`` component id (the
same one the poll callback writes figures into), so we listen on every
``panel-graph``'s ``clickData`` and use the registry built here to keep
only the clicks coming from a *clickable detector* panel.
"""

from __future__ import annotations

import logging

from dash import ALL, Dash, Input, Output, ctx, no_update

from ..panels.base import Panel
from ..panels.channel_selector import ChannelSelectorPanel
from ..panels.detector import DetectorPanel

logger = logging.getLogger(__name__)


def register(app: Dash, panels_by_tab: dict[str, list[Panel]]) -> None:
    # panel_id of a clickable detector -> (target tab id, its selector panel).
    nav: dict[str, tuple[str, ChannelSelectorPanel]] = {}
    for panels in panels_by_tab.values():
        for panel in panels:
            if not isinstance(panel, DetectorPanel):
                continue
            target = panel.link_tab()
            if not target:
                continue
            selector = _find_channel_selector(panels_by_tab.get(target, []))
            if selector is None:
                logger.warning(
                    "detector panel %s has link_tab=%r but that tab has no "
                    "channel_selector panel; clicks will be ignored",
                    panel.panel_id, target,
                )
                continue
            nav[panel.panel_id] = (target, selector)

    if not nav:
        return

    @app.callback(
        Output("tabs", "value"),
        Input({"type": "panel-graph", "panel": ALL, "index": ALL}, "clickData"),
        prevent_initial_call=True,
    )
    def on_module_click(_all_click_data):
        trigger = ctx.triggered_id
        if not isinstance(trigger, dict):
            return no_update
        entry = nav.get(trigger.get("panel"))
        if entry is None:
            return no_update

        click_data = ctx.triggered[0]["value"]
        points = (click_data or {}).get("points") or []
        if not points:
            return no_update
        channel = points[0].get("customdata")
        if channel is None:  # clicked an empty cell (no module there)
            return no_update

        target_tab, selector = entry
        selector.select_channel(int(channel))
        return target_tab


def _find_channel_selector(panels: list[Panel]) -> ChannelSelectorPanel | None:
    for panel in panels:
        if isinstance(panel, ChannelSelectorPanel):
            return panel
    return None
