#ifndef HIDRA_DRY_XDC_PRODUCER_TEST_FIXTURES_HH
#define HIDRA_DRY_XDC_PRODUCER_TEST_FIXTURES_HH

#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdint>

namespace fs = std::filesystem;

/**
 * Utility class for generating XDC test data files
 */
class XDCTestDataGenerator {
public:
  /**
   * Generate a valid XDC event with specified parameters
   *
   * @param num_data_words Number of data words (0x00 to 0xFF)
   * @param event_number Event number to embed
   * @param spill_number Spill number to embed
   * @return Vector of uint32_t representing a complete XDC event
   */
  static std::vector<uint32_t> GenerateValidEvent(
      uint32_t num_data_words,
      uint32_t event_number = 1,
      uint32_t spill_number = 0) {
    
    constexpr uint32_t XDC_EVENT_MARKER = 0xccaaffee;
    constexpr uint32_t XDC_HEADER_END_MARKER = 0xaccadead;
    constexpr uint32_t XDC_EVENT_TRAILER = 0xbbeeddaa;
    constexpr uint32_t XDC_HEADER_WORDS = 14u;
    constexpr uint32_t XDC_TRAILER_WORDS = 1u;

    uint32_t event_size = XDC_HEADER_WORDS + XDC_TRAILER_WORDS + num_data_words;

    std::vector<uint32_t> event;
    event.reserve(event_size);

    // Header (14 words)
    event.push_back(XDC_EVENT_MARKER);           // [0] marker
    event.push_back(event_number);               // [1] event_number
    event.push_back(spill_number);               // [2] spill_number
    event.push_back(XDC_HEADER_WORDS);           // [3] header_size
    event.push_back(XDC_TRAILER_WORDS);          // [4] trailer_size
    event.push_back(num_data_words);             // [5] data_size
    event.push_back(event_size);                 // [6] event_size
    event.push_back(0x00000001);                 // [7] time_sec
    event.push_back(0x00000000);                 // [8] time_usec
    event.push_back(0x00000001);                 // [9] trigger_mask
    event.push_back(0x00000000);                 // [10] is_ped_mask
    event.push_back(0x00000000);                 // [11] is_ped_from_scaler
    event.push_back(0x00000000);                 // [12] sanity_flag
    event.push_back(XDC_HEADER_END_MARKER);      // [13] header_end_marker

    // Data words
    for (uint32_t i = 0; i < num_data_words; ++i) {
      event.push_back(0xdeadbeef + i);
    }

    // Trailer
    event.push_back(XDC_EVENT_TRAILER);

    return event;
  }

  /**
   * Write event(s) to a file in ASCII hex format
   *
   * @param filepath Path to output file
   * @param events Vector of event vectors to write
   * @return true on success, false on failure
   */
  static bool WriteEventsToFile(
      const fs::path& filepath,
      const std::vector<std::vector<uint32_t>>& events) {
    
    std::ofstream file(filepath);
    if (!file.is_open()) {
      return false;
    }

    for (const auto& event : events) {
      for (size_t i = 0; i < event.size(); ++i) {
        file << std::hex << std::uppercase << std::setfill('0') << std::setw(8)
             << event[i];
        if (i < event.size() - 1) {
          file << " ";
        }
      }
      file << "\n";
    }

    file.close();
    return true;
  }

  /**
   * Write a single event to file
   */
  static bool WriteEventToFile(
      const fs::path& filepath,
      const std::vector<uint32_t>& event) {
    
    std::vector<std::vector<uint32_t>> events = {event};
    return WriteEventsToFile(filepath, events);
  }

  /**
   * Generate and write multiple valid events
   *
   * @param filepath Path to output file
   * @param num_events Number of events to generate
   * @param num_data_words Data words per event
   * @return true on success
   */
  static bool GenerateValidFile(
      const fs::path& filepath,
      uint32_t num_events = 3,
      uint32_t num_data_words = 32) {
    
    std::vector<std::vector<uint32_t>> events;
    for (uint32_t i = 0; i < num_events; ++i) {
      events.push_back(GenerateValidEvent(num_data_words, i + 1, 0));
    }
    
    return WriteEventsToFile(filepath, events);
  }

  /**
   * Generate file with corrupted header end marker
   */
  static bool GenerateBadHeaderEndMarkerFile(const fs::path& filepath) {
    auto event = GenerateValidEvent(32, 1, 0);
    event[13] = 0xffffffff;  // Corrupt header end marker
    return WriteEventToFile(filepath, event);
  }

  /**
   * Generate file with invalid event size
   */
  static bool GenerateBadEventSizeFile(const fs::path& filepath) {
    auto event = GenerateValidEvent(32, 1, 0);
    event[6] = 0xdeadbeef;  // Corrupt event size
    return WriteEventToFile(filepath, event);
  }
};

/**
 * RAII wrapper for temporary test files
 */
class TemporaryXDCFile {
public:
  explicit TemporaryXDCFile(const fs::path& directory, const std::string& name)
      : m_filepath(directory / name) {
    fs::create_directories(directory);
  }

  ~TemporaryXDCFile() {
    if (fs::exists(m_filepath)) {
      fs::remove(m_filepath);
    }
  }

  const fs::path& GetPath() const { return m_filepath; }
  std::string GetPathString() const { return m_filepath.string(); }

  bool WriteEvent(const std::vector<uint32_t>& event) {
    return XDCTestDataGenerator::WriteEventToFile(m_filepath, event);
  }

  bool WriteEvents(const std::vector<std::vector<uint32_t>>& events) {
    return XDCTestDataGenerator::WriteEventsToFile(m_filepath, events);
  }

private:
  fs::path m_filepath;
};

#endif  // HIDRA_DRY_XDC_PRODUCER_TEST_FIXTURES_HH
