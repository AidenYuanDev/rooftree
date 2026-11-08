# This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
#
# The build's switches: cmake -DASAN=ON ..., see the README's table

option(EXAMPLES                "Build the examples"                                         ON)
option(APPS                    "Build the applications in apps/"                            ON)
option(BUILD_TESTING           "Build the tests"                                            ON)
option(BUILD_BENCHMARKS        "Build the benchmarks"                                       OFF)
option(USE_COREPCH             "Precompiled headers for the libraries"                      ON)
option(WITH_WARNINGS           "All the compiler's warnings"                                ON)
option(WITH_WARNINGS_AS_ERRORS "Warnings stop the build"                                    OFF)
option(WITH_COREDEBUG          "Full debug information (-g3)"                               OFF)
option(NOJEM                   "Don't link jemalloc"                                        OFF)
set(WITH_LINKER "" CACHE STRING "The linker for -fuse-ld= (mold, lld), empty for the system's")
option(ASAN                    "Address sanitizer"                                          OFF)
option(UBSAN                   "Undefined behaviour sanitizer"                              OFF)
option(TSAN                    "Thread sanitizer"                                           OFF)
option(MSAN                    "Memory sanitizer (Clang)"                                   OFF)
option(BUILD_TIME_ANALYSIS     "-ftime-trace (Clang)"                                       OFF)
option(HELGRIND                "Tell valgrind's helgrind of libstdc++'s synchronization"    OFF)

# The sanitizers and helgrind bring their own allocator
set(USE_JEMALLOC ON)
if(NOJEM OR ASAN OR TSAN OR MSAN OR HELGRIND)
  set(USE_JEMALLOC OFF)
endif()
