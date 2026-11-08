find_package(fmt REQUIRED)

add_library(fmt INTERFACE)

target_link_libraries(fmt
  INTERFACE
    fmt::fmt)
