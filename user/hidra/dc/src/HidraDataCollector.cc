#pragma once

#include <eudaq/DataCollector.hh>
#include <eudaq/Factory.hh>
#include <eudaq/Logger.hh>
#include <eudaq/Event.hh>
#include <eudaq/Configuration.hh>

#include <cstdint>
#include <string>
#include <memory>
#include <map>
#include <set>
#include <chrono>

class HidraDataCollector : public eudaq::DataCollector {
public:
  HidraDataCollector(const std::string &name, const std::string &runcontrol)
      : eudaq::DataCollector(name, runcontrol) {}

  ~HidraDataCollector() override = default;

  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraDataCollector");

private:


  // --- Per-source event container ---
  struct SourceEvent {
    eudaq::ConnectionSPC connection;
    eudaq::EventSP event;
    uint64_t timestamp;
  };

  // --- Buffer for a given trigger ---
  struct PendingTrigger {
    uint64_t trigger_number;
    std::map<std::string, SourceEvent> events_by_source;
    uint64_t first_seen_ns;
  };

  uint64_t m_event_count;
  uint64_t m_max_events;
  bool m_stop_sent = false;
  bool m_running = false;

  std::set<std::string> m_expected_sources;
  std::map<uint64_t, PendingTrigger> m_pending_events;
  uint64_t m_sync_timeout_us = 1000000;
  uint64_t m_tstamp_window_ns = 50000;


  // user custom

  uint64_t getTimens(){
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  }

  bool IsExpectedSource(std::string &source){
    if (m_expected_sources.empty()) return true;
    return m_expected_sources.count(source) != 0;
  }

  bool IsComplete(PendingTrigger &pending){
    for (const auto &s : m_expected_sources) {
      if (pending.events_by_source.count(s) == 0) return false;
    }
    return true;
  }

  uint64_t TimestampSpread(const PendingTrigger &pending) const {
    if (pending.events_by_source.size() <= 1) {
      return true;
    }

    uint64_t t_min = UINT64_MAX;
    uint64_t t_max = 0;

    for (const auto &item : pending.events_by_source) {
      const auto timestamp = item.second.timestamp;
      t_min = std::min(t_min, timestamp);
      t_max = std::max(t_max, timestamp);
    }
    return (t_max - t_min);
  }

  eudaq::EventSP BuildFullEvent(PendingTrigger &pending){

    // just a skeleton. Full event must be built here
    auto it = pending.events_by_source.begin();
    
    if (it == pending.events_by_source.end()) {
      EUDAQ_ERROR("Cannot build event out of NO PENDING triggers");
      return nullptr;
    }

    auto fullEvt = std::move(it->second.event);
    fullEvt->SetTriggerN(pending.trigger_number);
    fullEvt->SetTag("N_SOURCES", std::to_string(pending.events_by_source.size()));

    return fullEvt;
  }

  void FlushOldIncompleteEvents() {

    for (auto it = m_pending_events.begin(); it != m_pending_events.end(); ) {

      uint64_t age_ns = getTimens() - it->second.first_seen_ns;
           

      if (age_ns <= m_sync_timeout_us*1000) {
        ++it;
        continue;
      }

      uint64_t trigger = it->first;

      EUDAQ_WARN("Timeout waiting for complete event for trigger "+ std::to_string(trigger));

      

      // if one wants to discard:
      // it = m_pending_events.erase(it);

      auto mergedEvt = BuildFullEvent(it->second);

      if (!mergedEvt){
	EUDAQ_WARN("Failed to build incomplete event for trigger "+std::to_string(trigger));
	it = m_pending_events.erase(it);
	return;
      }

      mergedEvt->SetTag("SYNC_STATUS", "INCOMPLETE");
      WriteEvent(std::move(mergedEvt));
      ++m_event_count;
      it = m_pending_events.erase(it);
    }
  }
  

  void CheckMaxEvents() {
    
    if (!m_stop_sent && m_max_events != 0 && m_event_count >= m_max_events) {
      m_stop_sent = true;
      EUDAQ_INFO("Max events reached. Sending STOP request");
      SetStatus(eudaq::Status::STATE_RUNNING, "STOP_REQUEST");
      SendStatus();
    }
    
  }

  //////////////////////////////////////////////
  
  void DoInitialise() override {
    auto ini = GetInitConfiguration();
    if (!ini) {
      EUDAQ_WARN("HidraDataCollector: missing init configuration");
    }
    
    m_event_count = 0;
    m_pending_events.clear();

    EUDAQ_INFO("HidraDataCollector initialized");
  }

  void DoConfigure() override {
    auto conf = GetConfiguration();
    
    m_event_count = 0;
    m_stop_sent = false;
    m_pending_events.clear();
    m_expected_sources.clear();
    
    if (!conf) {
      EUDAQ_WARN("HidraDataCollector: missing run configuration");
    }

    m_max_events = conf->Get("MAX_EVENTS", 0);
    m_sync_timeout_us = conf->Get("SYNC_TIMEOUT_US", 1000);
    m_tstamp_window_ns = conf->Get("TIMESTAMP_WINDOW_NS", 50000);

    std::string configsources = conf->Get("EXPECTED_SOURCES", "");
    
    /////// splitting the string
    if (configsources == ""){
      m_expected_sources = std::set<std::string>{};
    }
    else {
      std::stringstream ss(configsources);
      std::string token;
      while (std::getline(ss, token, ',')){
	m_expected_sources.insert(token);
      }
    }
    ////////

    if (m_expected_sources.empty()){
      EUDAQ_WARN("No EXPECTED_SOURCES configured. Collector will accept everything but cannot require completeness.");
    }

    EUDAQ_INFO("HidraDataCollector configured");
    
  }

  void DoStartRun() override {
    m_event_count = 0;
    m_stop_sent = false;
    m_running = true;
    m_pending_events.clear();
    EUDAQ_INFO("HidraDataCollector start run " + std::to_string(GetRunNumber()));
  }

  void DoStopRun() override {
    m_stop_sent = true;
    m_running = false;

    // TODO: flush incomplete event

    m_pending_events.clear();
    
    EUDAQ_INFO("HidraDataCollector stop run " + std::to_string(GetRunNumber()));
  }

  void DoReset() override {
    m_event_count = 0;
    m_stop_sent = false;
    m_running = false;
    m_max_events = 0;
    m_pending_events.clear();
    m_expected_sources.clear();
    EUDAQ_INFO("HidraDataCollector reset");
  }

  void DoTerminate() override {
    EUDAQ_INFO("HidraDataCollector terminate");
  }


  void DoConnect(eudaq::ConnectionSPC id) override {
    EUDAQ_INFO("Connected: " + id->GetType() + " / " + id->GetName());
    eudaq::DataCollector::DoConnect(id);
  }

  void DoDisconnect(eudaq::ConnectionSPC id) override {
    EUDAQ_INFO("Disconnected: " + id->GetType() + " / " + id->GetName());
    eudaq::DataCollector::DoDisconnect(id);
  }

  void DoReceive(eudaq::ConnectionSPC id, eudaq::EventSP ev) override {

    if(!m_running) return;
    
    if (!ev) {
      EUDAQ_WARN("HidraDataCollector received null event");
      return;
    }

    auto source = id->GetName();
    auto desc = ev->GetDescription();

    if (!IsExpectedSource(source)){
      EUDAQ_WARN("Event received from unexpected source "+source);
      return;
    }

    if (ev->IsBORE()) {
      EUDAQ_INFO("Received BORE from " + id->GetName() + " type=" + desc);
      // TODO let's collect info to write a file header
      return;
    }
    
    if (ev->IsEORE()) {
      EUDAQ_INFO("Received EORE from " + id->GetName() + " type=" + desc);
      // TODO let's collect info to write a file header
      return;
    }

    // stop when reaching max number of events
    CheckMaxEvents();
    if (m_stop_sent) return;

    // FLUSH INCOMPLETE EVENTS

    uint64_t trigger_number = ev->GetTriggerN();
    uint64_t timestamp = ev->GetTimestampBegin(); // TODO: implement better logic for jitter of boards

    auto &pending = m_pending_events[trigger_number]; // creates a default if trigger_number does not exist

    ////////////// BEFFERING PENDING //////////
    
    // assign triggerN and time ....
    if (pending.events_by_source.empty()){
      pending.trigger_number = trigger_number;
      pending.first_seen_ns = getTimens();
    }

    // ... check if source duplicates ...
    if (pending.events_by_source.count(source) != 0){
      EUDAQ_ERROR("Duplicate event from source "+source+" for trigger "+std::to_string(trigger_number)+". REPLACING PREVIOUS ONE");
      // TODO : this is severe.. handle it!
    }

    // .. of not, assign also the SourceEvent
    pending.events_by_source[source] = SourceEvent{id, std::move(ev), timestamp};
    
    EUDAQ_DEBUG("Buffered: source "+source+" trig "+std::to_string(trigger_number)+" n_source "+std::to_string(pending.events_by_source.size()));

    //////////////////////////////////////////////

    if (!IsComplete(pending)){  // wait for next received, if this is not complete yet
      return;
    }

    uint64_t tstampSpread = TimestampSpread(pending);
    if (tstampSpread > m_tstamp_window_ns) {
      EUDAQ_ERROR("Timestamp mismatch for trigger "+std::to_string(trigger_number)+", max-min is "+std::to_string(tstampSpread));
      // TODO: handle this
    }

    auto mergedEvt = BuildFullEvent(pending);

    if (!mergedEvt){
      EUDAQ_WARN("Failed to build full event for trigger "+std::to_string(trigger_number));
      m_pending_events.erase(trigger_number);
      return;
    }

    // if arriving here, the event is complete
    mergedEvt->SetTag("SYNC_STATUS", "COMPLETE");

    WriteEvent(std::move(mergedEvt));

    ++m_event_count;

    m_pending_events.erase(trigger_number);

    CheckMaxEvents();
  }
};
    
 
namespace {
  auto dummy0 =
    eudaq::Factory<eudaq::DataCollector>::
	Register<HidraDataCollector, const std::string&, const std::string&>
	(HidraDataCollector::m_id_factory);

}
