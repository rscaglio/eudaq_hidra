#ifndef HIDRA_FERS2_TYPES_H
#define HIDRA_FERS2_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace hidra {
namespace fers2 {

enum class BoardState {
  kDisconnected,
  kConnected,
  kConfigured,
  kRunning
};

struct BoardStatus {
  BoardState state = BoardState::kDisconnected;
  int handle = -1;
  int last_return_code = 0;
  std::string last_error;
  int allocated_readout_bytes = 0;
};

struct FERSEvent {
  int board_id = -1;
  int board_index = -1;
  int data_qualifier = -1;
  double timestamp_us = 0.0;
  uint64_t trigger_id = 0;
  std::vector<uint8_t> payload;
};

} // namespace fers2
} // namespace hidra

#endif // HIDRA_FERS2_TYPES_H
