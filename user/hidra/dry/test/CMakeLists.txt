include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Enable testing
enable_testing()

# Find Google Test
find_package(GTest QUIET)

if(NOT GTEST_FOUND)
  message(STATUS "Google Test not found, attempting to use system GTest")
  # Try to find it another way or provide instructions
  message(WARNING "Google Test (gtest) is required for tests. Install with:")
  message(WARNING "  Ubuntu/Debian: sudo apt-get install libgtest-dev")
  message(WARNING "  macOS: brew install googletest")
  message(WARNING "  Or build from source: https://github.com/google/googletest")
  return()
endif()

# Create basic test executable
add_executable(hidra_dry_xdc_producer_test
  HidraDryXDCProducerTest.cc
)

# Create advanced test executable
add_executable(hidra_dry_xdc_producer_advanced_test
  HidraDryXDCProducerAdvancedTest.cc
)

# Link libraries for basic test
target_link_libraries(hidra_dry_xdc_producer_test
  ${EUDAQ_MODULE}_dry
  ${EUDAQ_CORE_LIBRARY}
  GTest::GTest
  GTest::Main
  pthread
)

# Link libraries for advanced test
target_link_libraries(hidra_dry_xdc_producer_advanced_test
  ${EUDAQ_MODULE}_dry
  ${EUDAQ_CORE_LIBRARY}
  GTest::GTest
  GTest::Main
  pthread
)

# Add tests to CTest
add_test(
  NAME HidraDryXDCProducerTest
  COMMAND hidra_dry_xdc_producer_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(
  NAME HidraDryXDCProducerAdvancedTest
  COMMAND hidra_dry_xdc_producer_advanced_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Set test properties
set_tests_properties(HidraDryXDCProducerTest PROPERTIES
  TIMEOUT 30
  LABELS "hidra;dry;producer;basic"
)

set_tests_properties(HidraDryXDCProducerAdvancedTest PROPERTIES
  TIMEOUT 60
  LABELS "hidra;dry;producer;advanced"
)

install(TARGETS 
  hidra_dry_xdc_producer_test
  hidra_dry_xdc_producer_advanced_test
  RUNTIME DESTINATION bin/tests
)
