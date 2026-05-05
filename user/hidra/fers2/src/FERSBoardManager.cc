#include "FERSBoardManager.h"

#include <vector>

#include "FERSlib.h"

namespace hidra {
namespace fers2 {

bool FERSBoardManager::BuildBoardsFromConfiguration(const FERSConfiguration &config,
                                                    int first_board_id) {
  boards_.clear();

  int board_id = first_board_id;
  for (const auto &path : config.open_paths()) {
    if (!AddBoard(board_id, path, nullptr)) {
      return false;
    }
    ++board_id;
  }
  return true;
}

bool FERSBoardManager::AddBoard(int board_id, const std::string &connection_path,
                                std::string *error) {
  for (const auto &board : boards_) {
    if (board.board_id() == board_id) {
      if (error != nullptr) {
        *error = "Board id already present: " + std::to_string(board_id);
      }
      return false;
    }
  }

  boards_.emplace_back(board_id, connection_path);
  return true;
}

bool FERSBoardManager::ConnectAll(int readout_mode, std::string *error) {
  for (auto &board : boards_) {
    if (!board.Connect(readout_mode)) {
      if (error != nullptr) {
        *error = "Connect failed for board id " + std::to_string(board.board_id()) +
                 ": " + board.status().last_error;
      }
      return false;
    }
  }
  return true;
}

bool FERSBoardManager::ConfigureAll(const FERSConfiguration &config, int configure_mode,
                                    bool load_config_file_first,
                                    std::string *error) {
  if (load_config_file_first && !config.LoadIntoLibrary(error)) {
    return false;
  }

  for (auto &board : boards_) {
    if (!board.Configure(config, configure_mode)) {
      if (error != nullptr) {
        *error = "Configure failed for board id " + std::to_string(board.board_id()) +
                 ": " + board.status().last_error;
      }
      return false;
    }
  }

  return true;
}

bool FERSBoardManager::StartAll(int start_mode, int run_number, std::string *error) {
  if (boards_.empty()) {
    if (error != nullptr) {
      *error = "No FERS boards configured.";
    }
    return false;
  }

  std::vector<int> handles;
  handles.reserve(boards_.size());
  for (const auto &board : boards_) {
    handles.push_back(board.handle());
  }

  int ret = FERS_StartAcquisition(handles.data(), static_cast<int>(handles.size()), start_mode, run_number);
  if (ret != 0) {
    if (error != nullptr) {
      *error = "FERS_StartAcquisition failed with code " + std::to_string(ret);
    }
    return false;
  }
  return true;
}

bool FERSBoardManager::StopAll(int start_mode, int run_number, std::string *error) {
  if (boards_.empty()) {
    return true;
  }

  std::vector<int> handles;
  handles.reserve(boards_.size());
  for (const auto &board : boards_) {
    handles.push_back(board.handle());
  }

  int ret = FERS_StopAcquisition(handles.data(), static_cast<int>(handles.size()), start_mode, run_number);
  if (ret != 0) {
    if (error != nullptr) {
      *error = "FERS_StopAcquisition failed with code " + std::to_string(ret);
    }
    return false;
  }
  return true;
}

bool FERSBoardManager::DisconnectAll(std::string *error) {
  for (auto &board : boards_) {
    if (!board.Disconnect()) {
      if (error != nullptr) {
        *error = "Disconnect failed for board id " + std::to_string(board.board_id()) +
                 ": " + board.status().last_error;
      }
      return false;
    }
  }
  return true;
}

std::vector<FERSEvent>
FERSBoardManager::ReadAvailableEvents(size_t max_events_per_board,
                                      std::string *error) {
  std::vector<FERSEvent> out;
  for (auto &board : boards_) {
    std::vector<FERSEvent> board_events;
    if (!board.ReadAvailableEvents(&board_events, max_events_per_board)) {
      if (error != nullptr) {
        *error = "Read failed for board id " + std::to_string(board.board_id()) +
                 ": " + board.status().last_error;
      }
      return {};
    }

    out.insert(out.end(), board_events.begin(), board_events.end());
  }

  return out;
}

FERSBoard *FERSBoardManager::FindBoard(int board_id) {
  for (auto &board : boards_) {
    if (board.board_id() == board_id) {
      return &board;
    }
  }
  return nullptr;
}

} // namespace fers2
} // namespace hidra
