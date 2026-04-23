#include "eudaq/Producer.hh"
#include <iostream>
#include <fstream>
#include <ratio>
#include <chrono>
#include <thread>
#include <limits>
#include <bit>
#include <cstdint>
#include <iterator>     // std::ostream_iterator
#include <algorithm>    // std::copy, std::min, std::max

#define FILE_HEADER_SIZE	25
#define EVENT_HEADER_SIZE	27
#define MAX_EVENT_SIZE		795	// for Ack Mode 03: EVENT_HEADER_SIZE+12*64

class HidraDryFERSProducer : public eudaq::Producer {
public:
  HidraDryFERSProducer(const std::string & name, const std::string & runcontrol);

  void DoInitialise() override;
  void DoConfigure() override;
  void DoStartRun() override;
  void DoStopRun() override;
  void DoTerminate() override;
  void DoReset() override;
  void ReadRawData(std::vector<char>& block, uint16_t data_size);
  std::string GetEventInfo(eudaq::Event* ev);
  uint64_t getTimeus();
  void sleepUntilNext(uint64_t b_last_evt, uint64_t b_current_evt, uint64_t last_real); // all in usec
  void ReadFileSize();
  void Mainloop();
  
  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraDryFERSProducer");
private:
  std::ifstream m_ifile;
  uint64_t m_ifile_size;
  std::string m_data_in_path;
  int m_event_spacing; // microsec
  std::thread m_thd_run;
  mutable bool m_exit_of_run;


};


namespace{
  auto dummy0 = eudaq::Factory<eudaq::Producer>::
    Register<HidraDryFERSProducer, const std::string&, const std::string&>(HidraDryFERSProducer::m_id_factory);
}


HidraDryFERSProducer::HidraDryFERSProducer(const std::string & name, const std::string & runcontrol)
  :eudaq::Producer(name, runcontrol), m_exit_of_run(false){  
}

void HidraDryFERSProducer::DoInitialise(){
  auto ini = GetInitConfiguration();
}

void HidraDryFERSProducer::DoConfigure(){
  auto conf = GetConfiguration();
  m_data_in_path = conf->Get("DATA_IN_PATH", "infile.dat");
  EUDAQ_INFO("Using FERS raw data file " + m_data_in_path);
  m_event_spacing = 1000* (int) conf->Get("REPLAY_EVENT_SPACING_MS", -1);
  std::string inforeplay = m_event_spacing < 0 ? "automatic" : std::to_string(m_event_spacing) + " us";
  EUDAQ_INFO("Replay rate set to "+inforeplay);
  ReadFileSize();
}

void HidraDryFERSProducer::DoStartRun(){
  m_exit_of_run = false;

  m_thd_run = std::thread(&HidraDryFERSProducer::Mainloop, this);
}

void HidraDryFERSProducer::DoStopRun(){
  m_exit_of_run = true;
  EUDAQ_INFO("Exiting HidraDryFERSProducer Run");
  if(m_thd_run.joinable()){
    m_thd_run.join();
  }
  m_ifile.close();
}

void HidraDryFERSProducer::DoReset(){
  m_exit_of_run = true;
  if(m_thd_run.joinable())
    m_thd_run.join();

  m_ifile.close();
  m_thd_run = std::thread();
  m_exit_of_run = false;
}

void HidraDryFERSProducer::DoTerminate(){
  m_exit_of_run = true;
  if(m_thd_run.joinable())
    m_thd_run.join();
}

void HidraDryFERSProducer::ReadFileSize() {
  m_ifile.open(m_data_in_path, std::ios::binary | std::ios::ate);
  if(!m_ifile.is_open()){
    EUDAQ_THROW("input data file (" + m_data_in_path +") can not open for reading");
  }

  m_ifile_size = (uint64_t)m_ifile.tellg();
  m_ifile.seekg(0, std::ios::beg);

}

void HidraDryFERSProducer::ReadRawData(std::vector<char>& block, uint16_t data_size) {
  m_ifile.read(block.data(), data_size);
}


std::string HidraDryFERSProducer::GetEventInfo(eudaq::Event* ev){
  std::string info = "Event Info:";
  info += " evtN "+std::to_string(ev->GetEventN());
  info += " trgN "+std::to_string(ev->GetTriggerN());
  info += " start/stop "+std::to_string(ev->GetTimestampBegin())+"/"+std::to_string(ev->GetTimestampEnd());
  info += " nblk "+std::to_string(ev->GetNumBlock());
  return info;
}

uint64_t HidraDryFERSProducer::getTimeus(){
  auto now = std::chrono::system_clock::now();
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
  return (uint64_t)us;
}

void HidraDryFERSProducer::sleepUntilNext(uint64_t last_evt, uint64_t current_evt, uint64_t last_real){
  if (last_real < 999) {
    EUDAQ_ERROR("Meaningless timestamp of the last event sent ("+std::to_string(last_real)+")");
    return;
  }
  if (m_event_spacing == 0) return;
  if (m_event_spacing > 0){
    uint64_t n = getTimeus();
    if ((n > last_real) && (n-last_real) < m_event_spacing){
      std::this_thread::sleep_for(std::chrono::microseconds(m_event_spacing - (n-last_real)));
      return;
    }
  }
  else{ // m_event_spacing < 0
    
    if (current_evt < last_evt){
      EUDAQ_ERROR("Current event timestamp "+std::to_string(current_evt)+" < last "+std::to_string(last_evt));
      return;
    }
    
    uint64_t n = getTimeus();
    
    //EUDAQ_DEBUG("sleepUtils "+std::to_string(last_evt)+" "+std::to_string(current_evt)+" "+std::to_string(last_real)+" "+std::to_string(n));
   
    if ( (n-last_real) < (current_evt - last_evt)){ 
      int usec = (int)((current_evt - last_evt) - (n-last_real));
      if (usec > 500000){
	EUDAQ_DEBUG("Sleeping "+std::to_string(usec/1000)+ " ms");
      }
      std::this_thread::sleep_for(std::chrono::microseconds(usec));
    }
    
    return;
  }
}

void HidraDryFERSProducer::Mainloop(){  
  //auto tp_start_run = std::chrono::steady_clock::now();
 
  EUDAQ_INFO("Entering Mainloop");

  while(!m_exit_of_run){
    
    uint16_t data_size = MAX_EVENT_SIZE;
    
    // file header
    // TODO: send a BORE with this
    uint8_t header_size = FILE_HEADER_SIZE; // 25 bytes	
    std::vector<uint8_t> file_header(header_size);
    m_ifile.read(reinterpret_cast<char*>(file_header.data()), header_size);
    int file_run_number;
    memcpy(&file_run_number, &file_header[7], 2);
    EUDAQ_WARN("Run number from file is "+std::to_string(file_run_number));
    eudaq::mSleep(2000);

   

    int block_id = 0;
    std::vector<char> block(data_size);
    std::vector<char> zeros(6,0);
   
    uint64_t current_trigger_id = std::numeric_limits<uint64_t>::max();

    //std::unique_ptr<eudaq::Event> current_evt;
    eudaq::EventUP current_evt;
    bool have_open_evt = false;
    uint64_t min_timestamp = std::numeric_limits<uint64_t>::max();
    uint64_t max_timestamp = 0;

    uint64_t evt_time_last_sent = 0;
    uint64_t real_time_last_sent = 0;

    uint64_t iblock = 0;
    uint64_t ievt = 0;
    for (;; iblock++){

      if(m_exit_of_run==true) break;

      auto ev = eudaq::Event::MakeUnique("FERSEvent");
      
      std::vector<uint8_t> eventsize_b(2); // 

      if (  !  m_ifile.read(reinterpret_cast<char*>(eventsize_b.data()), eventsize_b.size()) ){
	m_exit_of_run = true;
	EUDAQ_INFO("File finished. Event counter: "+std::to_string(iblock));
	continue; // should be equivalent to break here
      }


      uint16_t event_size;
      memcpy(&event_size, &eventsize_b[0], 2);

      if (event_size < 2 || event_size > MAX_EVENT_SIZE){
	EUDAQ_ERROR("Block "+std::to_string(iblock)+", inconsistent event size "+std::to_string(event_size));
	m_exit_of_run = true; // cannot decode anymore if the event size is not correct
	continue; 
      }
      else{
	//EUDAQ_DEBUG("Block "+std::to_string(iblock)+" size: "+std::to_string(event_size)); 
      }

      
      
      std::vector<uint8_t> eventblock(event_size - 2);

      m_ifile.read(reinterpret_cast<char*>(eventblock.data()), eventblock.size());
      
      

      uint64_t trigger_id = 0;
      memcpy(&trigger_id, &eventblock[9], 8);

      double d_trigger_timestamp;
      memcpy(&d_trigger_timestamp, &eventblock[1], 8);
      //trigger_timestamp = *static_cast<double*>&eventblock[1]
      uint64_t trigger_timestamp = static_cast<uint64_t>(d_trigger_timestamp*1000);
      
      uint8_t board_id;
      memcpy(&board_id, &eventblock[0], 1);

    
      if (!have_open_evt){
	EUDAQ_INFO("Creating the first event with trigID "+std::to_string(trigger_id));

	current_evt = eudaq::Event::MakeUnique("FERSEvent");
	current_trigger_id = trigger_id;
	current_evt->SetTriggerN(current_trigger_id);
	current_evt->SetEventN(current_trigger_id);
	current_evt->SetRunN(file_run_number);
	
	have_open_evt = true;
      }

      if (have_open_evt && trigger_id > current_trigger_id){

	current_evt->SetTimestamp(min_timestamp, max_timestamp, true);

	sleepUntilNext(evt_time_last_sent/1000, current_evt->GetTimestampBegin()/1000, real_time_last_sent);
	
	EUDAQ_INFO("Sending Dry FERS event "+std::to_string(ievt)+" (at block "+std::to_string(iblock)+")-- "+GetEventInfo(current_evt.get()));
	evt_time_last_sent = current_evt->GetTimestampBegin();
	real_time_last_sent = getTimeus();
	SendEvent(std::move(current_evt));
	ievt++;
	

	current_evt = eudaq::Event::MakeUnique("FERSEvent");
	current_trigger_id = trigger_id;
	min_timestamp = std::numeric_limits<uint64_t>::max();
	max_timestamp = 0;
	current_evt->SetTriggerN(current_trigger_id);
	current_evt->SetEventN(current_trigger_id);
	current_evt->SetRunN(file_run_number);
      }

      
      if (have_open_evt && trigger_id < current_trigger_id){
	EUDAQ_ERROR("Old trigger id detected: "+std::to_string(trigger_id)+", most recent is "+std::to_string(current_trigger_id)+". Skipping block "+std::to_string(iblock)+" in evt "+std::to_string(ievt));
	continue; // skip this block
      }

      
      min_timestamp = std::min(min_timestamp, trigger_timestamp);
      max_timestamp = std::max(max_timestamp, trigger_timestamp);
      current_evt->AddBlock(board_id, eventblock); // TODO better to use progressive index ?

     
    } // end of iblock loop

    if (have_open_evt && current_evt){
      current_evt->SetTimestamp(min_timestamp, max_timestamp, true);
      sleepUntilNext(evt_time_last_sent/1000, current_evt->GetTimestampBegin()/1000, real_time_last_sent);
      EUDAQ_INFO("SSending Dry FERS event "+std::to_string(ievt)+" (at block "+std::to_string(iblock)+")-- "+GetEventInfo(current_evt.get()));
      SendEvent(std::move(current_evt));
      ievt++;
    }

    m_exit_of_run = true;
  }
  EUDAQ_INFO("EXITING MAINLOOP THREAD");
}
      
      
	
	
   
   


