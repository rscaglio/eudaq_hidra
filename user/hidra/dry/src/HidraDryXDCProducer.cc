#include "../include/HidraDryXDCProducer.hh"
#include <chrono>
#include <cstring>

namespace {
  constexpr uint32_t XDC_EVENT_MARKER = 0xccaaffeeu;
  constexpr uint32_t XDC_HEADER_END_MARKER = 0xaccadeadu;
  constexpr uint32_t XDC_EVENT_TRAILER = 0xbbeeddaau;
  constexpr uint32_t XDC_HEADER_WORDS = 14u;
  constexpr uint32_t XDC_TRAILER_WORDS = 1u;
}

namespace{
  auto dummy0 = eudaq::Factory<eudaq::Producer>::
    Register<HidraDryXDCProducer, const std::string&, const std::string&>(HidraDryXDCProducer::m_id_factory);
}


HidraDryXDCProducer::HidraDryXDCProducer(const std::string & name, const std::string & runcontrol)
  :eudaq::Producer(name, runcontrol), m_event_delay_ms(0), m_exit_of_run(false){
}

void HidraDryXDCProducer::DoInitialise(){
  auto ini = GetInitConfiguration();
  (void) ini;
}

void HidraDryXDCProducer::DoConfigure(){
  auto conf = GetConfiguration();
  m_data_in_path = conf->Get("DATA_IN_PATH", "infile.txt");
  m_event_delay_ms = conf->Get("EVENT_DELAY_MS", static_cast<uint32_t>(0));
  EUDAQ_INFO("Using XDC raw data file " + m_data_in_path);
  ReadFileSize();
}

void HidraDryXDCProducer::DoStartRun(){
  m_exit_of_run = false;

  m_thd_run = std::thread(&HidraDryXDCProducer::Mainloop, this);
}

void HidraDryXDCProducer::DoStopRun(){
  m_exit_of_run = true;
  EUDAQ_INFO("Stopping HidraDryXDCProducer run");
  if(m_thd_run.joinable()){
    m_thd_run.join();
  }
  m_ifile.close();
}

void HidraDryXDCProducer::DoReset(){
  m_exit_of_run = true;
  if(m_thd_run.joinable())
    m_thd_run.join();

  m_ifile.close();
  m_thd_run = std::thread();
  m_exit_of_run = false;
}

void HidraDryXDCProducer::DoTerminate(){
  m_exit_of_run = true;
  if(m_thd_run.joinable())
    m_thd_run.join();
}

void HidraDryXDCProducer::ReadFileSize() {
  if (m_ifile.is_open()) {
    m_ifile.close();
  }

  m_ifile.open(m_data_in_path, std::ios::binary | std::ios::ate);
  if(!m_ifile.is_open()){
    EUDAQ_THROW("input data file (" + m_data_in_path +") can not open for reading");
  }

  m_ifile_size = (uint64_t)m_ifile.tellg();
  m_ifile.seekg(0, std::ios::beg);

}

bool HidraDryXDCProducer::ReadXDCEvent(std::vector<uint32_t> &event_words) {
  event_words.clear();

  std::vector<uint32_t> header(XDC_HEADER_WORDS, 0);
  if (!m_ifile.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(XDC_HEADER_WORDS * sizeof(uint32_t)))) {
    return false;
  }

  const uint32_t marker = header[0];
  const uint32_t header_size = header[3];
  const uint32_t trailer_size = header[4];
  const uint32_t data_size = header[5];
  const uint32_t event_size = header[6];
  const uint32_t header_end_marker = header[13];

  if (marker != XDC_EVENT_MARKER) {
    EUDAQ_WARN("Invalid XDC marker, stopping read loop");
    return false;
  }

  if (header_end_marker != XDC_HEADER_END_MARKER) {
    EUDAQ_WARN("Invalid XDC header end marker, stopping read loop");
    return false;
  }

  if (header_size != XDC_HEADER_WORDS || trailer_size != XDC_TRAILER_WORDS) {
    EUDAQ_WARN("Unexpected XDC header/trailer size, stopping read loop");
    return false;
  }

  if (event_size != (header_size + trailer_size + data_size)) {
    EUDAQ_WARN("Inconsistent XDC event size, stopping read loop");
    return false;
  }

  event_words.reserve(event_size);
  event_words.insert(event_words.end(), header.begin(), header.end());

  if (data_size > 0) {
    const std::size_t offset = event_words.size();
    event_words.resize(offset + data_size);
    if (!m_ifile.read(reinterpret_cast<char *>(&event_words[offset]), static_cast<std::streamsize>(data_size * sizeof(uint32_t)))) {
      EUDAQ_WARN("Reached EOF while reading XDC data words");
      return false;
    }
  }

  uint32_t trailer = 0;
  if (!m_ifile.read(reinterpret_cast<char *>(&trailer), static_cast<std::streamsize>(sizeof(uint32_t)))) {
    EUDAQ_WARN("Reached EOF while reading XDC trailer");
    return false;
  }
  if (trailer != XDC_EVENT_TRAILER) {
    EUDAQ_WARN("Invalid XDC trailer marker, stopping read loop");
    return false;
  }
  event_words.push_back(trailer);

  return true;
}

void HidraDryXDCProducer::Mainloop(){
  EUDAQ_INFO("Starting XDC dry readout loop");

  uint64_t loop_count = 0;
  while(!m_exit_of_run){
    std::vector<uint32_t> event_words;
    if (!ReadXDCEvent(event_words)) {
      m_exit_of_run = true;
      break;
    }

    auto ev = eudaq::Event::MakeUnique("XDCEvent");

    const uint32_t event_number = event_words[1];
    const uint32_t spill_number = event_words[2];
    const uint32_t data_size = event_words[5];
    const uint32_t event_size = event_words[6];
    const uint64_t event_time_sec = static_cast<uint64_t>(event_words[7]);
    const uint64_t event_time_usec = static_cast<uint64_t>(event_words[8]);
    const uint32_t trigger_mask = event_words[9];
    const uint32_t is_ped_mask = event_words[10];
    const uint32_t is_ped_from_scaler = event_words[11];
    const uint32_t sanity_flag = event_words[12];

    const uint64_t ts_begin_ns = event_time_sec * 1000000000ULL + event_time_usec * 1000ULL;
    ev->SetTimestamp(ts_begin_ns, ts_begin_ns + 1000ULL, true);
    ev->SetEventN(event_number);
    ev->SetTriggerN(event_number, true);

    ev->SetTag("spillNumber", spill_number);
    ev->SetTag("triggerMask", trigger_mask);
    ev->SetTag("isPedMask", is_ped_mask);
    ev->SetTag("isPedFromScaler", is_ped_from_scaler);
    ev->SetTag("sanityFlag", sanity_flag);
    ev->SetTag("dataWords", data_size);
    ev->SetTag("eventWords", event_size);

    std::vector<uint8_t> payload(data_size * sizeof(uint32_t));
    if (data_size > 0) {
      const uint32_t *payload_words = event_words.data() + XDC_HEADER_WORDS;
      std::memcpy(payload.data(), payload_words, payload.size());
    }
    ev->AddBlock(0, payload);

    if (loop_count == 0) {
      EUDAQ_INFO("First XDC event parsed: eventNumber=" + std::to_string(event_number)
        + ", spillNumber=" + std::to_string(spill_number)
        + ", dataWords=" + std::to_string(data_size));
    }

    SendEvent(std::move(ev));
    ++loop_count;

    if (m_event_delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(m_event_delay_ms));
    }
  }

  EUDAQ_INFO("Exiting XDC dry readout loop after " + std::to_string(loop_count) + " events");
}
