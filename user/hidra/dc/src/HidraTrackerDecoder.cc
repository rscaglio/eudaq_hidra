#include "HidraTrackerDecoder.hh"
#include "HidraUtils.hh"

#include <cstring>

namespace hidra {

HidraTrackerDecoder::HidraTrackerDecoder() = default;

void HidraTrackerDecoder::decode(const std::vector<uint8_t>& payload, HidraTrackerEvent& event,
                                 std::uint64_t trigger_n) const {
  event = HidraTrackerEvent{};
  event.trigger_n = trigger_n;

  const auto payload_size = payload.size();

  if (payload_size == 0) {
    HIDRA_ERROR("Tracker payload is empty. Aborting");
    return;
  }

  if (payload_size % sizeof(std::uint32_t) != 0) {
    HIDRA_ERROR("Tracker payload size {} is not a multiple of 4 bytes. Aborting", payload_size);
    return;
  }
  const std::size_t word_count = payload_size / sizeof(std::uint32_t);

  if (word_count % kValuesPerPlane != 0) {
    HIDRA_ERROR("Tracker payload has {} words, not a whole number of (x, y) plane pairs. Aborting", word_count);
    return;
  }

  std::vector<std::uint32_t> words(word_count);
  std::memcpy(words.data(), payload.data(), word_count * sizeof(std::uint32_t));

  const std::size_t n_planes = word_count / kValuesPerPlane;
  std::vector<double> X(n_planes, -1);
  std::vector<double> Y(n_planes, -1);

  for (std::size_t plane = 0; plane < n_planes; ++plane) {
    X[plane] = static_cast<double>(words[plane * kValuesPerPlane + 0]);
    Y[plane] = static_cast<double>(words[plane * kValuesPerPlane + 1]);
  }

  event.X = std::move(X);
  event.Y = std::move(Y);
}

} // namespace hidra
