add_library(rooftree-database)

CollectAndAddSourceFiles(
  rooftree-database
  ${CMAKE_CURRENT_SOURCE_DIR}
  EXCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders)

target_link_libraries(rooftree-database
  PRIVATE
    rooftree-core-interface
  PUBLIC
    rooftree-common
    postgresql)

set_target_properties(rooftree-database
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "rooftree")

if(USE_COREPCH)
  target_precompile_headers(rooftree-database PRIVATE PrecompiledHeaders/databasePCH.h)
endif()
