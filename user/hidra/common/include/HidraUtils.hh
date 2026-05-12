#ifndef HIDRA_UTILS_HH
#define HIDRA_UTILS_HH

#include <cstdint>
#include <fmt/core.h>

#include <type_traits>
#include <limits>
#include <string>

#include <eudaq/Event.hh>
#include <eudaq/Logger.hh>

namespace hidra::utils {

const std::map<std::string, std::map<std::string, int>> VMESpec{
  {"V792", {{"nchannels", 32}, {"dummy", 0}}},
  {"V792N", {{"nchannels", 16}, {"dummy", 0}}},
  {"V862", {{"nchannels", 32}, {"dummy", 0}}}
};

std::uint64_t getTimeus();
std::uint64_t getTimens();

std::string GetEventInfo(eudaq::Event* ev, int opt = 1);

std::map<std::string, std::string> parseConfigMap(const std::string& configstring);

std::pair<long long, long long> ComputeMeanAndStdDev(const std::vector<long long>& values);


int computeADCchannelFromGeo(const std::map<int, std::string>& vme_geo_map, int geo, int channel);

int computeMaxADCchannelFromGeoMap(const std::map<int, std::string>& vme_geo_map);

template <typename... Args> std::string format(const std::string& fmt_str, Args&&... args) {
#if FMT_VERSION >= 80000
  return fmt::format(fmt::runtime(fmt_str), std::forward<Args>(args)...);
#else
  return fmt::format(fmt_str, std::forward<Args>(args)...);
#endif
}

inline std::string getTagOr(const eudaq::Event& ev, const std::string& tag, const std::string& default_value) {

  if (!ev.HasTag(tag)) {
    EUDAQ_WARN("Returning default value for tag " + tag);
    return default_value;
  }

  return ev.GetTag(tag);
}

template <typename T> T getTagOr(const eudaq::Event& ev, const std::string& tag, T default_value) {
  static_assert(
      std::is_integral<T>::value,
      "getTagOr<T> only supports integral types");

  if (!ev.HasTag(tag)){
    EUDAQ_WARN("Returning default value for tag " + tag);
    return default_value;
  }

  const std::string& s = ev.GetTag(tag);
  try {
    unsigned long long v = std::stoull(s);
    if (v > std::numeric_limits<T>::max()) {
      EUDAQ_WARN("Returning default value for tag " + tag);
      return default_value;
    }
    return static_cast<T>(v);
  } catch (...) {
    EUDAQ_WARN("Returning default value for tag " + tag);
    return default_value;
  }
}

  

} // namespace hidra::utils

#define HIDRA_DEBUG(fmt, ...) EUDAQ_DEBUG(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_INFO(fmt, ...) EUDAQ_INFO(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_WARN(fmt, ...) EUDAQ_WARN(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_ERROR(fmt, ...) EUDAQ_ERROR(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_THROW(fmt, ...) EUDAQ_THROW(hidra::utils::format(fmt, ##__VA_ARGS__))

#endif
