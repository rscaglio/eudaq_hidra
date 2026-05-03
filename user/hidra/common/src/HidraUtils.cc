#include "HidraUtils.hh"

#include <sstream>
#include <chrono>

namespace hidra::utils {

  std::uint64_t getTimeus(){
    const auto now = std::chrono::system_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
    return static_cast<std::uint64_t>(us.count());
  }

  std::uint64_t getTimens(){
    const auto now = std::chrono::system_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
    return static_cast<std::uint64_t>(ns.count());
  }



} // namespace hidra::utils
