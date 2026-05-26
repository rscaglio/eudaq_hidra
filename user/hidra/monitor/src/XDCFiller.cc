#include "XDCFiller.hh"

XDCFiller::XDCFiller(HistogramRegistry& reg)
    : IHistogramFiller() {
  m_profile_adc = reg.Add(std::make_unique<TProfile>("ADC_mean", "Mean of ADC values", 100, 0, 100));
  m_hist_adc_inclusive = reg.Add(std::make_unique<TH1D>("ADC_inclusive", "Inclusive ADC values", 4096, 0, 4096));
  m_profile_tdc = reg.Add(std::make_unique<TProfile>("TDC_mean", "Mean of TDC values", 100, 0, 100));
  m_hist_tdc_inclusive = reg.Add(std::make_unique<TH1D>("TDC_inclusive", "Inclusive TDC values", 4096, 0, 4096));
}

void XDCFiller::Fill(const HidraEvent& event) {
  for (size_t i = 0; i < event.xdc.ADCvalues.size(); ++i) {
    m_profile_adc->Fill(i, event.xdc.ADCvalues[i]);
    m_hist_adc_inclusive->Fill(event.xdc.ADCvalues[i]);
  }
  for (size_t i = 0; i < event.xdc.TDCvalues.size(); ++i) {
    m_profile_tdc->Fill(i, event.xdc.TDCvalues[i]);
    m_hist_tdc_inclusive->Fill(event.xdc.TDCvalues[i]);
  }
  
}