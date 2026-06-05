aux_source_directory(src TRACKER_SRC)

add_library(${EUDAQ_MODULE}_tracker SHARED ${TRACKER_SRC})
target_link_libraries(${EUDAQ_MODULE}_tracker ${EUDAQ_CORE_LIBRARY})

install(TARGETS
  ${EUDAQ_MODULE}_tracker
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
