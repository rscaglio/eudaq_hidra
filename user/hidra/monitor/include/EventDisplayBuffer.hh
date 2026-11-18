#pragma once

#include "IHistogramFiller.hh"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

/** Bounded, non-cumulative ADC/FERS snapshots. All access holds publisher.Mutex(). */
class EventDisplayBuffer : public IHistogramFiller {
public:
  explicit EventDisplayBuffer(std::size_t capacity, int prescale);

  void Fill(const HidraEvent& ev) override;
  void Reset() override;

  std::string IndexJson() const;
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

  std::size_t m_capacity;
  int m_prescale;
  std::string m_session;
  uint64_t m_sequence{0}; // Never reused across run/reset boundaries.
  std::deque<Snapshot> m_events;
};
