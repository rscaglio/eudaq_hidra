#pragma once

#include "HidraEvent.hh"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/** Independently sampled, immutable ADC/FERS snapshots. No ROOT/histogram lock. */
class EventDisplayBuffer {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t kMaxBatchSize = 100;
  explicit EventDisplayBuffer(std::size_t capacity, int prescale, double max_hz = 20.0);

  /** At most max_hz copies/second, with no waiting on readers. Injectable time for tests. */
  bool Fill(const HidraEvent& ev, Clock::time_point now = Clock::now());
  void Reset();

  std::string IndexJson() const;
  /** Atomically retain a bounded batch before serializing outside the buffer lock. */
  std::string BatchJson() const;
  /** Empty string means the event was evicted or the buffer was reset. */
  std::string EventJson(const std::string& id) const;

private:
  struct Snapshot {
    std::string id;
    HidraEventMeta meta;
    std::vector<double> adc;
    std::vector<double> fers_hg;
    std::vector<double> fers_lg;
  };

  std::string Json(bool include_channels) const;

  const std::size_t m_capacity;
  const int m_prescale;
  const double m_max_hz;
  const Clock::duration m_interval;
  mutable std::mutex m_mutex;
  Clock::time_point m_next_capture{Clock::time_point::min()};
  std::string m_session;
  uint64_t m_sequence{0}; // Never reused across run/reset boundaries.
  std::deque<std::shared_ptr<const Snapshot>> m_events;
};
