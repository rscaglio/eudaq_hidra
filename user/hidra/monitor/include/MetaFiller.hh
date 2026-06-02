#pragma once
#include "HistogramRegistry.hh"
#include "IHistogramFiller.hh"

#include <TH1.h>

#include <cstdint>

/**
 * @brief Fills histograms from the per-event metadata (HidraEvent::meta).
 *
 * Produces the trigger-mask classification (gate / physics / pedestal / both),
 * the set of detectors present, the events-per-spill structure, the inter-event
 * time distribution, and single-bin "current value" readouts for spill / trigger
 * / run number.
 */
class MetaFiller : public IHistogramFiller {
public:
  explicit MetaFiller(HistogramRegistry& reg);
  void Fill(const HidraEvent& ev) override;
  void Reset() override;

private:
  TH1I* m_h_trigger_mask;
  TH1I* m_h_detectors_present;
  TH1D* m_h_events_per_spill;
  TH1D* m_h_dt_between_events; // inter-event time, log-binned, in microseconds
  TH1D* m_h_spill_current;
  TH1D* m_h_trigger_current;
  TH1D* m_h_run_current;

  // Run-relative state for the inter-event time: begin timestamp of the previous event.
  uint64_t m_last_begin_ns = 0;
  bool m_have_last = false;
};
