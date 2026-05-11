#include "HidraRootPayloadDecoders.hh"

#include <algorithm>
#include <cstring>

namespace hidra {

namespace {

template <typename T> T ReadLE(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
  T value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(T));
  return value;
}

void AddQuantity(std::vector<RootQuantity>& quantities, std::string name, double value, std::string unit = "") {
  quantities.push_back(RootQuantity{std::move(name), value, std::move(unit)});
}

} // namespace

bool HidraGenericPayloadDecoder::Matches(const RootDetectorPayload&) const { return true; }

void HidraGenericPayloadDecoder::Decode(const RootDetectorPayload& detector, std::vector<RootQuantity>& quantities) const {
  AddQuantity(quantities, "payload_bytes", static_cast<double>(detector.payload.size()), "B");
  const auto span_ns = (detector.event_time_end >= detector.event_time_begin)
                           ? static_cast<double>(detector.event_time_end - detector.event_time_begin)
                           : 0.0;
  AddQuantity(quantities, "timestamp_span", span_ns, "ns");
}

bool HidraXdcPayloadDecoder::Matches(const RootDetectorPayload& detector) const {
  return detector.det_id == 1 || detector.det_id == 7;
}

void HidraXdcPayloadDecoder::Decode(const RootDetectorPayload& detector, std::vector<RootQuantity>& quantities) const {
  HidraGenericPayloadDecoder{}.Decode(detector, quantities);

  const auto payload_size = detector.payload.size();
  if (payload_size % 4 != 0) {
    AddQuantity(quantities, "trailing_payload_bytes", static_cast<double>(payload_size % 4), "B");
  }

  const std::size_t word_count = payload_size / 4;
  AddQuantity(quantities, "xdc_data_words", static_cast<double>(word_count), "words");

  if (word_count == 0) {
    return;
  }

  std::vector<std::uint32_t> words(word_count);
  std::memcpy(words.data(), detector.payload.data(), word_count * sizeof(std::uint32_t));
  AddQuantity(quantities, "xdc_first_word", static_cast<double>(words.front()));
  AddQuantity(quantities, "xdc_last_word", static_cast<double>(words.back()));
  AddQuantity(quantities, "xdc_word_min", static_cast<double>(*std::min_element(words.begin(), words.end())));
  AddQuantity(quantities, "xdc_word_max", static_cast<double>(*std::max_element(words.begin(), words.end())));
}

bool HidraFersPayloadDecoder::Matches(const RootDetectorPayload& detector) const {
  return detector.det_id == 0 || detector.det_id == 6 || detector.producer.find("FERS") != std::string::npos;
}

void HidraFersPayloadDecoder::Decode(const RootDetectorPayload& detector, std::vector<RootQuantity>& quantities) const {
  HidraGenericPayloadDecoder{}.Decode(detector, quantities);

  const auto& payload = detector.payload;

  if (payload.size() >= 56) {
    AddQuantity(quantities, "fers2_tstamp_us", ReadLE<double>(payload, 0), "us");
    AddQuantity(quantities, "fers2_rel_tstamp_us", ReadLE<double>(payload, 8), "us");
    AddQuantity(quantities, "fers2_trigger_id", static_cast<double>(ReadLE<std::uint64_t>(payload, 32)));
    AddQuantity(quantities, "fers2_chmask", static_cast<double>(ReadLE<std::uint64_t>(payload, 40)));
    AddQuantity(quantities, "fers2_qdmask", static_cast<double>(ReadLE<std::uint64_t>(payload, 48)));
  }

  if (payload.size() >= 56 + 64 * 2) {
    std::uint64_t energy_sum = 0;
    std::uint16_t energy_max = 0;
    for (std::size_t index = 0; index < 64; ++index) {
      const auto value = ReadLE<std::uint16_t>(payload, 56 + index * sizeof(std::uint16_t));
      energy_sum += value;
      energy_max = std::max(energy_max, value);
    }
    AddQuantity(quantities, "fers2_energy_hg_sum", static_cast<double>(energy_sum));
    AddQuantity(quantities, "fers2_energy_hg_max", static_cast<double>(energy_max));
  }

  if (payload.size() >= 17 && payload.size() < 56) {
    AddQuantity(quantities, "fers_first_board_id", static_cast<double>(payload[0]));
    AddQuantity(quantities, "fers_first_trigger_timestamp_ns", ReadLE<double>(payload, 1) * 1000.0, "ns");
    AddQuantity(quantities, "fers_first_trigger_id", static_cast<double>(ReadLE<std::uint64_t>(payload, 9)));
  }
}

} // namespace hidra