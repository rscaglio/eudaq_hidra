#pragma once

#include <eudaq/Event.hh>

#include <cstdint>
#include <string>
#include <vector>

namespace hidra {

struct RootQuantity {
  std::string name;
  double value = 0.0;
  std::string unit;
};

struct RootDetectorPayload {
  int det_id = -1;
  std::string producer;
  std::uint64_t trigger_n = 0;
  std::uint64_t event_time_begin = 0;
  std::uint64_t event_time_end = 0;
  std::vector<std::uint8_t> payload;
  std::vector<RootQuantity> quantities;
};

class RootPayloadDecoder {
public:
  virtual ~RootPayloadDecoder() = default;
  virtual bool Matches(const RootDetectorPayload& detector) const = 0;
  virtual void Decode(const RootDetectorPayload& detector, std::vector<RootQuantity>& quantities) const = 0;
};

class HidraXdcPayloadDecoder final : public RootPayloadDecoder {
public:
  bool Matches(const RootDetectorPayload& detector) const override;
  void Decode(const RootDetectorPayload& detector, std::vector<RootQuantity>& quantities) const override;
};

class HidraFersPayloadDecoder final : public RootPayloadDecoder {
public:
  bool Matches(const RootDetectorPayload& detector) const override;
  void Decode(const RootDetectorPayload& detector, std::vector<RootQuantity>& quantities) const override;
};

class HidraGenericPayloadDecoder final : public RootPayloadDecoder {
public:
  bool Matches(const RootDetectorPayload& detector) const override;
  void Decode(const RootDetectorPayload& detector, std::vector<RootQuantity>& quantities) const override;
};

} // namespace hidra