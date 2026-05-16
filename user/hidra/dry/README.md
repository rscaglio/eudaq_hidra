# HidraDry

This folder contains HiDRA "dry" producers: software implementations used for testing and development without real hardware.

## Main Classes

### `HidraDryXDCProducer`

- reads XDC events from ASCII hex files, validates them, and sends EUDAQ events
- controls replay timing (automatic from timestamps or configured spacing)
- provides parsing/file metadata methods (`ReadXDCEvent`, `ReadFileSize`)

### `HidraDryFERSProducer`

- replays FERS data from file as an EUDAQ event stream
- manages event pacing (`sleepUntilNext`)
- gathers input file/read state information (`ReadFileInfo`)
