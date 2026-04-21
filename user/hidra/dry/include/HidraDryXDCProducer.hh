#ifndef HIDRA_DRY_XDC_PRODUCER_HH
#define HIDRA_DRY_XDC_PRODUCER_HH

#include "eudaq/Producer.hh"

#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

class HidraDryXDCProducer : public eudaq::Producer {
public:
  HidraDryXDCProducer(const std::string & name, const std::string & runcontrol);

  void DoInitialise() override;
  void DoConfigure() override;
  void DoStartRun() override;
  void DoStopRun() override;
  void DoTerminate() override;
  void DoReset() override;
  void ReadFileSize();
  bool ReadXDCEvent(std::vector<uint32_t> &event_words);
  void Mainloop();

  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraDryXDCProducer");

private:
  std::ifstream m_ifile;
  uint64_t m_ifile_size;
  std::string m_data_in_path;
  uint32_t m_event_delay_ms;
  std::thread m_thd_run;
  mutable bool m_exit_of_run;
};

#endif
