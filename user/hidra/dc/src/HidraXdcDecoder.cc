#include "HidraUtils.hh"
#include "HidraXdcDecoder.hh"

#include <cstring>

namespace hidra {

struct ADCHeaderWord {
  uint32_t raw;
  uint8_t type() const { return (raw >> 24) & 0x7; }
  uint8_t geo() const { return (raw >> 27) & 0x1F; }
  uint8_t crate() const { return (raw >> 16) & 0xFF; }
  uint8_t cnt() const { return (raw >> 8) & 0x3F; }
};

struct ADCTrailerWord {
  uint32_t raw;
  uint32_t evt_cnt() const { return raw & 0x7FFFFF; }
  uint8_t type() const { return (raw >> 24) & 0x7; }
  uint8_t geo() const { return (raw >> 27) & 0x1F; }
};

struct V792Word {
  uint32_t raw;
  uint16_t value() const { return raw & 0x7FF; }
  uint8_t ov() const { return (raw >> 12) & 0x1; }
  uint8_t un() const { return (raw >> 13) & 0x1; }
  uint8_t channel() const { return (raw >> 16) & 0x1F; }
  uint8_t type() const { return (raw >> 24) & 0x7; }
  uint8_t geo() const { return (raw >> 27) & 0x1F; }
};

HidraXdcDecoder::HidraXdcDecoder(std::map<int, std::string> vme_geo_map)
    : m_vme_geo_map(std::move(vme_geo_map)) {

  m_n_adc_channels = hidra::utils::computeMaxADCchannelFromGeoMap(m_vme_geo_map);
  HIDRA_INFO("HidraXdcDecoder configured with {} ADC channels based on VME geo map", m_n_adc_channels);
}

void HidraXdcDecoder::decode(const std::vector<uint8_t>& payload, HidraXdcEvent& event) const {
  event = HidraXdcEvent{};

  const auto payload_size = payload.size();

  if (payload_size == 0) {
    HIDRA_ERROR("XDC payload is empty. Aborting");
    return;
  }

  if (payload_size % 4 != 0) {
    HIDRA_ERROR("XDC payload size {} is not a multiple of 4 bytes. Aborting", payload_size);
    return;
  }
  const std::size_t word_count = payload_size / 4;
  if (word_count == 0) {
    HIDRA_ERROR("XDC payload is too short or empty. Aborting");
    return;
  }

  std::vector<std::uint32_t> words(word_count);
  std::memcpy(words.data(), payload.data(), word_count * sizeof(std::uint32_t));

  std::vector<double> ADCvalues(m_n_adc_channels, -1);
  std::vector<double> ADCflags(m_n_adc_channels, -1);
  std::vector<double> TDCvalues(1500, -1);
  std::vector<double> TDCflags(1500, -1);

  uint8_t expected_word_mask = 0b010; // 0b010 is header, 0b000 is channel, 0b100 is trailer

  for (auto it = words.begin(); it != words.end(); ++it) {

    auto word = *it;

    if ((word & 0xFE000000) == 0xFE000000) { // this is expected at the end of buffer
      continue;
    }

    else {

      expected_word_mask = 0b010;

      ADCHeaderWord W{word};

      if (W.type() != expected_word_mask) {
        HIDRA_ERROR("Unexpected XDC word type: {:08X} type {}. Should be Header Word. Aborting {}",
                    word,
                    W.type(),
                    word & 0xFE000000);
        return;
      }

      int nchan = W.cnt();
      const auto module_it = m_vme_geo_map.find(W.geo());
      const std::string module_type = module_it == m_vme_geo_map.end() ? "unknown" : module_it->second;

      expected_word_mask = 0b000;
      for (int ichan = 0; ichan < nchan; ++ichan) {
        ++it;
        if (it == words.end()) {
          HIDRA_ERROR("No more words in the XDC data block, while payload data word is expected. Aborting");
          return;
        }
        word = *it;
        if (module_type == "unknown" || module_type == "V792" ||
            module_type == "V862") { // Like this, V792 is the default
          V792Word V{word};
          if (V.type() != expected_word_mask) {
            HIDRA_ERROR("Unexpected XDC word type: {:08X} type {}. Should be Channel Word. Aborting", word, V.type());
            return;
          }
          if (V.geo() != W.geo()) {
            HIDRA_ERROR("Mismatched geo in XDC words: header geo {} vs channel geo {}. Aborting", W.geo(), V.geo());
            return;
          }
          int encoded_channel = (module_type == "unknown")
                                    ? V.channel()
                                    : hidra::utils::computeADCchannelFromGeo(m_vme_geo_map, V.geo(), V.channel());
          if (encoded_channel < 0 || encoded_channel >= m_n_adc_channels) {
            HIDRA_ERROR(
                "Encoded ADC channel index {} is out of bounds (0, {}). Skipping", encoded_channel, m_n_adc_channels);
          } else {
            ADCvalues[encoded_channel] = V.value();
            ADCflags[encoded_channel] = (V.ov() << 1) | V.un();
          }

        } // if 792 or 862 or unknown
        else {
          HIDRA_ERROR("Unknown XDC module type {} for crate {} geo {}. Cannot decode channel word. Aborting",
                      module_type,
                      W.crate(),
                      W.geo());
          return;
        }

      } // loop over channels

      ++it;
      if (it == words.end()) {
        HIDRA_ERROR("No more words in the XDC data block, while trailer is expected. Aborting");
        return;
      }
      word = *it;

      expected_word_mask = 0b100;

      ADCTrailerWord T{word};
      if (T.type() != expected_word_mask) {
        HIDRA_ERROR("Unexpected XDC word type: {:08X} type {}. Should be Trailer Word. Aborting", word, T.type());
        return;
      }
    }
  }
  event.ADCvalues = std::move(ADCvalues);
  event.ADCflags = std::move(ADCflags);
  event.TDCvalues = std::move(TDCvalues);
  event.TDCflags = std::move(TDCflags);
}

} // namespace hidra
