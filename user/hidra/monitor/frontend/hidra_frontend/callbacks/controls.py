"""Callbacks for the top-bar controls: Pause / Resume button + poll-rate dropdown."""

from __future__ import annotations

from dash import Dash, Input, Output, State


def register(app: Dash) -> None:
    @app.callback(
        Output("interval", "disabled"),
        Output("pause-btn", "children"),
        Input("pause-btn", "n_clicks"),
        State("interval", "disabled"),
        prevent_initial_call=True,
    )
    def toggle_pause(_n_clicks, currently_disabled):
        # Pausing simply disables the dcc.Interval — no extra state to
        # carry around. When the user resumes, the next tick fires
        # immediately and the figures catch up.
        new_disabled = not currently_disabled
        return new_disabled, ("▶ Resume" if new_disabled else "⏸ Pause")

    @app.callback(
        Output("interval", "interval"),
        Input("poll-rate-dropdown", "value"),
        prevent_initial_call=True,
    )
    def set_poll_rate(value_ms):
        # Changing `interval.interval` takes effect on the next tick;
        # Dash handles the timer reset for us.
        return int(value_ms)
