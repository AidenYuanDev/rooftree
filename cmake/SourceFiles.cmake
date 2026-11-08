# This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
#
# CollectAndAddSourceFiles(target dir [EXCLUDE dirs...]): the .cpp files under dir into target, its
# headers as a header file set, every directory an include directory. Directories starting with a dot
# (editor caches) are left out, and the ones in EXCLUDE with what is under them

function(CollectAndAddSourceFiles target dir)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "BASE_DIR;HEADER_VISIBILITY" "EXCLUDE")
  if(NOT arg_BASE_DIR)
    set(arg_BASE_DIR "${dir}")
  endif()
  if(NOT arg_HEADER_VISIBILITY)
    set(arg_HEADER_VISIBILITY PUBLIC)
  endif()
  if(dir IN_LIST arg_EXCLUDE)
    return()
  endif()

  file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.cpp" "${dir}/*.cc" "${dir}/*.c")
  file(GLOB headers CONFIGURE_DEPENDS "${dir}/*.h" "${dir}/*.hpp" "${dir}/*.inl")
  target_sources(${target} PRIVATE ${sources})

  # A file set's name may have letters, digits and underscores
  cmake_path(RELATIVE_PATH dir BASE_DIRECTORY "${arg_BASE_DIR}" OUTPUT_VARIABLE relative)
  string(MAKE_C_IDENTIFIER "headers_${relative}" set_name)
  target_sources(${target} ${arg_HEADER_VISIBILITY} FILE_SET ${set_name} TYPE HEADERS BASE_DIRS "${dir}" FILES ${headers})

  file(GLOB entries "${dir}/*")
  foreach(entry ${entries})
    cmake_path(GET entry FILENAME name)
    if(IS_DIRECTORY "${entry}" AND NOT name MATCHES "^\\.")
      CollectAndAddSourceFiles(${target} "${entry}" BASE_DIR "${arg_BASE_DIR}" HEADER_VISIBILITY ${arg_HEADER_VISIBILITY} EXCLUDE ${arg_EXCLUDE})
    endif()
  endforeach()
endfunction()
