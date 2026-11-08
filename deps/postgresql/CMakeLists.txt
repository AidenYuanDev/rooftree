# libpq, PostgreSQL's client library
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET GLOBAL libpq>=14)

add_library(postgresql INTERFACE)

target_link_libraries(postgresql
  INTERFACE
    PkgConfig::LIBPQ)
