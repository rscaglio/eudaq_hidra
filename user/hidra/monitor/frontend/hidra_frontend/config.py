"""YAML config loader.

Reads `config.yaml`, validates the basic shape, and returns a
typed `Config` object. The rest of the codebase only touches this
typed object, never the raw YAML — so misspelled keys fail loudly
here instead of producing weird behaviour later.

The dataclasses below mirror the sections of `config.yaml`. If you
add a new section to the YAML, mirror it here and load it in
`load_config()`.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml


@dataclass
class BackendCfg:
    url: str
    request_timeout_s: float


@dataclass
class PollingCfg:
    default_ms: int
    floor_ms: int
    choices_ms: list[int]
    server_pump_ms_hint: int


@dataclass
class OverlayCfg:
    enabled: bool
    search_dir: Path
    default_file: str | None


@dataclass
class PanelCfg:
    type: str
    params: dict[str, Any]


@dataclass
class TabCfg:
    id: str
    label: str
    panels: list[PanelCfg]


@dataclass
class Config:
    backend: BackendCfg
    polling: PollingCfg
    overlay: OverlayCfg
    tabs: list[TabCfg]
    decoder: str = "pure"
    config_dir: Path = field(default_factory=Path)


def _normalize_panel(raw: dict[str, Any]) -> PanelCfg:
    panel_type = raw["type"]
    params = {k: v for k, v in raw.items() if k != "type"}

    # channel_selector: expand template+range into explicit names list
    if panel_type == "channel_selector" and "names" not in params:
        if "template" in params and "range" in params:
            lo, hi = params["range"]
            params["names"] = [params["template"].format(ch=i) for i in range(lo, hi + 1)]

    return PanelCfg(type=panel_type, params=params)


def load_config(path: str | Path) -> Config:
    path = Path(path).resolve()
    with path.open() as f:
        raw = yaml.safe_load(f)

    config_dir = path.parent

    backend = BackendCfg(
        url=raw["backend"]["url"],
        request_timeout_s=float(raw["backend"].get("request_timeout_s", 2.0)),
    )

    p = raw["polling"]
    polling = PollingCfg(
        default_ms=int(p["default_ms"]),
        floor_ms=int(p["floor_ms"]),
        choices_ms=[int(x) for x in p["choices_ms"]],
        server_pump_ms_hint=int(p.get("server_pump_ms_hint", 0)),
    )

    o = raw.get("overlay") or {}
    search_dir = Path(o.get("search_dir", "reference"))
    if not search_dir.is_absolute():
        search_dir = (config_dir / search_dir).resolve()
    overlay = OverlayCfg(
        enabled=bool(o.get("enabled", False)),
        search_dir=search_dir,
        default_file=o.get("default_file"),
    )

    tabs: list[TabCfg] = []
    for t in raw["tabs"]:
        tabs.append(
            TabCfg(
                id=t["id"],
                label=t["label"],
                panels=[_normalize_panel(p) for p in t["panels"]],
            )
        )

    decoder = str(raw.get("decoder", "pure"))

    return Config(
        backend=backend,
        polling=polling,
        overlay=overlay,
        tabs=tabs,
        decoder=decoder,
        config_dir=config_dir,
    )
