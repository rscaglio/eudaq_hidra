include_directories(include /usr/include)
link_directories(/usr/lib)
aux_source_directory(src XDC_SRC)

set(CAEN_COMM_LIBRARY CAENComm)
set(CAEN_VME_LIBRARY CAENVME)

# Reuse top-level discovery when available so pre-check and link step use the
# same CAEN locations.
if(DEFINED HIDRA_CAENCOMM_LIBRARY AND HIDRA_CAENCOMM_LIBRARY)
  set(CAEN_COMM_LIBRARY ${HIDRA_CAENCOMM_LIBRARY})
endif()
if(DEFINED HIDRA_CAENVME_LIBRARY AND HIDRA_CAENVME_LIBRARY)
  set(CAEN_VME_LIBRARY ${HIDRA_CAENVME_LIBRARY})
endif()

# if(NOT EUDAQ_TTREE_LIBRARY)
#   list(REMOVE_ITEM MODULE_SRC src/QTPProducerEvent2TTreeEventConverter.cc)
#   list(REMOVE_ITEM MODULE_SRC src/DRSProducerEvent2TTreeEventConverter.cc)
#   list(REMOVE_ITEM MODULE_SRC src/FERSProducerEvent2TTreeEventConverter.cc)
# endif()

add_library(${EUDAQ_MODULE}_xdc SHARED ${XDC_SRC})
target_link_libraries(${EUDAQ_MODULE}_xdc ${EUDAQ_CORE_LIBRARY} ${EUDAQ_TTREE_LIBRARY} ${EUDAQ_ROOT_MONITOR_LIBRARY} ${CAEN_COMM_LIBRARY} ${CAEN_VME_LIBRARY} ${EUDAQ_MODULE}_common)
if(DEFINED HIDRA_CAENVME_HEADER_DIR AND HIDRA_CAENVME_HEADER_DIR)
  target_include_directories(${EUDAQ_MODULE}_xdc PRIVATE ${HIDRA_CAENVME_HEADER_DIR})
endif()
if(DEFINED HIDRA_CAENVME_TYPES_HEADER_DIR AND HIDRA_CAENVME_TYPES_HEADER_DIR)
  target_include_directories(${EUDAQ_MODULE}_xdc PRIVATE ${HIDRA_CAENVME_TYPES_HEADER_DIR})
endif()

install(TARGETS
  ${EUDAQ_MODULE}_xdc
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)

