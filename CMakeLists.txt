cmake_minimum_required(VERSION 3.24)

# add this options before PROJECT keyword
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

project(rooftree LANGUAGES C CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW) # use Boost's own BoostConfig.cmake
endif()

# build in RelWithDebInfo-mode by default if not explicitly set
if(CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
  set(CMAKE_DEFAULT_BUILD_TYPE "RelWithDebInfo" CACHE INTERNAL "")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Build in a directory of its own: cmake -S . -B build (and remove the CMakeCache.txt made here)")
endif()

include(cmake/Options.cmake)
include(cmake/Compiler.cmake)
include(cmake/SourceFiles.cmake)
include(cmake/Summary.cmake)

# add dependencies
add_subdirectory(deps)

# add core sources
add_subdirectory(src)

if(EXAMPLES)
  add_subdirectory(examples)
endif()

if(APPS)
  add_subdirectory(apps)
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Not registered with CTest: timings are only meaningful when run on their own
if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# Like the examples' rooftree.yaml: tests and benchmarks read the database they may use from rooftree_test.yaml,
# the .dist is always refreshed, rooftree_test.yaml is only created once so local edits survive re-configuring
if(BUILD_TESTING OR BUILD_BENCHMARKS)
  file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  file(COPY_FILE
    ${CMAKE_SOURCE_DIR}/tests/rooftree_test.yaml.dist
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree_test.yaml.dist
    ONLY_IF_DIFFERENT)
  if(NOT EXISTS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree_test.yaml)
    file(COPY_FILE
      ${CMAKE_SOURCE_DIR}/tests/rooftree_test.yaml.dist
      ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree_test.yaml)
  endif()
endif()
