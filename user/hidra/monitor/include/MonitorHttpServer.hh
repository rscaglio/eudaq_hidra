#pragma once

#include <THttpServer.h>

#include <memory>
#include <mutex>

class EventDisplayBuffer;

/** ROOT requests stay locked; standalone ADC/FERS JSON encoding runs unlocked. */
class MonitorHttpServer : public THttpServer {
public:
  MonitorHttpServer(const char* spec, EventDisplayBuffer* events);
  /** Called only by the publisher pump, with its histogram lock held. */
  void ProcessRequestsWithLock(std::unique_lock<std::mutex>& lock);

protected:
  void ProcessRequest(std::shared_ptr<THttpCallArg> arg) override;

private:
  EventDisplayBuffer* m_events;
  std::unique_lock<std::mutex>* m_pump_lock{nullptr};
  unsigned int m_root_depth{0};
};
