#include <eudaq/Producer.hh>
#include <eudaq/Event.hh>
#include <eudaq/Factory.hh>
#include <eudaq/Logger.hh>
#include "HidraUtils.hh"

#include <fstream>
#include <unordered_set>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cstdint>
#include <stdexcept>
#include <filesystem>

namespace hidra {

// Fixed constants from EventSerializer (Data Format Version 11)
constexpr std::uint16_t EVENT_MARKER = 0xB0BF;
constexpr std::uint16_t EVENT_HEADER_ENDMARKER = 0xBBBB;
constexpr std::uint16_t DETECTOR_EVENT_MARKER = 0xDEDE;
constexpr std::uint16_t DETECTOR_EVENT_ENDMARKER = 0xDDDD;
constexpr std::uint16_t EVENT_TRAILER = 0xD04E;
constexpr size_t MAX_N_DETECTORS = 8;

// Little-endian deserialization helper utilities
template <typename T>
T ReadLE(const std::vector<std::uint8_t>& buffer, size_t offset) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= (static_cast<T>(buffer[offset + i]) << (8 * i));
    }
    return value;
}

// Structure to hold individual subdetector records extracted from raw files
struct UnpackedSubEvent {
    uint8_t detID;
    uint32_t triggerN;
    uint32_t spillNumber;
    uint64_t timestampBegin;
    uint64_t nativeTimestampBegin;
    uint8_t triggerMask;
    uint8_t endianness;
    std::vector<uint8_t> payload;
};

class HidraDryProducer : public eudaq::Producer {
public:
    HidraDryProducer(const std::string& name, const std::string& runcontrol)
        : eudaq::Producer(name, runcontrol), m_running(false) {}

    ~HidraDryProducer() override {
        StopPlayback();
    }

    static const uint32_t m_id_factory = eudaq::cstr2hash("HidraDryProducer");

private:
    void DoInitialise() override {}

    void DoConfigure() override {
        auto conf = GetConfiguration();
        if (!conf) {
            HIDRA_THROW("DryProducer configuration parameters are missing!");
        }

        EUDAQ_LOG_LEVEL((int)(conf->Get("HIDRA_MUTE_DEBUG", 1)));

        // Path to the recorded raw storage binary file to play back
        m_raw_file_path = conf->Get("DRY_INPUT_RAW_FILE", "");
        if (m_raw_file_path.empty() || !std::filesystem::exists(m_raw_file_path)) {
            HIDRA_THROW("DRY_INPUT_RAW_FILE target is missing or unreadable: " + m_raw_file_path);
        }

        // Parse standard config: "3:TrackerProducer,2:FERS2Producer,1:QTPDProducer"
        std::string sources_cfg = conf->Get("EXPECTED_SOURCES", "");
        m_configured_source_ids.clear();
        
        if (!sources_cfg.empty()) {
            std::map<std::string, std::string> parsed_map = hidra::utils::parseConfigMap(sources_cfg);
            for (const auto& kv : parsed_map) {
                int detID = std::stoi(kv.first);
                // We only handle this subdetector instance if our active instance name matches the config token
                if (kv.second == GetName()) {
                    m_configured_source_ids.insert(detID);
                    HIDRA_INFO("DryProducer [" + GetName() + "] successfully bound to Detector ID: " + std::to_string(detID));
                }
            }
        }

        if (m_configured_source_ids.empty()) {
            HIDRA_WARN("DryProducer [" + GetName() + "] has no matching DetID entries inside EXPECTED_SOURCES");
        }

        // Playback timing throttle setup
        m_throttle_playback = conf->Get("DRY_THROTTLE_PLAYBACK", 1);
    }

    void DoStartRun() override {
        StopPlayback();
        m_run_number = GetRunNumber();
        m_running = true;

        // Send standard framework Beginning-Of-Run Event
        auto bore = eudaq::Event::MakeUnique("RawEvent");
        bore->SetBORE();
        bore->SetRunN(m_run_number);
        bore->SetTag("Producer", GetName());
        SendEvent(std::move(bore));

        // Launch decoupled binary stream parsing worker
        m_worker_thread = std::thread(&HidraDryProducer::PlaybackLoop, this);
        HIDRA_INFO("DryProducer [" + GetName() + "] began replaying run data from " + m_raw_file_path);
    }

    void DoStopRun() override {
        StopPlayback();
        
        // Send standard framework End-Of-Run Event
        auto eore = eudaq::Event::MakeUnique("RawEvent");
        eore->SetEORE();
        eore->SetRunN(m_run_number);
        SendEvent(std::move(eore));
        HIDRA_INFO("DryProducer [" + GetName() + "] stopped playback processing.");
    }

    void DoReset() override { StopPlayback(); }
    void DoTerminate() override { StopPlayback(); }

    void StopPlayback() {
        m_running = false;
        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }
    }

    void PlaybackLoop() {
        std::ifstream file(m_raw_file_path, std::ios::binary);
        if (!file.is_open()) {
            HIDRA_ERROR("Playback loop failed to read binary stream mapping: " + m_raw_file_path);
            return;
        }

        uint64_t last_event_ts = 0;
        auto last_wall_clock = std::chrono::steady_clock::now();

        while (m_running && file.peek() != EOF) {
            try {
                // Read next Global Event Package
                std::vector<uint8_t> header_bytes(15);
                if (!file.read(reinterpret_cast<char*>(header_bytes.data()), 15)) break;

                std::uint16_t marker = ReadLE<std::uint16_t>(header_bytes, 0);
                if (marker != EVENT_MARKER) {
                    HIDRA_ERROR("Stream corrupted! Expected Event Marker 0xB0BF, found: 0x" + eudaq::to_hex(marker));
                    break;
                }

                uint32_t total_event_size = ReadLE<uint32_t>(header_bytes, 11);
                
                // Pull remaining payload body matching global event package dimensions
                std::vector<uint8_t> complete_packet(total_event_size);
                std::memcpy(complete_packet.data(), header_bytes.data(), 15);
                
                if (!file.read(reinterpret_cast<char*>(complete_packet.data() + 15), total_event_size - 15)) {
                    HIDRA_ERROR("Unexpected EOF while parsing global event subdetector cluster chunks.");
                    break;
                }

                // Identify header boundaries to skip cleanly to SubDetector arrays
                uint32_t header_size = ReadLE<uint32_t>(complete_packet, 3);
                size_t anchorpoint_detsize = 47; // Matches index location inside Serialize() implementation

                // Parse nested subdetectors
                size_t current_offset = header_size;
                
                while (current_offset < (total_event_size - 2)) {
                    std::uint16_t det_marker = ReadLE<std::uint16_t>(complete_packet, current_offset);
                    if (det_marker == EVENT_TRAILER) {
                        break;
                    }
                    if (det_marker != DETECTOR_EVENT_MARKER) {
                        current_offset += 1; // Scan boundary alignment fallback
                        continue;
                    }

                    uint8_t parsed_detID = complete_packet[current_offset + 2];
                    uint16_t expected_block_len = ReadLE<uint16_t>(complete_packet, anchorpoint_detsize + 2 * parsed_detID);

                    // Verify if this subdetector matches the ID mask assigned to this specific producer instance
                    if (m_configured_source_ids.count(parsed_detID) != 0) {
                        UnpackedSubEvent subEv;
                        subEv.detID = parsed_detID;
                        subEv.triggerN = ReadLE<uint32_t>(complete_packet, current_offset + 3);
                        subEv.spillNumber = ReadLE<uint32_t>(complete_packet, current_offset + 7);
                        subEv.timestampBegin = ReadLE<uint64_t>(complete_packet, current_offset + 11);
                        subEv.nativeTimestampBegin = ReadLE<uint64_t>(complete_packet, current_offset + 19);
                        subEv.triggerMask = complete_packet[current_offset + 29];
                        subEv.endianness = complete_packet[current_offset + 30];

                        // Slice precise hardware block array payload boundaries
                        size_t payload_start = current_offset + 31;
                        size_t payload_end = current_offset + expected_block_len - 2; 
                        
                        if (payload_end >= payload_start && payload_end < complete_packet.size()) {
                            subEv.payload.assign(complete_packet.begin() + payload_start, complete_packet.begin() + payload_end);
                            
                            // Reconstruct and dispatch isolated framework data representation
                            DispatchEmulatedEvent(subEv);

                            // Optional playback throttling using hardware clock delta step timing emulation
                            if (m_throttle_playback && last_event_ts != 0 && subEv.timestampBegin > last_event_ts) {
                                uint64_t delta_ns = subEv.timestampBegin - last_event_ts;
                                // Sanity guard capping throttle step window at 1.5 seconds maximum
                                if (delta_ns < 1500000000ULL) {
                                    std::this_thread::sleep_for(std::chrono::nanoseconds(delta_ns));
                                }
                            }
                            last_event_ts = subEv.timestampBegin;
                        }
                    }

                    // Advance cursor loop past subdetector data envelope limits
                    current_offset += expected_block_len;
                }

            } catch (const std::exception& e) {
                HIDRA_ERROR("Exception caught during dry playback evaluation loop processing: " + std::string(e.what()));
                break;
            }
        }
        HIDRA_INFO("DryProducer finished processing file stream data.");
    }

    void DispatchEmulatedEvent(const UnpackedSubEvent& subEv) {
        // Build customized framework raw data package container
        auto ev = eudaq::Event::MakeUnique("TrackerRaw"); // Standard naming parsed by DataCollector schema 
        ev->SetRunN(m_run_number);
        ev->SetTriggerN(subEv.triggerN);
        
        // Preserve timing fields at detector precision level
        ev->SetTimestamp(subEv.timestampBegin, subEv.timestampBegin + 1);
        
        // Populate standard tracking and alignment string tags
        ev->SetTag("Producer", GetName());
        ev->SetTag("detID", std::to_string(subEv.detID));
        ev->SetTag("spillNumber", std::to_string(subEv.spillNumber));
        ev->SetTag("triggerMask", std::to_string(subEv.triggerMask));
        ev->SetTag("endianness", (subEv.endianness == 0x01) ? "LE" : "BE");
        
        if (subEv.nativeTimestampBegin != std::numeric_limits<uint64_t>::max()) {
            ev->SetTag("nativeTimestampBegin", std::to_string(subEv.nativeTimestampBegin));
        }

        // Reinsert the intact payload block
        if (!subEv.payload.empty()) {
            ev->AddBlock(0, subEv.payload.data(), subEv.payload.size());
            ev->SetTag("detectorDataSize", std::to_string(subEv.payload.size()));
        } else {
            ev->SetTag("detectorDataSize", "0");
        }

        // Transmit out to listening DataCollector
        SendEvent(std::move(ev));
    }

    std::string m_raw_file_path;
    std::unordered_set<int> m_configured_source_ids;
    std::atomic<bool> m_running;
    std::thread m_worker_thread;
    uint32_t m_run_number;
    int m_throttle_playback;
};

namespace {
auto dummy = eudaq::Factory<eudaq::Producer>::Register<HidraDryProducer, const std::string&, const std::string&>(
    HidraDryProducer::m_id_factory);
}

} // namespace hidra