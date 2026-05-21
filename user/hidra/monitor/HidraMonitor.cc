#include "HidraMonitor.hh"

HidraMonitor::HidraMonitor(const std::string& name, const std::string& runcontrol)
    : eudaq::Monitor(name, runcontrol) {}

HidraMonitor::DoInitialise() {
  m_histo_counter = new TH1I("histo_counter", "Event Counter", 1, 0, 1);
}

void HidraMonitor::DoConfigure() {}

void HidraMonitor::DoStartRun() {}

void HidraMonitor::DoStopRun() {}

void HidraMonitor::DoReset() {}

void HidraMonitor::DoTerminate() {}

void HidraMonitor::DoStatus() {}

void HidraMonitor::DoReceive(eudaq::EventSP ev) {
  m_histo_counter->Fill(0);
}