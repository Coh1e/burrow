cmake_minimum_required(VERSION 3.25)
project(brw LANGUAGES C)

include(CTest)
add_library(digby STATIC digby.c)
set_target_properties(digby PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(BUILD_TESTING)
  add_executable(digby_plan_test digby_plan_test.c)
  target_link_libraries(digby_plan_test PRIVATE digby)
  add_test(NAME digby_plan COMMAND digby_plan_test)
endif()
if(NOT WIN32)
  return()
endif()
if(NOT MINGW)
  message(FATAL_ERROR "This brw configuration requires Windows MinGW-w64")
endif()

set(archive_source "${CMAKE_CURRENT_SOURCE_DIR}/../.local/deps/sources/libarchive-3.7.7")
if(NOT EXISTS "${archive_source}/CMakeLists.txt")
  message(FATAL_ERROR "Run scripts/fetch-deps.ps1 to prepare the pinned libarchive sources")
endif()

# Native packages use tar and ZIP via libarchive.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(feature OPENSSL LIBB2 LZ4 LZMA ZSTD BZip2 LIBXML2 EXPAT
                PCREPOSIX PCRE2POSIX TAR CPIO CAT UNZIP ACL XATTR TEST INSTALL)
  set(ENABLE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(zlib_source "${CMAKE_CURRENT_SOURCE_DIR}/../.local/deps/sources/zlib-1.3.1")
if(NOT EXISTS "${zlib_source}/CMakeLists.txt")
  message(FATAL_ERROR "Run scripts/fetch-deps.ps1 to prepare the pinned zlib sources")
endif()
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${zlib_source}" zlib EXCLUDE_FROM_ALL)
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
# Resolve libarchive's zlib dependency to the pinned target in this build.
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
set(ZLIB_FOUND TRUE)
set(ZLIB_INCLUDE_DIR "${zlib_source};${CMAKE_CURRENT_BINARY_DIR}/zlib")
set(ZLIB_LIBRARY zlibstatic)
set(ZLIB_LIBRARIES zlibstatic)
set(ZLIB_WINAPI FALSE CACHE BOOL "" FORCE)
set(ZLIB_WINAPI_WITH_LIB zlibstatic CACHE STRING "" FORCE)
set(ZLIB_DLL FALSE CACHE BOOL "" FORCE)
set(ZLIB_DLL_WITH_LIB zlibstatic CACHE STRING "" FORCE)
set(WITHOUT_ZLIB_DLL TRUE CACHE BOOL "" FORCE)
set(WITHOUT_ZLIB_DLL_WITH_LIB zlibstatic CACHE STRING "" FORCE)
add_subdirectory("${archive_source}" archive EXCLUDE_FROM_ALL)
unset(CMAKE_DISABLE_FIND_PACKAGE_ZLIB)
set_target_properties(archive_static PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)

add_executable(brw_shim shim_windows.c)
target_link_options(brw_shim PRIVATE -municode -static)
set(shim_header "${CMAKE_CURRENT_BINARY_DIR}/shim_image.h")
add_custom_command(OUTPUT "${shim_header}"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:brw_shim>" "-DOUTPUT=${shim_header}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shim.cmake"
  DEPENDS brw_shim "${CMAKE_CURRENT_SOURCE_DIR}/embed_shim.cmake"
  VERBATIM)

add_executable(brw main_windows.c install_windows.c source_windows.c download_windows.c
  paths_windows.c init_windows.c init_command_windows.c output_windows.c "${shim_header}")
target_include_directories(brw PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${archive_source}/libarchive")
target_compile_definitions(brw PRIVATE LIBARCHIVE_STATIC)
target_link_libraries(brw PRIVATE digby archive_static bcrypt winhttp advapi32 user32)
target_link_options(brw PRIVATE -municode -static)
foreach(target brw brw_shim digby)
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()

if(BUILD_TESTING)
  find_program(POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)
  add_executable(brw_test_payload test_payload.c)
  target_link_options(brw_test_payload PRIVATE -static)
  add_test(NAME brw_local_install
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-local-install.ps1"
      -Brw "$<TARGET_FILE:brw>" -Fixture "$<TARGET_FILE:brw_test_payload>"
      -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  add_executable(brw_test_cancel test_cancel_windows.c)
  target_link_options(brw_test_cancel PRIVATE -municode -static)
  add_test(NAME brw_source
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-source.ps1" -CancelRunner "$<TARGET_FILE:brw_test_cancel>"
      -Brw "$<TARGET_FILE:brw>" -Fixture "$<TARGET_FILE:brw_test_payload>"
      -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME brw_dependencies
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-dependencies.ps1"
      -Brw "$<TARGET_FILE:brw>" -Fixture "$<TARGET_FILE:brw_test_payload>"
      -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME brw_paths
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-paths.ps1"
      -Brw "$<TARGET_FILE:brw>" -Fixture "$<TARGET_FILE:brw_test_payload>"
      -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  add_executable(brw_test_init test_init_windows.c init_windows.c)
  target_link_libraries(brw_test_init PRIVATE advapi32)
  target_link_options(brw_test_init PRIVATE -static)
  add_test(NAME brw_init_path COMMAND brw_test_init)
  add_executable(brw_test_init_registry test_init_registry_windows.c
    init_windows.c init_command_windows.c paths_windows.c output_windows.c)
  target_link_libraries(brw_test_init_registry PRIVATE advapi32 user32)
  target_link_options(brw_test_init_registry PRIVATE -municode -static)
  add_test(NAME brw_init_registry
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-init-registry.ps1"
      -Runner "$<TARGET_FILE:brw_test_init_registry>" -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
endif()
