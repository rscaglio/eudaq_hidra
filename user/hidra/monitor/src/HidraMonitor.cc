#include "HidraMonitor.hh"

#include "eudaq/Event.hh"
#include "eudaq/Factory.hh"
#include "eudaq/Logger.hh"
#include "HidraUtils.hh"

const uint32_t HidraMonitor::m_id_factory = eudaq::cstr2hash("HidraMonitor");

namespace {
auto reg = eudaq::Factory<eudaq::Monitor>::Register<HidraMonitor, const std::string&, const std::string&>(
  HidraMonitor::m_id_factory);
}

HidraMonitor::HidraMonitor(const std::string& name, const std::string& runcontrol)
    : eudaq::Monitor(name, runcontrol) {}

void HidraMonitor::DoInitialise() {
  m_histo_counter = new TH1I("histo_counter", "Event Counter", 1, 0, 1);
}

void HidraMonitor::DoConfigure() {
  HIDRA_INFO("HidraMonitor configured");
}

void HidraMonitor::DoStartRun() {}

void HidraMonitor::DoStopRun() {

  HIDRA_INFO("Number of events received: {}", m_histo_counter->GetBinContent(1));

}

void HidraMonitor::DoReset() {}

void HidraMonitor::DoTerminate() {}

void HidraMonitor::DoStatus() {}

void HidraMonitor::DoReceive(eudaq::EventSP ev) {
  HIDRA_INFO("HidraMonitor received event: {}", hidra::utils::GetEventInfo(ev.get()));
  m_histo_counter->Fill(0);
}