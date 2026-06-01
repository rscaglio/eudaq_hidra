"""Callbacks for the overlay file dropdown."""

from __future__ import annotations

from dash import Dash, Input, Output, State

from ..config import Config
from ..overlay import OverlayStore


def register(app: Dash, store: OverlayStore, config: Config) -> None:
    # If overlay is disabled in config.yaml there are no widgets to
    # wire up — skip registration entirely.
    if not config.overlay.enabled:
        return

    @app.callback(
        Output("overlay-file-dropdown", "options"),
        Input("overlay-refresh-btn", "n_clicks"),
    )
    def refresh_files(_n_clicks):
        # Dash invokes this once at startup with n_clicks=None and
        # again whenever the user clicks the "Refresh files" button.
        # Both paths run the same code: rescan the overlay directory.
        files = store.available_files()
        return [{"label": name, "value": name} for name in files]

    @app.callback(
        Output("client-state", "data"),
        Input("overlay-file-dropdown", "value"),
        State("client-state", "data"),
    )
    def pick_file(file_name, state):
        # The selected overlay file lives in `client-state` (a
        # browser-side dcc.Store) so the poll callback can read it via
        # State without triggering itself when the user opens/closes
        # the dropdown.
        state = dict(state or {})
        state["overlay_file"] = file_name
        store.clear_cache()
        return state
