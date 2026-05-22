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
    : m_xdc_decoder(std::move(vme_geo_map)) {}

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

  HidraXdcEvent xdc_event;
  m_xdc_decoder.decode(detector.payload, xdc_event);

  if (xdc_event.TDCvalues.empty()) {
    return;
  }

  AddBranchValues(branches, "ADCs", xdc_event.ADCvalues);
  AddBranchValues(branches, "ADCFlags", xdc_event.ADCflags);
  AddBranchValues(branches, "TDCs", xdc_event.TDCvalues);
  AddBranchValues(branches, "TDCFlags", xdc_event.TDCflags);
}

bool HidraFersPayloadDecoder::Matches(const RootDetectorPayload& detector) const {
  // TODO: temporary enabling only det_id 2 to abvoid using current decoder for Dry runs on 2025 data
  // return detector.det_id == 2 || detector.det_id == 7 || detector.producer.find("FERS") != std::string::npos;
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

void HidraFersPayloadDecoder::Decode(const RootDetectorPayload& detector,
                                     std::vector<RootQuantity>& quantities,
                                     RootBranchValues& branches) const {
  HidraGenericPayloadDecoder{}.Decode(detector, quantities, branches);

  HidraFersEvent fers_event;
  m_fers_decoder.decode(detector.payload, fers_event);

  AddBranchValues(branches, "FERStsamp_us", fers_event.FERStsamp_us);
  AddBranchValues(branches, "FERSrel_tsamp_us", fers_event.FERSrel_tsamp_us);
  AddBranchValues(branches, "FERStrigger_id", fers_event.FERStrigger_id);
  AddBranchValues(branches, "FERSboard_id", fers_event.FERSboard_id);
  AddBranchValues(branches, "FERShg", fers_event.FERShg);
  AddBranchValues(branches, "FERSlg", fers_event.FERSlg);
  AddBranchValues(branches, "FERStoa", fers_event.FERStoa);
  AddBranchValues(branches, "FERStot", fers_event.FERStot);
}

} // namespace hidra
