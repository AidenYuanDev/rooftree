find_package(yaml-cpp 0.8 REQUIRED)

# Not named yaml-cpp: yaml-cpp's package config already defines a (deprecated) target with that name
add_library(yamlcpp INTERFACE)

target_link_libraries(yamlcpp
  INTERFACE
    yaml-cpp::yaml-cpp)
