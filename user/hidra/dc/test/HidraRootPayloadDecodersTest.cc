#include <gtest/gtest.h>
#include "../include/HidraRootPayloadDecoders.hh"
#include <algorithm>
#include <vector>
#include <cstring>

namespace hidra {

// Test fixture for payload decoder tests
class PayloadDecoderTest : public ::testing::Test {
protected:
  RootDetectorPayload CreatePayload(int det_id, const std::string& producer, 
                                    std::uint64_t time_begin, std::uint64_t time_end,
                                    const std::vector<std::uint8_t>& payload_data = {}) {
    RootDetectorPayload payload;
    payload.det_id = det_id;
    payload.producer = producer;
    payload.trigger_n = 1;
    payload.event_time_begin = time_begin;
    payload.event_time_end = time_end;
    payload.payload = payload_data;
    return payload;
  }

  std::vector<std::uint8_t> CreateXDCPayload(size_t word_count = 10) {
    std::vector<std::uint8_t> payload(word_count * 4);
    // Fill with valid XDC word pattern (simplified)
    // Header word: type=0x2 (010), geo=0x1, crate=0x0, count=0x0
    uint32_t header = 0xA2000000;  // type 0x5 (101) in bits 26:24
    std::memcpy(payload.data(), &header, sizeof(uint32_t));
    
    // Add trailer word
    uint32_t trailer = 0xC0000000; // type 0x6 (110) in bits 26:24
    if (word_count > 1) {
      std::memcpy(payload.data() + (word_count - 1) * 4, &trailer, sizeof(uint32_t));
    }
    
    return payload;
  }

  std::vector<std::uint8_t> CreateFERSPayload() {
    // Create a minimal FERS payload (699 bytes)
    std::vector<std::uint8_t> payload(699, 0xFF);
    return payload;
  }
};

// ============ Generic Payload Decoder Tests ============

// Verifies that the generic decoder can be constructed without throwing.
TEST_F(PayloadDecoderTest, GenericDecoderConstructor) {
  EXPECT_NO_THROW({
    HidraGenericPayloadDecoder decoder;
  });
}

// Verifies that the generic decoder accepts payloads for any detector id.
TEST_F(PayloadDecoderTest, GenericDecoderMatchesAlwaysTrue) {
  HidraGenericPayloadDecoder decoder;
  
  auto payload1 = CreatePayload(1, "producer1", 100, 200);
  EXPECT_TRUE(decoder.Matches(payload1));
  
  auto payload2 = CreatePayload(99, "producer2", 0, 1000);
  EXPECT_TRUE(decoder.Matches(payload2));
}

// Verifies that generic branch names expose payload size and timestamp span.
TEST_F(PayloadDecoderTest, GenericDecoderBranchNames) {
  HidraGenericPayloadDecoder decoder;
  auto names = decoder.BranchNames();
  
  EXPECT_EQ(names.size(), 2);
  EXPECT_NE(std::find(names.begin(), names.end(), "payload_bytes"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "timestamp_span_ns"), names.end());
}

// Verifies that decoding an empty payload still fills generic quantities and branches.
TEST_F(PayloadDecoderTest, GenericDecoderDecodeEmptyPayload) {
  HidraGenericPayloadDecoder decoder;
  
  auto payload = CreatePayload(1, "test", 100, 200, {});
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
  
  // Check that quantities were added
  EXPECT_GT(quantities.size(), 0);
  // Check that branches were filled
  EXPECT_GT(branches.size(), 0);
}

// Verifies that payload_bytes is decoded exactly from a custom byte payload.
TEST_F(PayloadDecoderTest, GenericDecoderDecodeWithData) {
  HidraGenericPayloadDecoder decoder;
  
  std::vector<std::uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
  auto payload = CreatePayload(1, "test", 100, 300, data);
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
  
  // Check payload_bytes quantity
  auto it = std::find_if(quantities.begin(), quantities.end(),
                         [](const RootQuantity& q) { return q.name == "payload_bytes"; });
  EXPECT_NE(it, quantities.end());
  EXPECT_EQ(it->value, 5.0);  // 5 bytes
  EXPECT_EQ(it->unit, "B");
}

// Verifies that timestamp_span is computed as end minus begin.
TEST_F(PayloadDecoderTest, GenericDecoderDecodeTimestampSpan) {
  HidraGenericPayloadDecoder decoder;
  
  std::uint64_t begin = 1000;
  std::uint64_t end = 2500;
  auto payload = CreatePayload(1, "test", begin, end);
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  decoder.Decode(payload, quantities, branches);
  
  // Check timestamp_span quantity
  auto it = std::find_if(quantities.begin(), quantities.end(),
                         [](const RootQuantity& q) { return q.name == "timestamp_span"; });
  EXPECT_NE(it, quantities.end());
  EXPECT_EQ(it->value, 1500.0);  // 2500 - 1000 = 1500 ns
  EXPECT_EQ(it->unit, "ns");
}

// Verifies that timestamp_span is clamped to zero when end is earlier than begin.
TEST_F(PayloadDecoderTest, GenericDecoderDecodeInvalidTimestampSpan) {
  HidraGenericPayloadDecoder decoder;
  
  // When end < begin, span should be 0
  auto payload = CreatePayload(1, "test", 2500, 1000);
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  decoder.Decode(payload, quantities, branches);
  
  auto it = std::find_if(quantities.begin(), quantities.end(),
                         [](const RootQuantity& q) { return q.name == "timestamp_span"; });
  EXPECT_NE(it, quantities.end());
  EXPECT_EQ(it->value, 0.0);
}

// ============ XDC Payload Decoder Tests ============

// Verifies that the XDC decoder can be constructed with default settings.
TEST_F(PayloadDecoderTest, XDCDecoderConstructor) {
  EXPECT_NO_THROW({
    HidraXdcPayloadDecoder decoder({});
  });
}

// Verifies that the XDC decoder accepts an explicit VME geo map at construction.
TEST_F(PayloadDecoderTest, XDCDecoderWithGeoMap) {
  std::map<int, std::string> vme_map;
  vme_map[1] = "V792";
  vme_map[2] = "V862";
  
  EXPECT_NO_THROW({
    HidraXdcPayloadDecoder decoder(vme_map);
  });
}

// Verifies that XDC decoder matches detector id 1.
TEST_F(PayloadDecoderTest, XDCDecoderMatchesDet1) {
  HidraXdcPayloadDecoder decoder({});
  
  auto payload = CreatePayload(1, "test", 100, 200);
  EXPECT_TRUE(decoder.Matches(payload));
}

// Verifies that XDC decoder matches detector id 6.
TEST_F(PayloadDecoderTest, XDCDecoderMatchesDet6) {
  HidraXdcPayloadDecoder decoder({});
  
  auto payload = CreatePayload(6, "test", 100, 200);
  EXPECT_TRUE(decoder.Matches(payload));
}

// Verifies that XDC decoder rejects detector ids outside the supported set.
TEST_F(PayloadDecoderTest, XDCDecoderDoesNotMatchOtherDet) {
  HidraXdcPayloadDecoder decoder({});
  
  auto payload1 = CreatePayload(2, "test", 100, 200);
  EXPECT_FALSE(decoder.Matches(payload1));
  
  auto payload2 = CreatePayload(7, "test", 100, 200);
  EXPECT_FALSE(decoder.Matches(payload2));
}

// Verifies that XDC branch list includes ADC/TDC values and flags.
TEST_F(PayloadDecoderTest, XDCDecoderBranchNames) {
  HidraXdcPayloadDecoder decoder({});
  auto names = decoder.BranchNames();
  
  // Should have generic names plus XDC-specific ones
  EXPECT_GE(names.size(), 6);  // 2 generic + 4 XDC-specific
  EXPECT_NE(std::find(names.begin(), names.end(), "ADCs"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "ADCFlags"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "TDCs"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "TDCFlags"), names.end());
}

// Verifies that XDC decoder handles an empty payload without throwing.
TEST_F(PayloadDecoderTest, XDCDecoderDecodeEmptyPayload) {
  HidraXdcPayloadDecoder decoder({});
  
  auto payload = CreatePayload(1, "test", 100, 200, {});
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// ============ FERS Payload Decoder Tests ============

// Verifies that the FERS decoder can be constructed without throwing.
TEST_F(PayloadDecoderTest, FERSDecoderConstructor) {
  EXPECT_NO_THROW({
    HidraFersPayloadDecoder decoder;
  });
}

// Verifies that FERS decoder matches detector id 2.
TEST_F(PayloadDecoderTest, FERSDecoderMatchesDet2) {
  HidraFersPayloadDecoder decoder;
  
  auto payload = CreatePayload(2, "test", 100, 200);
  EXPECT_TRUE(decoder.Matches(payload));
}

// Verifies that FERS decoder rejects detector ids other than 2.
TEST_F(PayloadDecoderTest, FERSDecoderDoesNotMatchOtherDet) {
  HidraFersPayloadDecoder decoder;
  
  auto payload1 = CreatePayload(1, "test", 100, 200);
  EXPECT_FALSE(decoder.Matches(payload1));
  
  auto payload2 = CreatePayload(7, "test", 100, 200);
  EXPECT_FALSE(decoder.Matches(payload2));
}

// Verifies that FERS branch list includes all expected FERS-specific fields.
TEST_F(PayloadDecoderTest, FERSDecoderBranchNames) {
  HidraFersPayloadDecoder decoder;
  auto names = decoder.BranchNames();
  
  // Should have generic names plus FERS-specific ones
  EXPECT_GE(names.size(), 9);  // 2 generic + 7 FERS-specific
  EXPECT_NE(std::find(names.begin(), names.end(), "FERStsamp_us"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERSrel_tsamp_us"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERStrigger_id"), names.end());
}

// Verifies that FERS decoder handles an empty payload without throwing.
TEST_F(PayloadDecoderTest, FERSDecoderDecodeEmptyPayload) {
  HidraFersPayloadDecoder decoder;
  
  auto payload = CreatePayload(2, "test", 100, 200, {});
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// ============ RootDetectorPayload Tests ============

// Verifies default initialization values for RootDetectorPayload.
TEST(RootDetectorPayloadTest, DefaultConstruction) {
  RootDetectorPayload payload;
  EXPECT_EQ(payload.det_id, -1);
  EXPECT_EQ(payload.producer, "");
  EXPECT_EQ(payload.trigger_n, 0);
  EXPECT_EQ(payload.event_time_begin, 0);
  EXPECT_EQ(payload.event_time_end, 0);
  EXPECT_TRUE(payload.payload.empty());
  EXPECT_TRUE(payload.quantities.empty());
  EXPECT_TRUE(payload.branches.empty());
}

// Verifies assignment and storage of RootDetectorPayload fields.
TEST(RootDetectorPayloadTest, AssignValues) {
  RootDetectorPayload payload;
  payload.det_id = 5;
  payload.producer = "TestProducer";
  payload.trigger_n = 42;
  payload.event_time_begin = 1000;
  payload.event_time_end = 2000;
  payload.payload = {0x01, 0x02, 0x03};
  
  EXPECT_EQ(payload.det_id, 5);
  EXPECT_EQ(payload.producer, "TestProducer");
  EXPECT_EQ(payload.trigger_n, 42);
  EXPECT_EQ(payload.event_time_begin, 1000);
  EXPECT_EQ(payload.event_time_end, 2000);
  EXPECT_EQ(payload.payload.size(), 3);
}

// ============ Quantitities and Branches Tests ============

// Verifies default initialization values for RootQuantity.
TEST(RootQuantityTest, DefaultConstruction) {
  RootQuantity quantity;
  EXPECT_EQ(quantity.name, "");
  EXPECT_EQ(quantity.value, 0.0);
  EXPECT_EQ(quantity.unit, "");
}

// Verifies assignment and storage of RootQuantity fields.
TEST(RootQuantityTest, AssignValues) {
  RootQuantity quantity;
  quantity.name = "energy";
  quantity.value = 42.5;
  quantity.unit = "MeV";
  
  EXPECT_EQ(quantity.name, "energy");
  EXPECT_EQ(quantity.value, 42.5);
  EXPECT_EQ(quantity.unit, "MeV");
}

// Verifies insertion and retrieval semantics for RootBranchValues vectors.
TEST(RootBranchValuesTest, MapOperations) {
  RootBranchValues branches;
  
  // Add values
  branches["channel_0"].push_back(10.0);
  branches["channel_0"].push_back(20.0);
  branches["channel_1"].push_back(30.0);
  
  EXPECT_EQ(branches["channel_0"].size(), 2);
  EXPECT_EQ(branches["channel_1"].size(), 1);
  EXPECT_EQ(branches["channel_0"][0], 10.0);
  EXPECT_EQ(branches["channel_0"][1], 20.0);
  EXPECT_EQ(branches["channel_1"][0], 30.0);
}

} // namespace hidra
