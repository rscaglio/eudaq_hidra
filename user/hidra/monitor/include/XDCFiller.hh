#pragma once
#include "HistogramRegistry.hh"
#include "IHistogramFiller.hh"

#include <TH1D.h>
#include <TProfile.h>

class XDCFiller : public IHistogramFiller {
public:
  explicit XDCFiller(HistogramRegistry& reg, unsigned int n_adc_channels);
  void Fill(const HidraEvent&) override;

private:
  TProfile* m_profile_adc;
  TH1D* m_hist_adc_inclusive;
  std::vector<TH1D*> m_hist_adc_channels;

  TProfile* m_profile_tdc;
  TH1D* m_hist_tdc_inclusive;
};
