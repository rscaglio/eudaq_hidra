#pragma once

#include "HidraTrackerEvent.hh"

#include <cstdint>
#include <vector>

namespace hidra {

// Decoder for the HidraTrackerProducer block payload.
//
// Mirrors HidraXdcDecoder / HidraFersDecoder: it takes the raw little-endian
// byte payload of a tracker sub-event and fills a HidraTrackerEvent. The
// payload is a flat array of uint32 words, two per plane (x then y), exactly
// the `coordinates` block built in HidraTrackerProducer::SendRow.
//
// The number of planes is inferred from the payload length, so the same
// decoder keeps working if the provisional tracker format grows or shrinks the
// number of planes (it only requires a whole number of (x, y) pairs).
class HidraTrackerDecoder {
public:
  HidraTrackerDecoder();
  void decode(const std::vector<uint8_t>& payload, HidraTrackerEvent& event, std::uint64_t trigger_n) const;

  // Words per plane in the payload: one x and one y.
  static constexpr int kValuesPerPlane = 2;
};

} // namespace hidra
