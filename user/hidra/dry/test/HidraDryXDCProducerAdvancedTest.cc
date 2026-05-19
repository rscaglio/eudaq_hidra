// Tests for HidraDryXDCProducer — edge cases and robustness checks.
//
// Covers: varying data sizes, spill numbers, malformed headers, large events,
// sequential EOF handling, payload content, consecutive ReadFileSize calls,
// file-reopen behaviour, and timestamp field values.
//
// Run with:  ctest --output-on-failure -R HidraDryXDCProducerAdvancedTest
// Binary:    bin/tests/hidra_dry_xdc_producer_advanced_test

#include <gtest/gtest.h>
#include "../include/HidraDryXDCProducer.hh"
#include "HidraDryXDCProducerTestFixtures.hh"
#include <iomanip>

namespace fs = std::filesystem;

class HidraDryXDCProducerAdvancedTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir = fs::temp_directory_path() / "hidra_xdc_advanced_test";
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    fs::remove_all(test_dir);
  }

  fs::path test_dir;
};

// Event with varying data sizes
TEST_F(HidraDryXDCProducerAdvancedTest, EventsWithVaryingDataSizes) {
  TemporaryXDCFile test_file(test_dir, "varying_sizes.xdc");
  
  std::vector<std::vector<uint32_t>> events;
  events.push_back(XDCTestDataGenerator::GenerateValidEvent(0, 1, 0));    // No data
  events.push_back(XDCTestDataGenerator::GenerateValidEvent(16, 2, 0));   // Small
  events.push_back(XDCTestDataGenerator::GenerateValidEvent(256, 3, 0));  // Large
  
  EXPECT_TRUE(test_file.WriteEvents(events));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  for (uint32_t i = 0; i < 3; ++i) {
    std::vector<uint32_t> read_event;
    EXPECT_TRUE(producer.ReadXDCEvent(read_event));
    EXPECT_EQ(read_event[1], i + 1);  // event_number
  }
}

// Multiple spill numbers
TEST_F(HidraDryXDCProducerAdvancedTest, MultipleSpillNumbers) {
  TemporaryXDCFile test_file(test_dir, "spills.xdc");
  
  std::vector<std::vector<uint32_t>> events;
  for (uint32_t spill = 0; spill < 5; ++spill) {
    events.push_back(XDCTestDataGenerator::GenerateValidEvent(32, spill + 1, spill));
  }
  
  EXPECT_TRUE(test_file.WriteEvents(events));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  for (uint32_t spill = 0; spill < 5; ++spill) {
    std::vector<uint32_t> read_event;
    EXPECT_TRUE(producer.ReadXDCEvent(read_event));
    EXPECT_EQ(read_event[2], spill);  // spill_number
  }
}

// Bad header end marker
TEST_F(HidraDryXDCProducerAdvancedTest, BadHeaderEndMarker) {
  TemporaryXDCFile test_file(test_dir, "bad_header_end.xdc");
  EXPECT_TRUE(XDCTestDataGenerator::GenerateBadHeaderEndMarkerFile(test_file.GetPath()));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  std::vector<uint32_t> read_event;
  EXPECT_FALSE(producer.ReadXDCEvent(read_event));
}

// Bad event size
TEST_F(HidraDryXDCProducerAdvancedTest, BadEventSize) {
  TemporaryXDCFile test_file(test_dir, "bad_event_size.xdc");
  EXPECT_TRUE(XDCTestDataGenerator::GenerateBadEventSizeFile(test_file.GetPath()));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  std::vector<uint32_t> read_event;
  EXPECT_FALSE(producer.ReadXDCEvent(read_event));
}

// Large event with maximum data
TEST_F(HidraDryXDCProducerAdvancedTest, LargeEventWithMaximumData) {
  TemporaryXDCFile test_file(test_dir, "large_event.xdc");
  
  auto large_event = XDCTestDataGenerator::GenerateValidEvent(0x1000, 1, 0);
  EXPECT_TRUE(test_file.WriteEvent(large_event));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  std::vector<uint32_t> read_event;
  EXPECT_TRUE(producer.ReadXDCEvent(read_event));
  EXPECT_EQ(read_event[5], 0x1000);  // data_size
  EXPECT_EQ(read_event.size(), 0x1000 + 14 + 1);  // header + data + trailer
}

// Sequential reading with EOF
TEST_F(HidraDryXDCProducerAdvancedTest, SequentialReadingWithEOF) {
  TemporaryXDCFile test_file(test_dir, "sequential.xdc");
  
  std::vector<std::vector<uint32_t>> events;
  for (uint32_t i = 0; i < 100; ++i) {
    events.push_back(XDCTestDataGenerator::GenerateValidEvent(32, i + 1, 0));
  }
  EXPECT_TRUE(test_file.WriteEvents(events));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  uint32_t event_count = 0;
  std::vector<uint32_t> read_event;
  while (producer.ReadXDCEvent(read_event)) {
    ++event_count;
    EXPECT_EQ(read_event[1], event_count);  // event_number should increment
  }
  EXPECT_EQ(event_count, 100);
}

// Data payload verification
TEST_F(HidraDryXDCProducerAdvancedTest, DataPayloadVerification) {
  TemporaryXDCFile test_file(test_dir, "payload.xdc");
  
  auto event = XDCTestDataGenerator::GenerateValidEvent(64, 1, 0);
  EXPECT_TRUE(test_file.WriteEvent(event));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  std::vector<uint32_t> read_event;
  EXPECT_TRUE(producer.ReadXDCEvent(read_event));

  // Verify data payload (starts at header_size, which is 14)
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t expected = 0xdeadbeef + i;
    EXPECT_EQ(read_event[14 + i], expected)
        << "Data word at offset " << i << " mismatch";
  }
}

// Consecutive ReadFileSize calls maintain consistency
TEST_F(HidraDryXDCProducerAdvancedTest, ConsecutiveReadsAfterResize) {
  TemporaryXDCFile test_file(test_dir, "consecutive.xdc");

  auto event = XDCTestDataGenerator::GenerateValidEvent(32, 1, 0);
  EXPECT_TRUE(test_file.WriteEvent(event));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());

  // Two consecutive ReadFileSize calls should both leave the file readable
  producer.ReadFileSize();
  producer.ReadFileSize();

  // Should still be able to read the event after double ReadFileSize
  std::vector<uint32_t> read_event;
  EXPECT_TRUE(producer.ReadXDCEvent(read_event));
  EXPECT_EQ(read_event[1], 1u);
}

// ReadFileSize resets read position
TEST_F(HidraDryXDCProducerAdvancedTest, FileReopeningAfterClose) {
  TemporaryXDCFile test_file(test_dir, "reopen.xdc");

  std::vector<std::vector<uint32_t>> events;
  events.push_back(XDCTestDataGenerator::GenerateValidEvent(32, 1, 0));
  events.push_back(XDCTestDataGenerator::GenerateValidEvent(32, 2, 0));
  EXPECT_TRUE(test_file.WriteEvents(events));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  // Read first event
  std::vector<uint32_t> event1;
  EXPECT_TRUE(producer.ReadXDCEvent(event1));
  EXPECT_EQ(event1[1], 1u);

  // ReadFileSize resets to beginning
  producer.ReadFileSize();

  // Should read first event again
  std::vector<uint32_t> event1_again;
  EXPECT_TRUE(producer.ReadXDCEvent(event1_again));
  EXPECT_EQ(event1_again[1], 1u);
}

// Timestamp fields
TEST_F(HidraDryXDCProducerAdvancedTest, TimestampFields) {
  TemporaryXDCFile test_file(test_dir, "timestamps.xdc");
  
  auto event = XDCTestDataGenerator::GenerateValidEvent(0, 1, 0);
  EXPECT_TRUE(test_file.WriteEvent(event));

  HidraDryXDCProducer producer("Test", "tcp://localhost:44000");
  producer.SetDataPath(test_file.GetPathString());
  producer.ReadFileSize();

  std::vector<uint32_t> read_event;
  EXPECT_TRUE(producer.ReadXDCEvent(read_event));

  // Verify timestamp fields
  uint64_t time_sec = read_event[7];
  uint64_t time_usec = read_event[8];
  EXPECT_EQ(time_sec, 0x00000001);
  EXPECT_EQ(time_usec, 0x00000000);
}

