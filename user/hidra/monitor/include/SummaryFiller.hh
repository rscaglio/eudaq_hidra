#pragma once
#include "HistogramRegistry.hh"
#include "IHistogramFiller.hh"

#include <TH1.h>

#include <chrono>

class SummaryFiller : public IHistogramFiller {
public:
  explicit SummaryFiller(HistogramRegistry& reg);
  void Fill(const HidraEvent&) override;

private:
  std::chrono::steady_clock::time_point m_run_start;
  TH1I* m_h_event_count;
  TH1F* m_h_events_vs_time;
};