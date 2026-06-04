#pragma once

#include "HidraFersEvent.hh"
#include "IFersDecoder.hh"

#include <vector>
#include <cstdint>
#include <string>

namespace hidra {

class HidraFersDecoder : public IFersDecoder {
public:
  HidraFersDecoder();
  void decode(const std::vector<uint8_t>& payload, HidraFersEvent& event) const override;
};

} // namespace hidra
