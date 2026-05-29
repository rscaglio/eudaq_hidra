"""Panel type registry.

Add a new panel type:
  1. Create `panels/my_panel.py` with `class MyPanel(Panel)`.
  2. Import it here and add it to PANEL_TYPES.
  3. Reference its type name in config.yaml.

TODO(event_display): add an "event_display" panel type once the backend
publishes single-event objects (non-cumulative). The panel will need
its own dispatcher for fetching the latest event payload — design TBD.
"""

from __future__ import annotations

from .base import Panel
from .channel_selector import ChannelSelectorPanel
from .detector import DetectorPanel
from .grid import GridPanel
from .metric import MetricPanel

PANEL_TYPES: dict[str, type[Panel]] = {
    "grid": GridPanel,
    "metric": MetricPanel,
    "channel_selector": ChannelSelectorPanel,
    "detector": DetectorPanel,
}


def build_panel(panel_id: str, panel_type: str, params: dict) -> Panel:
    try:
        cls = PANEL_TYPES[panel_type]
    except KeyError as exc:
        raise ValueError(
            f"unknown panel type '{panel_type}'; known types: {sorted(PANEL_TYPES)}"
        ) from exc
    return cls(panel_id, params)


__all__ = ["Panel", "PANEL_TYPES", "build_panel"]
