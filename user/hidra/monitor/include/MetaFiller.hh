#pragma once
#include "HistogramRegistry.hh"
#include "IHistogramFiller.hh"

#include <TH1.h>

/**
 * @brief Fills histograms from the per-event metadata (HidraEvent::meta).
 *
 * Currently produces the trigger-mask classification (gate / physics / pedestal /
 * both), so physics and pedestal events can be told apart in the GUI.
 */
class MetaFiller : public IHistogramFiller {
public:
  explicit MetaFiller(HistogramRegistry& reg);
  void Fill(const HidraEvent& ev) override;

private:
  TH1I* m_h_trigger_mask;
};
