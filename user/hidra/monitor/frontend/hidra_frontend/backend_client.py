"""Thin client for the ROOT THttpServer monitor backend.

The backend serves histograms under `/Histograms/<name>/root.json` and
exposes a batch endpoint `POST /multi.json?number=N` whose body is a
newline-delimited list of subrequests. The trailing newline on the last
entry is mandatory (without it THttpServer silently drops the last item).
"""

from __future__ import annotations

import logging
from typing import Optional

import requests

logger = logging.getLogger(__name__)


# TODO(monitor_info): when the backend exposes GET /monitor_info.json,
# read pump_interval_ms from it on startup and clamp polling.floor_ms
# accordingly. Until then the static hint in config.yaml is the only
# source of truth.

# TODO(reset): when the backend exposes POST /reset (e.g. via
# THttpServer::RegisterCommand), add BackendClient.reset() and wire it
# to a "Reset histograms" button. Today DoReset() is only reachable
# from the EUDAQ RunControl GUI.


class BackendClient:
    def __init__(self, url: str, timeout_s: float = 2.0) -> None:
        self.url = url.rstrip("/")
        self.timeout_s = timeout_s

    def list_histograms(self) -> list[str]:
        """Return all histogram names registered under /Histograms.

        Used at startup to validate that names referenced by config.yaml
        actually exist on the server.
        """
        try:
            r = requests.get(f"{self.url}/h.json", timeout=self.timeout_s)
            r.raise_for_status()
            tree = r.json()
        except Exception as exc:
            logger.warning("list_histograms failed: %s", exc)
            return []

        for child in tree.get("_childs", []):
            if child.get("_name") == "Histograms":
                return [c["_name"] for c in child.get("_childs", [])]
        return []

    def fetch_multi(self, names: list[str]) -> dict[str, Optional[dict]]:
        """Batch-fetch the given histogram names.

        Returns a dict {name: obj_dict | None}. None means the server
        returned null for that sub-request (typically: histogram not
        registered).
        """
        if not names:
            return {}

        body = "".join(f"Histograms/{n}/root.json\n" for n in names)
        try:
            r = requests.post(
                f"{self.url}/multi.json",
                params={"number": len(names)},
                data=body.encode(),
                timeout=self.timeout_s,
            )
            r.raise_for_status()
            entries = r.json()
        except Exception as exc:
            logger.warning("fetch_multi failed: %s", exc)
            return {n: None for n in names}

        return {name: (entry if entry else None) for name, entry in zip(names, entries)}
