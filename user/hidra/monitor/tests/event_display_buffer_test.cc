#include "EventDisplayBuffer.hh"

#include <nlohmann/json.hpp>

#include <limits>
#include <chrono>
#include <iostream>
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
  using namespace std::chrono;
  const auto now = EventDisplayBuffer::Clock::now();
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
  Check(buffer.Fill(event, now), "first event was not sampled");
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
  Check(!buffer.Fill(event, now + microseconds(100)), "display rate cap ignored");
  Check(buffer.Fill(event, now + milliseconds(50)), "sample at next deadline missing");
  Check(nlohmann::json::parse(buffer.EventJson(first_id))["adc"][0] == 0, "stored event was not copied");
  event.meta.event_number = 102;
  buffer.Fill(event, now + milliseconds(100));
  const auto full = nlohmann::json::parse(buffer.IndexJson());
  Check(full["events"].size() == 2 && full["events"][0]["event_number"] == 101, "oldest event not evicted");
  Check(buffer.EventJson(first_id).empty(), "evicted event still available");
  const std::string last_id = full["events"][1]["id"];
  const auto frozen = nlohmann::json::parse(buffer.BatchJson());
  Check(frozen["events"].size() == 2 && frozen["events"][0]["adc"][0] == 999, "batch missing channel payloads");
  buffer.Reset();
  Check(nlohmann::json::parse(buffer.IndexJson())["events"].empty(), "reset retained old events");
  Check(buffer.EventJson(last_id).empty(), "reset event still fetchable");
  Check(frozen["events"][1]["id"] == last_id, "reset invalidated a downloaded batch");
  buffer.Fill(event, now + milliseconds(101));
  Check(nlohmann::json::parse(buffer.IndexJson())["events"][0]["id"] != last_id, "reset reused event identity");

  EventDisplayBuffer disabled(0, 1);
  disabled.Fill(event);
  Check(nlohmann::json::parse(disabled.IndexJson())["events"].empty(), "disabled buffer captured data");

  // Ten virtual seconds at 10 kHz: rate limiting must happen BEFORE copying.
  EventDisplayBuffer high_rate(100, 1);
  event.xdc.ADCvalues.assign(224, 123);
  event.fers.FERShg.assign(1280, 456);
  event.fers.FERSlg.assign(1280, 78);
  int captured = 0;
  const auto started = EventDisplayBuffer::Clock::now();
  for (int i = 0; i < 100000; ++i) {
    event.meta.event_number = i;
    event.xdc.ADCvalues[1] = i;
    captured += high_rate.Fill(event, now + microseconds(100 * i));
  }
  Check(captured == 200, "10 kHz input did not respect the independent 20 Hz cap");
  const auto sampled = nlohmann::json::parse(high_rate.BatchJson());
  Check(sampled["events"].size() == 100 && sampled["capture_max_hz"] == 20, "retention or sampling metadata wrong");
  Check(sampled["events"].front()["event_number"] == 50000, "wrong oldest retained sample");
  Check(sampled["events"].back()["event_number"] == 99500, "wrong newest retained sample");
  for (const auto& ev : sampled["events"]) {
    Check(ev["adc"][1] == ev["event_number"], "metadata and channel values mixed");
  }
  std::cout << "10 kHz virtual-clock test: 100000 candidates, " << captured
            << " copies, 100 retained; processing "
            << duration<double, std::milli>(EventDisplayBuffer::Clock::now() - started).count() << " ms\n";

  EventDisplayBuffer oversized(150, 1, 1000);
  for (int i = 0; i < 150; ++i) {
    event.meta.event_number = i;
    oversized.Fill(event, now + milliseconds(i));
  }
  const auto bounded = nlohmann::json::parse(oversized.BatchJson());
  Check(bounded["available"] == 150 && bounded["events"].size() == 100, "HTTP batch memory bound ignored");
  Check(bounded["events"][0]["event_number"] == 50, "bounded batch is not the most recent group");
  for (double hz : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    bool rejected = false;
    try {
      EventDisplayBuffer invalid(100, 1, hz);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "invalid rate silently disabled display throttling");
  }
}
