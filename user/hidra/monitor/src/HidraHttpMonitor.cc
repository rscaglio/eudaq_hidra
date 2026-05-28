#include "HidraHttpMonitor.hh"

#include "HidraEvent.hh"

#include "SummaryFiller.hh"
#include "XDCFiller.hh"
#include "HidraUtils.hh"
#include "ScopedTimer.hh"

#include <eudaq/Event.hh>
#include <eudaq/Factory.hh>

const uint32_t HidraHttpMonitor::m_id_factory = eudaq::cstr2hash("HidraHttpMonitor");

namespace {
auto _reg = eudaq::Factory<eudaq::Monitor>::Register<HidraHttpMonitor, const std::string&, const std::string&>(
    HidraHttpMonitor::m_id_factory);
}

// ── RunContext ────────────────────────────────────────────────────────────

HidraHttpMonitor::RunContext::RunContext(
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

HidraHttpMonitor::RunContext::~RunContext() noexcept {
  publisher.Stop();
  try {
    LogTelemetry();
  } catch (...) {
    // Best-effort telemetry: swallow exceptions to keep destructor safe.
  }
}

void HidraHttpMonitor::RunContext::LogTelemetry() {
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
  }
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

  m_xdc_decoder.emplace(vme_geo_map);
  m_fers_decoder.emplace();
}

void HidraHttpMonitor::DoStartRun() {
  HIDRA_INFO("Starting HidraHttpMonitor run");
  if (!m_xdc_decoder || !m_fers_decoder) {
    EUDAQ_THROW("Decoders not configured");
  }

  auto ctx = std::make_unique<RunContext>(m_port, m_pump_interval_ms, m_event_prescale, *m_xdc_decoder, *m_fers_decoder);

  std::unique_lock<std::shared_mutex> lock(m_ctx_mutex);
  m_ctx = std::move(ctx);
}

void HidraHttpMonitor::DoStopRun() {
  HIDRA_INFO("Stopping HidraHttpMonitor run");
  // The unique lock waits for all in-flight DoReceive to release the shared lock. Only then we destroy m_ctx: the
  // destruction safely joins the pump thread and shuts down THttpServer

  std::unique_lock<std::shared_mutex> lock(m_ctx_mutex);
  m_ctx.reset();
}

void HidraHttpMonitor::DoReset() {
  HIDRA_INFO("Resetting HidraHttpMonitor state");
  std::shared_lock<std::shared_mutex> lock(m_ctx_mutex);
  if (!m_ctx) {
    return;
  }
  std::lock_guard<std::mutex> fill_lock(m_ctx->publisher.Mutex());
  m_ctx->chain.Reset();
  m_ctx->registry.Reset();
}

void HidraHttpMonitor::DoTerminate() {
  HIDRA_INFO("Terminating HidraHttpMonitor");
  DoStopRun();
}

void HidraHttpMonitor::DoReceive(eudaq::EventSP ev) {
  // Shared lock: keeps m_ctx alive for the whole call.
  std::shared_lock<std::shared_mutex> lock(m_ctx_mutex);
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
