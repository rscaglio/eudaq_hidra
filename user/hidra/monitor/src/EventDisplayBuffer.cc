#include "EventDisplayBuffer.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace {
nlohmann::json Metadata(const std::string& id, const HidraEventMeta& meta) {
  return {{"id", id},
          {"run_number", meta.run_number},
          {"event_number", meta.event_number},
          {"trigger_number", meta.trigger_number},
          {"trigger_mask", meta.trigger_mask},
          {"spill_number", meta.spill_number == 0xFFFFFFFF ? nlohmann::json(nullptr) : nlohmann::json(meta.spill_number)},
          // Strings preserve nanosecond precision in the browser's JSON parser.
          {"timestamp_begin_ns", std::to_string(meta.timestamp_begin_ns)},
          {"timestamp_end_ns", std::to_string(meta.timestamp_end_ns)}};
}

nlohmann::json Channels(const std::vector<double>& values) {
  auto result = nlohmann::json::array();
  for (double value : values) {
    // Keep channel indexing and legitimate zeroes; missing decoder sentinels
    // become null, never a false zero signal on the face.
    if (std::isfinite(value) && value >= 0) {
      result.push_back(value);
    } else {
      result.push_back(nullptr);
    }
  }
  return result;
}
} // namespace

EventDisplayBuffer::EventDisplayBuffer(std::size_t capacity, int prescale, double max_hz)
    : m_capacity(capacity), m_prescale(prescale), m_max_hz(max_hz),
      m_interval([max_hz] {
        if (!std::isfinite(max_hz) || max_hz <= 0) {
          throw std::invalid_argument("event display max_hz must be finite and positive");
        }
        // Clamp before converting to the integer clock representation.
        const auto seconds = std::min(1.0 / max_hz, std::chrono::duration<double>(Clock::duration::max()).count() / 2);
        return std::max(Clock::duration(1), std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)));
      }()),
      m_session(std::to_string(std::chrono::system_clock::now().time_since_epoch().count())) {}

bool EventDisplayBuffer::Fill(const HidraEvent& ev, Clock::time_point now) {
  if (m_capacity == 0) {
    return false;
  }
  std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
  if (!lock.owns_lock() || now < m_next_capture) {
    return false;
  }
  // Only sampled events allocate/copy channel arrays. Readers retain immutable
  // shared pointers, so slow HTTP serialization never holds this lock.
  auto snapshot = std::make_shared<const Snapshot>(Snapshot{
      m_session + "-" + std::to_string(++m_sequence), ev.meta, ev.xdc.ADCvalues, ev.fers.FERShg, ev.fers.FERSlg});
  if (m_events.size() == m_capacity) {
    m_events.pop_front();
  }
  m_events.push_back(std::move(snapshot));
  m_next_capture = now + m_interval;
  return true;
}

void EventDisplayBuffer::Reset() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_events.clear();
  m_next_capture = Clock::time_point::min();
}

std::string EventDisplayBuffer::IndexJson() const {
  return Json(false);
}

std::string EventDisplayBuffer::BatchJson() const {
  return Json(true);
}

std::string EventDisplayBuffer::Json(bool include_channels) const {
  std::vector<std::shared_ptr<const Snapshot>> snapshots;
  std::size_t available;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    available = m_events.size();
    const auto count = include_channels ? std::min(available, kMaxBatchSize) : available;
    snapshots.assign(m_events.end() - count, m_events.end());
  }
  auto events = nlohmann::json::array();
  for (const auto& ev : snapshots) {
    auto item = Metadata(ev->id, ev->meta);
    if (include_channels) {
      item["adc"] = Channels(ev->adc);
      item["fers_hg"] = Channels(ev->fers_hg);
      item["fers_lg"] = Channels(ev->fers_lg);
    }
    events.push_back(std::move(item));
  }
  return nlohmann::json({{"capacity", m_capacity}, {"event_prescale", m_prescale},
                         {"capture_max_hz", m_max_hz}, {"available", available}, {"events", std::move(events)}}).dump();
}

std::string EventDisplayBuffer::EventJson(const std::string& id) const {
  std::shared_ptr<const Snapshot> snapshot;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = std::find_if(m_events.begin(), m_events.end(), [&id](const auto& ev) { return ev->id == id; });
    if (it == m_events.end()) {
      return {};
    }
    snapshot = *it;
  }
  auto result = Metadata(snapshot->id, snapshot->meta);
  result["adc"] = Channels(snapshot->adc);
  result["fers_hg"] = Channels(snapshot->fers_hg);
  result["fers_lg"] = Channels(snapshot->fers_lg);
  return result.dump();
}
