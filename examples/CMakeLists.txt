function(rooftree_add_example name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name}
    PRIVATE
      rooftree-core-interface
    PUBLIC
      ${ARGN})
  set_target_properties(${name}
    PROPERTIES
      COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
      FOLDER "examples")
endfunction()

rooftree_add_example(echo_server    rooftree-network)
rooftree_add_example(http_server    rooftree-network)
rooftree_add_example(websocket_server rooftree-network)
rooftree_add_example(arena_kcp_client rooftree-network boost-json flatbuffers)
# The arena's messages, the header a game client generates from the same schema
rooftree_add_flatbuffers(arena_kcp_client PRIVATE ${CMAKE_SOURCE_DIR}/apps/arena/schema/Arena.fbs)
rooftree_add_example(scheduler_demo rooftree-common)
rooftree_add_example(db_query       rooftree-database)

# The .dist is always refreshed, rooftree.yaml is only created
# once so local edits (database password etc.) survive re-configuring
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
# websocket_server's page, opened from the file system
configure_file(websocket.html ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/websocket.html COPYONLY)
file(COPY_FILE
  ${CMAKE_CURRENT_SOURCE_DIR}/rooftree.yaml.dist
  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree.yaml.dist
  ONLY_IF_DIFFERENT)
if(NOT EXISTS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree.yaml)
  file(COPY_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/rooftree.yaml.dist
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rooftree.yaml)
endif()
