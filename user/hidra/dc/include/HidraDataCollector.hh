#include "TransportBase.hh"
#include "HidraMergedBinaryWriter.hh"
#include "HidraRootEventWriter.hh"
#include <eudaq/DataCollector.hh>
#include <eudaq/Factory.hh>

#include <memory>
#include <map>
#include <string>


class HidraDataCollector : public eudaq::DataCollector {
  
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

public:
  HidraDataCollector(const std::string& name, const std::string& runcontrol)
      : eudaq::DataCollector(name, runcontrol) {}

  ~HidraDataCollector() override = default;

  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraDataCollector");

  void DoInitialise() override;
  void DoConfigure() override;
  void DoStartRun() override;
  void DoStopRun() override;
  void DoReset() override;
  void DoTerminate() override;
  void DoConnect(eudaq::ConnectionSPC id) override;
  void DoDisconnect(eudaq::ConnectionSPC id) override;
  void DoReceive(eudaq::ConnectionSPC id, eudaq::EventSP ev) override;

private:
  bool IsExpectedSource(std::string& source);
  bool IsExpectedSource(int& detID) { return m_is_source_enabled[detID]; }
  bool IsComplete(PendingTrigger& pending);
  uint64_t TimestampSpread(const PendingTrigger& pending) const;
  eudaq::EventSP BuildFullEvent(PendingTrigger& pending);
  bool EnqueueMergedEvent(const eudaq::EventSP& event);
  std::string MakeOutputFile(const std::string& extension) const;
  void FlushOldIncompleteEvents();
  void CheckMaxEvents();
  void UpdateStatusTags();


  const int MAX_SOURCES = 8;

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

  std::map<int, std::string> m_vme_geo_map;

  bool m_single_producer_mode = false; // when no sources are specified, only the first one is accepted and DetectorID 0 is assigned.
  bool m_write_binary_output = true;
  bool m_write_root_output = false;
  uint64_t m_writer_flush_interval_ms = 50;

  std::string m_filePattern;
  std::string m_fwType;
  std::string m_xdc_config_json;
  std::string m_binary_output_file;
  std::string m_root_output_file;

  std::unique_ptr<hidra::HidraMergedBinaryWriter> m_binary_writer;
  std::unique_ptr<hidra::HidraRootEventWriter> m_root_writer;
};

namespace {
auto dummy0 =
    eudaq::Factory<eudaq::DataCollector>::Register<HidraDataCollector, const std::string&, const std::string&>(
        HidraDataCollector::m_id_factory);

}
