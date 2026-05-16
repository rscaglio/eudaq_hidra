#include <gtest/gtest.h>
#include "../include/HidraDryXDCProducer.hh"
#include "HidraDryXDCProducerTestFixtures.hh"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class HidraDryXDCProducerTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir = fs::temp_directory_path() / "hidra_xdc_test";
    fs::create_directories(test_dir);

    // Create valid test data file (2 events, 32 data words each)
    valid_data_file = test_dir / "test_valid.xdc";
    {
      auto ev1 = XDCTestDataGenerator::GenerateValidEvent(32, 1, 0);
      auto ev2 = XDCTestDataGenerator::GenerateValidEvent(32, 2, 0);
      XDCTestDataGenerator::WriteEventsToFile(valid_data_file, {ev1, ev2});
    }

    // Create file with invalid start marker (first word != 0xccaaffee)
    invalid_marker_file = test_dir / "test_invalid_marker.xdc";
    {
      auto ev = XDCTestDataGenerator::GenerateValidEvent(32, 1, 0);
      ev[0] = 0xdeadbeef;  // corrupt the marker
      XDCTestDataGenerator::WriteEventsToFile(invalid_marker_file, {ev});
    }

    // Create truncated file (only 2 words written)
    truncated_file = test_dir / "test_truncated.xdc";
    {
      std::ofstream f(truncated_file);
      f << "CCAAFFEE 00000001\n";
    }

    // Create empty file
    empty_file = test_dir / "test_empty.xdc";
    { std::ofstream f(empty_file); }
  }

  void TearDown() override {
    fs::remove_all(test_dir);
  }

  fs::path test_dir;
  fs::path valid_data_file;
  fs::path invalid_marker_file;
  fs::path truncated_file;
  fs::path empty_file;
};

// Test 1: Constructor and basic initialization
TEST_F(HidraDryXDCProducerTest, ConstructorInitialization) {
  EXPECT_NO_THROW({
    HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  });
}

// Test 2: File size reading with valid file
TEST_F(HidraDryXDCProducerTest, ReadFileSizeValidFile) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(valid_data_file.string());

  EXPECT_NO_THROW({
    producer.ReadFileSize();
  });
  // File was opened successfully if we can read an event
  std::vector<uint32_t> event;
  EXPECT_TRUE(producer.ReadXDCEvent(event));
}

// Test 3: File size reading with non-existent file should throw
TEST_F(HidraDryXDCProducerTest, ReadFileSizeNonExistentFile) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath((test_dir / "nonexistent.xdc").string());

  EXPECT_THROW({
    producer.ReadFileSize();
  }, eudaq::Exception);
}

// Test 4: Read valid XDC event
TEST_F(HidraDryXDCProducerTest, ReadXDCEventValid) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(valid_data_file.string());
  producer.ReadFileSize();

  std::vector<uint32_t> event_words;
  EXPECT_TRUE(producer.ReadXDCEvent(event_words));
  EXPECT_EQ(event_words.size(), 0x2fu);  // 14 header + 32 data + 1 trailer
  EXPECT_EQ(event_words[0], 0xccaaffeeu);  // marker
  EXPECT_EQ(event_words[13], 0xaccadeadu);  // end marker
}

// Test 5: Read multiple events from file
TEST_F(HidraDryXDCProducerTest, ReadMultipleXDCEvents) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(valid_data_file.string());
  producer.ReadFileSize();

  std::vector<uint32_t> event1, event2;
  EXPECT_TRUE(producer.ReadXDCEvent(event1));
  EXPECT_TRUE(producer.ReadXDCEvent(event2));

  EXPECT_EQ(event1[1], 0x00000001);  // event number
  EXPECT_EQ(event2[1], 0x00000002);  // event number
}

// Test 6: Read event with invalid marker should return false
TEST_F(HidraDryXDCProducerTest, ReadXDCEventInvalidMarker) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(invalid_marker_file.string());
  producer.ReadFileSize();

  std::vector<uint32_t> event_words;
  EXPECT_FALSE(producer.ReadXDCEvent(event_words));
}

// Test 7: Read event from truncated file should return false
TEST_F(HidraDryXDCProducerTest, ReadXDCEventTruncatedFile) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(truncated_file.string());
  producer.ReadFileSize();

  std::vector<uint32_t> event_words;
  EXPECT_FALSE(producer.ReadXDCEvent(event_words));
}

// Test 8: Read from empty file should return false
TEST_F(HidraDryXDCProducerTest, ReadXDCEventEmptyFile) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(empty_file.string());
  producer.ReadFileSize();

  std::vector<uint32_t> event_words;
  EXPECT_FALSE(producer.ReadXDCEvent(event_words));
}

// Test 9: Verify event field extraction
TEST_F(HidraDryXDCProducerTest, EventFieldExtraction) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(valid_data_file.string());
  producer.ReadFileSize();

  std::vector<uint32_t> event_words;
  EXPECT_TRUE(producer.ReadXDCEvent(event_words));

  // Verify header structure
  EXPECT_EQ(event_words[0],  0xccaaffeeu);  // marker
  EXPECT_EQ(event_words[1],  0x00000001u);  // event_number
  EXPECT_EQ(event_words[2],  0x00000000u);  // spill_number
  EXPECT_EQ(event_words[3],  0x0000000eu);  // header_size (14 words)
  EXPECT_EQ(event_words[4],  0x00000001u);  // trailer_size
  EXPECT_EQ(event_words[5],  0x00000020u);  // data_size (32 words)
  EXPECT_EQ(event_words[6],  0x0000002fu);  // event_size (14+32+1=47)
  EXPECT_EQ(event_words[13], 0xaccadeadu);  // header_end_marker
  EXPECT_EQ(event_words[event_words.size()-1], 0xbbeeddaau);  // trailer
}

// Test 10: ReadFileSize enables reading events
TEST_F(HidraDryXDCProducerTest, ReadFileSizeEnablesEventReading) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(valid_data_file.string());
  producer.ReadFileSize();

  // Both events should be readable in order
  std::vector<uint32_t> ev1, ev2;
  EXPECT_TRUE(producer.ReadXDCEvent(ev1));
  EXPECT_TRUE(producer.ReadXDCEvent(ev2));
  EXPECT_EQ(ev1[1], 0x00000001u);
  EXPECT_EQ(ev2[1], 0x00000002u);
  // No more events
  std::vector<uint32_t> ev3;
  EXPECT_FALSE(producer.ReadXDCEvent(ev3));
}

// Test 11: ReadFileSize resets the read position to the beginning
TEST_F(HidraDryXDCProducerTest, ReadFileSizeResetsPosition) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(valid_data_file.string());

  // First pass: advance past the first event
  producer.ReadFileSize();
  std::vector<uint32_t> ev;
  EXPECT_TRUE(producer.ReadXDCEvent(ev));
  EXPECT_EQ(ev[1], 0x00000001u);

  // ReadFileSize again should rewind to beginning
  producer.ReadFileSize();
  std::vector<uint32_t> ev_again;
  EXPECT_TRUE(producer.ReadXDCEvent(ev_again));
  EXPECT_EQ(ev_again[1], 0x00000001u);  // back at first event
}

// Test 12: Data integrity check
TEST_F(HidraDryXDCProducerTest, DataIntegrity) {
  HidraDryXDCProducer producer("TestProducer", "tcp://localhost:44000");
  producer.SetDataPath(valid_data_file.string());
  producer.ReadFileSize();

  std::vector<uint32_t> event_words;
  EXPECT_TRUE(producer.ReadXDCEvent(event_words));

  // Verify data payload contains expected values (generator uses 0xdeadbeef + i)
  for (size_t i = 0; i < 0x20; ++i) {
    EXPECT_EQ(event_words[14 + i], 0xdeadbeef + i) << "Data word at offset " << i << " mismatch";
  }
}
