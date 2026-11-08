# Header only: Define.h includes valgrind/helgrind.h when HELGRIND is defined
find_path(VALGRIND_INCLUDE_DIR valgrind/helgrind.h REQUIRED)

add_library(valgrind INTERFACE)

target_include_directories(valgrind
  INTERFACE
    "${VALGRIND_INCLUDE_DIR}")
