#pragma once
/**
 * EUDAQ2 monitor plugin for HiDRA histogram publishing over HTTP.
 *
 * The monitor receives events in DoReceive(), decodes them, and forwards the
 * resulting HidraEvent to FillerChain, which updates the histogram registry.
 * Histograms are then exposed through HistogramPublisher via ROOT THttpServer.
 *
 * Event processing flow in DoReceive():
 * 1. Unpack the DataCollector sub-event envelope.
 * 2. Decode payload into HidraEvent outside the histogram-content lock.
 * 3. Call chain.Fill(HidraEvent) while holding the histogram-content lock.
 *
 * Threads:
 * - T_ctrl: RunControl thread executing lifecycle hooks.
 * - T_recv: DataReceiver thread invoking DoReceive() for each event.
 * - T_pump: HistogramPublisher internal thread periodically flushing HTTP queue.
 * - T_http: civetweb HTTP I/O thread, not modifying histograms.
 *
 * Locking model:
 * - m_ctx_mutex (std::shared_mutex) protects RunContext lifetime.
 * - publisher.Mutex() (std::mutex) protects histogram content.
 *
 * Lock order in DoReceive() is always:
 * m_ctx_mutex (shared) -> publisher.Mutex().
 * The reverse order must never be used.
 *
 * RunContext owns all run-scoped objects. It is created in DoStartRun() and
 * destroyed in DoStopRun().
 */

#include "FillerChain.hh"
#include "HistogramPublisher.hh"
#include "HistogramRegistry.hh"

#include <HidraXdcDecoder.hh>
#include <HidraFersDecoder.hh>

#include <eudaq/Factory.hh>
#include <eudaq/Monitor.hh>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <optional>
#include <atomic>

class HidraHttpMonitor : public eudaq::Monitor {
public:
  /** Construct the monitor plugin instance. */
  HidraHttpMonitor(const std::string& name, const std::string& runcontrol);

  /** EUDAQ lifecycle hook for initial setup before configuration. */
  void DoInitialise() override;
  /** EUDAQ lifecycle hook for reading and applying monitor configuration. */
  void DoConfigure() override;
  /** EUDAQ lifecycle hook for creating run-scoped context and starting services. */
  void DoStartRun() override;
  /** EUDAQ lifecycle hook for stopping services and destroying run-scoped context. */
  void DoStopRun() override;
  /** EUDAQ lifecycle hook for resetting monitor state while keeping the process alive. */
  void DoReset() override;
  /** EUDAQ lifecycle hook for final shutdown of monitor resources. */
  void DoTerminate() override;
  /** Receive and process a single EUDAQ event. */
  void DoReceive(eudaq::EventSP ev) override;

  static const uint32_t m_id_factory;

private:
  /** Run-scoped ownership bundle for objects created at run start and released at run stop. */
  struct RunContext {
    HistogramRegistry registry;
    HistogramPublisher publisher;
    FillerChain chain;

    /** Run-scoped decoders that may carry run-dependent state such as calibration caches. */
    hidra::HidraXdcDecoder xdc_decoder;
    hidra::HidraFersDecoder fers_decoder;

    DurationAccumulator duration_xdc_decode{"decode_xdc"};
    DurationAccumulator duration_fers_decode{"decode_fers"};

    int event_prescale{1};
    std::atomic<uint64_t> event_counter{0};

    /** Build a run context with configured decoders, HTTP port, pump interval, and event prescale. */
    RunContext(int port, int pump_interval_ms, int prescale, hidra::HidraXdcDecoder xdc_dec, hidra::HidraFersDecoder fers_dec, int n_adc_channels);
    ~RunContext() noexcept;
    void LogTelemetry();
  };

  int m_port{9090};
  int m_pump_interval_ms{20}; /**< Pump interval in milliseconds (~50 Hz default). */
  int m_event_prescale{1}; /**< Process 1 event every N events (N>=1). */
  /** Decoder templates loaded in DoConfigure() and copied to RunContext. */
  std::optional<hidra::HidraXdcDecoder>  m_xdc_decoder;
  std::optional<hidra::HidraFersDecoder> m_fers_decoder;

  /** Protects lifetime transitions of m_ctx across lifecycle and receive paths. */
  mutable std::shared_mutex m_ctx_mutex;
  /** Active run context, created in DoStartRun() and reset in DoStopRun(). */
  std::unique_ptr<RunContext> m_ctx;
};
