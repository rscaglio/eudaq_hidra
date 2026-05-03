#ifndef HIDRA_EVENT_SERIALIZER_HH
#define HIDRA_EVENT_SERIALIZER_HH

#include <cstdint>
#include <string>
#include <vector>
#include "eudaq/Event.hh"

namespace hidra {

class EventSerializer {
public:
	static std::vector<std::uint8_t> Serialize(const eudaq::Event &event);

	static void WriteToFile(
		const eudaq::Event &event,
		const std::string &filename
	);
};

} // namespace hidra

#endif
