#ifndef HIDRA_FERS2_BOARD_MANAGER_H
#define HIDRA_FERS2_BOARD_MANAGER_H

#include <cstddef>
#include <string>
#include <vector>

#include "FERSBoard.h"
#include "FERSConfiguration.h"
#include "FERSTypes.h"

namespace hidra {
namespace fers2 {

class FERSBoardManager {
public:
  bool BuildBoardsFromConfiguration(const FERSConfiguration &config,
                                    int first_board_id = 0);

  bool AddBoard(int board_id, const std::string &connection_path,
                std::string *error = nullptr);

  bool ConnectAll(int readout_mode = 0, std::string *error = nullptr);
  bool ConfigureAll(const FERSConfiguration &config, int configure_mode,
                    bool load_config_file_first = true,
                    std::string *error = nullptr);

  bool StartAll(int start_mode, int run_number, std::string *error = nullptr);
  bool StopAll(int start_mode, int run_number, std::string *error = nullptr);
  bool DisconnectAll(std::string *error = nullptr);

  std::vector<FERSEvent> ReadAvailableEvents(size_t max_events_per_board = 0,
                                             std::string *error = nullptr);

  FERSBoard *FindBoard(int board_id);
  const std::vector<FERSBoard> &boards() const { return boards_; }

private:
  std::vector<FERSBoard> boards_;
};

} // namespace fers2
} // namespace hidra

#endif // HIDRA_FERS2_BOARD_MANAGER_H
