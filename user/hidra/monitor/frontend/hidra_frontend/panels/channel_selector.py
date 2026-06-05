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
        # One dropdown can drive several per-channel histograms of the *same*
        # channel index (e.g. FERS HG and LG side by side): `templates` is a
        # list and the panel shows one graph slot per template. A single
        # `template` is the common case. The first template is the one used for
        # channel discovery and for the dropdown value / cross-nav selection.
        templates = params.get("templates")
        if not templates:
            templates = [params.get("template", DEFAULT_TEMPLATE)]
        self._templates = list(templates)
        self._template = self._templates[0]
        self._regex = _template_regex(self._template)
        # Auto-discovery matches `template + discover_suffix` and rebuilds the
        # base name from the captured channel. Needed when the backend exposes
        # no bare per-channel histogram (e.g. FERS only has the `_physics` /
        # `_pedestal` copies), so we discover via one of the suffixed names.
        self._discover_suffix = params.get("discover_suffix", "")
        self._discover_regex = _template_regex(self._template + self._discover_suffix)
        # Dropdown label style. With `board_labels: true` the labels read
        # "board B · ch L" (FERS, no calo mapping; board size injected from the
        # `fers:` config section); otherwise they are enriched with the calo
        # module name from the ADC mapping (e.g. "ch 5 · M105S").
        self._channels_per_board = (
            int(params["channels_per_board"])
            if params.get("board_labels") and params.get("channels_per_board")
            else None
        )
        self._names: list[str] = []
        self._selected: Optional[str] = None
        # When on, the single graph slot overlays several series of the
        # selected channel (one step line each). The panel builds its own
        # figure from the raw payloads, so it needs its own decoder (like
        # DetectorPanel). `split_suffixes` selects which series to overlay;
        # default = inclusive total + physics + pedestal.
        self._split = bool(params.get("show_trigger_split", False))
        self._split_series = self._build_split_series(params.get("split_suffixes"))
        self._decoder = get_decoder(params.get("decoder", "pure")) if self._split else None

    @staticmethod
    def _build_split_series(suffixes: Optional[list[str]]) -> list[tuple[str, str, str]]:
        if not suffixes:
            return TRIGGER_SPLIT_SERIES
        series: list[tuple[str, str, str]] = []
        for i, suffix in enumerate(suffixes):
            label, color = _SPLIT_SUFFIX_INFO.get(suffix, (suffix.lstrip("_") or "total", theme.PRIMARY))
            series.append((suffix, label, color))
        return series

    def gain_tag(self) -> Optional[str]:
        """`"HG"`/`"LG"` parsed from the template, used to pair this selector
        with the matching FERS board map. None when neither token is present
        or when the panel spans several gains (multiple templates) — in that
        case any gain's board map links to this single selector."""
        if len(self._templates) > 1:
            return None
        if "HG" in self._template:
            return "HG"
        if "LG" in self._template:
            return "LG"
        return None

    def _discover(self, available: list[str]) -> list[str]:
        matched: list[tuple[int, str]] = []
        for name in available:
            m = self._discover_regex.match(name)
            if m:
                # Rebuild the base (unsuffixed) name from the captured channel.
                matched.append((int(m.group(1)), self._template.format(ch=int(m.group(1)))))
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
        # Recompute the channel list on every call.
        params = self.params
        names = params.get("names")
        if not names:
            names = self._discover(params.get("available_histograms") or [])
        self._names = list(names)
        # Refresh the selection if it is no longer valid.
        if self._selected not in self._names:
            self._selected = self._names[0] if self._names else None
        mapping = None if self._channels_per_board else default_mapping()
        opts: list[dict] = []
        for name in self._names:
            ch = self._channel_of(name)
            if ch is None:
                opts.append({"label": name, "value": name})
                continue
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
            opts.append({"label": label, "value": name})
        return opts

    # ---- Panel API -------------------------------------------------------

    def _current_ch(self) -> Optional[int]:
        """Selected channel index (from the first-template dropdown value)."""
        return self._channel_of(self._selected) if self._selected else None

    def histogram_names(self) -> list[str]:
        ch = self._current_ch()
        if ch is None:
            return []
        names: list[str] = []
        for template in self._templates:
            base = template.format(ch=ch)
            if self._split:
                # Fetch the configured series (default: inclusive + physics +
                # pedestal) for this template's slot; they are overlaid.
                names += [base + suffix for suffix, _, _ in self._split_series]
            else:
                names.append(base)
        return names

    def figure_names(self) -> list[str]:
        # In split mode the panel builds its own overlaid figure from the
        # raw payloads, so it doesn't use the framework's per-name figures.
        return [] if self._split else self.histogram_names()

    def control_indices(self) -> list[int]:
        # One 1D plot per template slot.
        return list(range(len(self._templates)))

    def layout(self) -> html.Div:
        options = self._options()
        dropdown = dcc.Dropdown(
            id={"type": "channel-select", "panel": self.panel_id},
            options=options,
            value=self._selected,
            clearable=False,
            placeholder="(no channels on backend)" if not options else "select a channel",
            style={"width": "320px"},
        )
        # One graph slot per template, stacked vertically. With a single
        # template this is just the usual single plot.
        ch = self._current_ch()
        slots = [
            html.Div(
                className="plot-cell",
                style={"flex": "1", "minWidth": "0"},
                children=[
                    dcc.Graph(
                        id={"type": "panel-graph", "panel": self.panel_id, "index": i},
                        figure=theme.placeholder_figure(
                            template.format(ch=ch) if ch is not None else "no channel"
                        ),
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
                # Throwaway sink: the selection callback must write to at
                # least one Output. The real state lives in self._selected.
                dcc.Store(id={"type": "channel-select-sink", "panel": self.panel_id}),
            ]
        )

    def render(self, figs, payloads, client_state):
        ch = self._current_ch()
        if ch is None:
            return [theme.placeholder_figure("no channel selected") for _ in self._templates]
        out = []
        for template in self._templates:
            base = template.format(ch=ch)
            if self._split:
                # Overlay the configured series (e.g. physics / pedestal) of
                # this template into one figure. The Plotly legend acts as the
                # per-series on/off toggle (all shown by default).
                specs = [
                    (payloads.get(base + suffix), color, label)
                    for suffix, label, color in self._split_series
                ]
                out.append(overlay_figure(self._decoder, specs, base))
            else:
                out.append(figs.get(base, theme.placeholder_figure(base)))
        return out

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
