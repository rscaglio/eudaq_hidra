#include "MetaFiller.hh"

MetaFiller::MetaFiller(HistogramRegistry& reg)
    : IHistogramFiller("MetaFiller") {
  // One bin per trigger-mask value (0..3). Labels are cosmetic; events are filled
  // by numeric value, so events with no trigger mask (meta.trigger_mask < 0) are
  // simply not filled.
  m_h_trigger_mask = reg.Add(std::make_unique<TH1I>("trigger_mask", "Trigger mask;;events", 4, 0, 4));
  m_h_trigger_mask->SetCanExtend(TH1::kNoAxis);
  m_h_trigger_mask->GetXaxis()->SetBinLabel(1, "gate");     // 0
  m_h_trigger_mask->GetXaxis()->SetBinLabel(2, "physics");  // 1
  m_h_trigger_mask->GetXaxis()->SetBinLabel(3, "pedestal"); // 2
  m_h_trigger_mask->GetXaxis()->SetBinLabel(4, "both");     // 3
}

void MetaFiller::Fill(const HidraEvent& ev) {
  const int mask = ev.meta.trigger_mask;
  if (mask >= 0 && mask < 4) {
    m_h_trigger_mask->Fill(mask);
  }
}
