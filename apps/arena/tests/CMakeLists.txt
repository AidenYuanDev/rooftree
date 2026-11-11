# The arena's own tests, ctest runs them with the framework's
include("${Catch2_DIR}/Catch.cmake")

add_executable(arena-tests)

CollectAndAddSourceFiles(arena-tests ${CMAKE_CURRENT_SOURCE_DIR} HEADER_VISIBILITY PRIVATE)

target_link_libraries(arena-tests
  PRIVATE
    rooftree-core-interface
    arena-combat
    catch2)

set_target_properties(arena-tests
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "tests")

catch_discover_tests(arena-tests)
