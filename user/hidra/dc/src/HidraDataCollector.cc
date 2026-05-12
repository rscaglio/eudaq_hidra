#include "HidraDataCollector.hh"
#include "HidraRootPayloadDecoders.hh"
#include "HidraUtils.hh"
#include <cmath>
#include <eudaq/Event.hh>
#include <eudaq/Exception.hh>
#include <eudaq/FileNamer.hh>
#include <eudaq/Logger.hh>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <numeric>
#include <string>

// user custom

bool HidraDataCollector::IsExpectedSource(std::string& source) {
  auto it = m_expected_sources_map.find(source);
  if (it == m_expected_sources_map.end()) {
    return false;
  } else {
    return m_is_source_enabled[m_expected_sources_map.at(source)];
  }
}

bool HidraDataCollector::IsComplete(PendingTrigger& pending) {
  for (const auto& s : m_expected_sources_map) {
    if (pending.events_by_source.count(s.second) == 0) {
      return false;
    }
  }
  return true;
}

uint64_t HidraDataCollector::TimestampSpread(const PendingTrigger& pending) const {
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

std::string HidraDataCollector::MakeOutputFile(const std::string& extension) const {
  std::time_t time_now = std::time(nullptr);
  char time_buff[13];
  time_buff[12] = 0;
  std::strftime(time_buff, sizeof(time_buff), "%y%m%d%H%M%S", std::localtime(&time_now));

  return eudaq::FileNamer(m_filePattern).Set('X', extension).Set('R', GetRunNumber()).Set('D', std::string(time_buff));
}

bool HidraDataCollector::EnqueueMergedEvent(const eudaq::EventSP& event) {
  if (!event) {
    return false;
  }

  bool accepted = false;

  if (m_write_binary_output && m_binary_writer) {
    accepted = m_binary_writer->EnqueueEvent(event) || accepted;
  }

  if (m_write_root_output && m_root_writer) {
    accepted = m_root_writer->EnqueueEvent(event) || accepted;
  }

  if (!accepted) {
    HIDRA_WARN("Merged event for trigger {} was not accepted by any active writer", event->GetTriggerN());
  }

  return accepted;
}

eudaq::EventSP HidraDataCollector::BuildFullEvent(PendingTrigger& pending) {

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

void HidraDataCollector::FlushOldIncompleteEvents() {

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
    ++m_event_count;
    UpdateStatusTags();
    EnqueueMergedEvent(mergedEvt);

    it = m_pending_events.erase(it);
  }
}

void HidraDataCollector::CheckMaxEvents() {

  if (!m_stop_sent && m_max_events != 0 && m_event_count >= m_max_events) {
    m_stop_sent = true;
    HIDRA_INFO("Max events reached. Sending STOP request");
    SetStatus(eudaq::Status::STATE_RUNNING, "STOP_REQUEST");
    SendStatus();
  }
}

void HidraDataCollector::UpdateStatusTags() {
  SetStatusTag("EventN", std::to_string(m_event_count));
  SetStatusTag("Pending", std::to_string(m_pending_events.size()));
  SetStatusTag("Completes", std::to_string(m_n_complete_events));
  SetStatusTag("Incompletes", std::to_string(m_n_incomplete_events));
  SetStatusTag("EventsOnDisk", std::to_string(m_binary_writer ? m_binary_writer->GetWrittenEventCount() : 0));
  SetStatusTag("kBOnDisk", std::to_string(m_binary_writer ? m_binary_writer->GetWrittenByteCount()/1000 : 0));
  SetStatusTag("CalibTimingStatus", m_calib_timing_needed ? "Waiting" : (m_calib_timing_validated ? "Ok" : "Failed"));
  SetStatusTag("CalibTimingOffset", std::to_string(m_calib_timing_mean)+"+/-"+std::to_string(m_calib_timing_spread)+" ns");
  SendStatus();
}

//////////////////////////////////////////////

void HidraDataCollector::DoInitialise() {
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
  m_calib_timing_events.clear();
  m_calib_timing_validated = false;
  m_calib_timing_needed = true;

  HIDRA_INFO("HidraDataCollector initialized");
}

void HidraDataCollector::DoConfigure() {
  auto conf = GetConfiguration();

  m_event_count = 0;
  m_stop_sent = false;
  m_pending_events.clear();
  m_calib_timing_events.clear();
  m_calib_timing_validated = false;
  m_calib_timing_needed = true;

  m_expected_sources_map.clear();
  std::fill(m_is_source_enabled.begin(), m_is_source_enabled.end(), false);
  m_single_producer_mode = false;

  if (!conf) {
    HIDRA_ERROR("HidraDataCollector: missing run configuration");
  }

  m_filePattern = conf->Get("EUDAQ_FW_PATTERN", "$12D_run$6R$X");
  m_fwType = conf->Get("EUDAQ_FW", "native");
  m_write_binary_output = conf->Get("WRITE_BINARY_OUTPUT", 1);
  m_write_root_output = conf->Get("WRITE_ROOT_OUTPUT", 0);
  m_writer_flush_interval_ms = conf->Get("WRITER_FLUSH_INTERVAL_MS", 50);
 
  if (!m_write_binary_output && !m_write_root_output) {
    HIDRA_WARN("Both WRITE_BINARY_OUTPUT and WRITE_ROOT_OUTPUT are disabled. Collector will run without file output.");
  }

  m_max_events = conf->Get("MAX_EVENTS", 0);
  m_sync_timeout_us = conf->Get("SYNC_TIMEOUT_US", 1000);
  m_tstamp_window_ns = conf->Get("TIMESTAMP_WINDOW_NS", 50000);

  std::string configsources = conf->Get("EXPECTED_SOURCES", "");

  if (configsources == "") {
    m_expected_sources_map = std::map<std::string, int>{};
    std::fill(m_is_source_enabled.begin(), m_is_source_enabled.end(), false);
    m_is_source_enabled[0] = true;
    m_single_producer_mode = true;
  } else {
    std::map<std::string, std::string> tempprod = hidra::utils::parseConfigMap(configsources);
    for (const auto& kv : tempprod) {
      const std::string& idetid = kv.first;
      const std::string& iproducer = kv.second;
      int detID = std::stoi(idetid);
      if (detID >= 0 && detID < MAX_SOURCES) {
          HIDRA_INFO("Detector ID {} assigned to producer {}", detID, iproducer);
          m_expected_sources_map[iproducer] = detID;
          m_is_source_enabled[detID] = true;
      } else {
	HIDRA_THROW("Detector ID {} cannot be assigned. ID must be between "
		    "0 and {}",
		    detID,
		    MAX_SOURCES);
      }
    }
  }

  m_vme_geo_map.clear();
  std::string vmecrateconfig = conf->Get("VME_CRATE_1", "");
  if (vmecrateconfig != ""){
    std::map<std::string, std::string> tempvme = hidra::utils::parseConfigMap(vmecrateconfig);
    for (const auto& kv : tempvme) {
      const std::string& geo = kv.first;
      const std::string& modname = kv.second;
      int geoaddr = std::stoi(geo);
      m_vme_geo_map[geoaddr] = modname;
      HIDRA_INFO("VME module at geo address {} is {}", geoaddr, modname);
    }
  }
  

  if (m_expected_sources_map.empty()) {
    HIDRA_WARN("No EXPECTED_SOURCES configured. Collector will accept only the first source received, assigning it "
               "DetectorID 0");
  }

  HIDRA_INFO("HidraDataCollector configured");
}

void HidraDataCollector::DoStartRun() {
  m_event_count = 0;
  m_stop_sent = false;
  m_running = true;
  m_n_complete_events = 0;
  m_n_incomplete_events = 0;
  m_pending_events.clear();
  m_calib_timing_events.clear();
  m_calib_timing_validated = false;
  m_calib_timing_needed = true;

  m_binary_writer.reset();
  m_root_writer.reset();

  if (m_write_binary_output) {
    m_binary_output_file = MakeOutputFile(".raw");
    HIDRA_INFO("Output_file: {}", m_binary_output_file);
    auto binary_writer =
        std::make_unique<hidra::HidraMergedBinaryWriter>(m_binary_output_file, m_writer_flush_interval_ms);

    binary_writer->Start();
    if (!binary_writer->IsActive()) {
      HIDRA_ERROR("Binary writer could not start: {}", binary_writer->GetLastError());
      m_write_binary_output = false;
    } else {
      m_binary_writer = std::move(binary_writer);
      HIDRA_INFO("HidraDataCollector binary output file {}", m_binary_output_file);
    }
  }

  if (m_write_root_output) {
    m_root_output_file = MakeOutputFile(".root");
    auto root_writer =
        std::make_unique<hidra::HidraRootEventWriter>(m_root_output_file, m_writer_flush_interval_ms, 32,
                                                      m_vme_geo_map);
    root_writer->Start();
    if (!root_writer->IsActive()) {
      HIDRA_ERROR("ROOT writer could not start: {}", root_writer->GetLastError());
      m_write_root_output = false;
    } else {
      m_root_writer = std::move(root_writer);
      HIDRA_INFO("HidraDataCollector ROOT output file {}", m_root_output_file);
    }
  }

  HIDRA_INFO("HidraDataCollector start run {}", GetRunNumber());
}

void HidraDataCollector::DoStopRun() {
  m_stop_sent = true;
  m_running = false;
  // TOOD: what do we want to happen to incomplete events when we click stop? If discard, keep the line commented.
  FlushOldIncompleteEvents();
  m_pending_events.clear();
  m_calib_timing_events.clear();
  m_calib_timing_validated = false;
  m_calib_timing_needed = true;

  if (m_binary_writer) {
    m_binary_writer->Stop();
  }
  if (m_root_writer) {
    m_root_writer->Stop();
  }

  HIDRA_INFO("HidraDataCollector stop run {}", GetRunNumber());
}

void HidraDataCollector::DoReset() {
  m_event_count = 0;
  m_stop_sent = false;
  m_running = false;
  m_max_events = 0;
  m_pending_events.clear();
  m_calib_timing_events.clear();
  m_calib_timing_validated = false;
  m_calib_timing_needed = true;
  m_expected_sources_map.clear();
  std::fill(m_is_source_enabled.begin(), m_is_source_enabled.end(), false);

  if (m_binary_writer) {
    m_binary_writer->Stop();
  }
  if (m_root_writer) {
    m_root_writer->Stop();
  }
  HIDRA_INFO("HidraDataCollector reset");
}

void HidraDataCollector::DoTerminate() {
  m_running = false;

  if (m_binary_writer) {
    m_binary_writer->Stop();
  }
  if (m_root_writer) {
    m_root_writer->Stop();
  }
  HIDRA_INFO("HidraDataCollector terminate");
}

void HidraDataCollector::DoConnect(eudaq::ConnectionSPC id) {
  HIDRA_INFO("Connected: {} / {} ", id->GetType(), id->GetName());
  eudaq::DataCollector::DoConnect(id);
}

void HidraDataCollector::DoDisconnect(eudaq::ConnectionSPC id) {
  HIDRA_INFO("Disconnected: {} / {} ", id->GetType(), id->GetName());
  eudaq::DataCollector::DoDisconnect(id);
}

void HidraDataCollector::DoReceive(eudaq::ConnectionSPC id, eudaq::EventSP ev) {

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

  if (m_calib_timing_needed && m_calib_timing_events.size() < CALIB_TIMING_EVENTS) {
    m_calib_timing_events.push_back((long long)TimestampSpread(pending));
    HIDRA_INFO("Calibrated AAAAA {}", m_calib_timing_events[m_calib_timing_events.size()-1]);
  }
  else if (m_calib_timing_needed) {
    auto MuStd = hidra::utils::ComputeMeanAndStdDev(m_calib_timing_events);
    m_calib_timing_mean = MuStd.first;
    m_calib_timing_spread = MuStd.second * 5; // 5 sigma
    m_calib_timing_validated = true;

    for (int ii = 0; ii < m_calib_timing_events.size(); ii++) {
      long long t = m_calib_timing_events[ii];
      if (std::abs(t - m_calib_timing_mean) > m_calib_timing_spread) {
        m_calib_timing_validated = false;
        HIDRA_ERROR("Calibrated timing: timespread {}, calibrated to {}, at index {} is above the 5*std threshold of {} ns. Calibration failed.",
                   t,
                   t - m_calib_timing_mean,
                   ii,
                   m_calib_timing_spread);
        break;
      }
    }

    if (m_calib_timing_spread > m_tstamp_window_ns) {
      HIDRA_ERROR("Calibrated timing spread {} ns is above the configured timestamp window of {} ns. This may lead to "
                  "many incomplete events.",
                  m_calib_timing_spread,
                  m_tstamp_window_ns);
      m_calib_timing_validated = false;
    }

    if (m_calib_timing_validated) {
      HIDRA_INFO("Calibrated timing: timestamp spread mean is {} ns, 5*stddev is {} ns. Calibration successful.",
                 m_calib_timing_mean,
                 m_calib_timing_spread);
    } else {
      HIDRA_ERROR("Calibrated timing is not valid. Timestamp spread mean is {} ns, 5*stddev is {} ns. Calibration failed.",
                 m_calib_timing_mean,
                 m_calib_timing_spread);
    }

    m_calib_timing_needed = false; // TODO: we may want to recalibrate timing during the run, if we see many incomplete events, or after a certain time has passed, 
  }


  uint64_t tstampSpread = TimestampSpread(pending);
  if (std::abs((long long)tstampSpread - m_calib_timing_mean) > m_tstamp_window_ns) {
    HIDRA_ERROR("Timestamp mismatch for trigger {}, max-min is {}, calibrated mean is {}", trigger_number, tstampSpread, m_calib_timing_mean);
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

  ++m_event_count;
  UpdateStatusTags();
  EnqueueMergedEvent(mergedEvt);

  m_pending_events.erase(trigger_number);

  CheckMaxEvents();

  auto t_end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
  HIDRA_DEBUG("DoReceive (w/ complete building) took {} us", duration);
}
