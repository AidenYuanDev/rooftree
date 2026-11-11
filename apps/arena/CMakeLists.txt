# The combat rules on their own, nothing of the network or the database: the arena and its tests link them
add_library(arena-combat STATIC)

CollectAndAddSourceFiles(arena-combat ${CMAKE_CURRENT_SOURCE_DIR}/combat)

target_link_libraries(arena-combat
  PRIVATE
    rooftree-core-interface
  PUBLIC
    rooftree-common)

set_target_properties(arena-combat
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "apps")

add_executable(arena)

CollectAndAddSourceFiles(
  arena
  ${CMAKE_CURRENT_SOURCE_DIR}
  EXCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/combat
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/schema
    ${CMAKE_CURRENT_SOURCE_DIR}/sql
    ${CMAKE_CURRENT_SOURCE_DIR}/www
  HEADER_VISIBILITY PRIVATE)

# rooftree-server for the packets (Packet.h) and FlatJsonSchema, the page's JSON
target_link_libraries(arena
  PRIVATE
    rooftree-core-interface
    rooftree-server
    arena-combat
    utf8cpp)

# The messages (ArenaPackets.h)
rooftree_add_flatbuffers(arena PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/schema/Arena.fbs)

set_target_properties(arena
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "apps")

# Next to the binary, where arena.yaml's defaults look for them. Copied on every build (POST_BUILD
# would only run when arena relinks), so edits to the page or the schema show up after building;
# the page is read again on every request, the schema at the next start
add_custom_target(arena-files ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/www ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/arena-www
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/sql/arena.sql ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/arena.sql
  VERBATIM)
set_target_properties(arena-files PROPERTIES FOLDER "apps")
add_dependencies(arena arena-files)

# Like examples/rooftree.yaml: the .dist is always refreshed, arena.yaml is only created once
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
file(COPY_FILE
  ${CMAKE_CURRENT_SOURCE_DIR}/arena.yaml.dist
  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/arena.yaml.dist
  ONLY_IF_DIFFERENT)
if(NOT EXISTS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/arena.yaml)
  file(COPY_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/arena.yaml.dist
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/arena.yaml)
endif()

# Bots that fight over KCP, a client like a game's: the messages from the same schema
add_executable(arena_bots tools/arena_bots.cpp)
target_link_libraries(arena_bots
  PRIVATE
    rooftree-core-interface
    rooftree-network
    boost-json
    flatbuffers)
rooftree_add_flatbuffers(arena_bots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/schema/Arena.fbs)
set_target_properties(arena_bots
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "apps")

# A client that cheats and sends garbage, each check against what the server answers
add_executable(arena_rogue tools/arena_rogue.cpp)
target_link_libraries(arena_rogue
  PRIVATE
    rooftree-core-interface
    rooftree-network
    boost-json
    flatbuffers)
rooftree_add_flatbuffers(arena_rogue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/schema/Arena.fbs)
set_target_properties(arena_rogue
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "apps")

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
