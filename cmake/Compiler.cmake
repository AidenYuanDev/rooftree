# This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
#
# GCC or Clang on a Unix: C++20, the warnings, the sanitizers. rooftree-core-interface carries them,
# every target of ours links it

if(NOT UNIX)
  message(FATAL_ERROR "Rooftree builds on Unix-like systems only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(ROOFTREE_GCC ON)
  set(minimum 12.0)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(ROOFTREE_CLANG ON)
  set(minimum 16.0)
else()
  message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} isn't supported, use GCC or Clang")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS minimum)
  message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}: Rooftree needs ${minimum} or later (C++20 coroutines)")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(WITH_LINKER)
  include(CheckLinkerFlag)
  check_linker_flag(CXX "-fuse-ld=${WITH_LINKER}" linker_works)
  if(linker_works)
    add_link_options("-fuse-ld=${WITH_LINKER}")
  else()
    message(WARNING "Linker ${WITH_LINKER} isn't available, using the system's")
  endif()
endif()

add_library(rooftree-core-interface INTERFACE)

if(WITH_WARNINGS)
  target_compile_options(rooftree-core-interface INTERFACE
    -W -Wall -Wextra -Winit-self -Woverloaded-virtual -Wfatal-errors
    -Wno-unused-parameter
    # Members with default initializers are left out of designated initializers on purpose
    -Wno-missing-field-initializers)
  if(ROOFTREE_GCC)
    # False positives with std::optional
    target_compile_options(rooftree-core-interface INTERFACE -Winvalid-pch -Wno-maybe-uninitialized)
  else()
    target_compile_options(rooftree-core-interface INTERFACE -Wimplicit-fallthrough -Wno-mismatched-tags)
  endif()
endif()

if(WITH_COREDEBUG)
  target_compile_options(rooftree-core-interface INTERFACE -g3)
endif()

if(BUILD_TIME_ANALYSIS AND ROOFTREE_CLANG)
  target_compile_options(rooftree-core-interface INTERFACE -ftime-trace)
endif()

if(HELGRIND)
  target_compile_definitions(rooftree-core-interface INTERFACE HELGRIND)
endif()

# Our targets, through rooftree-core-interface; the system's libraries stay as they are
set(sanitizers "")
if(ASAN)
  # Recoverable: with ASAN_OPTIONS=halt_on_error=0 a run reports every error, not only the first
  list(APPEND sanitizers -fsanitize=address -fsanitize-address-use-after-scope -fsanitize-recover=address)
endif()
if(UBSAN)
  list(APPEND sanitizers -fsanitize=undefined)
endif()
if(TSAN)
  list(APPEND sanitizers -fsanitize=thread)
endif()
if(MSAN)
  if(NOT ROOFTREE_CLANG)
    message(FATAL_ERROR "MSAN needs Clang")
  endif()
  list(APPEND sanitizers -fsanitize=memory -fsanitize-memory-track-origins)
endif()
if(sanitizers)
  target_compile_options(rooftree-core-interface INTERFACE -fno-omit-frame-pointer ${sanitizers})
  target_link_options(rooftree-core-interface INTERFACE -fno-omit-frame-pointer ${sanitizers})
endif()
