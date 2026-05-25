#include "FERSBoard.h"

#include <cstring>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>

#include "FERSlib.h"
#include "FERSPayloadSerialization.h"

namespace hidra {
namespace fers2 {
namespace {

std::string BuildError(const std::string& prefix, int code) {
  return prefix + " (ret=" + std::to_string(code) + ")";
}

} // namespace

FERSBoard::FERSBoard(int board_id, const std::string& connection_path)
    : board_id_(board_id),
      connection_path_(connection_path) {}

FERSBoard::FERSBoard(int board_id,
                     const std::string& connection_path,
                     const FERSConfiguration* config,
                     int configure_mode,
                     int readout_mode)
    : board_id_(board_id), connection_path_(connection_path) {
  int handle = -1;
  int ret = FERS_OpenDevice(const_cast<char*>(connection_path_.c_str()), &handle);
  if (ret != 0) {
    throw FersError("FERS_OpenDevice failed for '" + connection_path_ + "'", ret);
  }

  int allocated_size = 0;
  ret = FERS_InitReadout(handle, readout_mode, &allocated_size);
  if (ret != 0) {
    FERS_CloseDevice(handle);
    throw FersError("FERS_InitReadout failed for '" + connection_path_ + "'", ret);
  }

  // adopt handle
  handle_ = FersHandle(handle);
  status_.allocated_readout_bytes = allocated_size;
  status_.handle = handle;
  status_.state = BoardState::kConnected;

  if (config != nullptr) {
    const auto params = config->EffectiveParamsForBoard(board_id_);
    for (const auto& entry : params) {
      int r = FERS_SetParam(handle_.get(), entry.first.c_str(), entry.second.c_str());
      if (r != 0) {
        throw FersError("FERS_SetParam failed for " + entry.first, r);
      }
    }
    ret = FERS_configure(handle_.get(), configure_mode);
    if (ret != 0) {
      throw FersError("FERS_configure failed for '" + connection_path_ + "'", ret);
    }
    status_.state = BoardState::kConfigured;
  }
}

FERSBoard::~FERSBoard() noexcept {
  if (!handle_) return;

  const int handle = handle_.get();
  const int max_attempts = 3;
  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    int ret = FERS_CloseReadout(handle);
    if (ret == 0) {
      break;
    }
    FERS_LibMsg(const_cast<char*>("[WARNING][BRD %02d] FERS_CloseReadout attempt %d failed (ret=%d)\n"),
               FERS_INDEX(handle), attempt, ret);
    if (attempt < max_attempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // Ensure the device is closed as well (FersHandle::reset performs retries
  // for CloseDevice). Always clear the handle to avoid reuse of a bad handle.
  handle_.reset();
}

bool FERSBoard::Connect(int readout_mode) {
  int handle = -1;
  int ret = FERS_OpenDevice(const_cast<char*>(connection_path_.c_str()), &handle);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_OpenDevice failed", ret);
    status_.state = BoardState::kDisconnected;
    return false;
  }

  int allocated_size = 0;
  ret = FERS_InitReadout(handle, readout_mode, &allocated_size);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_InitReadout failed", ret);
    status_.state = BoardState::kDisconnected;
    FERS_CloseDevice(handle);
    return false;
  }

  handle_ = FersHandle(handle);

  status_.allocated_readout_bytes = allocated_size;
  status_.handle = handle;
  status_.state = BoardState::kConnected;
  status_.last_error.clear();
  return true;
}

bool FERSBoard::Disconnect() {
  if (!handle_) {
    status_.state = BoardState::kDisconnected;
    return true;
  }

  const int handle = handle_.get();

  const int max_attempts = 3;
  int ret = 0;
  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    ret = FERS_CloseReadout(handle);
    status_.last_return_code = ret;
    if (ret == 0) {
      break;
    }
    status_.last_error = BuildError("FERS_CloseReadout failed", ret);
    FERS_LibMsg(const_cast<char*>("[WARNING][BRD %02d] FERS_CloseReadout attempt %d failed (ret=%d)\n"),
               FERS_INDEX(handle), attempt, ret);
    if (attempt < max_attempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // Attempt to close the device regardless; FersHandle::reset() will retry
  // CloseDevice internally. If CloseReadout ultimately failed, report failure
  // but still make a best-effort cleanup.
  handle_.reset();

  if (ret != 0) {
    status_.state = BoardState::kDisconnected;
    // status_.last_error already set above
    return false;
  }

  status_.handle = -1;
  status_.state = BoardState::kDisconnected;
  status_.last_error.clear();
  return true;
}

bool FERSBoard::Configure(const FERSConfiguration& config, int configure_mode) {
  if (!handle_) {
    status_.last_error = "Cannot configure: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  const int handle = handle_.get();
  const auto params = config.EffectiveParamsForBoard(board_id_);
  for (const auto& entry : params) {
    int ret = FERS_SetParam(handle, entry.first.c_str(), entry.second.c_str());
    if (ret != 0) {
      status_.last_return_code = ret;
      status_.last_error = BuildError("FERS_SetParam failed for " + entry.first, ret);
      return false;
    }
  }

  int ret = FERS_configure(handle, configure_mode);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_configure failed", ret);
    return false;
  }

  status_.state = BoardState::kConfigured;
  status_.last_error.clear();
  return true;
}

bool FERSBoard::StartAcquisition(int start_mode, int run_number) {
  if (!handle_) {
    status_.last_error = "Cannot start acquisition: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  const int handle = handle_.get();
  int one_handle[1] = {handle};
  int ret = FERS_StartAcquisition(one_handle, 1, start_mode, run_number);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_StartAcquisition failed", ret);
    return false;
  }

  status_.state = BoardState::kRunning;
  status_.last_error.clear();
  return true;
}

bool FERSBoard::StopAcquisition(int start_mode, int run_number) {
  if (!handle_) {
    status_.last_error = "Cannot stop acquisition: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  const int handle = handle_.get();
  int one_handle[1] = {handle};
  int ret = FERS_StopAcquisition(one_handle, 1, start_mode, run_number);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_StopAcquisition failed", ret);
    return false;
  }

  status_.state = BoardState::kConfigured;
  status_.last_error.clear();
  return true;
}

bool FERSBoard::SetHighVoltage(bool on) {
  if (!handle_) {
    status_.last_error = "Cannot change high voltage state: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  const int ret = FERS_HV_Set_OnOff(handle_.get(), on ? 1 : 0);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError(std::string("FERS_HV_Set_OnOff failed for ") + (on ? "on" : "off"), ret);
    return false;
  }

  status_.last_error.clear();
  return true;
}

bool FERSBoard::SendCommand(uint32_t command) {
  if (!handle_) {
    status_.last_error = "Cannot send command: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  int ret = FERS_SendCommand(handle_.get(), command);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_SendCommand failed", ret);
    return false;
  }

  status_.last_error.clear();
  return true;
}

bool FERSBoard::ReadAvailableEvents(std::vector<FERSEvent>* events, size_t max_events) {
  if (events == nullptr) {
    status_.last_error = "Output event vector is null.";
    status_.last_return_code = FERSLIB_ERR_INVALID_PARAM;
    return false;
  }

  if (!handle_) {
    status_.last_error = "Cannot read events: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  size_t read_count = 0;
  while (max_events == 0 || read_count < max_events) {
    int data_qualifier = 0;
    double tstamp_us = 0.0;
    void* event_ptr = nullptr;
    int nb = 0;

    int ret = FERS_GetEventFromBoard(handle_.get(), &data_qualifier, &tstamp_us, &event_ptr, &nb);
    status_.last_return_code = ret;
    if (ret == 2) {
      // Status code 2 means "no data available right now". Treat it as a
      // normal polling result so the producer can sleep and poll again later.
      status_.last_error.clear();
      break;
    }

    if (ret != 0) {
      status_.last_error = BuildError("FERS_GetEventFromBoard failed", ret);
      return false;
    }

    if (nb <= 0 || event_ptr == nullptr) {
      break;
    }

    FERSEvent event;
    event.board_id = board_id_;
    event.board_index = board_id_;
    event.data_qualifier = data_qualifier;
    event.timestamp_us = tstamp_us;

    if (!SerializeEvent(event_ptr, data_qualifier, &event)) {
      return false;
    }

    events->push_back(std::move(event));
    ++read_count;
  }

  status_.last_error.clear();
  return true;
}

bool FERSBoard::ReadMonitorStatus(BoardMonitorStatus* monitor_status) const {
  if (monitor_status == nullptr) {
    return false;
  }

  *monitor_status = BoardMonitorStatus{};
  monitor_status->board_id = board_id_;

  if (!handle_) {
    monitor_status->last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    monitor_status->last_error = "Cannot read monitor status: board is not connected.";
    return false;
  }

  bool ok = true;
  std::ostringstream errors;

  auto append_error = [&](const char* name, int ret) {
    if (ret == 0 || ret == FERSLIB_ERR_NOT_APPLICABLE) {
      return;
    }
    ok = false;
    monitor_status->last_return_code = ret;
    if (errors.tellp() > 0) {
      errors << "; ";
    }
    errors << name << " failed (ret=" << ret << ")";
  };

  float value = 0.0f;
  int ret = FERS_Get_FPGA_Temp(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->temp_fpga = value;
    monitor_status->fpga_temp_valid = true;
  }
  append_error("FERS_Get_FPGA_Temp", ret);

  ret = FERS_Get_Board_Temp(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->temp_board = value;
    monitor_status->board_temp_valid = true;
  }
  append_error("FERS_Get_Board_Temp", ret);

  ret = FERS_Get_TDC0_Temp(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->temp_tdc0 = value;
    monitor_status->tdc0_temp_valid = true;
  }
  append_error("FERS_Get_TDC0_Temp", ret);

  ret = FERS_Get_TDC1_Temp(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->temp_tdc1 = value;
    monitor_status->tdc1_temp_valid = true;
  }
  append_error("FERS_Get_TDC1_Temp", ret);

  ret = FERS_HV_Get_Vmon(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->hv_vmon = value;
    monitor_status->hv_vmon_valid = true;
  }
  append_error("FERS_HV_Get_Vmon", ret);

  ret = FERS_HV_Get_Imon(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->hv_imon = value;
    monitor_status->hv_imon_valid = true;
  }
  append_error("FERS_HV_Get_Imon", ret);

  ret = FERS_HV_Get_DetectorTemp(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->temp_detector = value;
    monitor_status->detector_temp_valid = true;
  }
  append_error("FERS_HV_Get_DetectorTemp", ret);

  ret = FERS_HV_Get_IntTemp(handle_.get(), &value);
  if (ret == 0) {
    monitor_status->temp_hv = value;
    monitor_status->hv_temp_valid = true;
  }
  append_error("FERS_HV_Get_IntTemp", ret);

  int on = 0;
  int ramping = 0;
  int over_current = 0;
  int over_voltage = 0;
  ret = FERS_HV_Get_Status(handle_.get(), &on, &ramping, &over_current, &over_voltage);
  if (ret == 0) {
    monitor_status->hv_on = on;
    monitor_status->hv_ramping = ramping;
    monitor_status->hv_over_current = over_current;
    monitor_status->hv_over_voltage = over_voltage;
    monitor_status->hv_status_valid = true;
  }
  append_error("FERS_HV_Get_Status", ret);

  monitor_status->last_error = errors.str();
  return ok;
}

bool FERSBoard::SerializeEvent(void* event_ptr, int data_qualifier, FERSEvent* out_event) {
  std::string error;
  if (!SerializeFersEventPayload(event_ptr, data_qualifier, board_id_, out_event, &error)) {
    status_.last_return_code = FERSLIB_ERR_NOT_APPLICABLE;
    status_.last_error = std::move(error);
    return false;
  }

  return true;
}

} // namespace fers2
} // namespace hidra
