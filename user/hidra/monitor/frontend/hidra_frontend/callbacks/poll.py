"""Polling callback: fetch histograms, build Plotly figures, write them into the DOM.

Two callbacks live here:

  1. `update_tab_content(tab_id)` — rebuilds the tab body whenever the
     user clicks a different tab. It writes both the new DOM and a
     small `tab-mount-state` Store. That Store is what `poll()` listens
     to (instead of `tabs.value` directly): Dash applies Store changes
     only after the DOM output is committed, so the poll always sees
     the rebuilt DOM, not the previous tab's.

  2. `poll(n_intervals, tab_mount_state, ...)` — the heart of the
     dashboard. Triggered by `dcc.Interval` ticks and by tab switches.
     For each tick:
       * collect the histogram names the active panels want,
       * fetch them all in one `POST /multi.json` call,
       * build Plotly figures (with optional overlay),
       * push the figures into the `panel-graph` slots via Dash's
         pattern-matching outputs.

Pattern-matching IDs (the curly-brace dicts you'll see below) are how
Dash addresses multiple components with one callback: every panel
graph carries an ID like `{"type": "panel-graph", "panel": <id>,
"index": <i>}`, and the callback's Output uses `ALL` to mean "every
component matching this shape".
"""

from __future__ import annotations

import logging

from dash import ALL, Dash, Input, Output, State, ctx, html, no_update

from ..backend_client import BackendClient
from ..config import Config
from ..decoders import Decoder
from ..figure_builder import to_figure
from ..overlay import OverlayStore
from ..panels.base import Panel
from ..perf import PERF, Phase

logger = logging.getLogger(__name__)

# How often to dump the perf summary to the log (in number of polls).
# 20 polls × 500 ms = roughly every 10 seconds with the default rate.
PERF_LOG_EVERY_POLLS = 20


def register(
    app: Dash,
    config: Config,
    panels_by_tab: dict[str, list[Panel]],
    client: BackendClient,
    overlay_store: OverlayStore,
    decoder: Decoder,
) -> None:
    # Monotonic counter. Each call to update_tab_content bumps it so
    # the tab-mount-state Store data is always different (different
    # "rev" value), which guarantees the poll callback fires — even if
    # the user clicks the same tab twice in a row.
    tab_mount_rev = 0

    @app.callback(
        Output("tab-content", "children"),
        Output("tab-mount-state", "data"),
        Input("tabs", "value"),
    )
    def update_tab_content(tab_id):
        nonlocal tab_mount_rev
        tab_mount_rev += 1
        panels = panels_by_tab.get(tab_id, [])
        children = html.Div([panel.layout() for panel in panels])
        return children, {"tab": tab_id, "rev": tab_mount_rev}

    @app.callback(
        # ALL means "address every component whose ID matches this
        # shape currently in the DOM". The callback must return a list
        # of figures with the same length as the number of matches.
        Output({"type": "panel-graph", "panel": ALL, "index": ALL}, "figure"),
        Output("status-bar", "children"),
        Input("interval", "n_intervals"),
        Input("tab-mount-state", "data"),
        State("client-state", "data"),
        prevent_initial_call=False,
    )
    def poll(n_intervals, tab_mount_state, client_state):
        # Defensive: in some edge cases Dash may invoke the callback
        # with a stale or unexpected Store value (e.g. during hot
        # reload). Treat anything that isn't a dict as empty.
        if not isinstance(tab_mount_state, dict):
            tab_mount_state = {}
        tab_id = tab_mount_state.get("tab")
        client_state = client_state or {}

        with Phase("poll.total"):
            panels = panels_by_tab.get(tab_id, [])

            # Ask every panel of the active tab which histogram names
            # it needs. The de-dup is so two panels showing the same
            # histogram only cause one HTTP fetch.
            wanted: list[str] = []
            for panel in panels:
                for name in panel.histogram_names():
                    if name not in wanted:
                        wanted.append(name)

            with Phase("poll.fetch_multi"):
                data = client.fetch_multi(wanted) if wanted else {}
            reachable = data and any(v is not None for v in data.values())

            overlay_file = client_state.get("overlay_file")

            # Build one Plotly figure per histogram. Overlay lookup is
            # cached inside OverlayStore so repeated polls don't reopen
            # the `.root` file.
            figs_by_name: dict[str, object] = {}
            with Phase("poll.to_figure_all"):
                for name, payload in data.items():
                    with Phase("poll.overlay_lookup"):
                        overlay_hist = overlay_store.get(overlay_file, name) if overlay_file else None
                    with Phase("poll.to_figure_one"):
                        figs_by_name[name] = to_figure(decoder, payload, name, overlay_hist=overlay_hist)

            # Each panel decides which figures land in its own graph
            # slots, in the order matching the `index` IDs it created
            # in `Panel.layout()`. We flatten the result across all
            # panels of the current tab.
            figures_out: list = []
            with Phase("poll.panel_render"):
                for panel in panels:
                    figures_out.extend(panel.render(figs_by_name, data, client_state))

            # Race guard. When the user switches tab quickly, a poll
            # triggered for the previous tab may still be running when
            # the DOM has already been rebuilt for the new tab. If we
            # write our `figures_out` (sized for the old tab) into the
            # new tab's panel-graph slots, Dash raises
            # InvalidCallbackReturnValue. Detect the mismatch and skip
            # the write — the next poll, triggered by tab-mount-state,
            # will be coherent.
            expected = len(ctx.outputs_list[0]) if ctx.outputs_list else 0
            if len(figures_out) != expected:
                return [no_update] * expected, no_update

        # Build the status bar message.
        pump_hint = config.polling.server_pump_ms_hint
        n_ok = sum(1 for v in data.values() if v is not None)
        n_total = len(data)
        overlay_text = f" · overlay: {overlay_file}" if overlay_file else ""
        if not wanted:
            status = f"no histograms requested · server pump ~{pump_hint} ms"
        elif not reachable:
            status = f"⚠️  cannot reach backend at {config.backend.url}{overlay_text}"
        else:
            status = (
                f"✅  {n_ok}/{n_total} histograms · poll #{n_intervals} · "
                f"server pump ~{pump_hint} ms{overlay_text}"
            )

        # Periodic perf summary so it's easy to spot when something
        # gets slow (e.g. lots of histograms added to a tab).
        if n_intervals and n_intervals % PERF_LOG_EVERY_POLLS == 0:
            logger.info("=== perf summary (window = last %d polls) ===", PERF_LOG_EVERY_POLLS)
            for line in PERF.summary_lines():
                logger.info(line)
            PERF.reset()

        return figures_out, status
