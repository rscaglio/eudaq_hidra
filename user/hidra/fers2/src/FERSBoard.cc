#include "FERSBoard.h"

#include <cstring>
#include <vector>

#include "FERSlib.h"

namespace hidra {
namespace fers2 {
namespace {

std::string BuildError(const std::string &prefix, int code) {
  return prefix + " (ret=" + std::to_string(code) + ")";
}

} // namespace

FERSBoard::FERSBoard(int board_id, const std::string &connection_path)
    : board_id_(board_id), connection_path_(connection_path) {}

bool FERSBoard::Connect(int readout_mode) {
  int ret = FERS_OpenDevice(const_cast<char *>(connection_path_.c_str()), &handle_);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_OpenDevice failed", ret);
    status_.state = BoardState::kDisconnected;
    handle_ = -1;
    return false;
  }

  int allocated_size = 0;
  ret = FERS_InitReadout(handle_, readout_mode, &allocated_size);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_InitReadout failed", ret);
    FERS_CloseDevice(handle_);
    handle_ = -1;
    status_.state = BoardState::kDisconnected;
    return false;
  }

  status_.allocated_readout_bytes = allocated_size;
  status_.handle = handle_;
  status_.state = BoardState::kConnected;
  status_.last_error.clear();
  return true;
}

bool FERSBoard::Disconnect() {
  if (handle_ < 0) {
    status_.state = BoardState::kDisconnected;
    return true;
  }

  int ret = FERS_CloseReadout(handle_);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_CloseReadout failed", ret);
    return false;
  }

  ret = FERS_CloseDevice(handle_);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_CloseDevice failed", ret);
    return false;
  }

  handle_ = -1;
  status_.handle = -1;
  status_.state = BoardState::kDisconnected;
  status_.last_error.clear();
  return true;
}

bool FERSBoard::Configure(const FERSConfiguration &config, int configure_mode) {
  if (handle_ < 0) {
    status_.last_error = "Cannot configure: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  const auto params = config.EffectiveParamsForBoard(board_id_);
  for (const auto &entry : params) {
    int ret = FERS_SetParam(handle_, entry.first.c_str(), entry.second.c_str());
    if (ret != 0) {
      status_.last_return_code = ret;
      status_.last_error = BuildError("FERS_SetParam failed for " + entry.first, ret);
      return false;
    }
  }

  int ret = FERS_configure(handle_, configure_mode);
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
  if (handle_ < 0) {
    status_.last_error = "Cannot start acquisition: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  int one_handle[1] = {handle_};
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
  if (handle_ < 0) {
    status_.last_error = "Cannot stop acquisition: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  int one_handle[1] = {handle_};
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

bool FERSBoard::SendCommand(uint32_t command) {
  if (handle_ < 0) {
    status_.last_error = "Cannot send command: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  int ret = FERS_SendCommand(handle_, command);
  status_.last_return_code = ret;
  if (ret != 0) {
    status_.last_error = BuildError("FERS_SendCommand failed", ret);
    return false;
  }

  status_.last_error.clear();
  return true;
}

bool FERSBoard::ReadAvailableEvents(std::vector<FERSEvent> *events,
                                    size_t max_events) {
  if (events == nullptr) {
    status_.last_error = "Output event vector is null.";
    status_.last_return_code = FERSLIB_ERR_INVALID_PARAM;
    return false;
  }

  if (handle_ < 0) {
    status_.last_error = "Cannot read events: board is not connected.";
    status_.last_return_code = FERSLIB_ERR_INVALID_HANDLE;
    return false;
  }

  size_t read_count = 0;
  while (max_events == 0 || read_count < max_events) {
    int data_qualifier = 0;
    double tstamp_us = 0.0;
    void *event_ptr = nullptr;
    int nb = 0;

    int ret = FERS_GetEventFromBoard(handle_, &data_qualifier, &tstamp_us,
                                     &event_ptr, &nb);
    status_.last_return_code = ret;
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

bool FERSBoard::SerializeEvent(void *event_ptr, int data_qualifier,
                               FERSEvent *out_event) {
  if (out_event == nullptr || event_ptr == nullptr) {
    status_.last_return_code = FERSLIB_ERR_INVALID_PARAM;
    status_.last_error = "SerializeEvent received null pointer.";
    return false;
  }

  const int base_dq = (data_qualifier & 0x0F);

  if (data_qualifier == DTQ_SERVICE) {
    const auto *ev = reinterpret_cast<const ServEvent_t *>(event_ptr);
    out_event->trigger_id = ev->TotTrg_cnt;
    out_event->payload.resize(sizeof(ServEvent_t));
    std::memcpy(out_event->payload.data(), ev, sizeof(ServEvent_t));
    return true;
  }

  if (base_dq == DTQ_SPECT || data_qualifier == DTQ_TSPECT) {
    const auto *ev = reinterpret_cast<const SpectEvent_t *>(event_ptr);
    out_event->trigger_id = ev->trigger_id;
    out_event->payload.resize(sizeof(SpectEvent_t));
    std::memcpy(out_event->payload.data(), ev, sizeof(SpectEvent_t));
    return true;
  }

  if (base_dq == DTQ_TIMING) {
    const auto *ev = reinterpret_cast<const ListEvent_t *>(event_ptr);
    out_event->trigger_id = ev->trigger_id;
    out_event->payload.resize(sizeof(ListEvent_t));
    std::memcpy(out_event->payload.data(), ev, sizeof(ListEvent_t));
    return true;
  }

  if (base_dq == DTQ_COUNT) {
    const auto *ev = reinterpret_cast<const CountingEvent_t *>(event_ptr);
    out_event->trigger_id = ev->trigger_id;
    out_event->payload.resize(sizeof(CountingEvent_t));
    std::memcpy(out_event->payload.data(), ev, sizeof(CountingEvent_t));
    return true;
  }

  if (base_dq == DTQ_TEST || data_qualifier == DTQ_TEST) {
    const auto *ev = reinterpret_cast<const TestEvent_t *>(event_ptr);
    out_event->trigger_id = ev->trigger_id;
    out_event->payload.resize(sizeof(TestEvent_t));
    std::memcpy(out_event->payload.data(), ev, sizeof(TestEvent_t));
    return true;
  }

  if (base_dq == DTQ_WAVE) {
    // Waveform events include pointer members. Keep this explicit until a
    // dedicated serialization format is implemented for waveform samples.
    status_.last_return_code = FERSLIB_ERR_NOT_APPLICABLE;
    status_.last_error = "Waveform event serialization is not implemented yet.";
    return false;
  }

  status_.last_return_code = FERSLIB_ERR_NOT_APPLICABLE;
  status_.last_error = "Unsupported data qualifier: " + std::to_string(data_qualifier);
  return false;
}

} // namespace fers2
} // namespace hidra
