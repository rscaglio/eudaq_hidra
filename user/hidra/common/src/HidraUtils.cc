#include "HidraUtils.hh"

#include <chrono>
#include <sstream>

namespace hidra::utils {

std::uint64_t getTimeus() {
  const auto now = std::chrono::system_clock::now();
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
  return static_cast<std::uint64_t>(us.count());
}

std::uint64_t getTimens() {
  const auto now = std::chrono::system_clock::now();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
  return static_cast<std::uint64_t>(ns.count());
}

std::string GetEventInfo(eudaq::Event* ev, int opt) {

  std::string info = "Event Info:";

  if (opt == 1) { // default

    if (ev->IsBORE() || ev->IsEORE()) {
      info += "BORE runN " + std::to_string(ev->GetRunN());
    } else if (ev->IsEORE()) {
      info += "EORE runN " + std::to_string(ev->GetRunN());
    } else {
      info += " evtN " + std::to_string(ev->GetEventN());
      info += " trgN " + std::to_string(ev->GetTriggerN());
      info += " start/stop " + std::to_string(ev->GetTimestampBegin()) + "/" + std::to_string(ev->GetTimestampEnd());
      info += " nblk " + std::to_string(ev->GetNumBlock());
      info += " totB " + ev->GetTag("eventWords");
    }
  }

  if (opt == 2) {
    info += " n_source " + ev->GetTag("N_SOURCES");
    info += " trig " + std::to_string(ev->GetTriggerN());
    info += " ts " + std::to_string(ev->GetTimestampBegin());
    info += " -- (s/tg/ev/ts) ";
    for (int isub = 0; isub < ev->GetNumSubEvent(); isub++) {
      info += "(" + ev->GetSubEvent(isub)->GetTag("Producer") + "/" +
              std::to_string(ev->GetSubEvent(isub)->GetTriggerN()) + "/" +
              std::to_string(ev->GetSubEvent(isub)->GetEventN()) + "/" +
              std::to_string(ev->GetSubEvent(isub)->GetTimestampBegin()) + ")";
    }

  }

  else {
    info += "PLEASE SPECIFY A VALID OPTION";
  }

  return info;
}

} // namespace hidra::utils
