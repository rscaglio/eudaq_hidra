include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Enable testing
enable_testing()

# Find Google Test
find_package(GTest QUIET)

if(NOT GTest_FOUND)
  message(STATUS "Google Test not found, attempting to use system GTest")
  # Try to find it another way or provide instructions
  message(WARNING "Google Test (gtest) is required for tests. Install with:")
  message(WARNING "  Ubuntu/Debian: sudo apt-get install libgtest-dev")
  message(WARNING "  macOS: brew install googletest")
  message(WARNING "  Or build from source: https://github.com/google/googletest")
  return()
endif()

# Create basic decoder test executable
add_executable(hidra_dc_payload_decoder_test
  HidraRootPayloadDecodersTest.cc
)

# Require C++17 for filesystem support used by tests
target_compile_features(hidra_dc_payload_decoder_test PRIVATE cxx_std_17)

# Create advanced decoder test executable
add_executable(hidra_dc_payload_decoder_advanced_test
  HidraRootPayloadDecodersAdvancedTest.cc
)

# Require C++17 for filesystem support used by tests
target_compile_features(hidra_dc_payload_decoder_advanced_test PRIVATE cxx_std_17)

# Link libraries for basic test
target_link_libraries(hidra_dc_payload_decoder_test
  ${EUDAQ_MODULE}_dc
  ${EUDAQ_MODULE}_common
  ${EUDAQ_CORE_LIBRARY}
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
)

# Link libraries for advanced test
target_link_libraries(hidra_dc_payload_decoder_advanced_test
  ${EUDAQ_MODULE}_dc
  ${EUDAQ_MODULE}_common
  ${EUDAQ_CORE_LIBRARY}
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
)

# Add tests to CTest
add_test(
  NAME HidraRootPayloadDecodersTest
  COMMAND hidra_dc_payload_decoder_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(
  NAME HidraRootPayloadDecodersAdvancedTest
  COMMAND hidra_dc_payload_decoder_advanced_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Set test properties
set_tests_properties(HidraRootPayloadDecodersTest PROPERTIES
  TIMEOUT 30
  LABELS "hidra;dc;payload;decoder;basic"
)

set_tests_properties(HidraRootPayloadDecodersAdvancedTest PROPERTIES
  TIMEOUT 60
  LABELS "hidra;dc;payload;decoder;advanced"
)

install(TARGETS 
  hidra_dc_payload_decoder_test
  hidra_dc_payload_decoder_advanced_test
  RUNTIME DESTINATION bin/tests
)
