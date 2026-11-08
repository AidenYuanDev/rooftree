add_subdirectory(common)
add_subdirectory(database)
add_subdirectory(server)

# Convenience target that pulls in every library
add_library(rooftree INTERFACE)
add_library(rooftree::rooftree ALIAS rooftree)
target_link_libraries(rooftree
  INTERFACE
    rooftree-common
    rooftree-network
    rooftree-database
    rooftree-server)
