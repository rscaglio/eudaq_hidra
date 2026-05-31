"""ChannelSelectorPanel — one per-channel histogram with a channel dropdown.

The backend publishes one TH1D per ADC channel (``ADC_channel_0``,
``ADC_channel_1``, ...). There are far too many to show at once, so this
panel shows a *single* channel's histogram and lets the user switch
channel with a dropdown.

Config (in ``config.yaml``)::

    - type: channel_selector
      template: "ADC_channel_{ch}"     # optional, this is the default

The channel list is normally **auto-discovered** from the live backend
at startup: every histogram whose name matches ``template`` becomes one
dropdown entry, in numeric order. To pin an explicit set instead, give a
``range: [lo, hi]`` (config.py expands ``template`` + ``range`` into a
``names`` list) or a ``names`` list directly.

Dropdown labels are enriched with the detector name from the calo
mapping when the channel is known (e.g. ``ch 5  ·  M105S``); otherwise
just the channel number is shown.

The currently-shown channel is kept in per-process instance state
(``self._selected``), updated by a callback registered in
``register_callbacks()``. The poll callback reads it through
``histogram_names()`` on the next tick, so the plot follows the dropdown
within one poll period. This relies on the single-worker deployment
documented in the README (per-process state is shared across callbacks).
"""

from __future__ import annotations

import re
from typing import Optional

from dash import Dash, Input, Output, dcc, html

from .. import theme
from ..mapping import default_mapping
from .base import Panel

DEFAULT_TEMPLATE = "ADC_channel_{ch}"


def _template_regex(template: str) -> re.Pattern:
    """Turn ``"ADC_channel_{ch}"`` into a regex capturing the channel number.

    Everything around the ``{ch}`` placeholder is matched literally.
    """
    head, _, tail = template.partition("{ch}")
    return re.compile(f"^{re.escape(head)}(\\d+){re.escape(tail)}$")


class ChannelSelectorPanel(Panel):
    def __init__(self, panel_id, params):
        super().__init__(panel_id, params)
        self._template = params.get("template", DEFAULT_TEMPLATE)
        self._regex = _template_regex(self._template)
        self._names: list[str] = []
        self._selected: Optional[str] = None

    def _discover(self, available: list[str]) -> list[str]:
        matched: list[tuple[int, str]] = []
        for name in available:
            m = self._regex.match(name)
            if m:
                matched.append((int(m.group(1)), name))
        matched.sort()
        return [name for _, name in matched]

    def _channel_of(self, name: str) -> Optional[int]:
        m = self._regex.match(name)
        return int(m.group(1)) if m else None

    def select_channel(self, ch: int) -> None:
        """Select a channel by its index (used by cross-panel navigation,
        e.g. clicking a module on the detector map).

        We set ``self._selected`` to the templated histogram name. The next
        time the panel is laid out, ``_options()`` keeps this selection as
        long as the channel exists on the backend (otherwise it falls back
        to the first available channel). ``histogram_names()`` then makes
        the poll fetch and draw it.
        """
        self._selected = self._template.format(ch=ch)

    def _options(self) -> list[dict]:
        # Ricalcola la lista canali ogni volta
        params = self.params
        names = params.get("names")
        if not names:
            names = self._discover(params.get("available_histograms") or [])
        self._names = list(names)
        # Aggiorna la selezione se non più valida
        if self._selected not in self._names:
            self._selected = self._names[0] if self._names else None
        mapping = default_mapping()
        opts: list[dict] = []
        for name in self._names:
            ch = self._channel_of(name)
            if ch is None:
                opts.append({"label": name, "value": name})
                continue
            try:
                label = f"ch {ch}  ·  {mapping.get_channel_name(ch)}"
            except KeyError:
                label = f"ch {ch}"
            opts.append({"label": label, "value": name})
        return opts

    # ---- Panel API -------------------------------------------------------

    def histogram_names(self) -> list[str]:
        return [self._selected] if self._selected else []

    def layout(self) -> html.Div:
        options = self._options()
        dropdown = dcc.Dropdown(
            id={"type": "channel-select", "panel": self.panel_id},
            options=options,
            value=self._selected,
            clearable=False,
            placeholder="(no channels on backend)" if not options else "select a channel",
            style={"width": "320px", "color": "#000"},
        )
        graph = dcc.Graph(
            id={"type": "panel-graph", "panel": self.panel_id, "index": 0},
            figure=theme.placeholder_figure(self._selected or "no channel"),
            style={"height": "420px"},
            config={"displayModeBar": False},
        )
        return html.Div(
            [
                html.Div(
                    style={"display": "flex", "alignItems": "center", "marginBottom": "12px", "gap": "8px"},
                    children=[
                        html.Span("Channel:", style={"color": theme.FG, "fontSize": "13px"}),
                        dropdown,
                    ],
                ),
                graph,
                # Throwaway sink: the selection callback must write to at
                # least one Output. The real state lives in self._selected.
                dcc.Store(id={"type": "channel-select-sink", "panel": self.panel_id}),
            ]
        )

    def render(self, figs, payloads, client_state):
        if not self._selected:
            return [theme.placeholder_figure("no channel selected")]
        return [figs.get(self._selected, theme.placeholder_figure(self._selected))]

    def register_callbacks(self, app: Dash) -> None:
        @app.callback(
            Output({"type": "channel-select-sink", "panel": self.panel_id}, "data"),
            Input({"type": "channel-select", "panel": self.panel_id}, "value"),
            prevent_initial_call=True,
        )
        def _on_select(value):
            # Persist the choice in instance state; the next poll picks it
            # up via histogram_names(). Returning `value` just satisfies
            # Dash's "every callback needs an Output" rule.
            if value:
                self._selected = value
            return value
