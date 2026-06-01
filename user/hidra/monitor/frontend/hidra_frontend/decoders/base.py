"""Decoder interface and shared data class.

Both the pure-Python and the PyROOT backends produce the same
`DecodedHist` so the figure builder doesn't have to branch on the
backend.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import numpy as np


@dataclass
class DecodedHist:
    """Backend-agnostic decoded histogram."""

    name: str
    title: str
    typename: str  # e.g. "TH1D", "TProfile", "TH2F", "TProfile2D"
    # 1D fields
    edges: np.ndarray = field(default_factory=lambda: np.empty(0))
    counts: np.ndarray = field(default_factory=lambda: np.empty(0))
    errors: Optional[np.ndarray] = None
    # 2D fields (filled only for TH2 / TProfile2D)
    x_edges: Optional[np.ndarray] = None
    y_edges: Optional[np.ndarray] = None
    z: Optional[np.ndarray] = None


class DecoderError(Exception):
    pass


class Decoder:
    """Decoder protocol — implement `decode(obj_dict) -> DecodedHist`."""

    def decode(self, obj_dict: dict) -> DecodedHist:
        raise NotImplementedError
