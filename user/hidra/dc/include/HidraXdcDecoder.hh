#pragma once

#include "HidraXdcEvent.hh"

#include <vector>
#include <cstdint>
#include <map>
#include <string>

namespace hidra {

class HidraXdcDecoder {
public:
  HidraXdcDecoder(std::map<int, std::string> vme_geo_map);
  void decode(const std::vector<uint8_t>& payload, HidraXdcEvent& event) const;

  int NADCChannels() const { return m_n_adc_channels; }

private:
  std::map<int, std::string> m_vme_geo_map;
  int m_n_adc_channels;
};

} // namespace hidra
