# HidraDataCollector

This folder contains the HiDRa EUDAQ `DataCollector` implementation and the
pluggable asynchronous writers used by the collector.

The collector receives merged EUDAQ events from the producers, builds the final
HiDRa merged event, and then forwards it to one or both of the available output
paths:

- binary writer: the existing HIDRA merged binary format handled by
  `EventSerializer`
- ROOT writer: an optional ROOT ntuple output for quick analysis

## Components

- `HidraDataCollector.cc` - merges producer subevents and routes completed
  events to the configured writers via thread-safe queues
- `HidraMergedBinaryWriter.*` - asynchronous binary writer for the HIDRA event
  format with dedicated worker thread
- `HidraRootEventWriter.*` - asynchronous ROOT ntuple writer with dedicated
  worker thread
- `HidraRootPayloadDecoders.*` - detector-specific payload decoders used by the
  ROOT writer

## Architecture

The collector implements a **producer-consumer pattern** with dedicated I/O threads:

1. **Producer thread** (main DataCollector):
   - Receives events from detector producers via EUDAQ
   - Merges sub-events from multiple sources into unified events
   - Enqueues complete merged events to writer queues
   - Returns immediately (non-blocking)

2. **Consumer threads** (one per writer):
   - Each writer (`HidraMergedBinaryWriter`, `HidraRootEventWriter`) owns a
     dedicated worker thread
   - Dequeues events from the writer's queue
   - Performs all I/O operations (file open/write/close, tree operations)
   - Handles flushing and cleanup on shutdown

**Key design principle**: All I/O for a writer happens exclusively in that
writer's worker thread. The main DataCollector thread never touches file
handles or ROOT objects directly. This eliminates:

- Race conditions on file/ROOT object access
- Complexity of cross-thread synchronization
- Deadlocks from mutex contention
- ROOT thread-safety violations (ROOT requires single-threaded access)

The two writers are completely independent, so the system naturally supports
binary-only, ROOT-only, or dual output modes without additional locking.

## Build options

The dc module is built from `user/hidra/dc/CMakeLists.txt` and supports an
optional ROOT writer build.

- `USER_HIDRA_DC_ROOT_OUTPUT=ON` enables compilation of the ROOT writer
- if ROOT is not available, the ROOT writer is disabled automatically

The module still builds the binary writer path regardless of ROOT support.

## Run-time configuration

The collector reads these configuration keys from the `DataCollector` section
of the `.conf` file:

- `WRITE_BINARY_OUTPUT` - enable or disable the standard binary output
- `WRITE_ROOT_OUTPUT` - enable or disable the ROOT ntuple output
- `WRITER_FLUSH_INTERVAL_MS` - polling / flush interval used by the writers
- `WRITER_FLUSH_EVERY_EVENTS` - flush after this many queued events

Example:

```ini
[DataCollector.HidraDataCollector]
EUDAQ_FW = native
EUDAQ_FW_PATTERN = out_data/run$3R_$12D$X
WRITE_BINARY_OUTPUT = 1
WRITE_ROOT_OUTPUT = 0
WRITER_FLUSH_INTERVAL_MS = 50
WRITER_FLUSH_EVERY_EVENTS = 32
```

## Output modes

- Binary only: set `WRITE_BINARY_OUTPUT = 1` and `WRITE_ROOT_OUTPUT = 0`
- ROOT only: set `WRITE_BINARY_OUTPUT = 0` and `WRITE_ROOT_OUTPUT = 1`
- Dual output: set both to `1`

The two writers are independent, so the collector can write to either output or
to both in parallel.

## Binary format

The binary writer uses the existing HIDRA merged format documented in
[../DataFormatv3.md](../DataFormatv3.md).

That document describes the event header, detector block layout, detector mask,
and trailer written by `EventSerializer`.

## ROOT layout

The ROOT writer keeps the same general structure used in the 2025 TB
conversion flow:

- event-level metadata branches for run/event/timing information
- detector quantities stored as branch vectors (`q_det`, `q_name`, `q_value`,
  `q_unit`)
- detector-specific parsing is separated so XDC and FERS handling can evolve
  independently

The ROOT writer is intended for quick inspection and analysis rather than for
preserving the raw binary payload as-is.

## Notes on payload decoding

The payload decoders are intentionally kept modular:

- XDC payload handling lives in the XDC decoder path
- FERS payload handling lives in the FERS decoder path
- the generic decoder provides fallback quantities for unknown payloads

This structure makes it easy to refine the decoding later without changing the
collector or writer orchestration logic.

### Modifying payload parsing for ROOT output

The decoders are implemented in `HidraRootPayloadDecoders.hh/cc`. To
check or modify how payloads are parsed:

1. **Locate the relevant decoder**: 
   - For XDC detectors: `HidraXdcPayloadDecoder`
   - For FERS detectors: `HidraFersPayloadDecoder`
   - For unknown detectors: `HidraGenericPayloadDecoder` (fallback)

2. **Modify the `Decode()` method**:
   Each decoder has a `Decode(const eudaq::Event&, Quantities&)` method that
   extracts quantities from the subevent payload and populates the `Quantities`
   struct with detector-specific fields.

3. **Add new quantities**:
   - Define the new field in the `Quantities` struct
   - Extract it in the `Decode()` method from the subevent payload
   - The `HidraRootEventWriter::WriteEvent()` method will automatically
     store it in the ROOT tree branches

4. **ROOT tree branches**:
   The ROOT writer stores all quantities as vectors in branches:
   - `q_det` - detector ID for each quantity
   - `q_name` - quantity name (e.g., "energy", "amplitude")
   - `q_value` - numeric value
   - `q_unit` - unit string

Example: to add a new XDC quantity, modify `HidraXdcPayloadDecoder::Decode()`
to extract and store the value, then update the `Quantities` struct accordingly.

The worker thread that owns the ROOT I/O will handle writing all quantities to
the ntuple automatically.
