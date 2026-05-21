find_package(ROOT REQUIRED COMPONENTS Core Hist)

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
    ${ROOT_INCLUDE_DIRS}
)

target_link_libraries(${EUDAQ_MODULE}_monitor
  PRIVATE
    ${EUDAQ_CORE_LIBRARY}
    ${EUDAQ_ROOT_MONITOR_LIBRARY}
    ${EUDAQ_MODULE}_common
    ${ROOT_LIBRARIES}
)

install(TARGETS
  ${EUDAQ_MODULE}_monitor
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
