# FlatBuffers: the library (reflection, the verifier) and flatc, which turns the schemas (.fbs) into
# headers at build time. A distribution's package or a build of its own (the Docker image's), found
# by CMAKE_PREFIX_PATH
find_package(flatbuffers CONFIG REQUIRED)

add_library(flatbuffers INTERFACE)

# A static build names its target flatbuffers, a package with only the shared library flatbuffers_shared
if(TARGET flatbuffers::flatbuffers)
  target_link_libraries(flatbuffers INTERFACE flatbuffers::flatbuffers)
else()
  target_link_libraries(flatbuffers INTERFACE flatbuffers::flatbuffers_shared)
endif()

if(TARGET flatbuffers::flatc)
  set(FLATC_EXECUTABLE flatbuffers::flatc CACHE INTERNAL "")
else()
  find_program(FLATC_EXECUTABLE flatc REQUIRED)
endif()

# rooftree_add_flatbuffers(<target> <PUBLIC|PRIVATE> <schema.fbs>...): for every schema its header
# (<name>_generated.h) and the binary schema embedded in another (<name>_bfbs_generated.h, what
# FlatJsonSchema reads), in the target's build directory, generated again when any of them changes
# (a schema may include the others)
function(rooftree_add_flatbuffers target visibility)
  set(outputDirectory ${CMAKE_CURRENT_BINARY_DIR}/fbs)
  set(outputs)
  set(schemas)
  foreach(schema ${ARGN})
    get_filename_component(schema ${schema} ABSOLUTE)
    list(APPEND schemas ${schema})
  endforeach()
  foreach(schema ${schemas})
    get_filename_component(name ${schema} NAME_WE)
    set(generated ${outputDirectory}/${name}_generated.h ${outputDirectory}/${name}_bfbs_generated.h)
    add_custom_command(
      OUTPUT ${generated}
      COMMAND ${FLATC_EXECUTABLE} --cpp --bfbs-gen-embed --scoped-enums --no-warnings -o ${outputDirectory} ${schema}
      DEPENDS ${schemas} ${FLATC_EXECUTABLE}
      COMMENT "Generating C++ code of ${name}.fbs"
      VERBATIM)
    list(APPEND outputs ${generated})
  endforeach()

  target_sources(${target} PRIVATE ${outputs})
  target_include_directories(${target} ${visibility} ${outputDirectory})
endfunction()
