#ifndef HIDRA_FERS2_BOARD_H
#define HIDRA_FERS2_BOARD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "FERSConfiguration.h"
#include "FERSTypes.h"

namespace hidra {
namespace fers2 {

/**
 * @brief High-level wrapper around a single CAEN FERS board.
 *
 * `FERSBoard` encapsulates the lifecycle of a single board: opening the
 * device, applying configuration parameters, starting/stopping acquisition
 * and reading available events. It translates low-level FERS API structures
 * into the module's `FERSEvent` representation.
 *
 * The class is intentionally non-copyable at the API level (it holds a
 * numeric handle) — copies would duplicate the notion of ownership. The
 * implementation uses simple return-boolean error reporting; callers should
 * read `status()` for diagnostic details.
 */
class FERSBoard {
public:
  /**
   * Construct a board object that will represent the device described by
   * `connection_path` (an `Open[n]` string extracted from the config file).
   */
  FERSBoard(int board_id, const std::string& connection_path);

  /**
   * Open the device and prepare readout buffers.
   * @param readout_mode Implementation-specific readout mode (default 0).
   * @return true on success.
   */
  bool Connect(int readout_mode = 0);

  /**
   * Close the device and release resources.
   * @return true on success.
   */
  bool Disconnect();

  /**
   * Apply configuration parameters for this board by consulting the
   * provided `FERSConfiguration`. The `configure_mode` is forwarded to the
   * underlying FERS API (e.g., hard vs soft configuration).
   */
  bool Configure(const FERSConfiguration& config, int configure_mode);

  /**
   * Start/stop acquisition on the board.
   * @param start_mode Mode value forwarded to FERSlib Start/Stop calls.
   * @param run_number Logical run number sent to hardware if applicable.
   */
  bool StartAcquisition(int start_mode, int run_number);
  bool StopAcquisition(int start_mode, int run_number);

  /**
   * Enable or disable the SiPM high voltage channel on this board.
   */
  bool SetHighVoltage(bool on);

  /**
   * Send a raw command to the board through the FERS API.
   * Useful for lower-level control or vendor-specific operations.
   */
  bool SendCommand(uint32_t command);

  /**
   * Read up to `max_events` events from the board. If `max_events == 0`,
   * read all available events. Events are appended to `events`.
   */
  bool ReadAvailableEvents(std::vector<FERSEvent>* events, size_t max_events = 0);

  int board_id() const { return board_id_; }
  int handle() const { return handle_; }
  const std::string& connection_path() const { return connection_path_; }
  const BoardStatus& status() const { return status_; }

private:
  /// Helper: convert the vendor event blob into a `FERSEvent` instance.
  bool SerializeEvent(void* event_ptr, int data_qualifier, FERSEvent* out_event);

  int board_id_ = -1;
  int handle_ = -1;
  std::string connection_path_;
  BoardStatus status_;
};

} // namespace fers2
} // namespace hidra

#endif // HIDRA_FERS2_BOARD_H
