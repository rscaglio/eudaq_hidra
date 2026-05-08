#include "EventSerializer.hh"
#include "HidraUtils.hh"
#include <eudaq/DataCollector.hh>
#include <eudaq/Event.hh>
#include <eudaq/Exception.hh>
#include <eudaq/Factory.hh>
#include <eudaq/FileNamer.hh>
#include <eudaq/Logger.hh>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <string>

class HidraDataCollector : public eudaq::DataCollector {
public:
  HidraDataCollector(const std::string& name, const std::string& runcontrol)
      : eudaq::DataCollector(name, runcontrol) {}

  ~HidraDataCollector() override = default;

  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraDataCollector");

private:
  const int MAX_SOURCES = 8;

  // --- Per-source event container ---
  struct SourceEvent {
    std::string ConnectionName;
    eudaq::EventSP event;
    uint64_t timestamp;
  };

  // --- Buffer for a given trigger ---
  struct PendingTrigger {
    uint64_t trigger_number;
    std::map<int, SourceEvent> events_by_source; // access through detID: events_by_source[x]
    uint64_t first_seen_ns;
  };

  bool m_is_replay_mode;
  // when in replay mode, it uses timestamp of greatest trigger instead of
  // runtime timestamp to evaluate whether incomplete events shall be built
  uint64_t m_event_count = 0;
  uint64_t m_max_events;
  int m_n_complete_events = 0;
  int m_n_incomplete_events = 0;
  bool m_stop_sent = false;
  bool m_running = false;

  std::vector<bool> m_is_source_enabled = std::vector<bool>(MAX_SOURCES, false);
  std::map<std::string, int> m_expected_sources_map;
  std::map<uint64_t, PendingTrigger> m_pending_events;
  uint64_t m_sync_timeout_us = 1000000;
  uint64_t m_tstamp_window_ns = 50000;

  bool m_single_producer_mode = false; // when no sources are specified, only the first one is accepted and DetectorID 0 is assigned.

  std::string m_filePattern;
  std::string m_fwType;
  std::ofstream m_hidraOutput;

  // user custom

  bool IsExpectedSource(std::string& source) {
    auto it = m_expected_sources_map.find(source);
    if (it == m_expected_sources_map.end()) {
      return false;
    } else {
      return m_is_source_enabled[m_expected_sources_map.at(source)];
    }
  }
  bool IsExpectedSource(int& detID) { return m_is_source_enabled[detID]; }

  bool IsComplete(PendingTrigger& pending) {
    for (const auto& s : m_expected_sources_map) {
      if (pending.events_by_source.count(s.second) == 0) {
        return false;
      }
    }
    return true;
  }

  uint64_t TimestampSpread(const PendingTrigger& pending) const {
    if (pending.events_by_source.size() <= 1) {
      return 0;
    }

    uint64_t t_min = UINT64_MAX;
    uint64_t t_max = 0;

    for (const auto& item : pending.events_by_source) {
      const auto timestamp = item.second.timestamp;
      t_min = std::min(t_min, timestamp);
      t_max = std::max(t_max, timestamp);
    }
    return (t_max - t_min);
  }

  eudaq::EventSP BuildFullEvent(PendingTrigger& pending) {

    // just a skeleton. Full event must be built here
    auto it = pending.events_by_source.begin();

    if (it == pending.events_by_source.end()) {
      EUDAQ_ERROR("Cannot build event out of NO PENDING triggers");
      return nullptr;
    }

    // event with no blocks. Header (tag) + subevents
    auto fullEvt = eudaq::Event::MakeUnique("MergedEvent");
    fullEvt->SetRunN(GetRunNumber());
    fullEvt->SetTriggerN(pending.trigger_number);
    fullEvt->SetTimestamp(pending.first_seen_ns, pending.first_seen_ns + 100UL);
    fullEvt->SetTag("N_SOURCES", std::to_string(pending.events_by_source.size()));

    for (const auto& is : m_expected_sources_map) {
      // will be overwritten if source is in the event
      fullEvt->SetTag(is.first + "_id",
                      is.second); // "XDCProducer_id = <detID>"
      fullEvt->SetTag(std::to_string(is.second) + "_size",
                      0); // "<detID>_size = 0"
    }

    for (; it != pending.events_by_source.end(); ++it) {
      fullEvt->SetTag(std::to_string(it->first) + "_size",
                      it->second.event->GetTag("eventWords")); // "<detID>_size = size"
      it->second.event->SetTag("Producer", it->second.ConnectionName);
      it->second.event->SetTag("detID", it->first);
      fullEvt->AddSubEvent(std::move(it->second.event));
    }

    HIDRA_DEBUG("MERGED EVENT BUILT: {}", hidra::utils::GetEventInfo(fullEvt.get(), 2));

    return fullEvt;
  }

  void FlushOldIncompleteEvents() {

    if (m_single_producer_mode) {
      return; 
    }

    for (auto it = m_pending_events.begin(); it != m_pending_events.end();) {

      uint64_t age_ns;

      if (!m_is_replay_mode) {
        age_ns = hidra::utils::getTimens() - it->second.first_seen_ns;
      } else {
        auto lastit = m_pending_events.rbegin();
        age_ns = lastit->second.first_seen_ns - it->second.first_seen_ns;
      }

      if (age_ns <= m_sync_timeout_us * 1000) {
        ++it;
        continue;
      }

      uint64_t trigger = it->first;

      HIDRA_WARN(
          "Timeout waiting for complete event for trigger {}: {} > {} ns", trigger, age_ns, m_sync_timeout_us * 1000);

      // if one wants to discard:
      // it = m_pending_events.erase(it);

      auto mergedEvt = BuildFullEvent(it->second);

      if (!mergedEvt) {
        HIDRA_WARN("Failed to build incomplete event for trigger {}", trigger);
        it = m_pending_events.erase(it);
        return;
      }

      mergedEvt->SetTag("SYNC_STATUS", "INCOMPLETE");
      m_n_incomplete_events++;
      //WriteEvent(std::move(mergedEvt));
      hidra::EventSerializer::WriteToStream(*mergedEvt, m_hidraOutput);
      ++m_event_count;
      UpdateStatusTags();

      it = m_pending_events.erase(it);
    }
  }

  void CheckMaxEvents() { 

    if (!m_stop_sent && m_max_events != 0 && m_event_count >= m_max_events) {
      m_stop_sent = true;
      HIDRA_INFO("Max events reached. Sending STOP request");
      SetStatus(eudaq::Status::STATE_RUNNING, "STOP_REQUEST");
      SendStatus();
    }
  }

  void UpdateStatusTags() {
    SetStatusTag("EventN", std::to_string(m_event_count));
    SetStatusTag("Pending", std::to_string(m_pending_events.size()));
    SetStatusTag("Completes", std::to_string(m_n_complete_events));
    SetStatusTag("Incompletes", std::to_string(m_n_incomplete_events));
    SendStatus();
  }

  //////////////////////////////////////////////

  void DoInitialise() override {
    auto ini = GetInitConfiguration();
    if (!ini) {
      HIDRA_WARN("HidraDataCollector: missing init configuration");
    }

    m_is_replay_mode = (bool)ini->Get("REPLAY_MODE", 0);

    if (m_is_replay_mode) {
      HIDRA_WARN("DataCollector is in REPLAY MODE");
    }

    m_event_count = 0;
    m_pending_events.clear();

    HIDRA_INFO("HidraDataCollector initialized");
  }

  void DoConfigure() override {
    auto conf = GetConfiguration();

    m_event_count = 0;
    m_stop_sent = false;
    m_pending_events.clear();

    m_expected_sources_map.clear();
    std::fill(m_is_source_enabled.begin(), m_is_source_enabled.end(), false);
    m_single_producer_mode = false;

    if (!conf) {
      HIDRA_ERROR("HidraDataCollector: missing run configuration");
    }

    m_filePattern = conf->Get("EUDAQ_FW_PATTERN", "$12D_run$6R$X");
    m_fwType = conf->Get("EUDAQ_FW", "native");

    m_max_events = conf->Get("MAX_EVENTS", 0);
    m_sync_timeout_us = conf->Get("SYNC_TIMEOUT_US", 1000);
    m_tstamp_window_ns = conf->Get("TIMESTAMP_WINDOW_NS", 50000);

    std::string configsources = conf->Get("EXPECTED_SOURCES", "");

    /////// splitting the string
    if (configsources == "") {
      m_expected_sources_map = std::map<std::string, int>{};
      std::fill(m_is_source_enabled.begin(), m_is_source_enabled.end(), false);
      m_is_source_enabled[0] = true;
      m_single_producer_mode = true;
    } else {
      std::stringstream ss(configsources);
      std::string token;
      while (std::getline(ss, token, ',')) {
        std::stringstream pairStream(token);
        std::string detIDs, sourcename;

        if (std::getline(pairStream, detIDs, ':') && std::getline(pairStream, sourcename)) {
          int detID = std::stoi(detIDs);
          if (detID >= 0 && detID < MAX_SOURCES) {
            HIDRA_INFO("Detector ID {} assigned to producer {}", detID, sourcename);
            m_expected_sources_map[sourcename] = detID;
            m_is_source_enabled[detID] = true;
          } else {
            HIDRA_THROW("Detector ID {} cannot be assigned. ID must be between "
                        "0 and {}",
                        detID,
                        MAX_SOURCES);
          }
        } // end of split :
      } // end of split ,
    }
    ////////

    if (m_expected_sources_map.empty()) {
      HIDRA_WARN("No EXPECTED_SOURCES configured. Collector will accept only the first source received, assigning it DetectorID 0");
    }

    HIDRA_INFO("HidraDataCollector configured");
  }

  void DoStartRun() override {
    m_event_count = 0;
    m_stop_sent = false;
    m_running = true;
    m_n_complete_events = 0;
    m_n_incomplete_events = 0;
    m_pending_events.clear();

    if (m_hidraOutput.is_open()) {
      m_hidraOutput.close();
    }

    std::string extension = "." + m_fwType;
    if (m_fwType == "native") {
      extension = ".raw";
    } else if (m_fwType == "root") {
      extension = ".root";
    } else if (m_fwType == "slcio") {
      extension = ".slcio";
    }

    std::time_t time_now = std::time(nullptr);
    char time_buff[13];
    time_buff[12] = 0;
    std::strftime(time_buff, sizeof(time_buff), "%y%m%d%H%M%S", std::localtime(&time_now));
    std::string outputFile = eudaq::FileNamer(m_filePattern)
                                 .Set('X', extension)
                                 .Set('R', GetRunNumber())
                                 .Set('D', std::string(time_buff));

    m_hidraOutput.open(outputFile, std::ios::binary);
    if (!m_hidraOutput) {
      HIDRA_THROW("Cannot open Hidra output file {} ", outputFile);
    }
    HIDRA_INFO("HidraDataCollector output file {}", outputFile);

    HIDRA_INFO("HidraDataCollector start run {}", GetRunNumber());
  }

  void DoStopRun() override {
    m_stop_sent = true;
    m_running = false;
    // TOOD: what do we want to happen to incomplete events when we click stop? If discard, keep the line commented.
    // FlushOldIncompleteEvents();
    m_pending_events.clear();
    HIDRA_INFO("HidraDataCollector stop run {}", GetRunNumber());
  }

  void DoReset() override {
    m_event_count = 0;
    m_stop_sent = false;
    m_running = false;
    m_max_events = 0;
    m_pending_events.clear();
    m_expected_sources_map.clear();
    std::fill(m_is_source_enabled.begin(), m_is_source_enabled.end(), false);

    if (m_hidraOutput.is_open()) {
      m_hidraOutput.close();
    }
    HIDRA_INFO("HidraDataCollector reset");
  }

  void DoTerminate() override {
    if (m_hidraOutput.is_open()) {
      m_hidraOutput.close();
    }
    HIDRA_INFO("HidraDataCollector terminate");
  }

  void DoConnect(eudaq::ConnectionSPC id) override {
    HIDRA_INFO("Connected: {} / {} ", id->GetType(), id->GetName());
    eudaq::DataCollector::DoConnect(id);
  }

  void DoDisconnect(eudaq::ConnectionSPC id) override {
    HIDRA_INFO("Disconnected: {} / {} ", id->GetType(), id->GetName());
    eudaq::DataCollector::DoDisconnect(id);
  }

  void DoReceive(eudaq::ConnectionSPC id, eudaq::EventSP ev) override {

    auto t_start = std::chrono::high_resolution_clock::now();

    if (!m_running) {
      return;
    }

    if (!ev) {
      HIDRA_WARN("HidraDataCollector received null event");
      return;
    }

    auto source = id->GetName();
    auto desc = ev->GetDescription();
    int detectorID = 0;

    if (m_single_producer_mode && m_expected_sources_map.empty() && m_pending_events.empty()) {
      // if here, we are in single producer mode and this is the first event received, so we assign the source to detID 0
      HIDRA_INFO("Single producer mode: assigning source {} to detID 0", source);
      m_expected_sources_map[source] = 0;
      m_is_source_enabled[0] = true;
    }

    if (!IsExpectedSource(source)) {
      HIDRA_ERROR("Event received from unexpected source {}", source);
      return;
    }

    detectorID = m_expected_sources_map[source];
    

    if (ev->IsBORE()) {
      HIDRA_INFO("Received BORE from {} type= {}", id->GetName(), desc);
      // TODO let's collect info to write a file header
      return;
    }

    if (ev->IsEORE()) {
      HIDRA_INFO("Received EORE from {} type= {}", id->GetName(), desc);
      // TODO let's collect info to write a file trailer
      return;
    }

    // stop when reaching max number of events
    CheckMaxEvents();
    if (m_stop_sent) {
      return;
    }

    FlushOldIncompleteEvents();

    uint64_t trigger_number = ev->GetTriggerN();
    uint64_t timestamp = ev->GetTimestampBegin(); // TODO: implement better
                                                  // logic for jitter of boards

    auto& pending = m_pending_events[trigger_number]; // creates a default if trigger_number
                                                      // does not exist

    ////////////// BUFFERING PENDING //////////

    // assign triggerN and time ....
    if (pending.events_by_source.empty()) {
      pending.trigger_number = trigger_number;
      pending.first_seen_ns = hidra::utils::getTimens();
    }

    // ... check if source duplicates ...
    if (pending.events_by_source.count(detectorID) != 0) {
      HIDRA_ERROR("Duplicate event from source/detID {}/{} for trigger {}. "
                  "REPLACING PREVIOUS ONE",
                  source,
                  detectorID,
                  trigger_number);
      // TODO : this is severe.. handle it!
    }

    // .. if not, assign also the SourceEvent
    pending.events_by_source[detectorID] = SourceEvent{id->GetName(), std::move(ev), timestamp};

    HIDRA_DEBUG("Buffered: source/detID {}/{} trig {}  n_source {}",
                source,
                detectorID,
                trigger_number,
                pending.events_by_source.size());

    //////////////////////////////////////////////


    if (!IsComplete(pending)) { // wait for next received, if this is not complete yet
      auto t_end = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
      HIDRA_DEBUG("DoReceive (w/o complete building) took {} us", duration);
      return;
    }

    uint64_t tstampSpread = TimestampSpread(pending);
    if (tstampSpread > m_tstamp_window_ns) {
      HIDRA_ERROR("Timestamp mismatch for trigger {}, max-min is {}", trigger_number, tstampSpread);
      // TODO: handle this
    }

    auto mergedEvt = BuildFullEvent(pending);

    if (!mergedEvt) {
      HIDRA_WARN("Failed to build full event for trigger {}", trigger_number);
      m_pending_events.erase(trigger_number);
      return;
    }

    // if arriving here, the event is complete
    mergedEvt->SetTag("SYNC_STATUS", "COMPLETE");
    m_n_complete_events++;

    //WriteEvent(std::move(mergedEvt));
    hidra::EventSerializer::WriteToStream(*mergedEvt, m_hidraOutput);
    ++m_event_count;
    UpdateStatusTags();
    

    m_pending_events.erase(trigger_number);

    CheckMaxEvents();

    auto t_end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
    HIDRA_DEBUG("DoReceive (w/ complete building) took {} us", duration);
  }
};

namespace {
auto dummy0 =
    eudaq::Factory<eudaq::DataCollector>::Register<HidraDataCollector, const std::string&, const std::string&>(
        HidraDataCollector::m_id_factory);

}
