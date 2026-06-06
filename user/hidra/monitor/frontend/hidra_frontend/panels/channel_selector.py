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
from ..decoders import get_decoder
from ..figure_builder import overlay_figure
from ..mapping import default_mapping
from .base import Panel
from .graph_controls import controls_overlay

DEFAULT_TEMPLATE = "ADC_channel_{ch}"

# When `show_trigger_split` is on, the panel overlays these series of the
# selected channel: by default the inclusive histogram plus its
# physics/pedestal copies. Each tuple is (name suffix, legend label, colour).
TRIGGER_SPLIT_SERIES = [
    ("", "total", theme.PRIMARY),
    ("_physics", "physics", theme.SECONDARY),
    ("_pedestal", "pedestal", theme.REFERENCE),
]

# Label + colour for a split series, keyed by name suffix. Lets a panel pick a
# subset/order of series via the `split_suffixes` config param (e.g. FERS has
# only physics/pedestal per channel, no inclusive "total").
_SPLIT_SUFFIX_INFO = {suffix: (label, color) for suffix, label, color in TRIGGER_SPLIT_SERIES}


def _template_regex(template: str) -> re.Pattern:
    """Turn ``"ADC_channel_{ch}"`` into a regex capturing the channel number.

    Everything around the ``{ch}`` placeholder is matched literally.
    """
    head, _, tail = template.partition("{ch}")
    return re.compile(f"^{re.escape(head)}(\\d+){re.escape(tail)}$")


class ChannelSelectorPanel(Panel):
    def __init__(self, panel_id, params):
        super().__init__(panel_id, params)
        # Two data-source modes, both selected by a single channel index:
        #
        #  * name mode (`templates`/`template`): one per-channel histogram per
        #    channel exists on the backend (e.g. ADC_channel_<N>); fetched by
        #    name. `templates` can list several (HG+LG side by side).
        #  * projection mode (`projection_templates`): the per-channel data lives
        #    in one TH2 per gain/trigger (e.g. FERS_HG_dist_physics, x=channel,
        #    y=ADC). A single channel is fetched as a server-side ProjectionY,
        #    encoded as a `<th2>#projy=<xbin>` token resolved by BackendClient.
        proj = params.get("projection_templates")
        self._projection = bool(proj)
        if self._projection:
            templates = proj
        else:
            templates = params.get("templates") or [params.get("template", DEFAULT_TEMPLATE)]
        if isinstance(templates, str):
            # A single template written without YAML list syntax: treat it as a
            # one-element list, not a sequence of characters.
            templates = [templates]
        self._templates = [str(t) for t in templates]
        self._template = self._templates[0]
        self._regex = _template_regex(self._template) if not self._projection else None
        # Auto-discovery (name mode) matches `template + discover_suffix` and
        # captures the channel number; needed when the backend exposes no bare
        # per-channel histogram (only the `_physics`/`_pedestal` copies).
        self._discover_suffix = params.get("discover_suffix", "")
        self._discover_regex = (
            _template_regex(self._template + self._discover_suffix) if not self._projection else None
        )
        # Projection mode has no per-channel names to discover: the channel
        # count is the per-channel TH2's x-axis size, learned from the backend
        # (GetNbinsX) via the injected client. Resolved lazily and retried while
        # unknown, so it recovers if the backend wasn't up yet at startup. An
        # explicit `n_channels` (e.g. in tests) skips the backend lookup.
        self._client = params.get("_client")
        self._n_channels = int(params.get("n_channels", 0))
        # Dropdown label style. With `board_labels: true` the labels read
        # "board B · ch L" (FERS, no calo mapping; board size from the `fers:`
        # config section); otherwise enriched with the calo module name.
        self._channels_per_board = (
            int(params["channels_per_board"])
            if params.get("board_labels") and params.get("channels_per_board")
            else None
        )
        self._selected_ch: Optional[int] = None
        # When on, each graph slot overlays several series of the selected
        # channel (one step line each), built from the raw payloads, so the
        # panel needs its own decoder. `split_suffixes` selects which series.
        self._split = bool(params.get("show_trigger_split", False))
        self._split_series = self._build_split_series(params.get("split_suffixes"))
        self._decoder = get_decoder(params.get("decoder", "pure")) if self._split else None

    @staticmethod
    def _build_split_series(suffixes) -> list[tuple[str, str, str]]:
        if not suffixes:
            return TRIGGER_SPLIT_SERIES
        if isinstance(suffixes, str):
            # A single suffix written without YAML list syntax: treat it as a
            # one-element list, not a sequence of characters.
            suffixes = [suffixes]
        series: list[tuple[str, str, str]] = []
        for suffix in suffixes:
            label, color = _SPLIT_SUFFIX_INFO.get(suffix, (suffix.lstrip("_") or "total", theme.PRIMARY))
            series.append((suffix, label, color))
        return series

    def gain_tag(self) -> Optional[str]:
        """`"HG"`/`"LG"` parsed from the (first) template, used to pair this
        selector with the matching FERS board map. None when neither token is
        present, or when the panel spans several gains (multiple templates) — in
        that case any gain's board map links to this single selector."""
        if len(self._templates) > 1:
            return None
        if "HG" in self._template:
            return "HG"
        if "LG" in self._template:
            return "LG"
        return None

    def _projection_probe(self) -> str:
        """A per-channel TH2 name to read the channel count from (its x axis)."""
        suffix = self._split_series[0][0] if self._split_series else ""
        return f"{self._templates[0]}{suffix}"

    def _channels(self) -> list[int]:
        """Sorted list of selectable channel indices."""
        if self._projection:
            # Learn (and cache) the channel count from the TH2 x axis the first
            # time it is needed; retry while still unknown (backend not up yet).
            if self._n_channels <= 0 and self._client is not None:
                self._n_channels = self._client.nbins_x(self._projection_probe()) or 0
            return list(range(self._n_channels))
        params = self.params
        names = params.get("names")
        if names:
            chans = {c for n in names if (c := self._channel_of(n)) is not None}
        else:
            chans = {
                int(m.group(1))
                for n in (params.get("available_histograms") or [])
                if (m := self._discover_regex.match(n))
            }
        return sorted(chans)

    def _channel_of(self, name: str) -> Optional[int]:
        m = self._regex.match(name) if self._regex else None
        return int(m.group(1)) if m else None

    def select_channel(self, ch: int) -> None:
        """Select a channel by index (cross-panel navigation, e.g. clicking a
        cell on a detector / FERS board map). The next poll fetches it."""
        self._selected_ch = int(ch)

    # ---- token / title helpers ------------------------------------------

    def _token(self, template: str, suffix: str, ch: int) -> str:
        """Fetch token for one series of one slot at channel `ch`."""
        if self._projection:
            # `<th2>#projy=<xbin>` — ROOT bins are 1-indexed (channel c -> c+1);
            # resolved by BackendClient into a server-side ProjectionY.
            return f"{template}{suffix}#projy={ch + 1}"
        return f"{template.format(ch=ch)}{suffix}"

    def _slot_tokens(self, template: str, ch: int) -> list[str]:
        if self._split:
            return [self._token(template, suffix, ch) for suffix, _, _ in self._split_series]
        return [self._token(template, "", ch)]

    def _slot_title(self, template: str, ch: Optional[int]) -> str:
        if ch is None:
            return "no channel"
        if self._projection:
            # Prettify the TH2 base name: strip a trailing "_dist" and turn
            # underscores into spaces (FERS_HG_dist -> "FERS HG", ADC_dist -> "ADC").
            base = template[:-len("_dist")] if template.endswith("_dist") else template
            return f"{base.replace('_', ' ')} · ch {ch}"
        return template.format(ch=ch)

    # ---- Panel API -------------------------------------------------------

    def histogram_names(self) -> list[str]:
        ch = self._selected_ch
        if ch is None:
            return []
        out: list[str] = []
        for template in self._templates:
            out += self._slot_tokens(template, ch)
        return out

    def figure_names(self) -> list[str]:
        # In split mode the panel builds its own overlaid figure from the
        # raw payloads, so it doesn't use the framework's per-name figures.
        return [] if self._split else self.histogram_names()

    def control_indices(self) -> list[int]:
        # One 1D plot per template slot.
        return list(range(len(self._templates)))

    def _options(self) -> list[dict]:
        chans = self._channels()
        if self._selected_ch not in chans:
            self._selected_ch = chans[0] if chans else None
        mapping = None if self._channels_per_board else default_mapping()
        opts: list[dict] = []
        for ch in chans:
            if self._channels_per_board:
                # FERS: board/local label, no calo mapping (those module names
                # belong to the ADC channels and would be misleading here).
                board, local = divmod(ch, self._channels_per_board)
                label = f"board {board}  ·  ch {local}"
            else:
                try:
                    label = f"ch {ch}  ·  {mapping.get_channel_name(ch)}"
                except KeyError:
                    label = f"ch {ch}"
            opts.append({"label": label, "value": ch})
        return opts

    def layout(self) -> html.Div:
        options = self._options()
        dropdown = dcc.Dropdown(
            id={"type": "channel-select", "panel": self.panel_id},
            options=options,
            value=self._selected_ch,
            clearable=False,
            placeholder="(no channels on backend)" if not options else "select a channel",
            style={"width": "320px"},
        )
        # One graph slot per template, stacked vertically.
        ch = self._selected_ch
        slots = [
            html.Div(
                className="plot-cell",
                style={"flex": "1", "minWidth": "0"},
                children=[
                    dcc.Graph(
                        id={"type": "panel-graph", "panel": self.panel_id, "index": i},
                        figure=theme.placeholder_figure(self._slot_title(template, ch)),
                        style={"height": "420px"},
                        config={"displayModeBar": False},
                    ),
                    controls_overlay(self.panel_id, i),
                ],
            )
            for i, template in enumerate(self._templates)
        ]
        return html.Div(
            [
                html.Div(
                    style={"display": "flex", "alignItems": "center", "marginBottom": "12px", "gap": "8px"},
                    children=[
                        html.Span("Channel:", style={"color": theme.FG, "fontSize": "13px"}),
                        dropdown,
                    ],
                ),
                html.Div(
                    style={"display": "flex", "flexDirection": "column", "gap": "12px"},
                    children=slots,
                ),
                # Throwaway sink: the selection callback must write to at least
                # one Output. The real state lives in self._selected_ch.
                dcc.Store(id={"type": "channel-select-sink", "panel": self.panel_id}),
            ]
        )

    def render(self, figs, payloads, client_state):
        ch = self._selected_ch
        if ch is None:
            return [theme.placeholder_figure("no channel selected") for _ in self._templates]
        out = []
        for template in self._templates:
            title = self._slot_title(template, ch)
            if self._split:
                # Overlay the configured series (e.g. physics / pedestal) of this
                # slot into one figure. The Plotly legend toggles each series.
                specs = [
                    (payloads.get(self._token(template, suffix, ch)), color, label)
                    for suffix, label, color in self._split_series
                ]
                out.append(overlay_figure(self._decoder, specs, title))
            else:
                token = self._token(template, "", ch)
                out.append(figs.get(token, theme.placeholder_figure(title)))
        return out

    def register_callbacks(self, app: Dash) -> None:
        @app.callback(
            Output({"type": "channel-select-sink", "panel": self.panel_id}, "data"),
            Input({"type": "channel-select", "panel": self.panel_id}, "value"),
            prevent_initial_call=True,
        )
        def _on_select(value):
            # Persist the choice in instance state; the next poll picks it up via
            # histogram_names(). Returning `value` satisfies Dash's Output rule.
            if value is not None:
                self._selected_ch = int(value)
            return value
