#include "ScopedTimer.hh"
#include "HistogramPublisher.hh"


HistogramPublisher::HistogramPublisher(HistogramRegistry& registry, int port)
    : m_registry(registry),
      m_port(port) {}

void HistogramPublisher::Start() {
  if (m_server) {
    return;
  }

  // "http:<port>" makes THttpServer use civetweb as the HTTP backend. THttpServer spawns its own I/O threads; our pump
  // thread is separate and only responsible for draining the request queue.

  const std::string spec = "http:" + std::to_string(m_port);
  m_server = std::make_unique<THttpServer>(spec.c_str());

  m_server->SetItemField("/", "_monitoring", "500"); // auto-refresh for JSROOT 500ms. API clients not affected.

  // Register all histograms booked so far. Start() is called after all
  // fillers are constructed, so the set is complete.
  m_registry.ForEach([this](TH1* h) { m_server->Register(kFolder, h); });

  m_pump_running.store(true);
  m_pump_thread = std::thread(&HistogramPublisher::PumpLoop, this);
}

void HistogramPublisher::Stop() {
  // First stop the pump thread (it may be inside ProcessRequests),
  // then destroy THttpServer (which joins civetweb worker threads).
  m_pump_running.store(false);
  if (m_pump_thread.joinable()) {
    m_pump_thread.join();
  }
  m_server.reset();
}

void HistogramPublisher::PumpLoop() {
  while (m_pump_running.load()) {
    {
      // Same mutex acquired by FillerChain around Fill().
      // Prevents TBufferJSON (inside ProcessRequests) from reading
      // a histogram while a filler is writing to it.
      std::lock_guard<std::mutex> lock(m_mutex);
      ScopedTimer t(m_process_requests);
      m_server->ProcessRequests();
    }
    // Release the lock between iterations to avoid starving fillers.
    // 20 ms -> about 50 cycles/s, which is more than enough for monitoring.
    std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
  }
}
