#include "HidraHttpMonitor.hh"

#include "HidraEvent.hh"

#include "SummaryFiller.hh"
#include "XDCFiller.hh"
#include "HidraUtils.hh"
#include "ScopedTimer.hh"

#include <eudaq/Event.hh>
#include <eudaq/Factory.hh>
#include <eudaq/FileNamer.hh>

#include <ctime>

const uint32_t HidraHttpMonitor::m_id_factory = eudaq::cstr2hash("HidraHttpMonitor");

namespace {
auto _reg = eudaq::Factory<eudaq::Monitor>::Register<HidraHttpMonitor, const std::string&, const std::string&>(
    HidraHttpMonitor::m_id_factory);
}

// ── MonitorContext ──────────────────────────────────────────────────────────

HidraHttpMonitor::MonitorContext::MonitorContext(
    int port,
    int pump_interval_ms,
  int prescale,
    hidra::HidraXdcDecoder xdc_dec,
    hidra::HidraFersDecoder fers_dec)
    : publisher(registry, port, pump_interval_ms),
      chain(publisher.Mutex()),
      xdc_decoder(std::move(xdc_dec)),
      fers_decoder(std::move(fers_dec)),
      event_prescale(prescale) {

  chain.Add(std::make_unique<SummaryFiller>(registry));
  chain.Add(std::make_unique<XDCFiller>(registry));

  // Start the HTTP server only after all fillers are constructed, so THttpServer sees the complete set of histograms
  // from the start.
  publisher.Start();
}

HidraHttpMonitor::MonitorContext::~MonitorContext() noexcept {
  publisher.Stop();
}

void HidraHttpMonitor::MonitorContext::ResetTelemetry() {
  // Caller holds publisher.Mutex(): ProcessRequestsTimer is written by the pump thread inside that same lock, and the
  // decode/fill timers are written by DoReceive which is not running at a run boundary.
  duration_xdc_decode.Reset();
  duration_fers_decode.Reset();
  chain.LockWaitTimer().Reset();
  for (const auto& filler : chain.Fillers()) {
    filler->Timer().Reset();
  }
  publisher.ProcessRequestsTimer().Reset();
}

void HidraHttpMonitor::MonitorContext::LogTelemetry() {
  HIDRA_INFO("=== Monitor Telemetry ===");

  HIDRA_INFO("  " + duration_xdc_decode.Summary());
  HIDRA_INFO("  " + duration_fers_decode.Summary());

  // Lock wait — this is the time spent waiting for the histogram lock in FillerChain::Fill(). If this is high, it means
  // the pump thread is contending with DoReceive for the lock, which may indicate that the pump frequency is too high
  // or that the fillers are doing too much work inside the critical section.
  HIDRA_INFO("  " + chain.LockWaitTimer().Summary());

  for (const auto& filler : chain.Fillers()) {
    HIDRA_INFO("  " + filler->Timer().Summary());
  }

  HIDRA_INFO("  " + publisher.ProcessRequestsTimer().Summary());
}

// ── HttpMonitor ───────────────────────────────────────────────────────────

HidraHttpMonitor::HidraHttpMonitor(const std::string& name, const std::string& runcontrol)
    : eudaq::Monitor(name, runcontrol) {}

void HidraHttpMonitor::DoInitialise() {
  HIDRA_INFO("Initializing HidraHttpMonitor");
  if (auto ini = GetInitConfiguration()) {
    m_port = ini->Get("HTTP_PORT", 9090);
    m_pump_interval_ms = ini->Get("PUMP_INTERVAL_MS", 20);
    m_event_prescale = ini->Get("EVENT_PRESCALE", 1);
    if (m_event_prescale == 0) {
      HIDRA_WARN("EVENT_PRESCALE=0 is invalid, forcing EVENT_PRESCALE=1");
      m_event_prescale = 1;
    }
    // FileNamer pattern for the ROOT file written at end-of-run. Set empty to disable saving.
    m_histo_output_pattern = ini->Get("HISTO_OUTPUT_PATTERN", m_histo_output_pattern);
  }
}

std::string HidraHttpMonitor::MakeHistoOutputFile() const {
  std::time_t time_now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&time_now, &tm_buf); // thread-safe variant; std::localtime uses shared static storage
  char time_buff[13];
  time_buff[12] = 0;
  std::strftime(time_buff, sizeof(time_buff), "%y%m%d%H%M%S", &tm_buf);

  return eudaq::FileNamer(m_histo_output_pattern)
      .Set('X', ".root")
      .Set('R', GetRunNumber())
      .Set('D', std::string(time_buff));
}

void HidraHttpMonitor::DoConfigure() {
  HIDRA_INFO("Configuring HidraHttpMonitor");
  auto conf = GetConfiguration();

  std::map<int, std::string> vme_geo_map;
  std::string vmecrateconfig = conf->Get("VME_CRATE_1", "");
  if (vmecrateconfig != "") {
    std::map<std::string, std::string> tempvme = hidra::utils::parseConfigMap(vmecrateconfig);
    for (const auto& kv : tempvme) {
      const std::string& geo = kv.first;
      const std::string& modname = kv.second;
      int geoaddr = std::stoi(geo);
      vme_geo_map[geoaddr] = modname;
      HIDRA_INFO("VME module at geo address {} is {}", geoaddr, modname);
    }
  }

  hidra::HidraXdcDecoder xdc_decoder(vme_geo_map);
  hidra::HidraFersDecoder fers_decoder;

  std::unique_lock<std::shared_mutex> lock(m_state_mutex);
  if (!m_ctx) {
    // First configure: build the long-lived monitoring context. This starts the HTTP server with empty histograms,
    // so the GUI is reachable from now on and stays up across run start/stop.
    m_ctx = std::make_unique<MonitorContext>(m_port, m_pump_interval_ms, m_event_prescale, std::move(xdc_decoder),
                                             std::move(fers_decoder));
  } else {
    // Reconfigure: keep the server alive, only swap the decoders to the new configuration.
    m_ctx->xdc_decoder = std::move(xdc_decoder);
    m_ctx->fers_decoder = std::move(fers_decoder);
  }

  // Clear the histogram contents left over from a previous run: a (re)configuration means a fresh setup, so the GUI
  // should not keep showing stale data. We deliberately do NOT reset the fillers' run-relative state here (e.g.
  // SummaryFiller's start-of-run time reference): that belongs to DoStartRun, as a configure may happen well before the
  // run actually starts. On the first configure the histograms are already empty, so this is a no-op.
  std::lock_guard<std::mutex> fill_lock(m_ctx->publisher.Mutex());
  m_ctx->registry.Reset();
}

void HidraHttpMonitor::DoStartRun() {
  HIDRA_INFO("Starting HidraHttpMonitor run");

  std::shared_lock<std::shared_mutex> lock(m_state_mutex);
  if (!m_ctx) {
    EUDAQ_THROW("HidraHttpMonitor started before being configured");
  }

  // Reset the histograms (in place, so the THttpServer keeps pointing at the same objects) and the per-run state, so
  // the new run starts from a clean slate while the server keeps serving.
  m_ctx->event_counter.store(0, std::memory_order_relaxed);
  std::lock_guard<std::mutex> fill_lock(m_ctx->publisher.Mutex());
  m_ctx->chain.Reset();
  m_ctx->registry.Reset();
  m_ctx->ResetTelemetry();
  m_ctx->run_active = true; // re-arm finalization for the new run
}

void HidraHttpMonitor::FinalizeRun() {
  // Caller holds m_state_mutex and guarantees m_ctx is set.
  // The telemetry log and the ROOT save are done while holding publisher.Mutex() on purpose: ROOT is not thread-safe,
  // so writing a TFile concurrently with the pump thread's TBufferJSON serialization would race on global ROOT state.
  // The only cost is briefly pausing HTTP serialization at end-of-run, when no events are arriving anyway.
  std::lock_guard<std::mutex> fill_lock(m_ctx->publisher.Mutex());
  if (!m_ctx->run_active) {
    return; // already finalized (e.g. STOP then TERMINATE), or no run was started
  }

  m_ctx->LogTelemetry();

  if (!m_histo_output_pattern.empty()) {
    const std::string path = MakeHistoOutputFile();
    if (m_ctx->registry.SaveToFile(path)) {
      HIDRA_INFO("Saved monitor histograms to {}", path);
    } else {
      HIDRA_WARN("Failed to save monitor histograms to {}", path);
    }
  }

  m_ctx->run_active = false;
}

void HidraHttpMonitor::DoStopRun() {
  HIDRA_INFO("Stopping HidraHttpMonitor run");
  // The run is over but we deliberately keep m_ctx (and its HTTP server) alive so the histograms of the run just
  // finished stay browsable until the next run starts or the monitor terminates. Here we just finalize the run (log
  // telemetry and snapshot the histograms to a ROOT file).

  std::shared_lock<std::shared_mutex> lock(m_state_mutex);
  if (!m_ctx) {
    return;
  }
  FinalizeRun();
}

void HidraHttpMonitor::DoReset() {
  HIDRA_INFO("Resetting HidraHttpMonitor state");
  std::shared_lock<std::shared_mutex> lock(m_state_mutex);
  if (!m_ctx) {
    return;
  }
  std::lock_guard<std::mutex> fill_lock(m_ctx->publisher.Mutex());
  m_ctx->chain.Reset();
  m_ctx->registry.Reset();
}

void HidraHttpMonitor::DoTerminate() {
  HIDRA_INFO("Terminating HidraHttpMonitor");
  // Final shutdown. The unique lock waits for in-flight DoReceive to finish. If the monitor is terminated while a run
  // is still active (no explicit STOP), finalize it first so telemetry and the ROOT snapshot are not lost; FinalizeRun
  // is a no-op if the run was already finalized. Then destroy the context, which stops the HTTP server.
  std::unique_lock<std::shared_mutex> lock(m_state_mutex);
  if (m_ctx) {
    FinalizeRun();
  }
  m_ctx.reset();
}

void HidraHttpMonitor::DoReceive(eudaq::EventSP ev) {
  // Shared lock: keeps m_ctx alive and the decoders stable for the whole call.
  std::shared_lock<std::shared_mutex> lock(m_state_mutex);
  if (!m_ctx) {
    return;
  }

  const uint64_t event_index = m_ctx->event_counter.fetch_add(1, std::memory_order_relaxed);
  if ((event_index % m_ctx->event_prescale) != 0) {
    return;
  }

  // ── Decoding — outside the histogram lock ───────────────────────────
  // Each decoder writes to its own field of HidraEvent. Decoding is read-only on the payload and doesn't touch
  // histograms, so there's no reason to keep it inside the critical section. The lock only protects the Fill() that
  // follows.

  HidraEvent decoded;

  for (size_t index = 0; index < ev->GetNumSubEvent(); ++index) {
    eudaq::EventSPC subevent = ev->GetSubEvent(index); // no copy, just a shared pointer copy of the subevent handle
    if (!subevent) {
      continue;
    }

    const int det_id = hidra::utils::getTagOr<int>(*subevent, "detID", index);
    const auto block_ids = subevent->GetBlockNumList();
    std::size_t total_payload_size = 0;
    for (const auto block_id : block_ids) {
      total_payload_size += subevent->GetBlock(block_id).size();
    }

    std::vector<std::uint8_t> detector_payload;
    detector_payload.reserve(total_payload_size);
    for (const auto block_id : block_ids) {
      const auto block = subevent->GetBlock(block_id);
      detector_payload.insert(detector_payload.end(), block.begin(), block.end());
    }

    if (det_id == 1 || det_id == 6) {
      ScopedTimer t(m_ctx->duration_xdc_decode);
      m_ctx->xdc_decoder.decode(detector_payload, decoded.xdc);
    } else if (det_id == 2) {
      ScopedTimer t(m_ctx->duration_fers_decode);
      m_ctx->fers_decoder.decode(detector_payload, decoded.fers);
    }
  }

  // --- Dispatch — inside the histogram lock --------------------------
  // FillerChain acquires publisher.Mutex() before calling the fillers.
  m_ctx->chain.Fill(decoded);
}
