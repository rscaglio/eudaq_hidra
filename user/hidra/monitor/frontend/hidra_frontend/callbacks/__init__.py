"""Callback registration entry point."""

from __future__ import annotations

from dash import Dash

from ..backend_client import BackendClient
from ..config import Config
from ..decoders import Decoder
from ..overlay import OverlayStore
from ..panels.base import Panel
from . import controls, overlay as overlay_cb, poll


def register_all(
    app: Dash,
    config: Config,
    panels_by_tab: dict[str, list[Panel]],
    client: BackendClient,
    overlay_store: OverlayStore,
    decoder: Decoder,
) -> None:
    controls.register(app)
    overlay_cb.register(app, overlay_store, config)
    poll.register(app, config, panels_by_tab, client, overlay_store, decoder)

    for panels in panels_by_tab.values():
        for panel in panels:
            panel.register_callbacks(app)
