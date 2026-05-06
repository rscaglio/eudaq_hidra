#ifndef HIDRA_EVENT_SERIALIZER_HH
#define HIDRA_EVENT_SERIALIZER_HH

#include "eudaq/Event.hh"
#include <cstdint>
#include <string>
#include <vector>

namespace hidra {

class EventSerializer {
public:
  static std::vector<std::uint8_t> Serialize(const eudaq::Event& event);

  static void WriteToFile(const eudaq::Event& event, const std::string& filename);
  static void WriteToStream(const eudaq::Event& event, std::ostream& out);
};

} // namespace hidra

#endif
