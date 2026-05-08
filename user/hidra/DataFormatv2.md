# HIDRA Binary Event Format (v2)

The binary event format is produced by the `EventSerializer` utility.

All fields are encoded in **little-endian** format.


## Event Structure

```text
+----------------------+
| Event Header         |
+----------------------+
| Detector Event #0    |
+----------------------+
| Detector Event #1    |
+----------------------+
| ...                  |
+----------------------+
| Event Trailer        |
+----------------------+
```

The event may contain up to 8 detector subevents.



## Event Header

| Offset | Type      | Field            | Description            |
| ------ | --------- | ---------------- | ---------------------- |
| 0      | uint16    | `marker`         | `0xB0BF`               |
| 2      | uint8     | `dataVersion`    | Format version `0x02`	 |
| 3      | uint32    | `headerSize`     | Header size in bytes   |
| 7      | uint32    | `trailerSize`    | Trailer size in bytes  |
| 11     | uint32    | `eventSize`      | Total event size, bytes|
| 15     | uint16    | `runNumber`      | Run number             |
| 17     | uint32    | `eventNumber`    | Trigger/event number   |
| 21     | uint32    | `spillNumber`    | Spill number           |
| 25     | uint64    | `eventTime`      | Global timestamp       |
| 33     | uint8     | `triggerMask`    | Trigger mask           |
| 34     | uint64    | `reserved64`     | Reserved               |
| 42     | uint32    | `reserved32`     | Reserved               |
| 46     | uint8     | `detectorMask`   | Active detectors bitmap|
| 47     | uint16[8] | `detectorSize[]` | Detector size metadata |
| 63     | uint16    | `endMarker`      | `0xBBBB`               |

The event header has a fixes size of `65 bytes (0x41)`

### Detector Mask

The `detectorMask` field encodes which Producers (detectors) are present in the event.
Each bit corresponds to a detector ID (`detID`), where bit `N` represents detector `N`.

Detector IDs are assigned in the `DataCollector` configuration through the `EXPECTED_SOURCES` entry in the `.conf` file:

```ini
EXPECTED_SOURCES = 6:DryFERSProducer,7:DryXDCProducer
```

Each source is specified as:

```text
detectorID:ProducerRuntimeName
```

where:

* `detectorID` is an integer between `0` and `7`
* `ProducerRuntimeName` is the runtime name of the EUDAQ Producer application

The following detector ID convention is recommended:

| detID (bit) | Producer |
| ----------- | -------- |
| 0           | Don't use |
| 1           | XDC       |
| 2           | FERS      |
| 6           | Dry XDC   |
| 7           | Dry FERS  |


Index 0 is assigned when the collector is run is `single producer` mode.

Example: `detectorMask = 0b00000101` means that detectors with `detID = 0` and `detID = 2` are present in the event. The corresponding detector event data blocks follow after the Header endMarker, ordered by increasing `detID`.



## Detector Event

Each detector payload is stored as:

| Offset   | Type   | Field            | Description                |
| -------- | ------ | ---------------- | -------------------------- |
| 0        | uint16 | `marker`         | `0xDEDE`                   |
| 2        | uint8  | `detectorID`     | Detector ID                |
| 3        | uint32 | `eventNumber`    | Local event/trigger number |
| 7        | uint32 | `spillNumber`    | Spill number       |
| 11       | uint64 | `eventTimeBegin` | Begin timestamp    |
| 19       | uint64 | `eventTimeEnd`   | End timestamp      |
| 27       | uint32 | `reserved`       | Reserved           |
| 31       | byte[] | `payload`        | Detector payload   |
| variable | uint16 | `endMarker`      | `0xDDDD`           |

The payload is the concatenation of all EUDAQ blocks associated to the detector.



## Event Trailer

| Type   | Value    |
| ------ | -------- |
| uint16 | `0xD04E` |
