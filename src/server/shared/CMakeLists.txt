# The server core shared by every app: sessions processed on the main
# thread per update, AntiDOS, the update loop, and the HTTP/WebSocket front ends that feed them
add_library(rooftree-server)

CollectAndAddSourceFiles(
  rooftree-server
  ${CMAKE_CURRENT_SOURCE_DIR}
  EXCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders)

target_link_libraries(rooftree-server
  PRIVATE
    rooftree-core-interface
    curl
    boost-url
    readline
  PUBLIC
    rooftree-network
    rooftree-database
    boost-json
    flatbuffers)

# Packet.h's Error, its header for the apps too
rooftree_add_flatbuffers(rooftree-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/packet/Packets.fbs)

set_target_properties(rooftree-server
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "rooftree")
