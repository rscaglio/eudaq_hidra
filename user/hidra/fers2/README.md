HiDRa FERS2 module
===================

Purpose
-------
This folder contains a modular reimplementation of the CAEN FERS integration
used by the HiDRa project. It provides:

- `FERSConfiguration` — parse and load CAEN FERSlib-style configuration files
- `FERSBoard` — a thin wrapper for a single FERS board lifecycle
- `FERSBoardManager` — multi-board orchestration and polling
- `HidraFERS2Producer` — EuDAQ producer integrating the manager into the run loop

Usage
-----
The module is built as a shared eudaq module and is enabled from the
`user/hidra/CMakeLists.txt` with the build option `USER_HIDRA_FERS2_BUILD`.


Notes
-----
- The code expects a system-installed `libcaenferslib.so` (1.3.0) available in
  standard library search paths (/usr/local/lib, /usr/lib).
- The implementation intentionally keeps all logic within `fers2/` to make
  switching between legacy and new producers straightforward.

Contact
-------
For questions about behaviour, configuration format or field tests, ping the
HiDRa maintainers in the repository or open an issue.
