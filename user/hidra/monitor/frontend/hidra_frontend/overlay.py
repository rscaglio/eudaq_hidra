"""Reference histogram overlay from local ROOT files.

Backed by uproot — a pure-Python ROOT file reader. No PyROOT, no
global lock, much lighter on memory than ROOT.TFile.

The store returns histograms as `DecodedHist` directly, the same shape
the live decoder produces, so the figure builder treats live and
overlay traces uniformly.

TODO(remote_overlay): currently assumes frontend and backend share
the filesystem. Replace `uproot.open(path)` with an HTTP fetch from a
future backend endpoint when they don't.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Optional

import numpy as np
import uproot

from .decoders import DecodedHist

logger = logging.getLogger(__name__)


class OverlayStore:
    def __init__(self, search_dir: Path) -> None:
        self.search_dir = Path(search_dir)
        self._cache: dict[tuple[str, str], Optional[DecodedHist]] = {}

    def available_files(self) -> list[str]:
        if not self.search_dir.is_dir():
            return []
        return sorted(p.name for p in self.search_dir.glob("*.root"))

    def get(self, file_name: Optional[str], hist_name: str) -> Optional[DecodedHist]:
        if not file_name:
            return None

        cache_key = (file_name, hist_name)
        if cache_key in self._cache:
            return self._cache[cache_key]

        path = self.search_dir / file_name
        if not path.is_file():
            logger.warning("overlay file not found: %s", path)
            self._cache[cache_key] = None
            return None

        try:
            with uproot.open(str(path)) as f:
                if hist_name not in f:
                    self._cache[cache_key] = None
                    return None
                obj = f[hist_name]
                decoded = _to_decoded(obj, hist_name)
        except Exception as exc:
            logger.warning("overlay read failed for %s/%s: %s", file_name, hist_name, exc)
            self._cache[cache_key] = None
            return None

        self._cache[cache_key] = decoded
        return decoded

    def clear_cache(self) -> None:
        self._cache.clear()


def _to_decoded(obj, hist_name: str) -> Optional[DecodedHist]:
    """Convert an uproot histogram to DecodedHist. Returns None for unsupported types."""
    classname = getattr(obj, "classname", "")

    if classname == "TProfile" or classname.startswith("TH1"):
        return DecodedHist(
            name=hist_name,
            title=getattr(obj, "title", hist_name) or hist_name,
            typename=classname,
            edges=np.asarray(obj.axis(0).edges(), dtype=np.float64),
            counts=np.asarray(obj.values(), dtype=np.float64),
            errors=np.asarray(obj.errors(), dtype=np.float64),
        )

    # TH2 / TProfile2D will come later — see decoders/pure.py TODO.
    logger.info("overlay: unsupported classname %r for %s", classname, hist_name)
    return None
