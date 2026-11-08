# libargon2 only ships a pkg-config file
pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET GLOBAL libargon2)

add_library(argon2 INTERFACE)

target_link_libraries(argon2
  INTERFACE
    PkgConfig::ARGON2)
