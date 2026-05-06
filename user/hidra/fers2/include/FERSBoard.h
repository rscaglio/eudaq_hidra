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

class FERSBoard {
public:
  FERSBoard(int board_id, const std::string& connection_path);

  bool Connect(int readout_mode = 0);
  bool Disconnect();

  bool Configure(const FERSConfiguration& config, int configure_mode);

  bool StartAcquisition(int start_mode, int run_number);
  bool StopAcquisition(int start_mode, int run_number);

  bool SendCommand(uint32_t command);

  bool ReadAvailableEvents(std::vector<FERSEvent>* events, size_t max_events = 0);

  int board_id() const { return board_id_; }
  int handle() const { return handle_; }
  const std::string& connection_path() const { return connection_path_; }
  const BoardStatus& status() const { return status_; }

private:
  bool SerializeEvent(void* event_ptr, int data_qualifier, FERSEvent* out_event);

  int board_id_ = -1;
  int handle_ = -1;
  std::string connection_path_;
  BoardStatus status_;
};

} // namespace fers2
} // namespace hidra

#endif // HIDRA_FERS2_BOARD_H
