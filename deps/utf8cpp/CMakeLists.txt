find_package(utf8cpp 4 REQUIRED)

add_library(utf8cpp INTERFACE)

target_link_libraries(utf8cpp
  INTERFACE
    utf8cpp::utf8cpp)
