#include "HidraRootPayloadDecoders.hh"
#include "HidraUtils.hh"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace hidra {

namespace {

template <typename T> T ReadLE(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
  T value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(T));
  return value;
}

template <typename T> T GetBits(T value, unsigned first_bit, unsigned last_bit) {
  static_assert(std::is_unsigned<T>::value, "GetBits expects an unsigned integer type");

  constexpr unsigned bit_count = std::numeric_limits<T>::digits;
  if (first_bit > last_bit || last_bit >= bit_count) {
    return 0;
  }

  const unsigned width = last_bit - first_bit + 1;
  const T mask = (width == bit_count) ? std::numeric_limits<T>::max() : ((T{1} << width) - T{1});
  return (value >> first_bit) & mask;
}

void AddQuantity(std::vector<RootQuantity>& quantities, std::string name, double value, std::string unit = "") {
  quantities.push_back(RootQuantity{std::move(name), value, std::move(unit)});
}

void AddBranchValue(RootBranchValues& branches, const std::string& name, double value) {
  branches[name].push_back(value);
}

void AddBranchValues(RootBranchValues& branches, const std::string& name, const std::vector<double>& values) {
  auto& branch = branches[name];
  branch.insert(branch.end(), values.begin(), values.end());
}

} // namespace

std::vector<std::string> RootPayloadDecoder::BranchNames() const {
  return {};
}

bool HidraGenericPayloadDecoder::Matches(const RootDetectorPayload&) const {
  return true;
}

HidraXdcPayloadDecoder::HidraXdcPayloadDecoder(std::map<int, std::string> vme_geo_map)
    : m_vme_geo_map(std::move(vme_geo_map)) {}

std::vector<std::string> HidraGenericPayloadDecoder::BranchNames() const {
  return {"payload_bytes", "timestamp_span_ns"};
}

void HidraGenericPayloadDecoder::Decode(const RootDetectorPayload& detector,
                                        std::vector<RootQuantity>& quantities,
                                        RootBranchValues& branches) const {
  AddQuantity(quantities, "payload_bytes", static_cast<double>(detector.payload.size()), "B");
  const auto span_ns = (detector.event_time_end >= detector.event_time_begin)
                           ? static_cast<double>(detector.event_time_end - detector.event_time_begin)
                           : 0.0;
  AddQuantity(quantities, "timestamp_span", span_ns, "ns");

  AddBranchValue(branches, "payload_bytes", static_cast<double>(detector.payload.size()));
  AddBranchValue(branches, "timestamp_span_ns", span_ns);
}

bool HidraXdcPayloadDecoder::Matches(const RootDetectorPayload& detector) const {
  return detector.det_id == 1 || detector.det_id == 6;
}

std::vector<std::string> HidraXdcPayloadDecoder::BranchNames() const {
  auto names = HidraGenericPayloadDecoder{}.BranchNames();
  names.push_back("ADCs");
  names.push_back("ADCFlags");
  names.push_back("TDCs");
  names.push_back("TDCFlags");
  return names;
}

void HidraXdcPayloadDecoder::Decode(const RootDetectorPayload& detector,
                                    std::vector<RootQuantity>& quantities,
                                    RootBranchValues& branches) const {
  HidraGenericPayloadDecoder{}.Decode(detector, quantities, branches);

  const auto payload_size = detector.payload.size();
  if (payload_size % 4 != 0) {
    HIDRA_ERROR("XDC payload size is not a multiple of 4 bytes {}. Aborting", payload_size);
    return;
  }
  const std::size_t word_count = payload_size / 4;
  if (word_count == 0) {
    HIDRA_WARN("XDC payload is empty");
    return;
  }

  std::vector<std::uint32_t> words(word_count);
  std::memcpy(words.data(), detector.payload.data(), word_count * sizeof(std::uint32_t));

  const int max_channel_index = hidra::utils::computeMaxADCchannelFromGeoMap(m_vme_geo_map);
  std::vector<double> ADCvalues(max_channel_index, -1);
  std::vector<double> ADCflags(max_channel_index, -1);
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
          if (encoded_channel < 0 || encoded_channel >= max_channel_index) {
            HIDRA_ERROR(
                "Encoded ADC channel index {} is out of bounds (0, {}). Skipping", encoded_channel, max_channel_index);
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
      word = *it;

      expected_word_mask = 0b100;

      ADCTrailerWord T{word};
      if (T.type() != expected_word_mask) {
        HIDRA_ERROR("Unexpected XDC word type: {:08X} type {}. Should be Trailer Word. Aborting", word, T.type());
        return;
      }
    }
  }

  AddBranchValues(branches, "ADCs", ADCvalues);
  AddBranchValues(branches, "ADCFlags", ADCflags);
  AddBranchValues(branches, "TDCs", TDCvalues);
  AddBranchValues(branches, "TDCFlags", TDCflags);
}

bool HidraFersPayloadDecoder::Matches(const RootDetectorPayload& detector) const {
  // TODO: temporary enabling only det_id 2 to abvoid using current decoder for Dry runs on 2025 data
  //return detector.det_id == 2 || detector.det_id == 7 || detector.producer.find("FERS") != std::string::npos;
  return detector.det_id == 2;
}

std::vector<std::string> HidraFersPayloadDecoder::BranchNames() const {
  auto names = HidraGenericPayloadDecoder{}.BranchNames();
  names.push_back("FERStsamp_us");
  names.push_back("FERSrel_tsamp_us");
  names.push_back("FERStrigger_id");
  names.push_back("FERSboard_id");
  names.push_back("FERShg");
  names.push_back("FERSlg");
  names.push_back("FERStoa");
  names.push_back("FERStot");
  return names;
}

/* reimplementing below (Nicolo)
void HidraFersPayloadDecoder::Decode(const RootDetectorPayload& detector,
                                     std::vector<RootQuantity>& quantities,
                                     RootBranchValues& branches) const {
  HidraGenericPayloadDecoder{}.Decode(detector, quantities, branches);

  const auto& payload = detector.payload;

  if (payload.size() >= 56) {
    const auto timestamp_us = ReadLE<double>(payload, 0);
    const auto rel_timestamp_us = ReadLE<double>(payload, 8);
    const auto trigger_id = static_cast<double>(ReadLE<std::uint64_t>(payload, 32));
    const auto chmask = static_cast<double>(ReadLE<std::uint64_t>(payload, 40));
    const auto qdmask = static_cast<double>(ReadLE<std::uint64_t>(payload, 48));

    AddQuantity(quantities, "fers2_tstamp_us", timestamp_us, "us");
    AddQuantity(quantities, "fers2_rel_tstamp_us", rel_timestamp_us, "us");
    AddQuantity(quantities, "fers2_trigger_id", trigger_id);
    AddQuantity(quantities, "fers2_chmask", chmask);
    AddQuantity(quantities, "fers2_qdmask", qdmask);

    AddBranchValue(branches, "fers2_tstamp_us", timestamp_us);
    AddBranchValue(branches, "fers2_rel_tstamp_us", rel_timestamp_us);
    AddBranchValue(branches, "fers2_trigger_id", trigger_id);
    AddBranchValue(branches, "fers2_chmask", chmask);
    AddBranchValue(branches, "fers2_qdmask", qdmask);
  }

  if (payload.size() >= 56 + 64 * 2) {
    std::uint64_t energy_sum = 0;
    std::uint16_t energy_max = 0;
    std::vector<double> energies_hg;
    energies_hg.reserve(64);
    for (std::size_t index = 0; index < 64; ++index) {
      const auto value = ReadLE<std::uint16_t>(payload, 56 + index * sizeof(std::uint16_t));
      energy_sum += value;
      energy_max = std::max(energy_max, value);
      energies_hg.push_back(static_cast<double>(value));
    }
    AddQuantity(quantities, "fers2_energy_hg_sum", static_cast<double>(energy_sum));
    AddQuantity(quantities, "fers2_energy_hg_max", static_cast<double>(energy_max));

    AddBranchValues(branches, "fers2_energy_hg", energies_hg);
    AddBranchValue(branches, "fers2_energy_hg_sum", static_cast<double>(energy_sum));
    AddBranchValue(branches, "fers2_energy_hg_max", static_cast<double>(energy_max));
  }

  if (payload.size() >= 17 && payload.size() < 56) {
    const auto board_id = static_cast<double>(payload[0]);
    const auto timestamp_ns = ReadLE<double>(payload, 1) * 1000.0;
    const auto trigger_id = static_cast<double>(ReadLE<std::uint64_t>(payload, 9));

    AddQuantity(quantities, "fers_first_board_id", board_id);
    AddQuantity(quantities, "fers_first_trigger_timestamp_ns", timestamp_ns, "ns");
    AddQuantity(quantities, "fers_first_trigger_id", trigger_id);

    AddBranchValue(branches, "fers_first_board_id", board_id);
    AddBranchValue(branches, "fers_first_trigger_timestamp_ns", timestamp_ns);
    AddBranchValue(branches, "fers_first_trigger_id", trigger_id);
  }

}
*/

void HidraFersPayloadDecoder::Decode(const RootDetectorPayload& detector,
                                     std::vector<RootQuantity>& quantities,
                                     RootBranchValues& branches) const {
  HidraGenericPayloadDecoder{}.Decode(detector, quantities, branches);

  const auto& payload = detector.payload; // this is concatenation of all blocks
  if (payload.size() % 699 != 0) {
    HIDRA_ERROR("Unexpected FERS payload size {}. Should be a multiple of 699", payload.size());
  }

  const int FERS_N_CHAN_MAX = 64 * 20;

  std::vector<double> FERStsamp_us(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERSrel_tsamp_us(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERStrigger_id(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERSboard_id(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERShg(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERSlg(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERStoa(FERS_N_CHAN_MAX, -1);
  std::vector<double> FERStot(FERS_N_CHAN_MAX, -1);

  int nboards = payload.size() / 699;
  for (int iboard = 0; iboard < nboards; ++iboard) {
    const auto* block_ptr = payload.data() + iboard * 699;
    FERS_spect_64 boardblock;
    std::memcpy(&boardblock, block_ptr, sizeof(FERS_spect_64));

    if (boardblock.board_id < 0 || boardblock.board_id >= 20) {
      HIDRA_ERROR("FERS block {} has invalid board_id {}. Skipping", iboard, boardblock.board_id);
      continue;
    }
    uint64_t allchan_mask = std::numeric_limits<uint64_t>::max();

    if (boardblock.chmask != allchan_mask) {
      HIDRA_WARN("FERS block {} has chmask {:016X}. Refusing to decode if less then 64 channels are enabled",
                 iboard,
                 boardblock.chmask);
      continue;
    }

    for (int ichan = 0; ichan < 64; ++ichan) {
      int index = boardblock.board_id * 64 + ichan;
      FERStsamp_us[index] = boardblock.tstamp_us;
      FERSrel_tsamp_us[index] = boardblock.rel_tstamp_us;
      FERStrigger_id[index] = static_cast<double>(boardblock.trigger_id);
      FERSboard_id[index] = static_cast<double>(boardblock.board_id);
      FERShg[index] = static_cast<double>(boardblock.energyHG[ichan]);
      FERSlg[index] = static_cast<double>(boardblock.energyLG[ichan]);
      FERStoa[index] = static_cast<double>(boardblock.tstamp[ichan]);
      FERStot[index] = static_cast<double>(boardblock.ToT[ichan]);
    }

  } // loop over board blocks

  AddBranchValues(branches, "FERStsamp_us", FERStsamp_us);
  AddBranchValues(branches, "FERSrel_tsamp_us", FERSrel_tsamp_us);
  AddBranchValues(branches, "FERStrigger_id", FERStrigger_id);
  AddBranchValues(branches, "FERSboard_id", FERSboard_id);
  AddBranchValues(branches, "FERShg", FERShg);
  AddBranchValues(branches, "FERSlg", FERSlg);
  AddBranchValues(branches, "FERStoa", FERStoa);
  AddBranchValues(branches, "FERStot", FERStot);
}

} // namespace hidra
