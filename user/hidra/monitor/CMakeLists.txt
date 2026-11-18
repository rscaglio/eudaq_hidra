find_package(ROOT REQUIRED COMPONENTS Core Hist RHTTP)

if(ROOT_USE_FILE)
  include(${ROOT_USE_FILE})
endif()

aux_source_directory(src MONITOR_SRC)

add_library(${EUDAQ_MODULE}_monitor SHARED
  ${MONITOR_SRC}
)

target_include_directories(${EUDAQ_MODULE}_monitor
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../dc/include
    ${ROOT_INCLUDE_DIRS}
)

target_link_libraries(${EUDAQ_MODULE}_monitor
  PRIVATE
    ${EUDAQ_CORE_LIBRARY}
    ${EUDAQ_ROOT_MONITOR_LIBRARY}
    ${EUDAQ_MODULE}_common
    ${EUDAQ_MODULE}_dc
    ${ROOT_LIBRARIES}
)

install(TARGETS
  ${EUDAQ_MODULE}_monitor
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)

if(BUILD_TESTING)
  add_executable(hidra_event_display_buffer_test
    tests/event_display_buffer_test.cc src/EventDisplayBuffer.cc)
  target_compile_features(hidra_event_display_buffer_test PRIVATE cxx_std_17)
  target_include_directories(hidra_event_display_buffer_test PRIVATE
    include ../dc/include ../common/include ${ROOT_INCLUDE_DIRS})
  add_test(NAME hidra_event_display_buffer COMMAND hidra_event_display_buffer_test)
endif()
