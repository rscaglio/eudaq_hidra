#pragma once

#include "HidraFersEvent.hh"

#include <vector>
#include <cstdint>
#include <string>

namespace hidra {

class HidraFersDecoder {
public:
  HidraFersDecoder();
  void decode(const std::vector<uint8_t>& payload, HidraFersEvent& event) const;
};

} // namespace hidra
