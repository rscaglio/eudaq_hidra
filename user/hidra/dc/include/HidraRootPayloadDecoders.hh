#pragma once

#include <eudaq/Event.hh>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hidra {

struct RootQuantity {
  std::string name;
  double value = 0.0;
  std::string unit;
};

using RootBranchValues = std::map<std::string, std::vector<double>>;

struct RootDetectorPayload {
  int det_id = -1;
  std::string producer;
  std::uint64_t trigger_n = 0;
  std::uint64_t event_time_begin = 0;
  std::uint64_t event_time_end = 0;
  std::vector<std::uint8_t> payload;
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
};

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

struct FERS_spect_64 {

  uint16_t block_size;
  uint8_t board_id;
  double tstamp_us;
  double rel_tstamp_us;
  uint64_t tstamp_clk;
  uint64_t Tref_tstamp;
  uint64_t trigger_id;
  uint64_t chmask;
  uint64_t qdmask;
  std::array<uint16_t, 64> energyHG;
  std::array<uint16_t, 64> energyLG;
  std::array<uint32_t, 64> tstamp;
  std::array<uint16_t, 64> ToT;
};

class RootPayloadDecoder {
public:
  virtual ~RootPayloadDecoder() = default;
  virtual bool Matches(const RootDetectorPayload& detector) const = 0;
  virtual void Decode(const RootDetectorPayload& detector,
                      std::vector<RootQuantity>& quantities,
                      RootBranchValues& branches) const = 0;
  virtual std::vector<std::string> BranchNames() const;
};

class HidraXdcPayloadDecoder final : public RootPayloadDecoder {
public:
  explicit HidraXdcPayloadDecoder(std::map<int, std::string> vme_geo_map = {});
  bool Matches(const RootDetectorPayload& detector) const override;
  void Decode(const RootDetectorPayload& detector,
              std::vector<RootQuantity>& quantities,
              RootBranchValues& branches) const override;
  std::vector<std::string> BranchNames() const override;

private:
  std::map<int, std::string> m_vme_geo_map;
};

class HidraFersPayloadDecoder final : public RootPayloadDecoder {
public:
  bool Matches(const RootDetectorPayload& detector) const override;
  void Decode(const RootDetectorPayload& detector,
              std::vector<RootQuantity>& quantities,
              RootBranchValues& branches) const override;
  std::vector<std::string> BranchNames() const override;
};

class HidraGenericPayloadDecoder final : public RootPayloadDecoder {
public:
  bool Matches(const RootDetectorPayload& detector) const override;
  void Decode(const RootDetectorPayload& detector,
              std::vector<RootQuantity>& quantities,
              RootBranchValues& branches) const override;
  std::vector<std::string> BranchNames() const override;
};

} // namespace hidra
