set(quickjs_source "${CMAKE_CURRENT_SOURCE_DIR}/../.local/deps/sources/quickjs-2026-06-04")
if(NOT EXISTS "${quickjs_source}/quickjs.c")
  message(FATAL_ERROR "Prepare the pinned Blueprint sources as described in brw/blueprint.md")
endif()
add_library(brw_quickjs STATIC
  "${quickjs_source}/quickjs.c" "${quickjs_source}/dtoa.c"
  "${quickjs_source}/libregexp.c" "${quickjs_source}/libunicode.c"
  "${quickjs_source}/cutils.c")
set_target_properties(brw_quickjs PROPERTIES C_STANDARD 11 C_EXTENSIONS YES)
target_include_directories(brw_quickjs SYSTEM PUBLIC "${quickjs_source}")
target_compile_definitions(brw_quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="2026-06-04")
target_compile_options(brw_quickjs PRIVATE -fwrapv)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(brw_quickjs PUBLIC m Threads::Threads)
endif()
configure_file("${quickjs_source}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/licenses/quickjs.txt" COPYONLY)
