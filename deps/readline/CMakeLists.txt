# The console client's line editing; readline only ships a
# pkg-config file
pkg_check_modules(READLINE REQUIRED IMPORTED_TARGET GLOBAL readline)

add_library(readline INTERFACE)

target_link_libraries(readline
  INTERFACE
    PkgConfig::READLINE)
