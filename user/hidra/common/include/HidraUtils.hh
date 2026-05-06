#ifndef HIDRA_UTILS_HH
#define HIDRA_UTILS_HH


#include <cstdint>
#include <fmt/core.h>

#include <eudaq/Logger.hh>
#include <eudaq/Event.hh>




namespace hidra::utils {

  
						   
  std::uint64_t getTimeus();
  std::uint64_t getTimens();

  template <typename... Args>
  std::string format(const std::string &fmt_str, Args&&... args){
    return fmt::format(fmt::runtime(fmt_str), std::forward<Args>(args)...);
  }

  template <typename T>
  T getTagOr(const eudaq::Event &ev, const std::string &tag, T default_value){
    
    static_assert(std::is_integral<T>::value, "getTagOr only supports integral types");

    if (!ev.HasTag(tag)) {
      EUDAQ_WARN("Returning default value for tag "+tag);
      return default_value;
    }
    
    const std::string &s = ev.GetTag(tag);
      
    try {
      unsigned long long v = std::stoull(s);

      if (v > std::numeric_limits<T>::max()) {
	EUDAQ_WARN("Returning default value for tag "+tag);
	return default_value;
      }
      
      return static_cast<T>(v);
    }
    catch (...) {
      EUDAQ_WARN("Returning default value for tag "+tag);
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
