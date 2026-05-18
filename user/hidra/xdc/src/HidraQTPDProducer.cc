#pragma once

#include <eudaq/Producer.hh>
#include <eudaq/Logger.hh>
#include <eudaq/Event.hh>
#include <eudaq/Configuration.hh>
#include <eudaq/Factory.hh>

#include <CAENVMElib.h>
#include <CAENVMEtypes.h>
#include "HidraUtils.hh"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t MAX_BLT_SIZE = 1024 * 4;
constexpr int32_t INVALID_HANDLE = -1;
constexpr int NCHAN = 250;
constexpr uint16_t INVALID_ADC = 4444;

constexpr uint32_t DATATYPE_FILLER = 0x06000000;

// --- V977 registers ---
constexpr uint16_t V977_INPUT_SET_REG = 0x0000;
constexpr uint16_t V977_INPUT_MASK_REG = 0x0002;
constexpr uint16_t V977_OUTPUT_MASK_REG = 0x000C;
constexpr uint16_t V977_OUTPUT_SET_REG = 0x000A;
constexpr uint16_t V977_OUTPUT_CLEAR_REG = 0x0010;
constexpr uint16_t V977_INPUT_READ_REG = 0x0004;
constexpr uint16_t V977_SINGLE_READ_REG = 0x0006;
constexpr uint16_t V977_SINGLE_READ_CLEAR = 0x0016;

struct BoardConfig {
  uint32_t baseAddr;
  uint16_t geoAddr;
  uint16_t crateNr;
};

inline std::string hex32(uint32_t value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
  return oss.str();
}

inline uint32_t parse_u32(const std::string& s) {
  std::size_t idx = 0;
  unsigned long v = std::stoul(s, &idx, 0); // base 0 => handles 0x...
  if (idx != s.size()) {
    throw std::runtime_error("Cannot parse uint32 from: " + s);
  }
  return static_cast<uint32_t>(v);
}

inline uint16_t parse_u16(const std::string& s) {
  std::size_t idx = 0;
  unsigned long v = std::stoul(s, &idx, 0);
  if (idx != s.size()) {
    throw std::runtime_error("Cannot parse uint16 from: " + s);
  }
  return static_cast<uint16_t>(v);
}

} // namespace

class HidraQTPDProducer : public eudaq::Producer {
public:
  HidraQTPDProducer(const std::string& name, const std::string& runcontrol)
      : eudaq::Producer(name, runcontrol),
        m_handle(INVALID_HANDLE),
        m_vmeError(false),
        m_running(false),
        m_terminate(false),
        m_runNumber(0),
        m_evt(0),
	m_spill_cnt(0), //NEW
        m_iped(100),
        m_controllerType(cvV2718),
        // m_pid(49086) // --- To connect controller via USB --- 
        m_pid(0) {
    m_adcval.fill(INVALID_ADC);
    m_buffer.fill(0);
    m_buffer[0] = DATATYPE_FILLER;
  }

  ~HidraQTPDProducer() override {
    try {
      StopAcquisitionThread();
      CloseController();
    } catch (...) {
    }
  }

  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraQTPDProducer");

private:
  void DoInitialise() override {
    auto ini = GetInitConfiguration();

    if (!ini) {
      EUDAQ_THROW("Init configuration is missing");
    }

    // --- Controller setup ---
    std::string ctrl = ini->Get("ControllerType", std::string("V2718"));
    if (ctrl == "V2718") {
      m_controllerType = cvV2718;
    }
    else {
      EUDAQ_THROW("Unsupported ControllerType: " + ctrl);
    }

    // m_pid = parse_u32(ini->Get("LinkOrPid", std::string("49086"))); // --- To connect controller via USB ---
    m_pid = parse_u32(ini->Get("LinkOrPid", std::string("0")));

    OpenController();

    EUDAQ_INFO("CAEN controller initialized");
  }

  void DoConfigure() override {
    auto conf = GetConfiguration();
    m_evt = 0;
    m_onspill = false; //NEW
    m_spill_cnt = 0;
    if (!conf) {
      EUDAQ_THROW("Run configuration is missing");
    }
    m_iped = static_cast<int>(parse_u16(conf->Get("Iped", std::string("100"))));

    m_boards.clear();

    // --- XDC module configuration --- 
    AddBoardFromConf(*conf, 0, "0x06000000", "2", "2");
    AddBoardFromConf(*conf, 1, "0x05000000", "4", "4");
    AddBoardFromConf(*conf, 2, "0x09000000", "8", "8");
    AddBoardFromConf(*conf, 3, "0x88880000", "16", "16");
    AddBoardFromConf(*conf, 4, "0x0B000000", "20", "20");

    if (m_boards.empty()) {
      EUDAQ_THROW("No boards configured");
    }

    m_vmeError = false;
    for (const auto& b : m_boards) {
      InitBoard(b);
    }
    if (m_vmeError) {
      EUDAQ_THROW("Board initialization failed: " + m_errorString);
    }

    // --- Register to perform block transfer ---
    for (std::size_t i = 0; i < m_boards.size(); ++i) {
      WriteReg(0x1004, 0xAA, m_boards[i].baseAddr);
    }

    // --- Set initial (0x02), active (0x03), last (0x01) and inactive (0x00) board ---
    if (m_boards.size() > 0) {
      WriteReg(0x101A, 0x02, m_boards[0].baseAddr);
    }
    if (m_boards.size() > 1) {
      WriteReg(0x101A, 0x03, m_boards[1].baseAddr);
    }
    if (m_boards.size() > 2) {
      WriteReg(0x101A, 0x03, m_boards[2].baseAddr);
    }
    if (m_boards.size() > 3) {
      WriteReg(0x101A, 0x03, m_boards[3].baseAddr);
    }
    if (m_boards.size() > 4) {
      WriteReg(0x101A, 0x01, m_boards[4].baseAddr);
    }

    m_adcval.fill(INVALID_ADC);

    // --- Configure I/O register ---
    m_v977_base = parse_u32(conf->Get("V977_BASE", "0x01000000")); // --- Base Address configuration ---

    WriteReg(V977_OUTPUT_CLEAR_REG, 0xFFFF, m_v977_base); // --- Reset all output registers ---
    WriteReg(V977_INPUT_MASK_REG, 0xFFF8, m_v977_base);   // --- Deactivate all input channels except channel 0, 1, 2 (or the ones you want to use) ---> chann 0 for trigger, 1 for m_onspill, 2 for outspill //NEW
    WriteReg(V977_OUTPUT_MASK_REG, 0xFFFE, m_v977_base);  // --- Deactivate all output channels except channel 0 ---
    WriteReg(V977_INPUT_SET_REG, 0x0000, m_v977_base);    // --- All inputs set to 0 ---
    PrepareForRun(); // --- To VETO the trigger ---

    EUDAQ_INFO("Initialized I/O at address: " + hex32(m_v977_base)); 

    EUDAQ_INFO("Producer configured");
  }

  void DoStartRun() override {
    StopAcquisitionThread();
    m_runNumber = GetRunNumber();
    m_evt = 0;
    m_spill_cnt = 0;
    m_adcval.fill(INVALID_ADC);
    m_running = true;
    m_onspill = false;
    SendBORE();
    WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base); // --- Clear output register ---
    WriteReg(V977_INPUT_SET_REG, 0x0000, m_v977_base);    // --- All inputs set to 0 ---
    PrepareForRun(); // --- To VETO the trigger ---
    m_thd_run = std::thread(&HidraQTPDProducer::MainLoop, this);
    EUDAQ_INFO("Starting run " + std::to_string(m_runNumber));
  }

  void DoStopRun() override {
    m_running = false;
    m_onspill = false;
    StopAcquisitionThread();
    SendEORE();
    HIDRA_INFO("Stopping run {}", m_runNumber);
    PrepareForRun();
  }

  void DoReset() override {
    StopAcquisitionThread();
    m_running = false;
    m_onspill = false;
    m_evt = 0;
    m_spill_cnt = 0;
    m_adcval.fill(INVALID_ADC);
    EUDAQ_INFO("Producer reset");
  }

  void DoTerminate() override {
    StopAcquisitionThread();
    CloseController();
    EUDAQ_INFO("Producer terminated");
  }

  void RunLoop() override {
    // -- Keep polling continuously and only acquire while m_running == true ---
    return;
    while (m_running) {
      if (m_handle < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      if (!m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      // --- Main function to read XDCs and use the I/O Register logic ---
      ReadOneBlockAndSendEvent();
    }
  }

  void PrepareForRun(){
         WriteReg(V977_OUTPUT_SET_REG, 0x0001, m_v977_base);   // --- VETO ON ---
	 HIDRA_INFO("trigger vetoed");
  }	  

  void MainLoop() {
	while (m_running) {
            
	    if (m_handle < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
		continue;
	    }
	    if (!m_running) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		continue;
	    }
	    
	    m_V977_pattern = ReadReg(V977_SINGLE_READ_REG, m_v977_base);
	    bool trigger      = m_V977_pattern & 0x0001;
	    bool spill_start  = m_V977_pattern & 0x0002;
	    bool spill_end    = m_V977_pattern & 0x0004;

	    //-------------------------------------------------
	    // INVALID CONDITION
	    //-------------------------------------------------
/*
	    if (spill_start && spill_end) {

		EUDAQ_ERROR("Spill START and END simultaneously active");

                //WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);   // --- CLEAR FLIP FLOP ---
		PrepareForRun();
		m_onspill = false;

		continue;
	    }
*/
	    //-------------------------------------------------
	    // OFF-SPILL STATE
	    //-------------------------------------------------

	    if (!m_onspill) {

		// ------------------------------
		// invalid END without START
		// ------------------------------

		if (spill_end) {

		    EUDAQ_ERROR(
			"END OF SPILL while already OFFSPILL "
			"(missed START?)"
		    );

//                    WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);   // --- CLEAR FLIP FLOP ---
                    m_clear_requested = true;
		}

		// ------------------------------
		// valid START
		// ------------------------------

		if (spill_start) {

		    WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);   // --- CLEAR FLIP FLOP ---	
		    m_clear_requested = false;
		    WriteReg(V977_OUTPUT_SET_REG, 0x0000, m_v977_base); //VETO OFF
		    m_onspill = true;
		    m_spill_cnt++;
		    EUDAQ_INFO(
			"========== START OF SPILL =========="
		    );
		}

		static int cnt_sp = 0;
		if(++cnt_sp % 1000000 == 0) {
		
			EUDAQ_INFO("Waiting for the spill to start...");
	        }		
	    } else {

	    //-------------------------------------------------
	    // ON-SPILL STATE
	    //-------------------------------------------------
		    if(spill_end) {
			PrepareForRun();
			
		     }
		    // Read again to catch end of spill edgecase after raising VETO manually
	            bool trigger = m_V977_pattern & 0x0001;
		    static int trig_print_cnt = 0;
		    if(trigger) { 
			    ReadOneBlockAndSendEvent(); 
		    } else {
			    trig_print_cnt++;
			    if(trig_print_cnt > 10000000){
	        	        EUDAQ_INFO("Still waiting for the trigger...");
				trig_print_cnt = 0;
	                    }
		    }
	
	            if(spill_end) {
			    m_onspill = false;
			    m_clear_requested = true;
		    } else if(spill_start) {
			EUDAQ_ERROR(
	                    "START OF SPILL while already ONSPILL "
	                    "(missed END?)"
                	); //TODO decide if it needs different handling
			m_clear_requested = true;
		    }
		    
	     }
	     if(m_clear_requested) {
			    WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);   // --- CLEAR FLIP FLOP ---
       			    m_clear_requested = false;
		    } 
	}
}

  /*void MainLoop() {
    // -- Keep polling continuously and only acquire while m_running == true ---
    while (m_running) {
      if (m_handle < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      if (!m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      
      m_V977_pattern = ReadReg(V977_SINGLE_READ_REG, m_v977_base); //NEW
      bool trigger_onspill_fired = m_V977_pattern & 0x0002; // --- Spill is when channel 2 is active ---
      bool trigger_offspill_fired = m_V977_pattern & 0x0004; // --- OffSpill is when channel 3 is active ---
      bool trigger = m_V977_pattern & 0x0001; // --- Trigger is when channel 1 is active ---
      
      if(!m_onspill) {
	       if(trigger_offspill_fired) {
		      EUDAQ_INFO("End of spill signal arrived while already off spill: missed start of spill signal.");
		      m_onspill = false;
		      WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);
		      continue;
	       }
	       
	       if(!trigger_onspill_fired && !trigger_offspill_fired) {
		       static int cnt_sp = 0;
		       if (++cnt_sp % 100000 == 0) {
			EUDAQ_INFO("Still waiting for the spill signal...");
		       }
		       WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);
		       m_onspill = false;
		       continue; 
	       }	       
	       
	       if(trigger_onspill_fired && trigger_offspill_fired) {
		       EUDAQ_INFO("Start and end of spill simultaneously arrived: Error.");
		       WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);
		       m_onspill = false;
		       continue; 
	       }	       
               
               if(trigger_onspill_fired) {
		      m_onspill = true;
                      WriteReg(V977_OUTPUT_SET_REG, 0x0000, m_v977_base); //VETO OFF 
		      EUDAQ_INFO("START OF SPILL");  
               }
	       
      }  

      // --- Main function to read XDCs and use the I/O Register logic ---
      ReadOneBlockAndSendEvent();

      if(trigger_offspill_fired) {
	    ++m_spill;
	    HIDRA_INFO("Spill number: {} has just finished", m_spill);
            m_onspill = false;
	    WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);
            PrepareForRun();
	    continue;
      }	      
      if(trigger_onspill_fired) {
	      EUDAQ_INFO("Begin of spill signal arrived while already on spill: missed end of spill signal.");
	      m_onspill = false;
	      WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);
              PrepareForRun();
	      continue;
      }
    }
  }*/

  void OpenController() {
    if (m_handle >= 0) {
      return;
    }

    uint32_t pid = m_pid;
    CVErrorCodes ret = CAENVME_Init2(m_controllerType, &pid, 0, &m_handle);
    if (ret != cvSuccess) {
      m_handle = INVALID_HANDLE;
      EUDAQ_THROW("Failed to open CAEN VME controller type " + std::to_string(m_controllerType) +
                  ", ret=" + std::to_string(ret));
    }
  }

  void CloseController() {
    if (m_handle < 0) {
      return;
    }

    int attempt = 0;
    while (CAENVME_End(m_handle) != cvSuccess) {
      if (++attempt > 20) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    m_handle = INVALID_HANDLE;
  }

  uint16_t ReadReg(uint16_t reg_addr, uint32_t baseAddr) {
    uint16_t data = 0;
    CVErrorCodes ret = CAENVME_ReadCycle(m_handle, baseAddr + reg_addr, &data, cvA32_U_DATA, cvD16);

    if (ret != cvSuccess) {
      m_errorString = "Cannot read at address " + hex32(baseAddr + reg_addr);
      m_vmeError = true;
    }
    return data;
  }

  void WriteReg(uint16_t reg_addr, uint16_t data, uint32_t baseAddr) {
    CVErrorCodes ret = CAENVME_WriteCycle(m_handle, baseAddr + reg_addr, &data, cvA32_U_DATA, cvD16);

    if (ret != cvSuccess) {
      m_errorString = "Cannot write at address " + hex32(baseAddr + reg_addr);
      m_vmeError = true;
    }
  }

  int InfoWord792(uint32_t w, uint8_t& chan, uint16_t& val) {
    int datatype = (w >> 24) & 0b111;
    if (datatype != 0) {
      return datatype;
    }
    chan = static_cast<uint8_t>((w >> 16) & 0b11111);
    val = static_cast<uint16_t>(w & 0x0FFF);
    return datatype;
  }

  void InitBoard(const BoardConfig& b) {
    EUDAQ_INFO("Initializing board at " + hex32(b.baseAddr));

    WriteReg(0x1002, b.geoAddr, b.baseAddr); // --- Geo addr ---
    WriteReg(0x1006, 0x80, b.baseAddr);      // --- reset XDCs board ---
    if (m_vmeError) {
      EUDAQ_THROW(m_errorString);
    }

    WriteReg(0x1008, 0x80, b.baseAddr); // --- release reset ---

    int model = (ReadReg(0x803E, b.baseAddr) & 0xFF) + ((ReadReg(0x803A, b.baseAddr) & 0xFF) << 8);

    EUDAQ_INFO("Board model at " + hex32(b.baseAddr) + ": " + std::to_string(model));

    WriteReg(0x103C, b.crateNr, b.baseAddr);                     // crate number
    WriteReg(0x1060, static_cast<uint16_t>(m_iped), b.baseAddr); // pedestal
    WriteReg(0x1010, 0x60, b.baseAddr);                          // BERR closes BLT
    WriteReg(0x1032, 0x0010, b.baseAddr);                        // disable zero suppression
    WriteReg(0x1032, 0x0008, b.baseAddr);                        // disable overrange suppression
    WriteReg(0x1032, 0x1000, b.baseAddr);                        // enable empty events
    WriteReg(0x1040, 0x0, b.baseAddr);                           // clear event counter
    WriteReg(0x1032, 0x4, b.baseAddr);                           // clear QTP
    WriteReg(0x1034, 0x4004, b.baseAddr);

    auto regr = ReadReg(0x1032, b.baseAddr);
    EUDAQ_INFO("Board programmed 0x" + IntToHex16(regr));
  }

  void ReadOneBlockAndSendEvent() {

   

	    constexpr uint32_t readAddress = 0xAA000000; // --- Block transfer address ---
	    int bcnt = 0;

	    CVErrorCodes ret = CAENVME_FIFOMBLTReadCycle(m_handle,
							 readAddress,
							 reinterpret_cast<char*>(m_buffer.data()),
							 static_cast<int>(MAX_BLT_SIZE),
							 cvA32_U_MBLT,
							 &bcnt);

	    if (ret != cvSuccess && ret != cvBusError) {
	      // bus error at end of block can be normal with BERR-enabled block read
	      EUDAQ_DEBUG("BLT Error: " + std::to_string(ret));
	      std::this_thread::sleep_for(std::chrono::milliseconds(1));
	    }

	    if (bcnt <= 0) {
	      std::this_thread::sleep_for(std::chrono::milliseconds(1));
	      EUDAQ_DEBUG("BCNT 0");
	      return;
	    }

	    const int wcnt = bcnt / 4;
	    if (wcnt <= 0) {
	      return;
	    }

	    m_adcval.fill(INVALID_ADC);

	    uint8_t adc_chan = 0xFF;
	    uint16_t adc_val = 0xFFFF;

	    for (int pnt = 0; pnt < wcnt; ++pnt) {
	      int dtype = InfoWord792(m_buffer[pnt], adc_chan, adc_val);
	      if (dtype == 0 && adc_chan < NCHAN) {
		m_adcval[adc_chan] = adc_val;
	      }
	    }
	    EUDAQ_INFO("Event Triggered");
	    //EUDAQ_INFO("V977 PATTERN: " + std::to_string(trigger));

	    // --- Build EUDAQ event ---
	    auto ev = eudaq::Event::MakeUnique("CAENQTPRaw");

	    ev->SetTriggerN(static_cast<uint32_t>(m_evt));
	    ev->SetEventN(static_cast<uint32_t>(m_evt));
	    ev->SetRunN(static_cast<uint32_t>(m_runNumber));

	    // TODO: clean clean clean tags everywhere in the class
	    /*
	    ev->SetTag("BCNT", std::to_string(bcnt));
	    ev->SetTag("WCNT", std::to_string(wcnt));
	    ev->SetTag("ADC0", std::to_string(m_adcval[0]));
	    ev->SetTag("ADC1", std::to_string(m_adcval[1]));
	    ev->SetTag("ADC2", std::to_string(m_adcval[2]));
	    ev->SetTag("ADC3", std::to_string(m_adcval[3]));
	    */

	    //ev->SetTag("V977", std::to_string(trigger));

	    std::vector<uint8_t> raw(reinterpret_cast<uint8_t*>(m_buffer.data()),
                             reinterpret_cast<uint8_t*>(m_buffer.data()) + bcnt);
	    ev->AddBlock(0, raw);

	    ev->SetTag("endianness","BE32"); // this tag is needed :)
	    SendEvent(std::move(ev));
	    ++m_evt;
	    
	    //WriteReg(V977_OUTPUT_CLEAR_REG, 0xF000, m_v977_base);
	    m_clear_requested = true;

    }
    
  void SendBORE() {
    auto bore = eudaq::Event::MakeUnique("CAENQTPRaw");
    bore->SetBORE();
    bore->SetRunN(static_cast<uint32_t>(m_runNumber));
    bore->SetTag("Producer", "HidraQTPDProducer");
    bore->SetTag("Iped", std::to_string(m_iped));
    bore->SetTag("NumBoards", std::to_string(m_boards.size()));

    for (std::size_t i = 0; i < m_boards.size(); ++i) {
      bore->SetTag("Board" + std::to_string(i) + "_Base", hex32(m_boards[i].baseAddr));
      bore->SetTag("Board" + std::to_string(i) + "_Geo", std::to_string(m_boards[i].geoAddr));
      bore->SetTag("Board" + std::to_string(i) + "_Crate", std::to_string(m_boards[i].crateNr));
    }

    start = std::chrono::steady_clock::now();
    SendEvent(std::move(bore));
  }

  void SendEORE() {
    auto eore = eudaq::Event::MakeUnique("CAENQTPRaw");
    eore->SetEORE();
    eore->SetRunN(static_cast<uint32_t>(m_runNumber));
    eore->SetTag("EventsSent", std::to_string(m_evt));
    stop = std::chrono::steady_clock::now();
    elapsed_t = stop - start;
    auto seconds_int = std::chrono::duration_cast<std::chrono::seconds>(elapsed_t).count();
    EUDAQ_INFO("The run has lasted: " + std::to_string(seconds_int) + " seconds.");
    SendEvent(std::move(eore));
  }

  void StopAcquisitionThread() {
    m_running = false;

    if (m_thd_run.joinable()) {
      m_thd_run.join();
    }
  }

  void AddBoardFromConf(const eudaq::Configuration& conf,
                        int idx,
                        const std::string& baseDefault,
                        const std::string& geoDefault,
                        const std::string& crateDefault) {
    const std::string p = "Board" + std::to_string(idx) + ".";

    const std::string enable = conf.Get(p + "Enable", std::string("1"));
    if (enable == "0" || enable == "false" || enable == "False") {
      return;
    }

    BoardConfig b;
    b.baseAddr = parse_u32(conf.Get(p + "BaseAddress", baseDefault));
    b.geoAddr = parse_u16(conf.Get(p + "GeoAddress", geoDefault));
    b.crateNr = parse_u16(conf.Get(p + "CrateNumber", crateDefault));
    m_boards.push_back(b);
  }

  std::string IntToHex16(uint16_t v) const {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << v;
    return oss.str();
  }

private:
  int32_t m_handle;
  bool m_vmeError;
  std::string m_errorString;
  uint16_t m_V977_pattern; //NEW
  bool m_onspill; //NEW
  bool m_clear_requested;

  uint32_t m_v977_base = 0;

  std::atomic<bool> m_running;
  std::atomic<bool> m_terminate;

  uint32_t m_runNumber;
  uint64_t m_evt;
  uint64_t m_spill_cnt; //NEW
  int m_iped;

  std::chrono::duration<double> elapsed_t;
  std::chrono::steady_clock::time_point start;
  std::chrono::steady_clock::time_point stop;

  CVBoardTypes m_controllerType;
  uint32_t m_pid;

  std::vector<BoardConfig> m_boards;
  std::array<uint32_t, 256 * 1024 / 4> m_buffer;
  std::array<uint16_t, NCHAN> m_adcval;

  std::thread m_thd_run;
};

namespace {
auto dummy0 = eudaq::Factory<eudaq::Producer>::Register<HidraQTPDProducer, const std::string&, const std::string&>(
    HidraQTPDProducer::m_id_factory);
}
