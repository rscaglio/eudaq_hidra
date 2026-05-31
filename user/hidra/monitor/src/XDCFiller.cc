#include "XDCFiller.hh"
#include "HidraUtils.hh"

XDCFiller::XDCFiller(HistogramRegistry& reg, unsigned int n_adc_channels)
    : IHistogramFiller("XDCFiller") {
  if (n_adc_channels == 0) {
    HIDRA_ERROR(
        "XDCFiller constructed with n_adc_channels=0. This may indicate a problem with the VME geo map configuration. "
        "Defaulting to 1 channel to avoid construction failure, but please check the configuration.");
    n_adc_channels = 1;
  }
  m_profile_adc =
      reg.Add(std::make_unique<TProfile>("ADC_mean", "Mean of ADC values", n_adc_channels, 0, n_adc_channels));
  m_hist_adc_inclusive = reg.Add(std::make_unique<TH1D>("ADC_inclusive", "Inclusive ADC values", 4096, 0, 4096));
  m_profile_tdc = reg.Add(std::make_unique<TProfile>("TDC_mean", "Mean of TDC values", 100, 0, 100));
  m_hist_tdc_inclusive = reg.Add(std::make_unique<TH1D>("TDC_inclusive", "Inclusive TDC values", 4096, 0, 4096));

  for (unsigned int i = 0; i < n_adc_channels; ++i) {
    TH1D* hist = reg.Add(std::make_unique<TH1D>(hidra::utils::format("ADC_channel_{}", i).c_str(),
                                                hidra::utils::format("ADC values for channel {}", i).c_str(),
                                                4096,
                                                0,
                                                4096));
    m_hist_adc_channels.push_back(hist);
  }
}

void XDCFiller::Fill(const HidraEvent& event) {
  for (size_t i = 0; i < event.xdc.ADCvalues.size(); ++i) {
    m_profile_adc->Fill(i, event.xdc.ADCvalues[i]);
    m_hist_adc_inclusive->Fill(event.xdc.ADCvalues[i]);
    if (i < m_hist_adc_channels.size()) {
      m_hist_adc_channels[i]->Fill(event.xdc.ADCvalues[i]);
    } else {
      HIDRA_ERROR("ADC channel index {} is out of bounds for histogram array. Skipping filling for this channel.", i);
    }
  }
  for (size_t i = 0; i < event.xdc.TDCvalues.size(); ++i) {
    m_profile_tdc->Fill(i, event.xdc.TDCvalues[i]);
    m_hist_tdc_inclusive->Fill(event.xdc.TDCvalues[i]);
  }
}