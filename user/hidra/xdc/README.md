# HidraQTPDProducer Trigger Workflow

This document describes the trigger-handling logic currently implemented in
`user/hidra/xdc/src/HidraQTPDProducer.cc`.

## V977 Channel Mapping

The producer treats the V977 inputs as:

| Signal | Input enum | Channel |
| --- | --- | --- |
| Fast trigger gate | `V977IN::cFastGate` | 0 |
| Physics trigger flag | `V977IN::cPhy` | 1 |
| Pedestal trigger flag | `V977IN::cPed` | 2 |
| Spill level | `V977IN::cSpill` | 3 |

The V977 outputs controlled by the producer are:

| Output | Output enum | Channel |
| --- | --- | --- |
| Global trigger veto | `V977OUT::cVeto` | same as `V977IN::cFastGate`, currently 0 |
| Physics veto | `V977OUT::cPhyVeto` | 4 |
| Pedestal veto | `V977OUT::cPedVeto` | 5 |

`SetVetoTriPedPhy(setTriHigh, setPedHigh, setPhyHigh)` is the central helper
for these outputs. Each argument uses this convention:

| Value | Meaning |
| --- | --- |
| `1` | force that output high |
| `0` | force that output low |
| `-1` | leave that output unchanged |

## Run Start

When `DoStartRun()` is called:

1. Any previous acquisition thread is stopped.
2. The run number is copied from EUDAQ.
3. Runtime counters are reset:
   - `m_evt`
   - `m_evt_phy`
   - `m_evt_ped`
   - `m_spillCount`
   - `m_evtTimeNs`
4. `m_running` is set to `true`.
5. A BORE event is sent.
6. The V977 is reset for the run:
   - `V977_OUTPUT_CLEAR_REG` is written with `0xF000`.
   - `V977_INPUT_SET_REG` is written with `0x0000`.
7. The global trigger is vetoed with `VetoTrigger()`.
8. The acquisition thread starts and runs `MainLoop()`.

At this point the loop owns trigger handling until stop/reset/terminate.

## Main Loop

`MainLoop()` runs while `m_running` is true.

```text
while running:
  if controller is not ready:
    wait/continue

  read V977 flip-flop pattern

  if fast trigger gate is latched:
    classify trigger
    check spill level
    read QDC data and send event
    update counters

  publish status counters

  choose whether the next accepted trigger should be pedestal or physics
  release global trigger veto
```

## Trigger Detection

Each loop reads the V977 single-hit register:

```cpp
pattern.raw = ReadReg(V977_SINGLE_READ_REG, m_v977Base);
```

The pattern is decoded as:

| Pattern field | Code condition |
| --- | --- |
| `pattern.trigger` | bit `V977IN::cFastGate` is set |
| `pattern.physics` | bit `V977IN::cPhy` is set |
| `pattern.pedestal` | bit `V977IN::cPed` is set |

Only `pattern.trigger` causes event readout. If it is false, the loop still
updates status and decides the next trigger type, but no event is read.

## Trigger Classification

When `pattern.trigger` is true:

1. The event timestamp is taken from `hidra::utils::getTimens()`.
2. `m_TriggerMask` is reset to `0`.
3. If `pattern.physics` is true, bit `0b01` is set.
4. If `pattern.pedestal` is true, bit `0b10` is set.

The resulting trigger masks are:

| Mask | Meaning |
| --- | --- |
| `0b00` | fast trigger without physics or pedestal classification |
| `0b01` | physics trigger |
| `0b10` | pedestal trigger |
| `0b11` | both physics and pedestal bits were latched |

If both physics and pedestal bits are set, the producer logs an error but still
continues with event readout. The ambiguous `0b11` mask is stored in the event
tag.

## Spill Check

For every accepted fast trigger, the loop checks the current spill input:

```cpp
getSpillSignal()
```

This reads `V977_INPUT_READ_REG` and tests bit `V977IN::cSpill`.

If the spill bit is not high, the producer logs an error:

```text
This trigger evt ... is received out of spill
```

The event is still read out and sent. The spill check is diagnostic only in the
current implementation.

## QDC Readout

`ReadOneBlockAndSendEvent()` handles the QDC side.

1. It waits for all configured boards to be ready and not busy.
   - Timeout: `100 us`
   - Poll interval: `10 us`
2. If boards are not ready before the timeout:
   - an error is logged,
   - V977 flip-flop clearing is requested,
   - no event is sent.
3. If boards are ready, the producer performs a FIFO MBLT read from
   `BLT_READ_ADDRESS`.
4. If the byte count is zero or invalid, the error is logged and no event is
   sent.
5. Otherwise, `SendDataEvent(byteCount)` is called.
6. V977 flip-flop clearing is requested.

The data event is created as `CAENQTPDRaw` and carries:

| Event field/tag | Value |
| --- | --- |
| trigger number | `m_evt` |
| event number | `m_evt` |
| run number | `m_runNumber` |
| block 0 | raw QDC bytes |
| `spillNumber` | `m_spillCount` |
| `triggerMask` | `m_TriggerMask` |
| `endianness` | `BE32` |
| timestamp | `m_evtTimeNs` to `m_evtTimeNs + 100` |
| `detectorDataSize` | raw byte count |

## Counter Update

After `ReadOneBlockAndSendEvent()` returns, `MainLoop()` increments counters:

1. `m_evt` is incremented for every fast trigger path, regardless of whether
   the QDC read actually sent an event.
2. `m_evt_phy` is incremented only if `m_TriggerMask == 0b01`.
3. `m_evt_ped` is incremented only if `m_TriggerMask == 0b10`.

Events with masks `0b00` or `0b11` increment only `m_evt`.

## Status Update

Every loop iteration sends status tags:

| Status tag | Value |
| --- | --- |
| `PhyTrigN` | `m_evt_phy` |
| `PedTrigN` | `m_evt_ped` |
| `SpillN` | `m_spillCount` |

The loop also logs a debug line with the total event count, trigger mask,
physics count, pedestal count, and spill count.

## Selecting The Next Trigger Type

At the end of every loop iteration, the producer chooses whether to accept a
pedestal trigger or a physics trigger next:

```cpp
requestPedestalNext()
```

The current rule is:

```cpp
((double)m_evt_phy / (double)m_evt_ped) > 10
```

If the ratio is greater than 10:

```cpp
SetVetoTriPedPhy(-1, 0, 1);
ReleaseTriggerVeto();
```

This leaves the global veto unchanged at first, clears the pedestal veto, sets
the physics veto, then releases the global trigger veto. In effect, the next
accepted classified trigger is intended to be pedestal.

Otherwise:

```cpp
SetVetoTriPedPhy(-1, 1, 0);
ReleaseTriggerVeto();
```

This sets the pedestal veto, clears the physics veto, then releases the global
trigger veto. In effect, the next accepted classified trigger is intended to be
physics.

## Run Stop

When `DoStopRun()` is called:

1. `m_running` is set to `false`.
2. `m_onspill` is set to `false`.
3. The acquisition thread is joined.
4. An EORE event is sent.
5. The global trigger is vetoed again with `VetoTrigger()`.

## Important Current Caveats

- `m_spillCount` is reset and reported, but it is not incremented by the active
  trigger path. The previous spill-state handlers are currently commented out.
- `requestPedestalNext()` divides by `m_evt_ped`. At the beginning of a run this
  counter is zero, so the first iterations depend on floating-point division by
  zero behavior.
- `RequestV977FlipFlopClear()` sets `m_clearRequested`, but
  `ClearV977FlipFlopsIfRequested()` is not called by the active `MainLoop()`.
  The active loop has a TODO asking whether the flip-flops should be cleared.
- `m_evt` is incremented after `ReadOneBlockAndSendEvent()` returns, even when
  readout failed before sending an event.
- Out-of-spill triggers are logged but not rejected.
