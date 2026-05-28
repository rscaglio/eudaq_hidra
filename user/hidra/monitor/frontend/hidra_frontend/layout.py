"""Top-level Dash layout.

This module builds the initial layout shown when the page loads:
header (title, pause button, poll-rate dropdown, overlay controls),
status bar, tab strip, and the empty container `tab-content` where the
per-tab panels are injected by the `update_tab_content` callback.

It also instantiates one `Panel` object per panel entry in
`config.yaml`. The Panel objects live in the `panels_by_tab` dict for
the entire app lifetime — they hold no per-poll state, only the
layout/parameters they were configured with.
"""

from __future__ import annotations

from dash import dcc, html

from . import theme
from .config import Config
from .panels import build_panel
from .panels.base import Panel


def build_panels(config: Config) -> dict[str, list[Panel]]:
    """Instantiate one Panel object per panel entry. Indexed by tab id."""
    by_tab: dict[str, list[Panel]] = {}
    for tab in config.tabs:
        panels: list[Panel] = []
        # `panel_id` is what the panel uses to scope its component IDs.
        # The `<tab>__p<i>` pattern makes IDs unique across tabs.
        for i, panel_cfg in enumerate(tab.panels):
            panel_id = f"{tab.id}__p{i}"
            panels.append(build_panel(panel_id, panel_cfg.type, panel_cfg.params))
        by_tab[tab.id] = panels
    return by_tab


def build(config: Config, panels_by_tab: dict[str, list[Panel]]) -> html.Div:
    first_tab = config.tabs[0].id
    pump_hint = config.polling.server_pump_ms_hint

    # Only offer poll-rate choices >= the configured floor.
    poll_choices = [
        {"label": f"{ms} ms", "value": ms}
        for ms in config.polling.choices_ms
        if ms >= config.polling.floor_ms
    ]

    # Overlay controls are only added when the feature is enabled in
    # config.yaml. If disabled, the corresponding callback in
    # callbacks/overlay.py is also skipped.
    overlay_controls: list = []
    if config.overlay.enabled:
        overlay_controls = [
            html.Span("Overlay:", style={"color": theme.FG, "fontSize": "13px"}),
            dcc.Dropdown(
                id="overlay-file-dropdown",
                options=[],
                value=config.overlay.default_file,
                placeholder="(none)",
                clearable=True,
                style={"width": "260px", "color": "#000"},
            ),
            html.Button(
                "Refresh files",
                id="overlay-refresh-btn",
                n_clicks=0,
                style=_button_style(),
            ),
        ]

    return html.Div(
        style={
            "backgroundColor": theme.BG,
            "minHeight": "100vh",
            "fontFamily": "monospace",
            "color": theme.FG,
            "padding": "16px",
        },
        children=[
            # ── Top bar: title, pause, poll rate, overlay ─────────────
            html.Div(
                style={"display": "flex", "alignItems": "center", "gap": "16px", "marginBottom": "8px"},
                children=[
                    html.H2("HiDRa Monitor", style={"color": theme.ACCENT, "margin": 0}),
                    html.Button("⏸ Pause", id="pause-btn", n_clicks=0, style=_button_style()),
                    html.Span("Poll:", style={"color": theme.FG, "fontSize": "13px"}),
                    dcc.Dropdown(
                        id="poll-rate-dropdown",
                        options=poll_choices,
                        value=config.polling.default_ms,
                        clearable=False,
                        style={"width": "120px", "color": "#000"},
                    ),
                    *overlay_controls,
                ],
            ),

            # ── Status bar (updated on every poll) ────────────────────
            html.Div(
                id="status-bar",
                style={"color": theme.OK, "marginBottom": "12px", "fontSize": "12px"},
                children=f"server pump ~{pump_hint} ms",
            ),

            # ── Tabs ─────────────────────────────────────────────────
            dcc.Tabs(
                id="tabs",
                value=first_tab,
                colors={"border": theme.SURFACE, "primary": theme.ACCENT, "background": theme.BG_ALT},
                children=[
                    dcc.Tab(
                        label=tab.label,
                        value=tab.id,
                        style={"color": theme.FG, "backgroundColor": theme.BG_ALT},
                        selected_style={"color": theme.ACCENT, "backgroundColor": theme.BG},
                    )
                    for tab in config.tabs
                ],
            ),

            # The active tab's panels are injected here by
            # `update_tab_content` in callbacks/poll.py.
            html.Div(id="tab-content", style={"marginTop": "16px"}),

            # Ticks every `interval` ms (changed at runtime by the
            # poll-rate dropdown). `disabled=True` pauses the polling.
            dcc.Interval(id="interval", interval=config.polling.default_ms, n_intervals=0, disabled=False),

            # Browser-side state. `dcc.Store` lets callbacks read/write
            # small dicts without round-tripping through the server
            # except when a callback is triggered.
            #   * client-state holds the selected overlay file (and
            #     any future bits the panels want to share).
            #   * tab-mount-state is bumped by update_tab_content so
            #     poll() runs only AFTER the DOM has been rebuilt for
            #     the new tab — preventing a race where the previous
            #     tab's figures land in the new tab's slots.
            dcc.Store(id="client-state", data={"overlay_file": config.overlay.default_file}),
            dcc.Store(id="tab-mount-state", data={"tab": first_tab, "rev": 0}),
        ],
    )


def _button_style() -> dict:
    return {
        "backgroundColor": theme.SURFACE,
        "color": theme.FG,
        "border": f"1px solid {theme.BORDER}",
        "padding": "6px 14px",
        "fontFamily": "monospace",
        "fontSize": "13px",
        "cursor": "pointer",
        "borderRadius": "4px",
    }
