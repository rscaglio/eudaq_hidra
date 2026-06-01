# HidraHttpMonitor

This folder contains the HiDRA EUDAQ `Monitor` implementation that publishes
online histograms over HTTP.

The monitor receives merged EUDAQ events from `HidraDataCollector` (a sampled
fraction, see `EVENT_PRESCALE`), decodes the detector payloads, fills a set of
ROOT histograms and exposes them through a ROOT `THttpServer` so they can be
browsed live with JSROOT at `http://<host>:<HTTP_PORT>`.

Run it with:

```sh
euCliMonitor -n HidraHttpMonitor
```

## Components

- `HidraHttpMonitor.*` - the EUDAQ monitor plugin: lifecycle hooks, event
  decoding and dispatch to the filler chain.
- `HistogramRegistry.*` - owns the histograms (one `TH1`-derived object per
  name), detached from any ROOT directory. Provides in-place `Reset()` and
  `SaveToFile()`.
- `HistogramPublisher.*` - owns the `THttpServer`, registers the histograms and
  drains the HTTP request queue from a dedicated pump thread. Owns the mutex
  that protects histogram content.
- `FillerChain.*` - ordered list of `IHistogramFiller`s, called under the
  histogram-content lock for every received event.
- `SummaryFiller.*` - run-level histograms (event count, events vs time).
- `XDCFiller.*` - XDC ADC/TDC histograms.

## HTTP server lifecycle (persistence across runs)

The HTTP server is **decoupled from the run lifecycle**: it is started once and
stays up across start/stop of runs, so the histograms of a finished run remain
browsable after the STOP. The long-lived state is bundled in `MonitorContext`
(registry, publisher/server, filler chain and decoders).

| Hook | Action |
|------|--------|
| `DoInitialise` | read init configuration (port, pump interval, prescale, output pattern). No server yet. |
| `DoConfigure` | first call: create `MonitorContext`, register fillers and **start the HTTP server** with empty histograms. Subsequent calls (reconfigure): keep the server, only rebuild the decoders. In both cases the histogram **contents** are cleared (see below). |
| `DoStartRun` | reset histograms and per-run state (event counter, telemetry, fillers' run-relative state such as the start-of-run time reference), then keep serving. |
| `DoStopRun` | log per-run telemetry and snapshot the histograms to a ROOT file. The server and the histograms stay alive and browsable. |
| `DoReset` | clear histogram contents and fillers' run-relative state. |
| `DoTerminate` | destroy the context, which stops the HTTP server. |
| `DoReceive` | decode the sub-events and fill the histograms. |

Because the server stays up, histograms are **reset in place** (`TH1::Reset()`),
never re-created: the `THttpServer` keeps pointing at the same `TH1` objects, so
their registered pointers stay valid. This implies the **set of histograms and
their binning must be fixed** (independent of the run configuration); today the
fillers book fixed histograms, so this holds.

### Histogram reset semantics

Two distinct things can be reset:

- `registry.Reset()` - zeroes the **histogram bin contents**.
- `chain.Reset()` - resets the fillers' **run-relative internal state** that is
  not stored in the histograms (e.g. `SummaryFiller`'s start-of-run timestamp,
  the time origin of the *events vs time* histogram).

They are applied as follows:

| Hook | `registry.Reset()` | `chain.Reset()` |
|------|:---:|:---:|
| `DoConfigure` | yes | no |
| `DoStartRun` | yes | yes |
| `DoReset` | yes | yes |

At `DoConfigure` only the histogram contents are cleared: the fillers'
run-relative state (e.g. the start-of-run time reference) is established at
`DoStartRun`, since a configure may happen well before the run actually starts.
No events are received between configure and start, so leaving the fillers'
state untouched at configure is safe.

## End-of-run ROOT snapshot

At every `DoStopRun` the current histograms are written to a ROOT file via
`HistogramRegistry::SaveToFile()` (the histograms remain owned by the registry;
writing does not transfer ownership). This lets users keep and compare the
histograms of past runs offline.

The output path is built from `HISTO_OUTPUT_PATTERN` with `eudaq::FileNamer`
(same mechanism as the data collector's `EUDAQ_FW_PATTERN`). Setting the pattern
to an empty string disables the snapshot.

## Configuration

Init configuration (`[Monitor.HidraHttpMonitor]` in the `.ini`), read in
`DoInitialise()`:

| Key | Default | Meaning |
|-----|---------|---------|
| `HTTP_PORT` | `9090` | TCP port of the HTTP server. |
| `PUMP_INTERVAL_MS` | `20` | Period of the pump thread draining the HTTP queue (clamped to >= 5 ms). |
| `EVENT_PRESCALE` | `1` | Process 1 event every N (>= 1) to reduce load. |
| `HISTO_OUTPUT_PATTERN` | `out_data/monitor_run$6R_$12D$X` | `FileNamer` pattern for the end-of-run ROOT file. `$R` run number, `$D` timestamp, `$X` extension (`.root`). Empty disables saving. |

Run configuration (`[Monitor.HidraHttpMonitor]` in the `.conf`), read in
`DoConfigure()`:

| Key | Default | Meaning |
|-----|---------|---------|
| `VME_CRATE_1` | (empty) | `geo:module` map describing the XDC VME crate, passed to the XDC decoder. |

## Threading & locking model

Threads:

- `T_ctrl` - RunControl thread running the lifecycle hooks.
- `T_recv` - DataReceiver thread calling `DoReceive()`.
- `T_pump` - publisher thread periodically flushing the HTTP queue.
- `T_http` - civetweb HTTP I/O thread (does not modify histograms).

There are two locks, and they protect two different things. They are always
taken in the order **`m_state_mutex` first, then `publisher.Mutex()`**, never the
reverse (taking them in the opposite order in two different threads could
deadlock).

### `publisher.Mutex()` — protects histogram *content*

This is a plain `std::mutex`, so it has only one mode: **exclusive**. Every
acquisition excludes every other; there is no "shared" variant.

It guards the bin contents and ROOT-internal state of the `TH1` objects. It is
held (exclusively) by:

- the pump thread while serialising the histograms to JSON (`ProcessRequests`);
- the fillers while updating histograms (`FillerChain::Fill`);
- the histogram resets (`registry.Reset()` / `chain.Reset()`);
- the end-of-run ROOT save (`registry.SaveToFile()`);
- the telemetry reset/log.

A plain exclusive mutex (rather than a shared one) is the right choice here. The
primary reason is that the pump thread *reads* the histograms (serialising them
to JSON) while a filler may *write* to them — an unambiguous read/write race that
must be serialised. On top of that, ROOT's `TH1` does not even guarantee safe
concurrent *read/read*, for two reasons:

- some apparently-const operations mutate internal state: e.g. statistics and
  serialisation may flush a histogram's internal fill buffer (`BufferEmpty()`),
  turning a "read" into a write;
- ROOT is not thread-safe by default — serialisation drives process-global
  machinery (the `TClass`/`TStreamerInfo` caches, the `TBuffer`, global error
  handling) that two threads can corrupt unless `ROOT::EnableThreadSafety()` is
  used.

Since every party is effectively a writer, a shared mode would buy nothing. (In
this monitor there is anyway a single histogram reader — the pump thread — so
read/read never actually occurs; the lock chiefly serialises pump-read vs
filler-write.)

### `m_state_mutex` — protects the context *structure*

This is a `std::shared_mutex`, and here the shared/unique distinction is
meaningful. It guards **the existence of `MonitorContext` and the identity of the
decoders** (the things `DoReceive` dereferences), *not* the histogram contents.
The rule is the classic readers/writer rule:

- **shared lock = "I only *use* the context as it is"**: I read the decoders,
  call into the filler chain, etc., without changing which objects exist. Several
  such users may proceed concurrently. Taken by `DoReceive`, `DoStartRun`,
  `DoStopRun`, `DoReset`.
- **unique lock = "I *change* the structure other users depend on"**: I create or
  destroy the context, or swap the decoders for new ones. This must exclude every
  shared user, otherwise a `DoReceive` could be decoding with a decoder that is
  being replaced or destroyed (data race / use-after-free). Taken by
  `DoConfigure` (creates the context / swaps decoders) and `DoTerminate`
  (destroys it).

Note that `DoStartRun`/`DoStopRun`/`DoReset` take `m_state_mutex` only **shared**
even though they reset or save histograms: they do not change the structure
(the context keeps existing, the decoders keep their identity), they only touch
histogram *content* — and that content is serialised by the *separate*
`publisher.Mutex()`. Holding `m_state_mutex` shared is just enough to guarantee
the context is not destroyed underneath them by a concurrent `DoTerminate`. This
is the whole point of the two-layer design: structural lifetime on one lock,
content access on the other.

A unique lock also waits for all in-flight shared holders to finish. That is what
makes `DoConfigure` (decoder swap) and `DoTerminate` (context destruction) safe
against a `DoReceive` that is still running.

### Per-hook locking summary

| Hook | `m_state_mutex` | `publisher.Mutex()` |
|------|-----------------|---------------------|
| `DoReceive` | shared | exclusive (inside `FillerChain::Fill`, decoding stays outside it) |
| `DoConfigure` | unique | exclusive (to clear histogram contents) |
| `DoStartRun` | shared | exclusive (reset histograms + telemetry) |
| `DoStopRun` | shared | exclusive (log telemetry + ROOT save) |
| `DoReset` | shared | exclusive (reset histograms) |
| `DoTerminate` | unique | not taken — destroying the context calls `publisher.Stop()`, which joins the pump thread; the pump thread itself needs `publisher.Mutex()`, so we must **not** hold it here or the join would deadlock |
