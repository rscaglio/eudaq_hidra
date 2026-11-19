#include "MonitorHttpServer.hh"
#include "EventDisplayBuffer.hh"

#include <string>
#include <stdexcept>

MonitorHttpServer::MonitorHttpServer(const char* spec, EventDisplayBuffer* events)
    : THttpServer(spec), m_events(events) {
  // The publisher owns the only pump. Disable ROOT's application-loop timer.
  SetTimer(0);
}

void MonitorHttpServer::ProcessRequestsWithLock(std::unique_lock<std::mutex>& lock) {
  if (!lock.owns_lock()) {
    throw std::logic_error("HTTP pump requires the histogram lock");
  }
  m_pump_lock = &lock;
  struct Clear {
    std::unique_lock<std::mutex>*& current;
    ~Clear() { current = nullptr; }
  } clear{m_pump_lock};
  ProcessRequests();
}

void MonitorHttpServer::ProcessRequest(std::shared_ptr<THttpCallArg> arg) {
  std::string path = arg->GetPathName();
  if (!path.empty() && path.front() == '/') {
    path.erase(0, 1);
  }
  if (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  if (path != "EventDisplay" || !m_events) {
    // Do not release the histogram lock in a nested ROOT batch's call stack.
    struct RootScope {
      unsigned int& depth;
      explicit RootScope(unsigned int& d) : depth(d) { ++depth; }
      ~RootScope() { --depth; }
    } root_scope(m_root_depth);
    THttpServer::ProcessRequest(std::move(arg));
    return;
  }

  const std::string file = arg->GetFileName();
  const auto encode = [&]() -> std::string {
    if (file == "batch.json") {
      return m_events->BatchJson();
    }
    if (file == "events.json") {
      return m_events->IndexJson();
    }
    if (file.size() > 5 && file.compare(file.size() - 5, 5, ".json") == 0) {
      return m_events->EventJson(file.substr(0, file.size() - 5));
    }
    return {};
  };
  std::string json;
  if (m_pump_lock && m_root_depth == 0) {
    struct Unlock {
      std::unique_lock<std::mutex>& lock;
      explicit Unlock(std::unique_lock<std::mutex>& l) : lock(l) { lock.unlock(); }
      ~Unlock() { lock.lock(); }
    } unlocked(*m_pump_lock);
    // The buffer retains immutable shared pointers briefly under its own lock.
    // JSON serialization holds neither the buffer lock nor the histogram lock.
    json = encode();
  } else {
    json = encode();
  }
  // All THttpCallArg/ROOT bookkeeping happens after reacquiring the ROOT lock.
  if (json.empty()) {
    arg->Set404();
  } else {
    arg->SetContentType("application/json");
    arg->SetContent(std::move(json));
  }
  arg->AddNoCacheHeader();
}
