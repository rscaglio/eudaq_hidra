#include "MetaFiller.hh"

#include <cmath>
#include <vector>

namespace {
// Log-spaced bin edges for the inter-event time histogram, covering a wide range
// (1 us .. 10 s) so the (variable) event rate always lands on a sensible scale.
std::vector<double> LogBinEdges(double lo_us, double hi_us, int bins_per_decade) {
  const double lo = std::log10(lo_us);
  const double hi = std::log10(hi_us);
  const int n = static_cast<int>(std::lround((hi - lo) * bins_per_decade));
  std::vector<double> edges(n + 1);
  for (int i = 0; i <= n; ++i) {
    edges[i] = std::pow(10.0, lo + (hi - lo) * i / n);
  }
  return edges;
}
} // namespace

MetaFiller::MetaFiller(HistogramRegistry& reg)
    : IHistogramFiller("MetaFiller") {
  // Trigger mask: one bin per value (0..3). Labels are cosmetic; events are filled
  // by numeric value, so events with no trigger mask (meta.trigger_mask < 0) are
  // simply not filled.
  m_h_trigger_mask = reg.Add(std::make_unique<TH1I>("trigger_mask", "Trigger mask;;events", 4, 0, 4));
  m_h_trigger_mask->SetCanExtend(TH1::kNoAxis);
  m_h_trigger_mask->GetXaxis()->SetBinLabel(1, "gate");     // 0
  m_h_trigger_mask->GetXaxis()->SetBinLabel(2, "physics");  // 1
  m_h_trigger_mask->GetXaxis()->SetBinLabel(3, "pedestal"); // 2
  m_h_trigger_mask->GetXaxis()->SetBinLabel(4, "both");     // 3

  // Detectors present: one bin per detID (0..7), filled from the set bits of the
  // detector mask. A detector dropping out shows fewer counts than the others.
  m_h_detectors_present =
      reg.Add(std::make_unique<TH1I>("detectors_present", "Detectors present;detID;events", 8, 0, 8));
  m_h_detectors_present->SetCanExtend(TH1::kNoAxis);
  for (int det = 0; det < 8; ++det) {
    m_h_detectors_present->GetXaxis()->SetBinLabel(det + 1, std::to_string(det).c_str());
  }

  // Events per spill: auto-extending axis so the (unbounded, slowly growing) spill
  // number does not need a fixed range.
  m_h_events_per_spill =
      reg.Add(std::make_unique<TH1D>("events_per_spill", "Events per spill;spill;events", 1, 0, 1));
  m_h_events_per_spill->SetCanExtend(TH1::kAllAxes);

  // Inter-event time (begin[i] - begin[i-1]), log-binned in microseconds.
  const std::vector<double> dt_edges = LogBinEdges(1.0, 1.0e7, 10); // 1 us .. 10 s, 10 bins/decade
  m_h_dt_between_events = reg.Add(std::make_unique<TH1D>(
      "dt_between_events", "Time between consecutive events;#Delta t [#mus];events",
      static_cast<int>(dt_edges.size()) - 1, dt_edges.data()));

  // Single-bin "current value" readouts (latest value, not a distribution).
  m_h_spill_current = reg.Add(std::make_unique<TH1D>("spill_current", "Current spill number", 1, 0, 1));
  m_h_trigger_current = reg.Add(std::make_unique<TH1D>("trigger_current", "Current trigger number", 1, 0, 1));
  m_h_run_current = reg.Add(std::make_unique<TH1D>("run_current", "Current run number", 1, 0, 1));
}

void MetaFiller::Reset() {
  m_last_begin_ns = 0;
  m_have_last = false;
}

void MetaFiller::Fill(const HidraEvent& ev) {
  const HidraEventMeta& meta = ev.meta;

  const int mask = meta.trigger_mask;
  if (mask >= 0 && mask < 4) {
    m_h_trigger_mask->Fill(mask);
  }

  if (meta.detector_mask >= 0) {
    for (int det = 0; det < 8; ++det) {
      if (meta.detector_mask & (1 << det)) {
        m_h_detectors_present->Fill(det);
      }
    }
  }

  if (meta.spill_number != 0xFFFFFFFFu) {
    m_h_events_per_spill->Fill(static_cast<double>(meta.spill_number));
    m_h_spill_current->SetBinContent(1, static_cast<double>(meta.spill_number));
  }

  // Inter-event time from the begin timestamps of consecutive events.
  if (m_have_last && meta.timestamp_begin_ns > m_last_begin_ns) {
    const double dt_us = static_cast<double>(meta.timestamp_begin_ns - m_last_begin_ns) * 1e-3;
    m_h_dt_between_events->Fill(dt_us);
  }
  m_last_begin_ns = meta.timestamp_begin_ns;
  m_have_last = true;

  m_h_trigger_current->SetBinContent(1, static_cast<double>(meta.trigger_number));
  m_h_run_current->SetBinContent(1, static_cast<double>(meta.run_number));
}
