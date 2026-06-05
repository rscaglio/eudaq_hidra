#include <eudaq/Configuration.hh>
#include <eudaq/Event.hh>
#include <eudaq/Factory.hh>
#include <eudaq/Logger.hh>
#include <eudaq/Producer.hh>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

// Change these names when the final tracker file format is known.
constexpr std::array<const char*, 10> TRACKER_COLUMNS = {
    "TriggerId",
    "Time stamp",
    "X",
    "Y",
    "Column5",
    "Column6",
    "Column7",
    "Column8",
    "Column9",
    "Column10",
};

constexpr const char* TRIGGER_COLUMN = "TriggerId";
constexpr const char* TIMESTAMP_COLUMN = "Time stamp";

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> Split(const std::string& line, char delimiter) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, delimiter)) {
    fields.push_back(Trim(field));
  }
  if (!line.empty() && line.back() == delimiter) {
    fields.emplace_back();
  }
  return fields;
}

std::string JoinColumns() {
  std::ostringstream stream;
  for (std::size_t index = 0; index < TRACKER_COLUMNS.size(); ++index) {
    if (index != 0) {
      stream << ',';
    }
    stream << TRACKER_COLUMNS[index];
  }
  return stream.str();
}

uint64_t ParseUnsigned(const std::string& value, const std::string& column, const std::filesystem::path& file,
                       std::size_t line_number) {
  std::size_t parsed = 0;
  try {
    const auto result = std::stoull(value, &parsed, 0);
    if (parsed != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return result;
  } catch (const std::exception&) {
    EUDAQ_THROW("Cannot parse column '" + column + "' at " + file.string() + ":" + std::to_string(line_number) +
                ": " + value);
  }
}

std::size_t FindColumn(const std::vector<std::string>& headers, const std::string& column) {
  const auto found = std::find(headers.begin(), headers.end(), column);
  if (found == headers.end()) {
    EUDAQ_THROW("Required tracker column is missing: " + column);
  }
  return static_cast<std::size_t>(found - headers.begin());
}

} // namespace

class HidraTrackerProducer : public eudaq::Producer {
public:
  HidraTrackerProducer(const std::string& name, const std::string& runcontrol)
      : eudaq::Producer(name, runcontrol) {}

  ~HidraTrackerProducer() override {
    StopWorker();
  }

  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraTrackerProducer");

private:
  void DoInitialise() override {}

  void DoConfigure() override {
    const auto conf = GetConfiguration();
    if (!conf) {
      EUDAQ_THROW("Run configuration is missing");
    }

    EUDAQ_LOG_LEVEL((int)(conf->Get("HIDRA_MUTE_DEBUG", 0)));
    m_directory = conf->Get("TRACKER_DIRECTORY", std::string(""));
    if (m_directory.empty()) {
      EUDAQ_THROW("TRACKER_DIRECTORY is missing from the run configuration");
    }
    if (!std::filesystem::is_directory(m_directory)) {
      EUDAQ_THROW("TRACKER_DIRECTORY is not a directory: " + m_directory.string());
    }

    const std::string delimiter = conf->Get("TRACKER_DELIMITER", std::string(","));
    if (delimiter.size() != 1) {
      EUDAQ_THROW("TRACKER_DELIMITER must contain exactly one character");
    }
    m_delimiter = delimiter.front();
    m_extension = conf->Get("TRACKER_FILE_EXTENSION", std::string(".csv"));
    m_poll_interval_ms = std::max(1, conf->Get("TRACKER_POLL_INTERVAL_MS", 100));
    m_timestamp_scale_ns = conf->Get("TRACKER_TIMESTAMP_SCALE_NS", uint64_t{1});
    if (m_timestamp_scale_ns == 0) {
      EUDAQ_THROW("TRACKER_TIMESTAMP_SCALE_NS must be greater than zero");
    }

    EUDAQ_INFO("HidraTrackerProducer watching " + m_directory.string());
  }

  void DoStartRun() override {
    StopWorker();
    m_run_number = GetRunNumber();
    m_events_sent = 0;
    m_processed_files.clear();
    m_observed_sizes.clear();

    auto bore = eudaq::Event::MakeUnique("TrackerRaw");
    bore->SetBORE();
    bore->SetRunN(static_cast<uint32_t>(m_run_number));
    bore->SetTag("Producer", "HidraTrackerProducer");
    bore->SetTag("Columns", JoinColumns());
    bore->SetTag("Directory", m_directory.string());
    SendEvent(std::move(bore));

    m_running = true;
    m_worker = std::thread(&HidraTrackerProducer::MainLoop, this);
    EUDAQ_INFO("Starting HidraTrackerProducer run " + std::to_string(m_run_number));
  }

  void DoStopRun() override {
    StopWorker();

    auto eore = eudaq::Event::MakeUnique("TrackerRaw");
    eore->SetEORE();
    eore->SetRunN(static_cast<uint32_t>(m_run_number));
    eore->SetTag("EventsSent", std::to_string(m_events_sent));
    SendEvent(std::move(eore));
    EUDAQ_INFO("Stopping HidraTrackerProducer run " + std::to_string(m_run_number));
  }

  void DoReset() override {
    StopWorker();
    m_events_sent = 0;
    m_processed_files.clear();
    m_observed_sizes.clear();
  }

  void DoTerminate() override {
    StopWorker();
  }

  void MainLoop() {
    while (m_running) {
      try {
        ScanDirectory();
      } catch (const std::exception& error) {
        EUDAQ_ERROR(std::string("Tracker directory scan failed: ") + error.what());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(m_poll_interval_ms));
    }
  }

  void ScanDirectory() {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(m_directory)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (!m_extension.empty() && entry.path().extension() != m_extension) {
        continue;
      }
      files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
      const auto key = file.string();
      if (m_processed_files.count(key) != 0) {
        continue;
      }

      const auto size = std::filesystem::file_size(file);
      const auto previous = m_observed_sizes.find(key);
      if (previous == m_observed_sizes.end() || previous->second != size) {
        m_observed_sizes[key] = size;
        continue;
      }

      ProcessFile(file);
      m_processed_files.insert(key);
      m_observed_sizes.erase(key);
    }
  }

  void ProcessFile(const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input.is_open()) {
      EUDAQ_THROW("Cannot open tracker file: " + file.string());
    }

    std::string line;
    if (!std::getline(input, line)) {
      EUDAQ_WARN("Ignoring empty tracker file: " + file.string());
      return;
    }

    const auto headers = Split(line, m_delimiter);
    ValidateHeaders(headers, file);

    std::size_t line_number = 1;
    while (m_running && std::getline(input, line)) {
      ++line_number;
      if (Trim(line).empty()) {
        continue;
      }

      const auto values = Split(line, m_delimiter);
      if (values.size() != headers.size()) {
        EUDAQ_WARN("Ignoring tracker row with " + std::to_string(values.size()) + " columns at " + file.string() +
                   ":" + std::to_string(line_number) + "; expected " + std::to_string(headers.size()));
        continue;
      }
      SendRow(headers, values, line, file, line_number);
    }

    EUDAQ_INFO("Processed tracker file " + file.string());
  }

  void ValidateHeaders(const std::vector<std::string>& headers, const std::filesystem::path& file) const {
    if (headers.size() != TRACKER_COLUMNS.size()) {
      EUDAQ_THROW("Tracker file " + file.string() + " has " + std::to_string(headers.size()) +
                  " columns; expected " + std::to_string(TRACKER_COLUMNS.size()));
    }
    for (std::size_t index = 0; index < headers.size(); ++index) {
      if (headers[index] != TRACKER_COLUMNS[index]) {
        EUDAQ_THROW("Tracker file " + file.string() + " column " + std::to_string(index + 1) + " is '" +
                    headers[index] + "'; expected '" + TRACKER_COLUMNS[index] + "'");
      }
    }
  }

  void SendRow(const std::vector<std::string>& headers, const std::vector<std::string>& values, const std::string& line,
               const std::filesystem::path& file, std::size_t line_number) {
    const auto trigger_index = FindColumn(headers, TRIGGER_COLUMN);
    const auto timestamp_index = FindColumn(headers, TIMESTAMP_COLUMN);
    const uint64_t trigger = ParseUnsigned(values[trigger_index], TRIGGER_COLUMN, file, line_number);
    const uint64_t timestamp = ParseUnsigned(values[timestamp_index], TIMESTAMP_COLUMN, file, line_number);

    if (trigger > std::numeric_limits<uint32_t>::max()) {
      EUDAQ_THROW("TriggerId is larger than the EUDAQ uint32 trigger number at " + file.string() + ":" +
                  std::to_string(line_number));
    }
    if (timestamp > (std::numeric_limits<uint64_t>::max() - 1) / m_timestamp_scale_ns) {
      EUDAQ_THROW("Time stamp overflows after TRACKER_TIMESTAMP_SCALE_NS at " + file.string() + ":" +
                  std::to_string(line_number));
    }

    auto event = eudaq::Event::MakeUnique("TrackerRaw");
    event->SetRunN(static_cast<uint32_t>(m_run_number));
    event->SetTriggerN(static_cast<uint32_t>(trigger));
    event->SetEventN(static_cast<uint32_t>(trigger));
    const uint64_t timestamp_ns = timestamp * m_timestamp_scale_ns;
    event->SetTimestamp(timestamp_ns, timestamp_ns + 1);
    event->SetTag("Producer", "HidraTrackerProducer");
    event->SetTag("SourceFile", file.filename().string());
    for (std::size_t index = 0; index < headers.size(); ++index) {
      event->SetTag(headers[index], values[index]);
    }

    const std::vector<uint8_t> raw_row(line.begin(), line.end());
    event->AddBlock(0, raw_row);
    SendEvent(std::move(event));
    ++m_events_sent;
  }

  void StopWorker() {
    m_running = false;
    if (m_worker.joinable()) {
      m_worker.join();
    }
  }

  std::filesystem::path m_directory;
  std::string m_extension = ".csv";
  char m_delimiter = ',';
  int m_poll_interval_ms = 100;
  uint64_t m_timestamp_scale_ns = 1;
  std::atomic<bool> m_running{false};
  std::thread m_worker;
  uint32_t m_run_number = 0;
  uint64_t m_events_sent = 0;
  std::unordered_set<std::string> m_processed_files;
  std::map<std::string, uintmax_t> m_observed_sizes;
};

namespace {
auto dummy0 = eudaq::Factory<eudaq::Producer>::Register<HidraTrackerProducer, const std::string&, const std::string&>(
    HidraTrackerProducer::m_id_factory);
}
