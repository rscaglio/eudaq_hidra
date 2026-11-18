#include "EventDisplayBuffer.hh"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
} // namespace

int main() {
  EventDisplayBuffer buffer(2, 3);
  HidraEvent event;
  event.meta.run_number = 42;
  event.meta.event_number = 100;
  event.meta.timestamp_begin_ns = 1795000000123456789ULL;
  event.xdc.ADCvalues = {0, -1, 123, std::numeric_limits<double>::quiet_NaN()};
  event.fers.FERShg = {12, 0, -1};
  event.fers.FERSlg = {4, 5};
  event.maxicc.FERShg = {999};
  event.tracker.X = {12}; // Tracker/MAXICC must never enter the snapshot.
  buffer.Fill(event);
  const auto index = nlohmann::json::parse(buffer.IndexJson());
  const std::string first_id = index["events"][0]["id"];
  Check(index["capacity"] == 2 && index["event_prescale"] == 3, "buffer configuration missing");
  auto snapshot = nlohmann::json::parse(buffer.EventJson(first_id));
  Check(snapshot["adc"] == nlohmann::json({0, nullptr, 123, nullptr}), "zero/sentinel channel indexing changed");
  Check(snapshot["fers_hg"] == nlohmann::json({12, 0, nullptr}), "FERS HG indexing changed");
  Check(snapshot["fers_lg"] == nlohmann::json({4, 5}), "FERS gains mixed");
  Check(snapshot["timestamp_begin_ns"] == "1795000000123456789", "timestamp precision lost");
  Check(snapshot["spill_number"].is_null(), "missing spill sentinel exposed as a value");
  Check(!snapshot.contains("maxicc") && !snapshot.contains("tracker"), "unrequested detector data exposed");

  event.xdc.ADCvalues[0] = 999;
  event.meta.event_number = 101;
  buffer.Fill(event);
  Check(nlohmann::json::parse(buffer.EventJson(first_id))["adc"][0] == 0, "stored event was not copied");
  event.meta.event_number = 102;
  buffer.Fill(event);
  const auto full = nlohmann::json::parse(buffer.IndexJson());
  Check(full["events"].size() == 2 && full["events"][0]["event_number"] == 101, "oldest event not evicted");
  Check(buffer.EventJson(first_id).empty(), "evicted event still available");
  const std::string last_id = full["events"][1]["id"];
  buffer.Reset();
  Check(nlohmann::json::parse(buffer.IndexJson())["events"].empty(), "reset retained old events");
  Check(buffer.EventJson(last_id).empty(), "reset event still fetchable");
  buffer.Fill(event);
  Check(nlohmann::json::parse(buffer.IndexJson())["events"][0]["id"] != last_id, "reset reused event identity");

  EventDisplayBuffer disabled(0, 1);
  disabled.Fill(event);
  Check(nlohmann::json::parse(disabled.IndexJson())["events"].empty(), "disabled buffer captured data");
}
