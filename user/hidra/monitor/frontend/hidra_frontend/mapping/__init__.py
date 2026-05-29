"""Calorimeter channel mapping.

Maps ADC channel indices to detector positions (module, row, column,
S/C type). The raw maps live in the bundled JSON files
(`adc_channels.json` = channel -> detector name, `modules.json` =
module -> [row, column]); `ADCMapping` joins them.

Most callers just want "channel index -> info" for the PMT channels —
use the module-level `get_pmt_channel_info()` helper, which loads the
bundled JSON once and caches it.
"""

from __future__ import annotations

from pathlib import Path

from .calo_mapping import ADCMapping

_MAPPING_DIR = Path(__file__).parent
_ADC_CHANNELS_FILE = _MAPPING_DIR / "adc_channels.json"
_MODULES_FILE = _MAPPING_DIR / "modules.json"

_default_mapping: ADCMapping | None = None


def default_mapping() -> ADCMapping:
    """Return the process-wide `ADCMapping` built from the bundled JSON."""
    global _default_mapping
    if _default_mapping is None:
        _default_mapping = ADCMapping(_ADC_CHANNELS_FILE, _MODULES_FILE)
    return _default_mapping


def get_pmt_channel_info() -> dict[int, dict[str, int | str]]:
    """Channel index -> {channel, name, module, type, row, column}.

    Only PMT channels (names matching `M<n>S` / `M<n>C`) are included;
    non-PMT channels like the muon counter are skipped.
    """
    return default_mapping().get_pmt_channels_info()


__all__ = ["ADCMapping", "default_mapping", "get_pmt_channel_info"]
