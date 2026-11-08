# KCP (https://github.com/skywind3000/kcp, MIT, kcp/LICENSE): the ARQ under Rooftree::Net::Kcp. One C
# file without a distribution package, so it is bundled, unchanged: tag 1.7.1, commit 8004f7e
add_library(kcp STATIC kcp/ikcp.c)

target_include_directories(kcp
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/kcp)

# Its code is not ours to fix (-w); position independent code for the PIE executables that link it
set_target_properties(kcp
  PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
    FOLDER "deps")
target_compile_options(kcp PRIVATE -w)
