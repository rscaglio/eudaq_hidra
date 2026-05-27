#include <eudaq/Configuration.hh>
#include <eudaq/Event.hh>
#include <eudaq/Factory.hh>
#include <eudaq/Logger.hh>
#include <eudaq/Producer.hh>

#include <CAENVMElib.h>
#include <CAENVMEtypes.h>
#include "HidraUtils.hh"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {


// SET THESE ACCORDING TO HARDWARE SIGNALS
enum V977IN {
  cFastGate = 0,
  cPhy = 1,
  cPed = 2,
  cSpillStart = 3,
  cSpillEnd = 4
};
enum V977OUT {
  cVeto = static_cast<int>(V977IN::cFastGate),
  cPedVeto = 5
};
////////////////////

constexpr std::size_t MAX_BLT_SIZE = 1024 * 4; // TODO check this
constexpr int32_t INVALID_HANDLE = -1;
constexpr uint32_t DATATYPE_FILLER = 0x06000000;
constexpr uint32_t BLT_READ_ADDRESS = 0xAA000000;

// V977 registers.
constexpr uint16_t V977_INPUT_SET_REG = 0x0000;
constexpr uint16_t V977_INPUT_MASK_REG = 0x0002;
constexpr uint16_t V977_INPUT_READ_REG = 0x0004;
constexpr uint16_t V977_OUTPUT_MASK_REG = 0x000C;
constexpr uint16_t V977_OUTPUT_SET_REG = 0x000A;
constexpr uint16_t V977_OUTPUT_CLEAR_REG = 0x0010; // A dummy write access to this register clears all the channels FLIP-FLOP.
constexpr uint16_t V977_SINGLE_READ_REG = 0x0006;


// CAEN V792/QDC registers used by this producer.
constexpr uint16_t V792_GEO_ADDRESS_REG = 0x1002;
constexpr uint16_t V792_BIT_SET_1_REG = 0x1006;
constexpr uint16_t V792_BIT_CLEAR_1_REG = 0x1008;
constexpr uint16_t V792_BLT_EVENT_NUMBER_REG = 0x1004;
constexpr uint16_t V792_CRATE_SELECT_REG = 0x103C;
constexpr uint16_t V792_IPED_REG = 0x1060;
constexpr uint16_t V792_CONTROL_1_REG = 0x1010;
constexpr uint16_t V792_BIT_SET_2_REG = 0x1032;
constexpr uint16_t V792_BIT_CLEAR_2_REG = 0x1034;
constexpr uint16_t V792_EVENT_COUNTER_RESET_REG = 0x1040;
constexpr uint16_t V792_MCST_CBLT_ADDRESS_REG = 0x101A;
constexpr uint16_t V792_MODEL_HIGH_REG = 0x803A;
constexpr uint16_t V792_MODEL_LOW_REG = 0x803E;
constexpr uint16_t V792_STATUS_1_REG = 0x100E;

constexpr uint16_t V792_MCST_FIRST = 0x02;
constexpr uint16_t V792_MCST_MIDDLE = 0x03;
constexpr uint16_t V792_MCST_LAST = 0x01;

struct BoardConfig {
  uint32_t baseAddr;
  uint16_t geoAddr;
  uint16_t crateNr;
};

struct BoardDefaults {
  const char* baseAddr;
  const char* geoAddr;
  const char* crateNr;
};

const std::array<BoardDefaults, 5> DEFAULT_BOARDS = {{
    {"0x06000000", "1", "1"},
    {"0x05000000", "2", "2"},
    {"0x09000000", "3", "3"},
    {"0x88880000", "4", "4"},
    {"0x0B000000", "5", "5"},
}};

struct V977Pattern {
  uint16_t raw = 0;
  bool trigger = false;
  bool physics = false;
  bool pedestal = false;
  bool spillStart = false;
  bool spillEnd = false;
};

std::string hex32(uint32_t value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
  return oss.str();
}

std::string hex16(uint16_t value) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << value;
  return oss.str();
}

uint32_t parse_u32(const std::string& value) {
  std::size_t parsed = 0;
  const unsigned long result = std::stoul(value, &parsed, 0);
  if (parsed != value.size()) {
    throw std::runtime_error("Cannot parse uint32 from: " + value);
  }
  return static_cast<uint32_t>(result);
}

uint16_t parse_u16(const std::string& value) {
  std::size_t parsed = 0;
  const unsigned long result = std::stoul(value, &parsed, 0);
  if (parsed != value.size()) {
    throw std::runtime_error("Cannot parse uint16 from: " + value);
  }
  return static_cast<uint16_t>(result);
}

bool is_disabled(const std::string& value) {
  return value == "0" || value == "false" || value == "False";
}

} // namespace

class HidraQTPDProducer : public eudaq::Producer {
public:
  HidraQTPDProducer(const std::string& name, const std::string& runcontrol)
      : eudaq::Producer(name, runcontrol),
        m_handle(INVALID_HANDLE),
        m_vmeError(false),
        m_onspill(false),
        m_clearRequested(false),
        m_running(false),
        m_runNumber(0),
        m_evt(0),
        m_evt_ped(0),
        m_evt_phy(0),
        m_spillCount(0),
        m_iped(100),
        m_controllerType(cvV2718),
        m_pid(0) {
    ResetReadoutBuffers();
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
    const auto ini = GetInitConfiguration();
    if (!ini) {
      EUDAQ_THROW("Init configuration is missing");
    }

    ConfigureController(*ini);
    OpenController();
    EUDAQ_INFO("CAEN controller initialized");
  }

  void DoConfigure() override {
    const auto conf = GetConfiguration();
    if (!conf) {
      EUDAQ_THROW("Run configuration is missing");
    }

    EUDAQ_LOG_LEVEL((int)(conf->Get("HIDRA_MUTE_DEBUG", 0)));
    ResetRunState();
    LoadRunConfiguration(*conf);

    ConfigureBoards();
    ConfigureV977andVeto();

    EUDAQ_INFO("Producer configured");
  }

  void DoStartRun() override {
    StopAcquisitionThread();

    m_runNumber = GetRunNumber();
    ResetRunState();
    m_running = true;

    SendBORE();
    ResetV977ForRun();
    
    m_thread = std::thread(&HidraQTPDProducer::MainLoop, this);
    EUDAQ_INFO("Starting run " + std::to_string(m_runNumber));

    ReleaseTriggerVeto();
  }

  void DoStopRun() override {
    m_running = false;
    m_onspill = false;
    StopAcquisitionThread();
    SendEORE();
    HIDRA_INFO("Stopping run {}", m_runNumber);
    VetoTrigger();
  }

  void DoReset() override {
    StopAcquisitionThread();
    ResetRunState();
    EUDAQ_INFO("Producer reset");
  }

  void DoTerminate() override {
    StopAcquisitionThread();
    CloseController();
    EUDAQ_INFO("Producer terminated");
  }

  void RunLoop() override {
    // This producer owns its acquisition thread so the EUDAQ default loop stays idle.
  }

  void ConfigureController(const eudaq::Configuration& ini) {
    const std::string controller = ini.Get("ControllerType", std::string("V2718"));
    if (controller != "V2718") {
      EUDAQ_THROW("Unsupported ControllerType: " + controller);
    }

    m_controllerType = cvV2718;
    // Use LinkOrPid=49086 for the USB controller path used by older setups.
    m_pid = parse_u32(ini.Get("LinkOrPid", std::string("0")));
  }

  void LoadRunConfiguration(const eudaq::Configuration& conf) {
    m_iped = static_cast<int>(parse_u16(conf.Get("Iped", std::string("100"))));
    m_v977Base = parse_u32(conf.Get("V977_BASE", std::string("0x01000000")));

    // TODO: do not hard code the number of boards. Read the number of enabled ones from hidra.conf
    m_boards.clear();
    for (std::size_t index = 0; index < DEFAULT_BOARDS.size(); ++index) {
      AddBoardFromConf(conf, index, DEFAULT_BOARDS[index]);
    }

    if (m_boards.empty()) {
      EUDAQ_THROW("No boards configured");
    }
  }

  void ConfigureBoards() {
    m_vmeError = false;
    for (const auto& board : m_boards) {
      InitBoard(board);
    }
    ThrowIfVmeError("Board initialization failed");

    ConfigureBlockTransfer();
    ConfigureMulticastChain();
    ThrowIfVmeError("Board readout-chain configuration failed");
  }

  void ConfigureBlockTransfer() {
    for (const auto& board : m_boards) {
      WriteReg(V792_BLT_EVENT_NUMBER_REG, 0xAA, board.baseAddr); 
    }
  }

  void ConfigureMulticastChain() {
    for (std::size_t index = 0; index < m_boards.size(); ++index) {
      WriteReg(V792_MCST_CBLT_ADDRESS_REG, MulticastRole(index), m_boards[index].baseAddr);
    }
  }

  uint16_t MulticastRole(std::size_t index) const {
    // TODO: move to config
    if (index == 0) {
      return V792_MCST_FIRST;
    }
    if (index == 4) { // TODO: do not hard code number 4. If fewer than 5  boards are enabled, no board becomes LAST
      return V792_MCST_LAST;
    }
    return V792_MCST_MIDDLE;
  }

  /// V977 handlers
  void SetSingleV977OutputReg(bool isHigh, int chan) { // chan starts from 0
    std::lock_guard<std::mutex> lock(m_v977OutputSetMutex);
    uint16_t outputSet = ReadReg(V977_OUTPUT_SET_REG, m_v977Base);
    ThrowIfVmeError("V977 output set read failed");

    uint16_t setBitmask = static_cast<uint16_t>(1u << chan);

    if (isHigh) {
      outputSet = outputSet | setBitmask;
    } else {
      outputSet = outputSet & ~setBitmask;
    }

    WriteReg(V977_OUTPUT_SET_REG, outputSet, m_v977Base);
    ThrowIfVmeError("V977 output set write failed");
  }

  void SetAllV977OutputReg(bool isHigh){
    uint16_t setBitMask = isHigh ? 0xFFFF : 0x0000;
    WriteReg(V977_OUTPUT_SET_REG, setBitMask, m_v977Base);
    ThrowIfVmeError("V977 output set write failed");
  }


  void ConfigureV977andVeto() {


    ClearV977FlipFlops();

    
    // Why masking the unused channels?
    /*
    uint16_t output_mask = 0xFFFF;
    output_mask &= ~(1u << V977OUT::cVeto);

    uint16_t input_mask = 0xFFFF;
    input_mask &= ~(1u << V977IN::cFastGate);
    input_mask &= ~(1u << V977IN::cPed);
    input_mask &= ~(1u << V977IN::cPhy);
    WriteReg(V977_INPUT_MASK_REG, input_mask, m_v977Base); // not needed?
    WriteReg(
        V977_OUTPUT_MASK_REG,
        output_mask,
        m_v977Base); // the relevant output is “masked” and no output signal is produced regardless the FLIP FLOPs
                     // status. The output signal can be produced anyway via the relevant bit in the OUTPUT SET register
    */
    
    WriteReg(V977_INPUT_SET_REG, 0x0000, m_v977Base);
    VetoTrigger();
    ThrowIfVmeError("V977 configuration failed");

    EUDAQ_INFO("Initialized I/O at address: " + hex32(m_v977Base));
  }

  void ResetV977ForRun() {
    ClearV977FlipFlops();
    WriteReg(V977_INPUT_SET_REG, 0x0000, m_v977Base);                       // all inputs set to 0
  }

  void VetoTrigger() {
    uint16_t setBitMask = 0x0000 | (1u << V977OUT::cVeto) | (1u << V977OUT::cPedVeto);
    WriteReg(V977_OUTPUT_SET_REG, setBitMask, m_v977Base);
    ThrowIfVmeError("V977 output set write failed while calling VetoTrigger");
    HIDRA_INFO("trigger vetoed");
  }

  void ReleaseTriggerVeto(bool releasePedVeto = false) {
    uint16_t setBitMask = 0x0000 & (1u << V977OUT::cPedVeto);
    if (releasePedVeto) setBitMask = 0x0000;
    WriteReg(V977_OUTPUT_SET_REG, setBitMask, m_v977Base);
    ThrowIfVmeError("V977 output set write failed while calling VetoTrigger");
    HIDRA_INFO("trigger vetoed");
  }

  bool requestPedestalNext(){
    return ((double)m_evt_phy / (double)m_evt_ped) > 10;
  }

  bool CheckBoardsReady(int timeout_us, int sleep_cycle_time_us, int sleep_begin_time_ns = 0) {

    if (sleep_begin_time_ns > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_begin_time_ns));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    while (std::chrono::steady_clock::now() < deadline) {

      /*
      // ----- Option 1 ----
      // TODO: this would try BLT address, but V792 manual says that 0x100E doesn't work like this for V792N
      const uint16_t statusAll = ReadReg(V792_STATUS_1_REG, BLT_READ_ADDRESS);
      bool atLeastOneReady = (statusAll & (1 << 1)) != 0; // Accessing GLOBAL READY
      bool atLeastOneBusy = (statusAll & (1 << 3)) != 0;  // Accessing GLOBAL BUSY
      if (atLeastOneReady && !atLeastOneBusy) {
        return true;
      }
      // ------------------
      */

      // ----- Option 2 -----
      bool allReady = true;
      bool anyBusy = false;
      for (const auto& board : m_boards) {
        const uint16_t statusb = ReadReg(V792_STATUS_1_REG, board.baseAddr);
        bool thisReady = (statusb & 1) != 0;       // Accessing READY
        bool thisBusy = (statusb & (1 << 2)) != 0; // Accessing BUSY
        allReady &= thisReady;
        anyBusy |= thisBusy;
      }
      if (allReady && !anyBusy) {
        return true;
      }
      // --------------------

      std::this_thread::sleep_for(std::chrono::microseconds(sleep_cycle_time_us));
    }
    return false;
  }

  void MainLoop() {
    while (m_running) {
      if (!ControllerIsReady()) {
        continue;
      }

      const V977Pattern pattern = ReadV977FlipFlopPattern();

      if (pattern.spillStart && pattern.spillEnd && !pattern.trigger) {
        // Every events clears the flip flops. So, if here, there was a spill without events
        HIDRA_WARN("Passed through spill {} with no events", m_spillCount);
        m_spillCount++;
        ClearV977FlipFlops();
      }


      if (pattern.trigger){
        m_evtTimeNs = hidra::utils::getTimens();
        m_TriggerMask = 0x0;
        if (pattern.physics) m_TriggerMask |= 0b01;
        if (pattern.pedestal) m_TriggerMask |= 0b10;
        if (m_TriggerMask == 0b11) {
          HIDRA_WARN("Both ped and phy signals were latched for this evt {}. This will be reported in the trigger mask", m_evt);
        }
        
        bool eventHandlingOk = ReadOneBlockAndSendEvent();
        m_evt++;
        if (m_TriggerMask == 0b01) m_evt_phy++;
        if (m_TriggerMask == 0b10) m_evt_ped++;
      

        // TODO: veto is still active and we can do what we want.. but slowing down. Remove and create a dedicated thread
        SetStatusTag("PhyTrigN", std::to_string(m_evt_phy));
        SetStatusTag("PedTrigN", std::to_string(m_evt_ped));
        SetStatusTag("SpillN", std::to_string(m_spillCount));
        SendStatus();
        HIDRA_DEBUG("Evt {} mask {}. Phy {} Ped {} Spill {}", m_evt, m_TriggerMask, m_evt_phy, m_evt_ped, m_spillCount);
        /////////////////
      
        // Set pedestal veto if we want pedestal next;
        SetSingleV977OutputReg(!requestPedestalNext(), V977OUT::cPedVeto); // TODO r-m-w not needed if this is the only controlled output

        if (pattern.spillStart) {
          m_spillCount++;
        }
        ClearV977FlipFlops(); // this will release the Trigger veto and clear the spill pattern as well
        ReleaseTriggerVeto();
      }

      // std::this_thread::sleep_for(std::chrono::microseconds(5)); // TODO: add a sleep here?
      
    }
  }

  bool ControllerIsReady() {
    if (m_handle >= 0) { // TODO: what is exactly m_handle ?
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return false;
  }

  V977Pattern ReadV977FlipFlopPattern() {
    V977Pattern pattern;
    pattern.raw = ReadReg(V977_SINGLE_READ_REG, m_v977Base);
    pattern.trigger = (pattern.raw & (1u << V977IN::cFastGate)) != 0;
    pattern.physics = (pattern.raw & (1u << V977IN::cPhy)) != 0;
    pattern.pedestal = (pattern.raw & (1u << V977IN::cPed)) != 0;
    pattern.spillStart = (pattern.raw & (1u << V977IN::cSpillStart)) != 0;
    pattern.spillEnd = (pattern.raw & (1u << V977IN::cSpillEnd)) != 0;
    return pattern;
  }

 
  void ClearV977FlipFlops() {
    WriteReg(V977_OUTPUT_CLEAR_REG, 0xFFFF, m_v977Base);
    ThrowIfVmeError("V977 OUTPUT CLEAR write failed");
  }

  

  void OpenController() {
    if (m_handle >= 0) {
      return;
    }

    uint32_t pid = m_pid;
    const CVErrorCodes ret = CAENVME_Init2(m_controllerType, &pid, 0, &m_handle);
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

  uint16_t ReadReg(uint16_t regAddr, uint32_t baseAddr) {
    uint16_t data = 0;
    const CVErrorCodes ret = CAENVME_ReadCycle(m_handle, baseAddr + regAddr, &data, cvA32_U_DATA, cvD16);
    if (ret != cvSuccess) {
      m_errorString = "Cannot read at address " + hex32(baseAddr + regAddr);
      m_vmeError = true;
    }
    return data;
  }

  void WriteReg(uint16_t regAddr, uint16_t data, uint32_t baseAddr) {
    const CVErrorCodes ret = CAENVME_WriteCycle(m_handle, baseAddr + regAddr, &data, cvA32_U_DATA, cvD16);
    if (ret != cvSuccess) {
      m_errorString = "Cannot write at address " + hex32(baseAddr + regAddr);
      m_vmeError = true;
    }
  }

  void ThrowIfVmeError(const std::string& context) {
    if (m_vmeError) {
      EUDAQ_THROW(context + ": " + m_errorString);
    }
  }

  void InitBoard(const BoardConfig& board) {
    EUDAQ_INFO("Initializing board at " + hex32(board.baseAddr));

    WriteReg(V792_GEO_ADDRESS_REG, board.geoAddr, board.baseAddr);
    WriteReg(V792_BIT_SET_1_REG, 0x80, board.baseAddr);
    ThrowIfVmeError("Board reset failed");

    WriteReg(V792_BIT_CLEAR_1_REG, 0x80, board.baseAddr);

    const int model = (ReadReg(V792_MODEL_LOW_REG, board.baseAddr) & 0xFF) +
                      ((ReadReg(V792_MODEL_HIGH_REG, board.baseAddr) & 0xFF) << 8);
    EUDAQ_INFO("Board model at " + hex32(board.baseAddr) + ": " + std::to_string(model));

    WriteReg(V792_CRATE_SELECT_REG, board.crateNr, board.baseAddr);
    WriteReg(V792_IPED_REG, static_cast<uint16_t>(m_iped), board.baseAddr);
    WriteReg(V792_CONTROL_1_REG, 0x60, board.baseAddr);
    WriteReg(V792_BIT_SET_2_REG, 0x0010, board.baseAddr);
    WriteReg(V792_BIT_SET_2_REG, 0x0008, board.baseAddr);
    WriteReg(V792_BIT_SET_2_REG, 0x1000, board.baseAddr);
    WriteReg(V792_EVENT_COUNTER_RESET_REG, 0x0, board.baseAddr);
    WriteReg(V792_BIT_SET_2_REG, 0x4, board.baseAddr);
    WriteReg(V792_BIT_CLEAR_2_REG, 0x4004, board.baseAddr);

    const uint16_t bitSet2 = ReadReg(V792_BIT_SET_2_REG, board.baseAddr);
    EUDAQ_INFO("Board programmed 0x" + hex16(bitSet2));
  }

  bool ReadOneBlockAndSendEvent() { // return false if errors

    int ready_timeout_us = 100; // TODO: test then move to config
    int ready_cycle_us = 10;

    if (!CheckBoardsReady(ready_timeout_us, ready_cycle_us)) {
      HIDRA_ERROR("QDC data not ready after {} us", ready_timeout_us);
      return false;
    }

    int byteCount = 0;
    const CVErrorCodes ret = CAENVME_FIFOMBLTReadCycle(m_handle,
                                                       BLT_READ_ADDRESS,
                                                       reinterpret_cast<char*>(m_buffer.data()),
                                                       static_cast<int>(MAX_BLT_SIZE),
                                                       cvA32_U_MBLT,
                                                       &byteCount);

    if (ret != cvSuccess && ret != cvBusError) {
      EUDAQ_ERROR("BLT Error: " + std::to_string(ret));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (byteCount <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      EUDAQ_ERROR("BCNT = 0, controller replied with 0 bytes");
      return false;
    }

    const int wordCount = byteCount / 4;
    if (wordCount <= 0) {
      EUDAQ_ERROR("BCNT = 0, controller replied with less than 4 bytes");
      return false;
    }

    HIDRA_INFO("Event Triggered, m_evt = {}", m_evt);

    SendDataEvent(byteCount);
    return true;
  }

  void SendDataEvent(int byteCount) {
    auto event = eudaq::Event::MakeUnique("CAENQTPDRaw");
    event->SetTriggerN(static_cast<uint32_t>(m_evt));
    event->SetEventN(static_cast<uint32_t>(m_evt));
    event->SetRunN(static_cast<uint32_t>(m_runNumber));

    const uint8_t* rawBegin = reinterpret_cast<const uint8_t*>(m_buffer.data());
    const std::vector<uint8_t> raw(rawBegin, rawBegin + byteCount);
    if (byteCount != raw.size()) {
      HIDRA_ERROR("Event supposed to have {} bytes. Block with {} bytes", byteCount, raw.size());
    }
    event->AddBlock(0, raw);

    event->SetTag("spillNumber", std::to_string(m_spillCount));
    event->SetTag("triggerMask", std::to_string(m_TriggerMask));
    event->SetTag("endianness", "BE32");
    event->SetTimestamp(m_evtTimeNs, m_evtTimeNs + 100ULL);
    event->SetTag("detectorDataSize", std::to_string(raw.size()));
    SendEvent(std::move(event));
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

    m_runStart = std::chrono::steady_clock::now();
    SendEvent(std::move(bore));
  }

  void SendEORE() {
    auto eore = eudaq::Event::MakeUnique("CAENQTPRaw");
    eore->SetEORE();
    eore->SetRunN(static_cast<uint32_t>(m_runNumber));
    eore->SetTag("EventsSent", std::to_string(m_evt));

    const auto runStop = std::chrono::steady_clock::now();
    const auto elapsed = runStop - m_runStart;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    EUDAQ_INFO("The run has lasted: " + std::to_string(seconds) + " seconds.");

    SendEvent(std::move(eore));
  }

  void StopAcquisitionThread() {
    m_running = false;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void AddBoardFromConf(const eudaq::Configuration& conf, std::size_t index, const BoardDefaults& defaults) {
    const std::string prefix = "Board" + std::to_string(index) + ".";
    const std::string enable = conf.Get(prefix + "Enable", std::string("0"));
    if (is_disabled(enable)) {
      return;
    }

    BoardConfig board;
    board.baseAddr = parse_u32(conf.Get(prefix + "BaseAddress", std::string(defaults.baseAddr)));
    board.geoAddr = parse_u16(conf.Get(prefix + "GeoAddress", std::string(defaults.geoAddr)));
    board.crateNr = parse_u16(conf.Get(prefix + "CrateNumber", std::string(defaults.crateNr)));
    m_boards.push_back(board);
  }

  void ResetRunState() {
    m_running = false;
    m_onspill = false;
    m_clearRequested = false;
    m_evt = m_evt_ped = m_evt_phy = 0;
    m_spillCount = 0;
    m_evtTimeNs = 0;
    m_spillWaitLogCounter = 0;
    m_triggerWaitLogCounter = 0;
  }

  void ResetReadoutBuffers() {
    m_buffer.fill(0);
    m_buffer[0] = DATATYPE_FILLER;
  }

private:
  int32_t m_handle;
  bool m_vmeError;
  std::string m_errorString;

  bool m_onspill;
  bool m_clearRequested;
  uint32_t m_v977Base = 0;

  std::atomic<bool> m_running;

  uint32_t m_runNumber;
  uint64_t m_evt;
  uint64_t m_evt_phy;
  uint64_t m_evt_ped;
  uint32_t m_spillCount; // TODO to be implemented
  int m_iped;
  uint64_t m_evtTimeNs = 0;
  uint8_t m_TriggerMask = 0xFF; // TODO: already forwarded as tag. To be implemented!

  int m_spillWaitLogCounter = 0;
  int m_triggerWaitLogCounter = 0;

  std::chrono::steady_clock::time_point m_runStart;

  CVBoardTypes m_controllerType;
  uint32_t m_pid;

  std::vector<BoardConfig> m_boards;
  std::array<uint32_t, 256 * 1024 / 4> m_buffer;

  std::thread m_thread;
  std::mutex m_v977OutputSetMutex; // Without the mutex, two threads could both read the same old OUTPUT SET value,
                                   // modify different bits, and whichever writes last would accidentally erase the
                                   // other thread’s change.
};

namespace {
auto dummy0 = eudaq::Factory<eudaq::Producer>::Register<HidraQTPDProducer, const std::string&, const std::string&>(
    HidraQTPDProducer::m_id_factory);
}
