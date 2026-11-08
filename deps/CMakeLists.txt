# Every dependency is wrapped in an INTERFACE target named after it, so the
# libraries only ever link "boost", "postgresql", ... and dependency specific definitions live here

# Imported targets are directory-scoped by default, the wrappers' consumers live in other directories
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

find_package(PkgConfig REQUIRED)

add_subdirectory(threads)
add_subdirectory(boost)
add_subdirectory(openssl)
add_subdirectory(fmt)
add_subdirectory(yaml-cpp)
add_subdirectory(utf8cpp)
add_subdirectory(argon2)
add_subdirectory(postgresql)
add_subdirectory(curl)
add_subdirectory(readline)
add_subdirectory(flatbuffers)
add_subdirectory(kcp)

# The blog's --create-admin prints the TOTP link as a QR code
if(APPS)
  add_subdirectory(qrencode)
endif()

if(USE_JEMALLOC)
  add_subdirectory(jemalloc)
endif()

if(HELGRIND)
  add_subdirectory(valgrind)
endif()

if(BUILD_TESTING OR BUILD_BENCHMARKS)
  add_subdirectory(catch2)
endif()
