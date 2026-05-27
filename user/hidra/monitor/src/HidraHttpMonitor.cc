#include "HidraHttpMonitor.hh"

#include "HidraEvent.hh"

#include "SummaryFiller.hh"
#include "XDCFiller.hh"
#include "HidraUtils.hh"

#include <eudaq/Event.hh>
#include <eudaq/Factory.hh>

const uint32_t HidraHttpMonitor::m_id_factory = eudaq::cstr2hash("HidraHttpMonitor");

namespace {
auto _reg = eudaq::Factory<eudaq::Monitor>::Register<HidraHttpMonitor, const std::string&, const std::string&>(
    HidraHttpMonitor::m_id_factory);
}

// ── RunContext ────────────────────────────────────────────────────────────

HidraHttpMonitor::RunContext::RunContext(int port, hidra::HidraXdcDecoder xdc_dec, hidra::HidraFersDecoder fers_dec)
    : publisher(registry, port),
      chain(publisher.Mutex()),
      xdc_decoder(std::move(xdc_dec)),
      fers_decoder(std::move(fers_dec)) {

  chain.Add(std::make_unique<SummaryFiller>(registry));
  chain.Add(std::make_unique<XDCFiller>(registry));

  // Start the HTTP server only after all fillers are constructed, so THttpServer sees the complete set of histograms
  // from the start.
  publisher.Start();
}

HidraHttpMonitor::RunContext::~RunContext() {
  publisher.Stop();
}

// ── HttpMonitor ───────────────────────────────────────────────────────────

HidraHttpMonitor::HidraHttpMonitor(const std::string& name, const std::string& runcontrol)
    : eudaq::Monitor(name, runcontrol) {}

void HidraHttpMonitor::DoInitialise() {
  HIDRA_INFO("Initializing HidraHttpMonitor");
  if (auto ini = GetInitConfiguration()) {
    m_port = ini->Get("HTTP_PORT", 9090);
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

  auto ctx = std::make_unique<RunContext>(m_port, *m_xdc_decoder, *m_fers_decoder);

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

  // ── Decoding — outside the histogram lock ───────────────────────────
  // Each decoder writes to its own field of HidraEvent. Decoding is read-only on the payload and doesn't touch
  // histograms, so there's no reason to keep it inside the critical section. The lock only protects the Fill() that
  // follows.

  HidraEvent decoded;

  HIDRA_DEBUG("Received event {} with {} subevents", ev->GetEventID(), ev->GetNumSubEvent());
  for (size_t index = 0; index < ev->GetNumSubEvent(); ++index) {
    eudaq::EventSPC subevent = ev->GetSubEvent(index);  // no copy, just a shared pointer copy of the subevent handle
    if (!subevent) {
      continue;
    }

    const int det_id = hidra::utils::getTagOr<int>(*subevent, "detID", index);
    HIDRA_DEBUG("Decoding subevent {} with detID {}", index, det_id);
    const std::string producer = subevent->HasTag("Producer") ? subevent->GetTag("Producer") : "";
    std::vector<std::uint8_t> detector_payload;
    for (const auto block_id : subevent->GetBlockNumList()) {
      const auto block = subevent->GetBlock(block_id);
      detector_payload.insert(detector_payload.end(), block.begin(), block.end());
    }

    if (det_id == 1 || det_id == 6) {
      m_ctx->xdc_decoder.decode(detector_payload, decoded.xdc);
    }
    else if (det_id == 2) {
        m_ctx->fers_decoder.decode(detector_payload, decoded.fers);
    }
  }


  // --- Dispatch — inside the histogram lock --------------------------
  // FillerChain acquires publisher.Mutex() before calling the fillers.
  m_ctx->chain.Fill(decoded);
}
