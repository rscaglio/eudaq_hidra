#include <gtest/gtest.h>
#include "../include/HidraRootPayloadDecoders.hh"
#include <algorithm>
#include <limits>
#include <vector>
#include <cstring>

namespace hidra {

// Test fixture for advanced payload decoder tests
class AdvancedPayloadDecoderTest : public ::testing::Test {
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

  // Create a valid XDC word (32-bit)
  uint32_t CreateXDCWord(uint8_t type, uint8_t geo, uint8_t crate, uint8_t cnt_or_other) {
    uint32_t word = 0;
    word |= (type & 0x7) << 24;      // bits 26:24
    word |= (geo & 0x1F) << 27;      // bits 31:27
    word |= (crate & 0xFF) << 16;    // bits 23:16
    word |= (cnt_or_other & 0xFF) << 8;  // bits 15:8
    return word;
  }

  std::vector<std::uint8_t> CreateValidXDCPayload() {
    std::vector<uint32_t> words;
    
    // Header word: type=0x2, geo=0x1, crate=0x0, cnt=0x0
    words.push_back(CreateXDCWord(0x2, 0x1, 0x0, 0x0));
    
    // Trailer word: type=0x4, geo=0x1
    words.push_back(CreateXDCWord(0x4, 0x1, 0x0, 0x0));
    
    // Convert to byte vector
    std::vector<std::uint8_t> payload(words.size() * 4);
    std::memcpy(payload.data(), words.data(), payload.size());
    return payload;
  }

  std::vector<std::uint8_t> CreateMalformedXDCPayload() {
    // Payload with odd number of bytes (not multiple of 4)
    return {0x01, 0x02, 0x03, 0x04, 0x05};
  }

  std::vector<std::uint8_t> CreateValidFERSPayload(size_t size_bytes) {
    // FERS payloads should be multiples of 699 bytes
    std::vector<std::uint8_t> payload(size_bytes, 0);
    
    // Fill with some pattern
    for (size_t i = 0; i < size_bytes; ++i) {
      payload[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    return payload;
  }

  std::vector<std::uint8_t> CreateXDCWordsPayload(const std::vector<uint32_t>& words) {
    std::vector<std::uint8_t> payload(words.size() * sizeof(uint32_t));
    std::memcpy(payload.data(), words.data(), payload.size());
    return payload;
  }

  uint32_t MakeXDCHeader(uint8_t geo, uint8_t nchan) {
    return (static_cast<uint32_t>(geo & 0x1F) << 27) | (static_cast<uint32_t>(0b010) << 24) |
           (static_cast<uint32_t>(nchan & 0x3F) << 8);
  }

  uint32_t MakeXDCChannel(uint8_t geo, uint8_t channel, uint16_t value, uint8_t ov, uint8_t un) {
    return (static_cast<uint32_t>(geo & 0x1F) << 27) | (static_cast<uint32_t>(0b000) << 24) |
           (static_cast<uint32_t>(channel & 0x1F) << 16) | (static_cast<uint32_t>(un & 0x1) << 13) |
           (static_cast<uint32_t>(ov & 0x1) << 12) | static_cast<uint32_t>(value & 0x7FF);
  }

  uint32_t MakeXDCTrailer(uint8_t geo, uint32_t evt_cnt = 0) {
    return (static_cast<uint32_t>(geo & 0x1F) << 27) | (static_cast<uint32_t>(0b100) << 24) |
           (evt_cnt & 0x7FFFFF);
  }

  std::vector<std::uint8_t> CreateFERSPayloadFromBlock(const FERS_spect_64& block) {
    // Decoder expects sizeof(FERS_spect_64) bytes per block; allocate exactly that
    // to avoid out-of-bounds read when decoder does memcpy(&boardblock, block_ptr, sizeof(FERS_spect_64))
    std::vector<std::uint8_t> payload(sizeof(FERS_spect_64), 0);
    std::memcpy(payload.data(), &block, sizeof(FERS_spect_64));
    return payload;
  }
};

// ============ Advanced Generic Decoder Tests ============

// Verifies that generic decoding is stable and accurate with a large payload.
TEST_F(AdvancedPayloadDecoderTest, GenericDecoderLargePayload) {
  HidraGenericPayloadDecoder decoder;
  
  // Create large payload (1MB)
  std::vector<std::uint8_t> large_data(1024 * 1024);
  std::fill(large_data.begin(), large_data.end(), 0xAA);
  
  auto payload = CreatePayload(1, "test", 0, 1000000, large_data);
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
  
  auto it = std::find_if(quantities.begin(), quantities.end(),
                         [](const RootQuantity& q) { return q.name == "payload_bytes"; });
  EXPECT_NE(it, quantities.end());
  EXPECT_EQ(it->value, 1024.0 * 1024.0);
}

// Verifies repeated decoding over multiple events fills expected generic branches.
TEST_F(AdvancedPayloadDecoderTest, GenericDecoderMultipleDecodes) {
  HidraGenericPayloadDecoder decoder;
  
  std::vector<RootBranchValues> all_branches;
  
  // Decode multiple payloads
  for (int i = 0; i < 5; ++i) {
    auto payload = CreatePayload(1, "test", i * 100, (i + 1) * 100, 
                                 std::vector<std::uint8_t>(i + 1, 0xFF));
    std::vector<RootQuantity> quantities;
    RootBranchValues branches;
    
    decoder.Decode(payload, quantities, branches);
    all_branches.push_back(branches);
  }
  
  EXPECT_EQ(all_branches.size(), 5);
  for (size_t i = 0; i < all_branches.size(); ++i) {
    EXPECT_TRUE(all_branches[i].count("payload_bytes") > 0);
    EXPECT_TRUE(all_branches[i].count("timestamp_span_ns") > 0);
  }
}

// Verifies branch vectors accumulate entries when decoding multiple times into the same map.
TEST_F(AdvancedPayloadDecoderTest, GenericDecoderBranchValuesAccumulation) {
  HidraGenericPayloadDecoder decoder;
  
  RootBranchValues branches;
  
  // Decode same payload twice into same branches
  auto payload = CreatePayload(1, "test", 100, 200, {0x01, 0x02});
  std::vector<RootQuantity> quantities1;
  
  decoder.Decode(payload, quantities1, branches);
  size_t size_after_first = branches["payload_bytes"].size();
  
  std::vector<RootQuantity> quantities2;
  decoder.Decode(payload, quantities2, branches);
  size_t size_after_second = branches["payload_bytes"].size();
  
  EXPECT_GT(size_after_second, size_after_first);
}

// ============ Advanced XDC Decoder Tests ============

// Verifies XDC decoding does not throw when geo map is empty.
TEST_F(AdvancedPayloadDecoderTest, XDCDecoderEmptyGeoMap) {
  std::map<int, std::string> empty_map;
  HidraXdcPayloadDecoder decoder(empty_map);
  
  auto payload = CreatePayload(1, "test", 100, 200, CreateValidXDCPayload());
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// Verifies XDC decoding accepts a multi-module geo map configuration.
TEST_F(AdvancedPayloadDecoderTest, XDCDecoderWithComplexGeoMap) {
  std::map<int, std::string> vme_map;
  vme_map[1] = "V792";
  vme_map[2] = "V792";
  vme_map[5] = "V862";
  vme_map[10] = "V862";
  
  HidraXdcPayloadDecoder decoder(vme_map);
  
  auto payload = CreatePayload(1, "test", 100, 200, CreateValidXDCPayload());
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// Verifies malformed XDC payload size is handled gracefully without exceptions.
TEST_F(AdvancedPayloadDecoderTest, XDCDecoderMalformedPayloadSize) {
  HidraXdcPayloadDecoder decoder(std::map<int,std::string>{});
  
  // Payload with size not multiple of 4
  auto payload = CreatePayload(1, "test", 100, 200, CreateMalformedXDCPayload());
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// Verifies XDC branch names are deterministic and independent of geo map content.
TEST_F(AdvancedPayloadDecoderTest, XDCDecoderBranchNamesConsistency) {
  HidraXdcPayloadDecoder decoder1(std::map<int,std::string>{});
  
  std::map<int, std::string> vme_map;
  vme_map[1] = "V792";
  HidraXdcPayloadDecoder decoder2(vme_map);
  
  auto names1 = decoder1.BranchNames();
  auto names2 = decoder2.BranchNames();
  
  // Should have same branch names regardless of geo map
  EXPECT_EQ(names1, names2);
}

// Verifies exact ADC value and flag decoding for a handcrafted single-channel XDC event.
TEST_F(AdvancedPayloadDecoderTest, XDCDecoderDecodesSingleChannelExactValue) {
  std::map<int, std::string> vme_map;
  vme_map[5] = "V792";
  HidraXdcPayloadDecoder decoder(vme_map);

  const auto payload = CreateXDCWordsPayload(
      {MakeXDCHeader(/*geo=*/5, /*nchan=*/1),
       MakeXDCChannel(/*geo=*/5, /*channel=*/7, /*value=*/1234, /*ov=*/1, /*un=*/0),
       MakeXDCTrailer(/*geo=*/5)});

  auto detector = CreatePayload(1, "test", 100, 200, payload);
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;

  decoder.Decode(detector, quantities, branches);

  ASSERT_TRUE(branches.count("ADCs") > 0);
  ASSERT_TRUE(branches.count("ADCFlags") > 0);
  ASSERT_GE(branches["ADCs"].size(), 32u);
  ASSERT_GE(branches["ADCFlags"].size(), 32u);

  EXPECT_EQ(branches["ADCs"][7], 1234.0);
  EXPECT_EQ(branches["ADCFlags"][7], 2.0); // flag = (ov << 1) | un = 2
  EXPECT_EQ(branches["ADCs"][6], -1.0);
}

// Verifies geo-based channel offset mapping is applied to the decoded ADC index.
TEST_F(AdvancedPayloadDecoderTest, XDCDecoderAppliesGeoChannelOffsetFromMap) {
  std::map<int, std::string> vme_map;
  vme_map[1] = "V792";
  vme_map[5] = "V792";
  HidraXdcPayloadDecoder decoder(vme_map);

  const auto payload = CreateXDCWordsPayload(
      {MakeXDCHeader(/*geo=*/5, /*nchan=*/1),
       MakeXDCChannel(/*geo=*/5, /*channel=*/3, /*value=*/77, /*ov=*/0, /*un=*/1),
       MakeXDCTrailer(/*geo=*/5)});

  auto detector = CreatePayload(1, "test", 100, 200, payload);
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;

  decoder.Decode(detector, quantities, branches);

  ASSERT_TRUE(branches.count("ADCs") > 0);
  ASSERT_TRUE(branches.count("ADCFlags") > 0);
  ASSERT_GE(branches["ADCs"].size(), 64u);

  const std::size_t encoded_index = 32 + 3; // geo 1 contributes first 32 channels
  EXPECT_EQ(branches["ADCs"][encoded_index], 77.0);
  EXPECT_EQ(branches["ADCFlags"][encoded_index], 1.0); // flag = (ov << 1) | un = 1
}

// ============ Advanced FERS Decoder Tests ============

// Verifies decoder behavior for one valid-size FERS block (699 bytes).
TEST_F(AdvancedPayloadDecoderTest, FERSDecoderValidPayloadSize699) {
  HidraFersPayloadDecoder decoder;
  
  auto payload = CreatePayload(2, "test", 100, 200, CreateValidFERSPayload(699));
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// Verifies decoder behavior for two concatenated valid-size FERS blocks.
TEST_F(AdvancedPayloadDecoderTest, FERSDecoderValidPayloadSize1398) {
  HidraFersPayloadDecoder decoder;
  
  // Multiple blocks (699 * 2)
  auto payload = CreatePayload(2, "test", 100, 200, CreateValidFERSPayload(1398));
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// Verifies invalid FERS payload size is reported but does not throw.
TEST_F(AdvancedPayloadDecoderTest, FERSDecoderInvalidPayloadSize) {
  HidraFersPayloadDecoder decoder;
  
  // Payload size not multiple of 699
  auto payload = CreatePayload(2, "test", 100, 200, CreateValidFERSPayload(700));
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
}

// Verifies FERS decoder exposes the complete expected branch set.
TEST_F(AdvancedPayloadDecoderTest, FERSDecoderBranchNamesComplete) {
  HidraFersPayloadDecoder decoder;
  auto names = decoder.BranchNames();
  
  // Verify all expected branch names are present
  EXPECT_NE(std::find(names.begin(), names.end(), "payload_bytes"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "timestamp_span_ns"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERStsamp_us"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERSrel_tsamp_us"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERStrigger_id"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERSboard_id"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERShg"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERSlg"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERStoa"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "FERStot"), names.end());
}

// Verifies exact value decoding for one selected board/channel in a handcrafted FERS block.
TEST_F(AdvancedPayloadDecoderTest, FERSDecoderDecodesExactValuesForOneBoardChannel) {
  HidraFersPayloadDecoder decoder;

  FERS_spect_64 block{};
  block.board_id = 3;
  block.tstamp_us = 12.5;
  block.rel_tstamp_us = 2.75;
  block.trigger_id = 4242;
  block.chmask = std::numeric_limits<uint64_t>::max();
  block.energyHG[5] = 111;
  block.energyLG[5] = 22;
  block.tstamp[5] = 3333;
  block.ToT[5] = 44;

  const auto payload = CreateFERSPayloadFromBlock(block);
  auto detector = CreatePayload(2, "test", 100, 200, payload);
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;

  decoder.Decode(detector, quantities, branches);

  const std::size_t index = 3 * 64 + 5;
  ASSERT_TRUE(branches.count("FERStsamp_us") > 0);
  ASSERT_TRUE(branches.count("FERSrel_tsamp_us") > 0);
  ASSERT_TRUE(branches.count("FERStrigger_id") > 0);
  ASSERT_TRUE(branches.count("FERSboard_id") > 0);
  ASSERT_TRUE(branches.count("FERShg") > 0);
  ASSERT_TRUE(branches.count("FERSlg") > 0);
  ASSERT_TRUE(branches.count("FERStoa") > 0);
  ASSERT_TRUE(branches.count("FERStot") > 0);

  EXPECT_EQ(branches["FERStsamp_us"][index], 12.5);
  EXPECT_EQ(branches["FERSrel_tsamp_us"][index], 2.75);
  EXPECT_EQ(branches["FERStrigger_id"][index], 4242.0);
  EXPECT_EQ(branches["FERSboard_id"][index], 3.0);
  EXPECT_EQ(branches["FERShg"][index], 111.0);
  EXPECT_EQ(branches["FERSlg"][index], 22.0);
  EXPECT_EQ(branches["FERStoa"][index], 3333.0);
  EXPECT_EQ(branches["FERStot"][index], 44.0);
  EXPECT_EQ(branches["FERShg"][2 * 64 + 5], -1.0);
}

// ============ Polymorphic Decoder Tests ============

// Verifies detector-id based selection logic across generic, XDC, and FERS decoders.
TEST_F(AdvancedPayloadDecoderTest, PolymorphicDecoderSelection) {
  // Test that different detector IDs can be handled with appropriate decoders
  HidraGenericPayloadDecoder generic_decoder;
  HidraXdcPayloadDecoder xdc_decoder(std::map<int,std::string>{});
  HidraFersPayloadDecoder fers_decoder;
  
  // XDC payload
  auto xdc_payload = CreatePayload(1, "test", 100, 200);
  EXPECT_TRUE(xdc_decoder.Matches(xdc_payload));
  EXPECT_TRUE(generic_decoder.Matches(xdc_payload));
  
  // FERS payload
  auto fers_payload = CreatePayload(2, "test", 100, 200);
  EXPECT_FALSE(xdc_decoder.Matches(fers_payload));
  EXPECT_TRUE(fers_decoder.Matches(fers_payload));
  EXPECT_TRUE(generic_decoder.Matches(fers_payload));
  
  // Generic payload (det_id 99)
  auto generic_payload = CreatePayload(99, "test", 100, 200);
  EXPECT_FALSE(xdc_decoder.Matches(generic_payload));
  EXPECT_FALSE(fers_decoder.Matches(generic_payload));
  EXPECT_TRUE(generic_decoder.Matches(generic_payload));
}

// Verifies running generic then XDC decoding produces non-regressing output containers.
TEST_F(AdvancedPayloadDecoderTest, DecoderChainedDecoding) {
  HidraGenericPayloadDecoder generic;
  HidraXdcPayloadDecoder xdc(std::map<int,std::string>{});
  
  auto payload = CreatePayload(1, "test", 100, 200, {0x01, 0x02, 0x03, 0x04});
  
  // Decode with generic first
  std::vector<RootQuantity> quantities_generic;
  RootBranchValues branches_generic;
  generic.Decode(payload, quantities_generic, branches_generic);
  
  // Then decode with XDC
  std::vector<RootQuantity> quantities_xdc;
  RootBranchValues branches_xdc;
  xdc.Decode(payload, quantities_xdc, branches_xdc);
  
  // XDC should have decoded more than generic
  EXPECT_GE(quantities_xdc.size(), quantities_generic.size());
  EXPECT_GE(branches_xdc.size(), branches_generic.size());
}

// ============ Edge Cases Tests ============

// Verifies zero begin/end timestamps produce a zero timestamp span.
TEST_F(AdvancedPayloadDecoderTest, GenericDecoderZeroTimestamp) {
  HidraGenericPayloadDecoder decoder;
  
  auto payload = CreatePayload(1, "test", 0, 0, {0x01});
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
  
  auto it = std::find_if(quantities.begin(), quantities.end(),
                         [](const RootQuantity& q) { return q.name == "timestamp_span"; });
  EXPECT_NE(it, quantities.end());
  EXPECT_EQ(it->value, 0.0);
}

// Verifies very large timestamp ranges are decoded without precision loss in expected value.
TEST_F(AdvancedPayloadDecoderTest, GenericDecoderLargeTimestampSpan) {
  HidraGenericPayloadDecoder decoder;
  
  std::uint64_t large_begin = 0;
  std::uint64_t large_end = 1e12;  // Very large span
  
  auto payload = CreatePayload(1, "test", large_begin, large_end, {0x01});
  std::vector<RootQuantity> quantities;
  RootBranchValues branches;
  
  EXPECT_NO_THROW({
    decoder.Decode(payload, quantities, branches);
  });
  
  auto it = std::find_if(quantities.begin(), quantities.end(),
                         [](const RootQuantity& q) { return q.name == "timestamp_span"; });
  EXPECT_NE(it, quantities.end());
  EXPECT_EQ(it->value, 1e12);
}

// Verifies XDC matching rules over a sweep of detector ids.
TEST_F(AdvancedPayloadDecoderTest, XDCDecoderAllDetIDs) {
  HidraXdcPayloadDecoder decoder(std::map<int,std::string>{});
  
  for (int det_id = 0; det_id <= 10; ++det_id) {
    auto payload = CreatePayload(det_id, "test", 100, 200);
    
    bool matches = decoder.Matches(payload);
    if (det_id == 1 || det_id == 6) {
      EXPECT_TRUE(matches) << "det_id " << det_id << " should match XDC decoder";
    } else {
      EXPECT_FALSE(matches) << "det_id " << det_id << " should not match XDC decoder";
    }
  }
}

// Verifies FERS matching rules over a sweep of detector ids.
TEST_F(AdvancedPayloadDecoderTest, FERSDecoderAllDetIDs) {
  HidraFersPayloadDecoder decoder;
  
  for (int det_id = 0; det_id <= 10; ++det_id) {
    auto payload = CreatePayload(det_id, "test", 100, 200);
    
    bool matches = decoder.Matches(payload);
    if (det_id == 2) {
      EXPECT_TRUE(matches) << "det_id " << det_id << " should match FERS decoder";
    } else {
      EXPECT_FALSE(matches) << "det_id " << det_id << " should not match FERS decoder";
    }
  }
}

} // namespace hidra
