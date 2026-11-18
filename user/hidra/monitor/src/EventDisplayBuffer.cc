#include "EventDisplayBuffer.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

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

EventDisplayBuffer::EventDisplayBuffer(std::size_t capacity, int prescale)
    : IHistogramFiller("event_display"), m_capacity(capacity), m_prescale(prescale),
      m_session(std::to_string(std::chrono::system_clock::now().time_since_epoch().count())) {}

void EventDisplayBuffer::Fill(const HidraEvent& ev) {
  if (m_capacity == 0) {
    return;
  }
  if (m_events.size() == m_capacity) {
    m_events.pop_front();
  }
  m_events.push_back({m_session + "-" + std::to_string(++m_sequence), ev.meta, ev.xdc.ADCvalues,
                      ev.fers.FERShg, ev.fers.FERSlg});
}

void EventDisplayBuffer::Reset() {
  m_events.clear();
}

std::string EventDisplayBuffer::IndexJson() const {
  auto events = nlohmann::json::array();
  for (const auto& ev : m_events) {
    events.push_back(Metadata(ev.id, ev.meta));
  }
  return nlohmann::json({{"capacity", m_capacity}, {"event_prescale", m_prescale}, {"events", events}}).dump();
}

std::string EventDisplayBuffer::EventJson(const std::string& id) const {
  const auto it = std::find_if(m_events.begin(), m_events.end(), [&id](const Snapshot& ev) { return ev.id == id; });
  if (it == m_events.end()) {
    return {};
  }
  auto result = Metadata(it->id, it->meta);
  result["adc"] = Channels(it->adc);
  result["fers_hg"] = Channels(it->fers_hg);
  result["fers_lg"] = Channels(it->fers_lg);
  return result.dump();
}
