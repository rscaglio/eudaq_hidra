#pragma once

#include <eudaq/Monitor.hh>
#include <TH1I.h>

class HidraMonitor : public eudaq::Monitor {
public:
  HidraMonitor(const std::string& name, const std::string& runcontrol);
  ~HidraMonitor() override = default;
  void DoInitialise() override;
  void DoConfigure() override;
  void DoStartRun() override;
  void DoStopRun() override;
  void DoReset() override;
  void DoTerminate() override;
  void DoStatus() override;
  void DoReceive(eudaq::EventSP ev) override;

  static const uint32_t m_id_factory;

private:
  TH1I* m_histo_counter;
};