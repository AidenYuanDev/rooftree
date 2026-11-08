find_package(Catch2 3 REQUIRED)

add_library(catch2 INTERFACE)

target_link_libraries(catch2
  INTERFACE
    Catch2::Catch2WithMain)
