add_library(rooftree-common)

CollectAndAddSourceFiles(
  rooftree-common
  ${CMAKE_CURRENT_SOURCE_DIR}
  EXCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders
    ${CMAKE_CURRENT_SOURCE_DIR}/network)

target_link_libraries(rooftree-common
  PRIVATE
    rooftree-core-interface
    utf8cpp
    argon2
  PUBLIC
    yamlcpp
    threads
    boost
    openssl
    fmt)

# Every executable links rooftree-common, so this swaps the allocator for all of them
if(TARGET jemalloc)
  target_link_libraries(rooftree-common
    PUBLIC
      jemalloc)
endif()

if(TARGET valgrind)
  target_link_libraries(rooftree-common
    PUBLIC
      valgrind)
endif()

set_target_properties(rooftree-common
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "rooftree")

if(USE_COREPCH)
  target_precompile_headers(rooftree-common PRIVATE PrecompiledHeaders/commonPCH.h)
endif()

add_subdirectory(network)
