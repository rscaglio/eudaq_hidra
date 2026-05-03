#include "EventSerializer.hh"

#include <fstream>
#include <stdexcept>

namespace hidra {

std::vector<std::uint8_t> EventSerializer::Serialize(const eudaq::Event &event)
{
	std::vector<std::uint8_t> buffer;

	const auto event_number = event.GetEventN();

	buffer.push_back(static_cast<std::uint8_t>((event_number >> 0)  & 0xFF));
	buffer.push_back(static_cast<std::uint8_t>((event_number >> 8)  & 0xFF));
	buffer.push_back(static_cast<std::uint8_t>((event_number >> 16) & 0xFF));
	buffer.push_back(static_cast<std::uint8_t>((event_number >> 24) & 0xFF));

	return buffer;
}

void EventSerializer::WriteToFile(
	const eudaq::Event &event,
	const std::string &filename
)
{
	const auto buffer = Serialize(event);

	std::ofstream output(filename, std::ios::binary);
	if(!output) {
		throw std::runtime_error("Cannot open output file: " + filename);
	}

	output.write(
		reinterpret_cast<const char *>(buffer.data()),
		static_cast<std::streamsize>(buffer.size())
	);
}

} // namespace hidra
