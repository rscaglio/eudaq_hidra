#include "FERSBoardManager.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "FERSlib.h"
#include "FersException.h"

namespace hidra {
namespace fers2 {

std::string BuildConcentratorSummary(const FERS_CncInfo_t& info) {
  std::ostringstream oss;
  oss << "PID=" << info.pid << " Model=" << info.ModelName << " Code=" << info.ModelCode
      << " FW=" << info.FPGA_FWrev << " SW=" << info.SW_rev << " Links=" << info.NumLink;
  return oss.str();
}

void FERSBoardManager::BuildBoardsFromConfiguration(const FERSConfiguration& config,
                                                   int first_board_id,
                                                   int readout_mode,
                                                   int configure_mode) {
  boards_.clear();
  board_routes_.clear();
  concentrators_.clear();

  int board_id = first_board_id;
  for (const auto& path : config.open_paths()) {
    try {
      // Construct and fully initialise the board. Throws on failure.
      boards_.emplace_back(board_id, path, &config, configure_mode, readout_mode);
    } catch (const FersError& e) {
      FERS_LibMsg(const_cast<char*>("[WARNING][BRD %02d] Ignoring board path '%s': %s\n"),
                 board_id,
                 path.c_str(),
                 e.what());
    }
    ++board_id;
  }

  if (boards_.empty()) {
    throw FersError(std::string("No FERS boards configured from provided configuration"));
  }
}

FERSBoardManager::FERSBoardManager(const FERSConfiguration& config,
                                   int first_board_id,
                                   int readout_mode,
                                   int configure_mode) {
  BuildBoardsFromConfiguration(config, first_board_id, readout_mode, configure_mode);
}

FERSBoardManager::~FERSBoardManager() noexcept {
  std::string error;
  DisconnectAll(&error);
}

bool FERSBoardManager::AddBoard(int board_id, const std::string& connection_path, std::string* error) {
  for (const auto& board : boards_) {
    if (board.board_id() == board_id) {
      if (error != nullptr) {
        *error = "Board id already present: " + std::to_string(board_id);
      }
      return false;
    }
  }

  try {
    RegisterBoardRoute(board_id, connection_path);
  } catch (const FersError& e) {
    if (error != nullptr) {
      *error = e.what();
    }
    return false;
  }

  boards_.emplace_back(board_id, connection_path);
  return true;
}

void FERSBoardManager::ConnectAll(int readout_mode) {
  for (auto& entry : concentrators_) {
    OpenAndLogConcentrator(&entry.second); // may throw
  }

  for (auto& board : boards_) {
    if (!board.Connect(readout_mode)) {
      throw FersError("Connect failed for board id " + std::to_string(board.board_id()) + ": " + board.status().last_error);
    }
  }
}

void FERSBoardManager::RegisterBoardRoute(int board_id, const std::string& connection_path) {
  TDLBoardRoute route;
  const bool is_tdl = connection_path.find(":tdl:") != std::string::npos;

  if (is_tdl) {
    const std::string marker = ":tdl:";
    const size_t marker_pos = connection_path.find(marker);
    if (marker_pos == std::string::npos) {
      throw FersError("Path does not contain a TDL segment");
    }

    char cnc_buffer[512] = {0};
    if (FERS_Get_CncPath(const_cast<char*>(connection_path.c_str()), cnc_buffer) != 0 || cnc_buffer[0] == '\0') {
      throw FersError("Cannot infer concentrator path from '" + connection_path + "'");
    }

    const std::string remainder = connection_path.substr(marker_pos + marker.size());
    const size_t sep = remainder.find(':');
    if (sep == std::string::npos) {
      throw FersError("TDL path is missing chain/node indexes: " + connection_path);
    }

    const std::string chain_text = remainder.substr(0, sep);
    const std::string node_text = remainder.substr(sep + 1);
    try {
      size_t consumed = 0;
      route.chain = std::stoi(chain_text, &consumed, 0);
      if (consumed != chain_text.size()) {
        throw FersError("TDL path has invalid chain/node indexes: " + connection_path);
      }

      consumed = 0;
      route.node = std::stoi(node_text, &consumed, 0);
      if (consumed != node_text.size()) {
        throw FersError("TDL path has invalid chain/node indexes: " + connection_path);
      }
    } catch (const std::exception&) {
      throw FersError("TDL path has invalid chain/node indexes: " + connection_path);
    }

    if (route.chain < 0 || route.chain >= FERSLIB_MAX_NTDL || route.node < 0 || route.node >= FERSLIB_MAX_NNODES) {
      throw FersError("TDL chain/node out of range: " + connection_path);
    }

    route.cnc_path = cnc_buffer;

    ConcentratorRecord* concentrator = GetOrCreateConcentrator(route.cnc_path);
    if (concentrator == nullptr) {
      throw FersError("Failed to allocate concentrator record for " + route.cnc_path);
    }

    const auto node_key = std::make_pair(route.chain, route.node);
    if (!concentrator->occupied_nodes.insert(node_key).second) {
      throw FersError("Duplicate TDL chain/node for concentrator " + route.cnc_path + ": chain " +
                      std::to_string(route.chain) + " node " + std::to_string(route.node));
    }

    concentrator->board_ids.push_back(board_id);
  }

  route.is_tdl = is_tdl;
  board_routes_.push_back(route);
}

FERSBoardManager::ConcentratorRecord* FERSBoardManager::GetOrCreateConcentrator(const std::string& cnc_path) {
  auto it = concentrators_.find(cnc_path);
  if (it != concentrators_.end()) {
    return &it->second;
  }

  ConcentratorRecord record;
  record.cnc_path = cnc_path;
  auto inserted = concentrators_.emplace(cnc_path, std::move(record));
  return &inserted.first->second;
}

void FERSBoardManager::OpenAndLogConcentrator(ConcentratorRecord* concentrator) {
  if (concentrator == nullptr) {
    return;
  }

  if (concentrator->info_logged) {
    return;
  }

  if (!concentrator->discovered) {
    int handle = -1;
    int ret = FERS_OpenDevice(const_cast<char*>(concentrator->cnc_path.c_str()), &handle);
    if (ret != 0) {
      throw FersError("Failed to open concentrator '" + concentrator->cnc_path + "'", ret);
    }

    concentrator->handle = FersHandle(handle);

    FERS_CncInfo_t info{};
    ret = FERS_ReadConcentratorInfo(handle, &info);
    if (ret != 0) {
      throw FersError("Failed to read concentrator info for '" + concentrator->cnc_path + "'", ret);
    }

    concentrator->info = info;
    concentrator->discovered = true;
  }

  int total_boards = 0;
  for (const auto& chain : concentrator->info.ChainInfo) {
    total_boards += chain.BoardCount;
  }

  if (total_boards <= 0) {
    throw FersError("Concentrator '" + concentrator->cnc_path + "' reported no connected boards");
  }

  const int handle = concentrator->handle.get();
  FERS_LibMsg(const_cast<char*>("[INFO][CNC %02d] Connected concentrator %s (%s), board_count=%d\n"),
              FERS_INDEX(handle),
              concentrator->cnc_path.c_str(),
              BuildConcentratorSummary(concentrator->info).c_str(),
              total_boards);
  for (uint16_t chain = 0; chain < concentrator->info.NumLink && chain < 8; ++chain) {
    const auto& chain_info = concentrator->info.ChainInfo[chain];
    if (chain_info.BoardCount > 0) {
      FERS_LibMsg(const_cast<char*>("[INFO][CNC %02d] Chain %u: board_count=%u rate=%.3f cps throughput=%.3f Mbps\n"),
          FERS_INDEX(handle),
          chain,
          chain_info.BoardCount,
          chain_info.EventRate,
          chain_info.Mbps);
    }
  }

  concentrator->info_logged = true;
}

bool FERSBoardManager::ConfigureAll(const FERSConfiguration& config,
                                    int configure_mode,
                                    bool load_config_file_first,
                                    std::string* error) {
  if (load_config_file_first && !config.LoadIntoLibrary(error)) {
    return false;
  }

  for (auto& board : boards_) {
    if (!board.Configure(config, configure_mode)) {
      if (error != nullptr) {
        *error = "Configure failed for board id " + std::to_string(board.board_id()) + ": " + board.status().last_error;
      }
      return false;
    }
  }

  return true;
}

bool FERSBoardManager::SetHighVoltageAll(bool on, std::string* error) {
  for (auto& board : boards_) {
    if (!board.SetHighVoltage(on)) {
      if (error != nullptr) {
        *error = "High voltage toggle failed for board id " + std::to_string(board.board_id()) + ": " +
                 board.status().last_error;
      }
      return false;
    }
  }

  return true;
}

bool FERSBoardManager::StartAll(int start_mode, int run_number, std::string* error) {
  if (boards_.empty()) {
    if (error != nullptr) {
      *error = "No FERS boards configured.";
    }
    return false;
  }

  std::vector<int> handles;
  handles.reserve(boards_.size());
  for (const auto& board : boards_) {
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

bool FERSBoardManager::StopAll(int start_mode, int run_number, std::string* error) {
  if (boards_.empty()) {
    return true;
  }

  std::vector<int> handles;
  handles.reserve(boards_.size());
  for (const auto& board : boards_) {
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

bool FERSBoardManager::DisconnectAll(std::string* error) {
  bool ok = true;
  std::string combined_error;
  for (auto& board : boards_) {
    if (!board.Disconnect()) {
      ok = false;
      if (!combined_error.empty()) {
        combined_error += "; ";
      }
      combined_error += "Disconnect failed for board id " + std::to_string(board.board_id()) + ": " +
                        board.status().last_error;
    }
  }
  if (!ok && error != nullptr) {
    *error = combined_error;
  }
  return ok;
}

std::vector<FERSEvent> FERSBoardManager::ReadAvailableEvents(size_t max_events_per_board, std::string* error) {
  std::vector<FERSEvent> out;
  for (auto& board : boards_) {
    std::vector<FERSEvent> board_events;
    if (!board.ReadAvailableEvents(&board_events, max_events_per_board)) {
      if (error != nullptr) {
        *error = "Read failed for board id " + std::to_string(board.board_id()) + ": " + board.status().last_error;
      }
      return {};
    }

    out.insert(out.end(), board_events.begin(), board_events.end());
  }

  return out;
}

std::vector<BoardMonitorStatus> FERSBoardManager::ReadMonitorStatuses(std::string* error) const {
  std::vector<BoardMonitorStatus> out;
  out.reserve(boards_.size());

  for (const auto& board : boards_) {
    BoardMonitorStatus status;
    if (!board.ReadMonitorStatus(&status)) {
      if (error != nullptr) {
        *error = "Monitor status read failed for board id " + std::to_string(board.board_id()) + ": " +
                 status.last_error;
      }
      return {};
    }

    out.push_back(std::move(status));
  }

  return out;
}

FERSBoard* FERSBoardManager::FindBoard(int board_id) {
  for (auto& board : boards_) {
    if (board.board_id() == board_id) {
      return &board;
    }
  }
  return nullptr;
}

} // namespace fers2
} // namespace hidra
