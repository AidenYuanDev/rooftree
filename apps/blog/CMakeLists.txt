# The blog's API server. The frontend (web/) and the migrations (db/) are built and applied with pnpm,
# see apps/blog/README.md
add_executable(blog)

CollectAndAddSourceFiles(
  blog
  ${CMAKE_CURRENT_SOURCE_DIR}/server
  HEADER_VISIBILITY PRIVATE)

target_link_libraries(blog
  PRIVATE
    rooftree-core-interface
    rooftree-server
    utf8cpp
    qrencode)

# The operations' packets (ApiPackets.h): Api.fbs includes the others
rooftree_add_flatbuffers(blog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/server/schema/Admin.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/server/schema/Reader.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/server/schema/Api.fbs)

set_target_properties(blog
  PROPERTIES
    COMPILE_WARNING_AS_ERROR ${WITH_WARNINGS_AS_ERRORS}
    FOLDER "apps")

# Next to the binary, where blog.yaml's default looks for them; refreshed on every build
add_custom_target(blog-files ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/db/migrations ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/blog-migrations
  VERBATIM)
set_target_properties(blog-files PROPERTIES FOLDER "apps")
add_dependencies(blog blog-files)

# Like arena.yaml: the .dist is always refreshed, blog.yaml is only created once
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
file(COPY_FILE
  ${CMAKE_CURRENT_SOURCE_DIR}/blog.yaml.dist
  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/blog.yaml.dist
  ONLY_IF_DIFFERENT)
if(NOT EXISTS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/blog.yaml)
  file(COPY_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/blog.yaml.dist
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/blog.yaml)
endif()
