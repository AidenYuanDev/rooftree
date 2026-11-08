# Catch.cmake (catch_discover_tests) ships next to Catch2Config.cmake, found in deps/catch2
include("${Catch2_DIR}/Catch.cmake")

add_executable(tests)

collectandaddsourcefiles(tests ${CMAKE_CURRENT_SOURCE_DIR} HEADER_VISIBILITY
                         PRIVATE)

target_link_libraries(tests PRIVATE rooftree-core-interface rooftree
                                    catch2)

rooftree_add_flatbuffers(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/TestPackets.fbs)

# Default for TestDatabaseConfig.h, ROOFTREE_TEST_CONFIG overrides it
target_compile_definitions(tests PRIVATE ROOFTREE_TEST_CONFIG_FILE="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree_test.yaml")

set_target_properties(
  tests PROPERTIES COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS} FOLDER
                                                                       "tests")

catch_discover_tests(tests TEST_SPEC "~[database]")
# The database tests share their tables: one at a time, also under ctest -j
catch_discover_tests(tests TEST_SPEC "[database]" PROPERTIES RESOURCE_LOCK test_database)
