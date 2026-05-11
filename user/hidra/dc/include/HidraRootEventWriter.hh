#pragma once

#include <eudaq/Event.hh>

#include <cstdint>
#include <memory>
#include <string>

namespace hidra {

class HidraRootEventWriter {
public:
  HidraRootEventWriter(const std::string& output_file, std::uint64_t flush_interval_ms = 50,
                       std::size_t flush_every_events = 32);
  ~HidraRootEventWriter();

  void Start();
  void Stop();

  bool EnqueueEvent(eudaq::EventSP event);

  bool IsActive() const;
  bool HasError() const;
  std::string GetLastError() const;
  std::uint64_t GetWrittenEventCount() const;
  std::size_t GetPendingEventCount() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace hidra