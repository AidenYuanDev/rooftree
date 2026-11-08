# Replaces malloc/free for the whole process
# jemalloc only ships a pkg-config file
pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET GLOBAL jemalloc)

add_library(jemalloc INTERFACE)

target_link_libraries(jemalloc
  INTERFACE
    PkgConfig::JEMALLOC)
