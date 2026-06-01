#include "XDCFiller.hh"
#include "HidraUtils.hh"

XDCFiller::XDCFiller(HistogramRegistry& reg,
                     unsigned int n_adc_channels,
                     unsigned int n_tdc_channels,
                     int saturation_threshold_adc,
                     int saturation_threshold_tdc)
    : IHistogramFiller("XDCFiller"),
      m_saturation_threshold_adc(saturation_threshold_adc),
      m_saturation_threshold_tdc(saturation_threshold_tdc) {
  if (n_adc_channels == 0) {
    HIDRA_ERROR(
        "XDCFiller constructed with n_adc_channels=0. This may indicate a problem with the VME geo map configuration. "
        "Defaulting to 1 channel to avoid construction failure, but please check the configuration.");
    n_adc_channels = 1;
  }
  if (n_tdc_channels == 0) {
    HIDRA_ERROR("XDCFiller constructed with n_tdc_channels=0.");
    n_tdc_channels = 1;
  }
  m_profile_adc =
      reg.Add(std::make_unique<TProfile>("ADC_mean", "Mean of ADC values", n_adc_channels, 0, n_adc_channels));
  m_hist_adc_inclusive = reg.Add(std::make_unique<TH1D>("ADC_inclusive", "Inclusive ADC values", 4096, 0, 4096));
  m_profile_adc_saturation = reg.Add(
      std::make_unique<TProfile>("ADC_saturation", "Saturation fraction per ADC channel", n_adc_channels, 0, n_adc_channels));
  m_profile_tdc =
      reg.Add(std::make_unique<TProfile>("TDC_mean", "Mean of TDC values", n_tdc_channels, 0, n_tdc_channels));
  m_hist_tdc_inclusive = reg.Add(std::make_unique<TH1D>("TDC_inclusive", "Inclusive TDC values", 4096, 0, 4096));
  m_profile_tdc_saturation = reg.Add(
      std::make_unique<TProfile>("TDC_saturation", "Saturation fraction per TDC channel", n_tdc_channels, 0, n_tdc_channels));

  for (unsigned int i = 0; i < n_adc_channels; ++i) {
    TH1D* hist = reg.Add(std::make_unique<TH1D>(hidra::utils::format("ADC_channel_{}", i).c_str(),
                                                hidra::utils::format("ADC values for channel {}", i).c_str(),
                                                4096,
                                                0,
                                                4096));
    m_hist_adc_channels.push_back(hist);
  }
  for (unsigned int i = 0; i < n_tdc_channels; ++i) {
    TH1D* hist = reg.Add(std::make_unique<TH1D>(hidra::utils::format("TDC_channel_{}", i).c_str(),
                                                hidra::utils::format("TDC values for channel {}", i).c_str(),
                                                4096,
                                                0,
                                                4096));
    m_hist_tdc_channels.push_back(hist);
  }
}

void XDCFiller::Fill(const HidraEvent& event) {
  for (size_t i = 0; i < event.xdc.ADCvalues.size(); ++i) {
    const double value = event.xdc.ADCvalues[i];
    // The decoder leaves channels with no hit at the -1 sentinel; skip them so
    // they don't drag down ADC_mean or dilute the saturation fraction.
    if (value < 0) {
      continue;
    }
    m_profile_adc->Fill(i, value);
    m_hist_adc_inclusive->Fill(value);
    if (i < m_hist_adc_channels.size()) {
      m_hist_adc_channels[i]->Fill(value);
    } else {
      HIDRA_ERROR("ADC channel index {} is out of bounds for histogram array. Skipping filling for this channel.", i);
    }
    m_profile_adc_saturation->Fill(i, value > m_saturation_threshold_adc ? 1 : 0);
  }

  for (size_t i = 0; i < event.xdc.TDCvalues.size(); ++i) {
    const double value = event.xdc.TDCvalues[i];
    if (value < 0) {
      continue;
    }
    m_profile_tdc->Fill(i, value);
    m_hist_tdc_inclusive->Fill(value);
    if (i < m_hist_tdc_channels.size()) {
      m_hist_tdc_channels[i]->Fill(value);
      m_profile_tdc_saturation->Fill(i, value > m_saturation_threshold_tdc ? 1 : 0);
    }
  }
}
