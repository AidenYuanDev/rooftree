# libqrencode only ships a pkg-config file
pkg_check_modules(QRENCODE REQUIRED IMPORTED_TARGET GLOBAL libqrencode)

add_library(qrencode INTERFACE)

target_link_libraries(qrencode
  INTERFACE
    PkgConfig::QRENCODE)
