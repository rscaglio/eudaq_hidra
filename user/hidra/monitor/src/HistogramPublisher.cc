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


  m_server->SetItemField("/", "_monitoring", "500");  // auto-refresh for JSROOT 500ms. API clients not affected.

  // Registra tutti gli istogrammi prenotati finora. Start() viene chiamato
  // dopo la costruzione di tutti i filler, quindi il set è completo.
  m_registry.ForEach([this](TH1* h) { m_server->Register(kFolder, h); });

  m_pump_running.store(true);
  m_pump_thread = std::thread(&HistogramPublisher::PumpLoop, this);
}

void HistogramPublisher::Stop() {
  // Prima ferma il pump thread (che potrebbe stare dentro ProcessRequests),
  // poi distruggi THttpServer (che fa join dei thread civetweb).
  m_pump_running.store(false);
  if (m_pump_thread.joinable()) {
    m_pump_thread.join();
  }
  m_server.reset();
}

void HistogramPublisher::PumpLoop() {
  while (m_pump_running.load()) {
    {
      // Stesso mutex che FillerChain acquisisce attorno a Fill().
      // Impedisce a TBufferJSON (dentro ProcessRequests) di leggere
      // un istogramma mentre un filler lo sta scrivendo.
      std::lock_guard<std::mutex> lock(m_mutex);
      m_server->ProcessRequests();
    }
    // Rilascia il lock tra un'iterazione e l'altra per non affamare i
    // filler. 20 ms → ~50 cicli/s, più che sufficienti per il monitoring.
    std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIntervalMs));
  }
}
