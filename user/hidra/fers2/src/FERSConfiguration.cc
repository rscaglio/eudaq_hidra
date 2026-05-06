#include "FERSConfiguration.h"

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include "FERSlib.h"

namespace hidra {
namespace fers2 {
namespace {

std::string Trim(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }

  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }

  return input.substr(start, end - start);
}

bool ParseLine(const std::string& line, std::string* key, std::string* value) {
  std::string no_comment = line;
  const size_t comment_pos = no_comment.find('#');
  if (comment_pos != std::string::npos) {
    no_comment = no_comment.substr(0, comment_pos);
  }

  no_comment = Trim(no_comment);
  if (no_comment.empty()) {
    return false;
  }

  std::istringstream iss(no_comment);
  if (!(iss >> *key)) {
    return false;
  }

  std::string remainder;
  std::getline(iss, remainder);
  *value = Trim(remainder);
  return !value->empty();
}

bool ParseIndexedKey(const std::string& key,
                     std::string* base_name,
                     int* board_index,
                     std::string* channel_suffix) {
  if (base_name == nullptr || board_index == nullptr || channel_suffix == nullptr) {
    return false;
  }

  const size_t open = key.find('[');
  if (open == std::string::npos) {
    *base_name = key;
    *board_index = -1;
    channel_suffix->clear();
    return true;
  }

  const size_t close = key.find(']', open);
  if (close == std::string::npos) {
    return false;
  }

  *base_name = key.substr(0, open);
  try {
    *board_index = std::stoi(key.substr(open + 1, close - open - 1));
  } catch (...) {
    return false;
  }

  if (close + 1 < key.size()) {
    *channel_suffix = key.substr(close + 1);
  } else {
    channel_suffix->clear();
  }

  return true;
}

bool IsJanusOnlyParameter(const std::string& base_name) {
  static const std::set<std::string> kJanusOnly = {
      "EnableJobs",
      "JobFirstRun",
      "JobLastRun",
      "RunSleep",
      "RunNumber_AutoIncr",
      "DataAnalysis",
      "DataFilePath",
      "OF_OutFileUnit",
      "OF_EnMaxSize",
      "OF_MaxSize",
      "OF_RawData",
      "OF_ListLL",
      "OF_ListBin",
      "OF_ListAscii",
      "OF_ListCSV",
      "OF_Sync",
      "OF_ServiceInfo",
      "OF_RunInfo",
      "OF_SpectHisto",
      "OF_ToAHisto",
      "OF_ToTHisto",
      "OF_MCS",
      "OF_Staircase",
      "Load",
      "StartRunMode",
      "StopRunMode",
      "EventBuildingMode",
      "TstampCoincWindow",
      "PresetTime",
      "PresetCounts",
      "EnableRawDataRead",
      "EnLiveParamChange",
      "AskHVShutDownOnExit",
      "OutFileEnableMask",
      "OutFileUnit",
      "MaxOutFileSize",
  };

  return kJanusOnly.find(base_name) != kJanusOnly.end();
}

} // namespace

bool FERSConfiguration::FromFile(const std::string& path, FERSConfiguration* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "Output configuration pointer is null.";
    }
    return false;
  }

  std::ifstream in(path.c_str());
  if (!in.is_open()) {
    if (error != nullptr) {
      *error = "Cannot open configuration file: " + path;
    }
    return false;
  }

  FERSConfiguration cfg;
  cfg.source_file_ = path;

  std::string line;
  while (std::getline(in, line)) {
    std::string key;
    std::string value;
    if (!ParseLine(line, &key, &value)) {
      continue;
    }

    if (key.rfind("Open", 0) == 0) {
      cfg.open_paths_.push_back(value);
      continue;
    }

    std::string base_name;
    int board_index = -1;
    std::string channel_suffix;
    if (!ParseIndexedKey(key, &base_name, &board_index, &channel_suffix)) {
      if (error != nullptr) {
        *error = "Malformed configuration key: " + key;
      }
      return false;
    }

    if (IsJanusOnlyParameter(base_name)) {
      continue;
    }

    std::string normalized_key = base_name + channel_suffix;
    if (board_index >= 0) {
      cfg.SetBoardOverride(board_index, normalized_key, value);
    } else {
      cfg.default_params_[normalized_key] = value;
    }
  }

  *out = cfg;
  return true;
}

bool FERSConfiguration::LoadIntoLibrary(std::string* error) const {
  if (source_file_.empty()) {
    if (error != nullptr) {
      *error = "Configuration source file is empty.";
    }
    return false;
  }

  int ret = FERS_LoadConfigFile(const_cast<char*>(source_file_.c_str()));
  if (ret != 0) {
    if (error != nullptr) {
      *error = "FERS_LoadConfigFile failed with code " + std::to_string(ret) + " for file " + source_file_;
    }
    return false;
  }

  return true;
}

void FERSConfiguration::SetBoardOverride(int board_id, const std::string& param_name, const std::string& value) {
  board_overrides_[board_id][param_name] = value;
}

std::map<std::string, std::string> FERSConfiguration::EffectiveParamsForBoard(int board_id) const {
  std::map<std::string, std::string> effective = default_params_;
  auto it = board_overrides_.find(board_id);
  if (it != board_overrides_.end()) {
    for (const auto& entry : it->second) {
      effective[entry.first] = entry.second;
    }
  }
  return effective;
}

} // namespace fers2
} // namespace hidra
