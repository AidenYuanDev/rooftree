add_library(rooftree-network)

CollectAndAddSourceFiles(
  rooftree-network
  ${CMAKE_CURRENT_SOURCE_DIR}
  EXCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders)

target_link_libraries(rooftree-network
  PRIVATE
    rooftree-core-interface
    utf8cpp
    boost-url
    kcp
  PUBLIC
    rooftree-common)

set_target_properties(rooftree-network
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "rooftree")

if(USE_COREPCH)
  target_precompile_headers(rooftree-network PRIVATE PrecompiledHeaders/networkPCH.h)
endif()
