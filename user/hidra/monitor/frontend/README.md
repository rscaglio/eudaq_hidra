# HiDRA monitor frontend

A web dashboard that polls the C++ `HidraHttpMonitor` backend
(which exposes histograms over HTTP via ROOT's `THttpServer`) and
renders them as interactive Plotly charts. The layout is declared
in `config.yaml`; custom behaviour is plug-in (Python `Panel`
subclasses).

```
                                  ┌───────────────────────────────┐
   C++ HidraHttpMonitor           │  Browser (Dash app)           │
   ┌──────────────────┐  POST     │  ┌──────────────────────────┐ │
   │ THttpServer      │  /multi.  │  │ dcc.Tabs, dcc.Graph(s),  │ │
   │ /Histograms/...  │◄──json───►│  │ poll-rate dropdown, ...  │ │
   │ port 9090        │  every    │  └──────────────────────────┘ │
   └──────────────────┘  500 ms   │           ▲                   │
                                  │           │ figures           │
                                  │  ┌──────────────────────────┐ │
                                  │  │ Python callbacks         │ │
                                  │  │  ├ poll() — fetch+draw   │ │
                                  │  │  ├ tab switch            │ │
                                  │  │  └ overlay / controls    │ │
                                  │  └──────────────────────────┘ │
                                  │           port 8050            │
                                  └───────────────────────────────┘
```

## Run (one command)

```sh
cd user/hidra/monitor/frontend
./run.sh                            # open http://localhost:8050
```

`run.sh` creates `.venv/` on first launch (with
`--system-site-packages` so the venv inherits the system PyROOT),
installs `requirements.txt`, and serves the Dash app via **gunicorn**
— a production WSGI server, so no "development server" warning.
Dependencies are re-synced automatically whenever `requirements.txt`
changes (or pass `--reinstall` to force it).

If you sourced `user/hidra/misc/setup.sh` from the repo root, the
shell function `run_frontend` is a shortcut for the same thing:

```sh
source user/hidra/misc/setup.sh
run_frontend                        # ./run.sh
run_frontend --port 8060            # forwards args to run.sh
```

Common options:

```sh
./run.sh --port 8060                # override port
./run.sh --host 127.0.0.1           # bind locally only
./run.sh --config other.yaml        # override config file
./run.sh --workers 2                # more gunicorn workers (default 1)
./run.sh --reinstall                # force reinstall of requirements
```

Note on workers: Dash keeps per-process state (backend client,
overlay cache, perf counters) in memory, so multiple gunicorn workers
each have their own copy. The default of 1 is the safe choice for
this dashboard.

Note on browser sessions: some interactive state also lives per
process and is therefore **shared across all connected browsers**, not
isolated per session. In particular the channel-selector's current
channel (`ChannelSelectorPanel._selected`) and the rate panel's
history/EMA (`RatePanel._history`/`_ema`/`_prev_count`) are global: if
two people open the dashboard at once, picking a channel in one tab
changes it for everyone, and the rate sparkline mixes both clients'
poll cadences. This is acceptable for the intended single-screen
control-room use; making it per-session would require routing that
state through per-session `dcc.Store`s instead of instance attributes.

### Dev server (Flask, with `--debug` hot-reload)

If you specifically want the Flask dev server (e.g. for `--debug`
auto-reload), the legacy entry point still works inside the venv:

```sh
source .venv/bin/activate
python app.py --debug
```

You will see the usual "development server" warning — that's by
design; use `./run.sh` instead unless you actually need the
auto-reloader.

The backend must already be running and reachable on the URL set in
`config.yaml` (default `http://localhost:9090`).

## Project layout

```
app.py                       # entry point: load config, build app, run server
config.yaml                  # YOU edit this to add tabs/histograms
requirements.txt
reference/                   # drop .root files here for overlay (gitignored)

hidra_frontend/
  config.py                  # parses config.yaml into typed dataclasses
  backend_client.py          # HTTP wrapper around /h.json and /multi.json
  figure_builder.py          # DecodedHist + Plotly = go.Figure
  theme.py                   # colors + base Plotly layout
  perf.py                    # phase timing helpers used in the poll path
  layout.py                  # top-level Dash layout (header, tabs, stores)
  overlay.py                 # reads reference .root files via uproot

  decoders/                  # JSON payload → DecodedHist
    base.py                  #   shared types (DecodedHist, Decoder)
    pure.py                  #   default: pure numpy, no PyROOT
    pyroot.py                #   fallback: TBufferJSON.ConvertFromJSON

  panels/                    # how a tab's content is built
    base.py                  #   Panel ABC — implement this for custom layouts
    histogram_grid.py        #   "histograms" panel type (the default)
    channel_selector.py      #   stub for future per-channel TH1Ds

  callbacks/                 # Dash callbacks (one file per concern)
    poll.py                  #   the main one: fetch + render
    controls.py              #   pause + poll rate
    overlay.py               #   reference file dropdown
```

## Configuration

Everything is in `config.yaml`. Reference:

```yaml
backend:
  url: http://localhost:9090
  request_timeout_s: 2.0          # HTTP timeout per request

decoder: pure                     # pure (default) or pyroot

polling:
  default_ms: 500                 # initial poll period
  floor_ms: 50                    # UI dropdown never goes below this
  choices_ms: [100, 200, 500, 1000, 2000, 5000]
  server_pump_ms_hint: 20         # shown in status bar, mirrors backend
                                  # PUMP_INTERVAL_MS in dry.ini/hidra.ini

overlay:
  enabled: true                   # set false to hide overlay controls
  search_dir: reference           # where to look for .root files
  default_file: null

tabs:
  - id: summary                   # url-safe slug, must be unique
    label: Summary                # human readable, shown on the tab
    panels:
      - type: histograms          # see "panel types" below
        cols: 2
        histograms: [event_count, events_vs_time]
```

## Common tasks

### Add a histogram to an existing tab

Edit `config.yaml`, find the tab, append the histogram name to the
`histograms:` list of one of its panels. Save and reload the browser
— no restart needed if you re-run `python app.py` after the edit.

### Add a new tab

Append an entry under `tabs:` in `config.yaml`:

```yaml
  - id: my_tab
    label: My tab
    panels:
      - type: histograms
        cols: 2
        histograms: [some_hist, other_hist]
```

### Built-in panel types

- `histograms` — fixed list of histograms in an N-column grid.
  Params: `histograms: [name1, name2, ...]`, `cols: 2` (default).
- `metric` — show each histogram's content as a single big number
  ("scorecard"). Good for counter-like histograms (e.g.
  `event_count`, a TH1I with one bin). The displayed number is the
  sum of all in-range bins. Params: `histograms: [name1, ...]`.
- `channel_selector` — show one per-channel histogram at a time with
  a dropdown to switch channel. Built for the backend's
  `ADC_channel_<N>` TH1Ds. The channel list is auto-discovered from
  the backend (every name matching `template`), so it always matches
  the current VME geo map. Dropdown labels are enriched with the
  detector name from the calo mapping when known (e.g. `ch 5 · M105S`).
  Params: `template: "ADC_channel_{ch}"` (default). To pin an explicit
  set instead of auto-discovery, add `range: [lo, hi]` or a `names`
  list. The selected channel updates the plot on the next poll tick.

### Add a custom panel (custom layout / widgets)

When `histograms` isn't enough — e.g. you want a slider, a multi-row
custom layout, or special interactions — write a Panel subclass.

1. Create `hidra_frontend/panels/my_panel.py`:

   ```python
   from dash import dcc, html
   from .base import Panel

   class MyPanel(Panel):
       def histogram_names(self):
           # Histograms this panel needs on each poll. Can come
           # from self.params (read from config.yaml).
           return self.params.get("histograms", [])

       def layout(self):
           # Build the Dash component tree. Use IDs of the form
           # {"type": "panel-graph", "panel": self.panel_id, "index": i}
           # for any dcc.Graph slot you want the poll callback to fill.
           return html.Div([...])

       def render(self, figs, client_state):
           # Called once per poll. Return one Plotly figure per
           # panel-graph slot, in the same order as layout().
           return [figs.get(n) for n in self.histogram_names()]

       def register_callbacks(self, app):
           # OPTIONAL — only if your panel has its own widgets
           # (sliders, dropdowns, etc.) that need callbacks.
           pass
   ```

2. Register the panel type in `hidra_frontend/panels/__init__.py`:

   ```python
   from .my_panel import MyPanel
   PANEL_TYPES["my_panel"] = MyPanel
   ```

3. Reference it from `config.yaml`:

   ```yaml
   - id: my_tab
     label: My tab
     panels:
       - type: my_panel
         histograms: [foo, bar]
   ```

### Use the overlay (reference histograms from .root files)

Drop one or more `*.root` files into `reference/`. Reload the
dashboard, click **Refresh files**, pick a file from the dropdown,
and a dashed reference trace appears on top of each live histogram
with the same name.

### Change colors / spacing

Edit `hidra_frontend/theme.py`. All colors and the base Plotly
layout used by every figure are defined there.

### Change polling rate at runtime

Use the dropdown in the top bar. Change the available choices via
`polling.choices_ms` in `config.yaml`.

### Switch decoder

`decoder: pyroot` in `config.yaml` falls back to ROOT's
`TBufferJSON.ConvertFromJSON` for decoding payloads. Useful if the
pure decoder doesn't understand a new histogram type yet.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Status bar: "cannot reach backend" | Backend not running, wrong URL, firewall | `curl http://localhost:9090/h.json` and check `backend.url` in `config.yaml`. |
| A graph shows "missing on server" | Histogram name in `config.yaml` doesn't match what the backend exposes | `curl http://localhost:9090/h.json` to see the real names. |
| "decode error" annotation | Decoder raised an exception | Look at the python log; try `decoder: pyroot` as a fallback. |
| Dashboard is sluggish at 100 ms polling | Too many large histograms per poll | Increase the polling period; or check the perf summary in the log (printed every 20 polls) — `poll.to_figure_one` is usually the bottleneck. |
| Browser tab flashes white when switching | You're on an older version without `placeholder_figure` | Pull the latest, or check that `theme.placeholder_figure(name)` is used as the initial `dcc.Graph.figure`. |
| `InvalidCallbackReturnValue` in the log | Race between tab switch and a still-in-flight poll | Already guarded in `poll.py` (`expected vs len(figures_out)`). If it reappears, leave the dashboard up for 1 s and the next poll fixes it. |
| Overlay dropdown is empty | `reference/` doesn't exist or is empty | Create the directory and drop a `.root` file in it. Then click **Refresh files**. |

## Performance notes

The poll callback is the only hot path. It does, in order:

1. `client.fetch_multi(names)` — one `POST /multi.json` batched
   request. Cost: ~15 ms for 6 histograms, dominated by the
   backend's `PUMP_INTERVAL_MS` (default 20 ms — that's the floor).
2. `decoder.decode(payload)` — ~0.2 ms / histogram with the pure
   decoder, ~2 ms / histogram with the pyroot decoder.
3. `_add_trace(fig, decoded, ...)` — ~1-3 ms / histogram (Plotly
   figure construction).
4. Overlay lookup — ~0.1 ms / histogram (cached after the first
   read of each file).

Total at the default config (6 histograms, no overlay) is ~70 ms
per poll. With the default polling rate of 500 ms there's plenty
of headroom.

The phase timer prints a summary every 20 polls to the python log:

```
=== perf summary (window = last 20 polls) ===
  poll.total              total= 1450.0 ms  n=20  mean= 72.500 ms
  poll.to_figure_all      total= 1100.0 ms  n=20  mean= 55.000 ms
  poll.fetch_multi        total=  300.0 ms  n=20  mean= 15.000 ms
  ...
```

Numbers are aggregated across all phases inside `with Phase("..."):`
context managers in `poll.py` and `figure_builder.py`.

## Branch / development workflow

The frontend lives on the `monitor_frontend` branch. The frontend
touches only files under `user/hidra/monitor/frontend/`, so it
never conflicts with backend changes.

Stay in sync with master:

```sh
git fetch
git rebase origin/master
```

Python is **not** wired into CMake. The setup.sh helpers
(`cmake_config`, `build_hidra`, `runhidra`) ignore this folder.

## Roadmap (grep for `TODO(...)`)

- `TODO(monitor_info)` — read `pump_interval_ms` from a backend
  endpoint instead of mirroring it in `config.yaml`.
- `TODO(reset)` — wire a "Reset histograms" button when the backend
  exposes a reset endpoint.
- `TODO(event_display)` — single-event panel type; needs a
  non-cumulative backend feed.
- `TODO(remote_overlay)` — fetch reference files over HTTP instead
  of through the shared filesystem.
