find_package(Threads REQUIRED)

add_library(threads INTERFACE)

target_link_libraries(threads
  INTERFACE
    Threads::Threads)
