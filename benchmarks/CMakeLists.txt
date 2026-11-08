# Catch2 benchmarks, run with ./benchmarks (all) or e.g. ./benchmarks "[thread]".
# Kept out of CTest on purpose: they take a while and only mean something on an idle machine
# with an optimized build (RelWithDebInfo/Release).
add_executable(benchmarks)

collectandaddsourcefiles(benchmarks ${CMAKE_CURRENT_SOURCE_DIR} HEADER_VISIBILITY
                         PRIVATE)

target_link_libraries(benchmarks PRIVATE rooftree-core-interface rooftree
                                         catch2)

rooftree_add_flatbuffers(benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/BenchPackets.fbs)

# TestDatabaseConfig.h is shared with the tests, see there
target_include_directories(benchmarks PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(benchmarks PRIVATE ROOFTREE_TEST_CONFIG_FILE="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree_test.yaml")

set_target_properties(
  benchmarks PROPERTIES COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS} FOLDER
                                                                            "benchmarks")
