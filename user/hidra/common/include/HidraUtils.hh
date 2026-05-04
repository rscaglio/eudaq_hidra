#ifndef HIDRA_UTILS_HH
#define HIDRA_UTILS_HH


#include <cstdint>
#include <fmt/core.h>

#include <eudaq/Logger.hh>




namespace hidra::utils {

  inline static std::map<std::string, int> Producers{
						     {"HidraQTPDProducer", 0},
						     {"HidraFERSProducer", 1},
						     {"CrystalProducer", 2},
						     {"TrackerProducer", 3},
						     // 16 slots max
						     {"DryXDCProducer", 14},
						     {"DryFERSProducer", 15}
  };
						    
						     

  std::uint64_t getTimeus();
  std::uint64_t getTimens();

  template <typename... Args>
  std::string format(const std::string &fmt_str, Args&&... args){
    return fmt::format(fmt_str, std::forward<Args>(args)...);
  }

} // namespace hidra::utils

#define HIDRA_DEBUG(fmt, ...) EUDAQ_DEBUG(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_INFO(fmt, ...) EUDAQ_INFO(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_WARN(fmt, ...) EUDAQ_WARN(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_ERROR(fmt, ...) EUDAQ_ERROR(hidra::utils::format(fmt, ##__VA_ARGS__))
#define HIDRA_THROW(fmt, ...) EUDAQ_THROW(hidra::utils::format(fmt, ##__VA_ARGS__))


#endif
