"""Panel type registry.

Add a new panel type:
  1. Create `panels/my_panel.py` with `class MyPanel(Panel)`.
  2. Import it here and add it to PANEL_TYPES.
  3. Reference its type name in config.yaml.

"""

from __future__ import annotations

from .base import Panel
from .channel_selector import ChannelSelectorPanel
from .detector import DetectorPanel
from .event_display import EventDisplayPanel
from .fers_board import FERSBoardPanel
from .histogram_grid import HistogramGridPanel
from .mask_strip import MaskStripPanel
from .maxicc_detector import MAXICCDetectorPanel
from .metric import MetricPanel
from .overlay import OverlayPanel
from .rate import RatePanel
from .tracker import TrackerPanel
from .sipm_detector import SiPMDetectorPanel

PANEL_TYPES: dict[str, type[Panel]] = {
    "histograms": HistogramGridPanel,
    "mask_strip": MaskStripPanel,
    "metric": MetricPanel,
    "channel_selector": ChannelSelectorPanel,
    "detector": DetectorPanel,
    "event_display": EventDisplayPanel,
    "fers_board": FERSBoardPanel,
    "sipm_detector": SiPMDetectorPanel,
    "maxicc_detector": MAXICCDetectorPanel,
    "overlay": OverlayPanel,
    "rate": RatePanel,
    "tracker": TrackerPanel,
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
