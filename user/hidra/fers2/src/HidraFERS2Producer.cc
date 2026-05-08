#include "eudaq/Configuration.hh"
#include "eudaq/Event.hh"
#include "eudaq/Factory.hh"
#include "eudaq/Logger.hh"
#include "eudaq/Producer.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "HidraUtils.hh"
#include "FERSBoardManager.h"
#include "FERSlib.h"
#undef max
#undef min

namespace {

using hidra::fers2::FERSBoard;
using hidra::fers2::FERSBoardManager;
using hidra::fers2::FERSConfiguration;
using hidra::fers2::FERSEvent;

std::string FormatSummary(const FERSEvent& event) {
  std::ostringstream oss;
  oss << "board=" << event.board_id << " dq=" << event.data_qualifier << " trig=" << event.trigger_id;
  if (!event.payload.empty()) {
    oss << " bytes=" << event.payload.size();
  }

  const int base_dq = event.data_qualifier & 0x0F;
  if (base_dq == DTQ_SPECT || event.data_qualifier == DTQ_TSPECT) {
    if (event.payload.size() >= sizeof(SpectEvent_t)) {
      const auto* spect = reinterpret_cast<const SpectEvent_t*>(event.payload.data());
      oss << " chmask=0x" << std::hex << spect->chmask << std::dec;
      oss << " nhits=";
      int nhits = 0;
      for (size_t channel = 0; channel < 64; ++channel) {
        if (spect->energyHG[channel] != 0 || spect->energyLG[channel] != 0) {
          ++nhits;
        }
      }
      oss << nhits;
      oss << " HG[0..3]=[" << spect->energyHG[0] << ',' << spect->energyHG[1] << ',' << spect->energyHG[2] << ','
          << spect->energyHG[3] << "]";
      oss << " LG[0..3]=[" << spect->energyLG[0] << ',' << spect->energyLG[1] << ',' << spect->energyLG[2] << ','
          << spect->energyLG[3] << "]";
      if (event.data_qualifier == DTQ_TSPECT) {
        oss << " ToT[0..3]=[" << spect->ToT[0] << ',' << spect->ToT[1] << ',' << spect->ToT[2] << ','
            << spect->ToT[3] << "]";
      }
    }
  } else if (base_dq == DTQ_TIMING) {
    if (event.payload.size() >= sizeof(ListEvent_t)) {
      const auto* timing = reinterpret_cast<const ListEvent_t*>(event.payload.data());
      oss << " nhits=" << timing->nhits;
      const uint16_t preview = timing->nhits < 4 ? timing->nhits : 4;
      oss << " ch=[";
      for (uint16_t index = 0; index < preview; ++index) {
        if (index != 0) {
          oss << ',';
        }
        oss << static_cast<int>(timing->channel[index]);
      }
      oss << "]";
      oss << " ToA=[";
      for (uint16_t index = 0; index < preview; ++index) {
        if (index != 0) {
          oss << ',';
        }
        oss << timing->ToA[index];
      }
      oss << "]";
      oss << " ToT=[";
      for (uint16_t index = 0; index < preview; ++index) {
        if (index != 0) {
          oss << ',';
        }
        oss << timing->ToT[index];
      }
      oss << "]";
    }
  } else if (base_dq == DTQ_COUNT) {
    if (event.payload.size() >= sizeof(CountingEvent_t)) {
      const auto* counting = reinterpret_cast<const CountingEvent_t*>(event.payload.data());
      oss << " chmask=0x" << std::hex << counting->chmask << std::dec;
      oss << " counts[0..3]=[" << counting->counts[0] << ',' << counting->counts[1] << ',' << counting->counts[2]
          << ',' << counting->counts[3] << "]";
      oss << " T_OR=" << counting->t_or_counts << " Q_OR=" << counting->q_or_counts;
    }
  } else if (base_dq == DTQ_SERVICE) {
    if (event.payload.size() >= sizeof(ServEvent_t)) {
      const auto* service = reinterpret_cast<const ServEvent_t*>(event.payload.data());
      oss << " service trig=" << service->TotTrg_cnt << " rej=" << service->RejTrg_cnt
          << " suppr=" << service->SupprTrg_cnt << " hv=" << service->hv_Vmon << "V"
          << " temp=" << service->tempBoard << "C";
    }
  }

  return oss.str();
}

int ParseIntegerOrKeyword(const std::string& value, const std::map<std::string, int>& keywords, int fallback) {
  try {
    size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed, 0);
    if (consumed == value.size()) {
      return parsed;
    }
  } catch (...) {
  }

  const auto it = keywords.find(value);
  if (it != keywords.end()) {
    return it->second;
  }

  return fallback;
}

std::string JoinBoardIds(const std::vector<int>& board_ids) {
  std::ostringstream oss;
  for (size_t index = 0; index < board_ids.size(); ++index) {
    if (index != 0) {
      oss << ",";
    }
    oss << board_ids[index];
  }
  return oss.str();
}

} // namespace

class HidraFERS2Producer : public eudaq::Producer {
public:
  HidraFERS2Producer(const std::string& name, const std::string& runcontrol)
      : eudaq::Producer(name, runcontrol) {}

  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraFERS2Producer");

private:
  void DoInitialise() override {
    auto ini = GetInitConfiguration();
    (void)ini;
  }

  void DoConfigure() override {
    auto conf = GetConfiguration();
    m_evt_f = 0;
    if (!conf) {
      EUDAQ_THROW("Run configuration is missing");
    }

    m_config_file = conf->Get("FERS_CONF_FILE", std::string(""));
    if (m_config_file.empty()) {
      EUDAQ_THROW("FERS_CONF_FILE is missing from the run configuration");
    }

    m_readout_mode = conf->Get("FERS_READOUT_MODE", 0);
    m_configure_mode = ParseIntegerOrKeyword(conf->Get("FERS_CONFIGURE_MODE", std::string("CFG_HARD")),
                                             {{"CFG_HARD", CFG_HARD}, {"CFG_SOFT", CFG_SOFT}},
                                             CFG_HARD);
    m_start_mode = ParseIntegerOrKeyword(conf->Get("FERS_START_MODE", std::string("STARTRUN_ASYNC")),
                                         {{"ASYNC", STARTRUN_ASYNC},
                                          {"CHAIN_T0", STARTRUN_CHAIN_T0},
                                          {"CHAIN_T1", STARTRUN_CHAIN_T1},
                                          {"TDL", STARTRUN_TDL},
                                          {"STARTRUN_ASYNC", STARTRUN_ASYNC},
                                          {"STARTRUN_CHAIN_T0", STARTRUN_CHAIN_T0},
                                          {"STARTRUN_CHAIN_T1", STARTRUN_CHAIN_T1},
                                          {"STARTRUN_TDL", STARTRUN_TDL}},
                                         STARTRUN_ASYNC);

    m_poll_sleep_us = conf->Get("FERS_POLL_SLEEP_US", 1000);
    m_max_events_per_board = conf->Get("FERS_MAX_EVENTS_PER_BOARD", 0); // --- When not specified in the config file ---> run forever ---
    m_send_trigger_number = conf->Get("FERS_SEND_TRIGGER_NUMBER", 1) != 0;
    m_send_timestamp = conf->Get("FERS_SEND_TIMESTAMP", 1) != 0;

    std::string error;
    if (!FERSConfiguration::FromFile(m_config_file, &m_config, &error)) {
      EUDAQ_THROW("Cannot parse FERS configuration file: " + error);
    }

    m_board_manager = FERSBoardManager{};
    if (!m_board_manager.BuildBoardsFromConfiguration(m_config, 0)) {
      EUDAQ_THROW("No FERS boards found in configuration file: " + m_config_file);
    }

    if (!m_board_manager.ConnectAll(m_readout_mode, &error)) {
      m_board_manager.DisconnectAll(nullptr);
      EUDAQ_THROW(error);
    }

    if (!m_board_manager.ConfigureAll(m_config, m_configure_mode, true, &error)) {
      m_board_manager.DisconnectAll(nullptr);
      EUDAQ_THROW(error);
    }

    m_board_ids.clear();
    m_board_ids.reserve(m_board_manager.boards().size());
    for (const auto& board : m_board_manager.boards()) {
      m_board_ids.push_back(board.board_id());
      m_event_queues[board.board_id()] = {};
    }

    EUDAQ_INFO("Configured FERS2 boards: " + JoinBoardIds(m_board_ids));
  }

  void DoStartRun() override {
    StopAcquisitionThread();
    m_exit_of_run = false;
    m_evt_f = 0;
    m_run_number = GetRunNumber();
    m_event_queues.clear();
    for (const auto& board : m_board_manager.boards()) {
      m_event_queues[board.board_id()] = {};
    }

    auto bore = eudaq::Event::MakeUnique("FERSProducer");
    bore->SetBORE();
    bore->SetRunN(static_cast<uint32_t>(m_run_number));
    bore->SetTag("Producer", "HidraFERS2Producer");
    bore->SetTag("FERS_CONF_FILE", m_config_file);
    SendEvent(std::move(bore));

    std::string error;
    if (!m_board_manager.StartAll(m_start_mode, m_run_number, &error)) {
      EUDAQ_THROW(error);
    }

    EUDAQ_INFO("Starting FERS2 run " + std::to_string(m_run_number));
  }

  void DoStopRun() override {
    StopAcquisitionThread();
    m_exit_of_run = true;
    std::string error;
    if (!m_board_manager.StopAll(m_start_mode, m_run_number, &error)) {
      EUDAQ_WARN(error);
    }

    auto eore = eudaq::Event::MakeUnique("FERSProducer");
    eore->SetEORE();
    eore->SetRunN(static_cast<uint32_t>(m_run_number));
    SendEvent(std::move(eore));

    EUDAQ_INFO("Stopping FERS2 run " + std::to_string(m_run_number));
  }

  void DoReset() override {
    StopAcquisitionThread();
    m_exit_of_run = true;
    m_evt_f = 0;
    std::string error;
    if (!m_board_manager.StopAll(m_start_mode, m_run_number, &error)) {
      EUDAQ_WARN(error);
    }
    if (!m_board_manager.DisconnectAll(&error)) {
        EUDAQ_WARN(error);
    }
    m_board_ids.clear();
    m_event_queues.clear();
  }

  void DoTerminate() override {
    m_exit_of_run = true;
    StopAcquisitionThread();
    std::string error;
    if (!m_board_manager.StopAll(m_start_mode, m_run_number, &error)) {
      EUDAQ_WARN(error);
    }
    if (!m_board_manager.DisconnectAll(&error)) {
      EUDAQ_WARN(error);
    }
  }

  void RunLoop() override {
    while (!m_exit_of_run) {
      std::string error;
      const auto events = m_board_manager.ReadAvailableEvents(m_max_events_per_board, &error);
      if (!error.empty()) {
        EUDAQ_THROW(error);
      }

      for (const auto& event : events) {
        HIDRA_DEBUG("Read FERS event {}", FormatSummary(event));
        m_event_queues[event.board_id].push_back(event);
      }

      FlushAlignedEvents();

      if (m_poll_sleep_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(m_poll_sleep_us));
      }
    }

    FlushAlignedEvents();
  }

  void FlushAlignedEvents() {
    while (true) {
      if (m_board_ids.empty()) {
        return;
      }

      bool all_have_data = true;
      for (int board_id : m_board_ids) {
        if (m_event_queues[board_id].empty()) {
          all_have_data = false;
          break;
        }
      }

      if (!all_have_data) {
        return;
      }

      uint64_t trigger_n = std::numeric_limits<uint64_t>::max();
      for (int board_id : m_board_ids) {
        trigger_n = std::min(trigger_n, m_event_queues[board_id].front().trigger_id);
      }

      std::vector<int> matched_boards;
      matched_boards.reserve(m_board_ids.size());
      for (int board_id : m_board_ids) {
        auto& queue = m_event_queues[board_id];
        if (!queue.empty() && queue.front().trigger_id == trigger_n) {
          matched_boards.push_back(board_id);
        }
      }

      if (matched_boards.empty()) {
        return;
      }

      if (matched_boards.size() != m_board_ids.size()) {
        std::ostringstream missing;
        bool first = true;
        for (int board_id : m_board_ids) {
          const bool matched =
              std::find(matched_boards.begin(), matched_boards.end(), board_id) != matched_boards.end();
          if (!matched) {
            if (!first) {
              missing << ", ";
            }
            first = false;
            missing << board_id;
          }
        }

        EUDAQ_WARN("FERS2 alignment gap at trigger " + std::to_string(trigger_n) +
                   ", missing boards: " + missing.str());
      }

      auto ev = eudaq::Event::MakeUnique("FERSProducer");
      ev->SetTag("Producer", "HidraFERS2Producer");
      if (m_send_trigger_number) {
        ev->SetTriggerN(trigger_n);
      }
      ev->SetEventN(trigger_n);

      std::size_t total_payload_bytes = 0;

      bool timestamp_set = false;
      for (int board_id : m_board_ids) {
        auto& queue = m_event_queues[board_id];
        if (queue.empty() || queue.front().trigger_id != trigger_n) {
          continue;
        }

        const auto& event = queue.front();
        if (m_send_timestamp && !timestamp_set) {
          const uint64_t start_ts = event.timestamp_us > 0.0 ? static_cast<uint64_t>(event.timestamp_us) : 0u;
          ev->SetTimestamp(start_ts, start_ts + 1u);
          timestamp_set = true;
        }

        ev->AddBlock(static_cast<uint32_t>(board_id), event.payload);
        total_payload_bytes += event.payload.size();
        queue.pop_front();
      }

      ev->SetTag("eventWords", std::to_string(total_payload_bytes));

      SendEvent(std::move(ev));
      ++m_evt_f;
     
    }
  }

  void StopAcquisitionThread() {
    m_exit_of_run = true;

    if (m_thd_run.joinable()) {
      m_thd_run.join();
    }
  }

  FERSConfiguration m_config;
  FERSBoardManager m_board_manager;
  std::map<int, std::deque<FERSEvent>> m_event_queues;
  std::vector<int> m_board_ids;
  std::string m_config_file;
  int m_run_number = 0;
  int m_readout_mode = 0;
  int m_configure_mode = CFG_HARD;
  int m_start_mode = STARTRUN_ASYNC;
  uint64_t m_evt_f;
  uint64_t m_max_events_per_board;
  int m_poll_sleep_us = 1000;
  bool m_send_trigger_number = true;
  bool m_send_timestamp = true;
  bool m_exit_of_run = false;
  std::thread m_thd_run;
};

namespace {
auto dummy0 = eudaq::Factory<eudaq::Producer>::Register<HidraFERS2Producer, const std::string&, const std::string&>(
    HidraFERS2Producer::m_id_factory);
}
