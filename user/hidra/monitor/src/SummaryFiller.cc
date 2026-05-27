#include "SummaryFiller.hh"

SummaryFiller::SummaryFiller(HistogramRegistry& reg)
    : IHistogramFiller(),
      m_run_start(std::chrono::steady_clock::now()) {
  m_h_event_count = reg.Add(std::make_unique<TH1I>("event_count", "Event count", 1, 0, 1));
  m_h_events_vs_time =
      reg.Add(std::make_unique<TH1F>("events_vs_time", "Events vs time;elapsed time [s];events / bin", 300, 0, 300));
}

void SummaryFiller::Reset() {
  m_run_start = std::chrono::steady_clock::now();
}

void SummaryFiller::Fill(const HidraEvent& event) {
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_run_start).count();
  m_h_event_count->Fill(0);
  m_h_events_vs_time->Fill(elapsed);
}