cmake_minimum_required(VERSION 3.10...3.25)
project(brw LANGUAGES C)
set(CMAKE_C_STANDARD 99)
if(WIN32)
  cmake_minimum_required(VERSION 3.25)
endif()

include(CTest)
add_library(digby STATIC digby.c literal.c)
set_target_properties(digby PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
option(BRW_ENABLE_CPP_MODULE "Format literal search offsets with the optional C++23 module" OFF)
if(BRW_ENABLE_CPP_MODULE)
  enable_language(CXX)
  add_library(brw_literal_offset STATIC literal_offset.cpp)
  target_compile_features(brw_literal_offset PRIVATE cxx_std_23)
  set_target_properties(brw_literal_offset PROPERTIES CXX_EXTENSIONS NO)
  target_compile_definitions(digby PRIVATE BRW_ENABLE_CPP_MODULE)
  target_link_libraries(digby PRIVATE brw_literal_offset)
  if(BUILD_TESTING)
    add_executable(literal_offset_test literal_offset_test.c)
    target_link_libraries(literal_offset_test PRIVATE brw_literal_offset)
    add_test(NAME digby_cpp_abi COMMAND literal_offset_test)
  endif()
endif()
if(BUILD_TESTING)
  add_executable(digby_plan_test digby_plan_test.c)
  target_link_libraries(digby_plan_test PRIVATE digby)
  add_test(NAME digby_plan COMMAND digby_plan_test)
  add_executable(literal_test literal_test.c)
  target_link_libraries(literal_test PRIVATE digby)
  add_test(NAME digby_literal COMMAND literal_test)
endif()
option(BRW_MINIMAL "Build the offline Linux file, text and tar CLI" OFF)
option(BRW_BLUEPRINT "Build the offline QuickJS Blueprint CLI" ${WIN32})
if(BRW_BLUEPRINT AND BRW_MINIMAL)
  message(FATAL_ERROR "Use a separate build directory for the QuickJS configuration")
endif()
if(WIN32 OR BRW_MINIMAL OR BRW_BLUEPRINT)
  include(archive.cmake)
endif()
if(BRW_BLUEPRINT)
  include(blueprint.cmake)
endif()
if(BRW_MINIMAL AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "BRW_MINIMAL requires Linux")
endif()
if(NOT WIN32 AND NOT BRW_MINIMAL)
  if(BRW_BLUEPRINT)
    add_executable(brw blueprint_main.c)
    target_link_libraries(brw PRIVATE brw_blueprint)
  endif()
  option(BRW_TEST_GIT "Build managed worktree contract tests with the pinned Git sources" OFF)
  if(BUILD_TESTING AND BRW_TEST_GIT)
    include(git.cmake)
  endif()
  return()
endif()
if(WIN32 AND NOT MINGW)
  message(FATAL_ERROR "This brw configuration requires Windows MinGW-w64")
endif()

if(BRW_MINIMAL)
  add_executable(brw minimal_linux.c)
  set_target_properties(brw PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    LINK_FLAGS "-static -s -Wl,-u,pthread_mutex_lock")
  target_compile_options(brw PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(brw PRIVATE "${archive_source}/libarchive")
  # GCC 4.8's static unwind tables reference these libc thread primitives.
  target_link_libraries(brw PRIVATE digby archive_static pthread)
  if(BUILD_TESTING)
    add_test(NAME minimal_cli COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/minimal_test.sh" "$<TARGET_FILE:brw>")
  endif()
  return()
endif()

include(git.cmake)
configure_file("${git_source}/COPYING" "${CMAKE_CURRENT_BINARY_DIR}/licenses/libgit2.txt" COPYONLY)

add_executable(brw_shim shim_windows.c)
target_link_options(brw_shim PRIVATE -municode -static -Wl,--no-insert-timestamp)
add_executable(brw_gui_shim WIN32 shim_windows.c)
target_link_options(brw_gui_shim PRIVATE -municode -static -Wl,--no-insert-timestamp)
set(shim_header "${CMAKE_CURRENT_BINARY_DIR}/shim_image.h")
add_custom_command(OUTPUT "${shim_header}"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:brw_shim>" "-DGUI=$<TARGET_FILE:brw_gui_shim>" "-DOUTPUT=${shim_header}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shim.cmake"
  DEPENDS brw_shim brw_gui_shim "${CMAKE_CURRENT_SOURCE_DIR}/embed_shim.cmake"
  VERBATIM)

add_library(brw_common OBJECT main_windows.c git_windows.c cleanup_windows.c evidence_windows.c source_windows.c
  download_windows.c paths_windows.c init_windows.c init_command_windows.c settings_command_windows.c
  output_windows.c "${shim_header}")
target_include_directories(brw_common SYSTEM PRIVATE "${git_source}/include")
if(BRW_BLUEPRINT)
  target_compile_definitions(brw_common PRIVATE BRW_BLUEPRINT)
  target_sources(brw_common PRIVATE blueprint_argv_windows.c)
endif()
function(add_brw_cli target)
  add_executable(${target} install_windows.c settings_windows.c $<TARGET_OBJECTS:brw_common>)
  add_dependencies(${target} brw_common)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${archive_source}/libarchive")
  target_compile_definitions(${target} PRIVATE LIBARCHIVE_STATIC)
  target_link_libraries(${target} PRIVATE digby archive_static brw_worktree bcrypt winhttp advapi32 user32 shell32 ktmw32)
  if(BRW_BLUEPRINT)
    target_link_libraries(${target} PRIVATE brw_blueprint)
  endif()
  target_link_options(${target} PRIVATE -municode -static)
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()
add_brw_cli(brw)
foreach(target brw_common brw_shim brw_gui_shim digby)
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()

if(BUILD_TESTING)
  add_brw_cli(brw_test_interrupt)
  target_compile_definitions(brw_test_interrupt PRIVATE BRW_TEST_INTERRUPTS)
  find_program(POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)
  add_test(NAME brw_git COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-git.ps1"
    -Brw "$<TARGET_FILE:brw>" -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME brw_settings
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-settings.ps1"
      -Brw "$<TARGET_FILE:brw_test_interrupt>" -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  option(BRW_TEST_USER_REGISTRY "Test KTM in a disposable current-user registry subtree" OFF)
  if(BRW_TEST_USER_REGISTRY)
    add_test(NAME brw_settings_user_registry
      COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-settings.ps1" -UserRegistry
        -Brw "$<TARGET_FILE:brw_test_interrupt>" -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  add_executable(brw_test_payload test_payload.c)
  target_link_options(brw_test_payload PRIVATE -municode -static)
  add_executable(brw_test_gui WIN32 test_gui_windows.c)
  target_link_options(brw_test_gui PRIVATE -municode -static)
  add_test(NAME brw_local_install
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-local-install.ps1" -Gui "$<TARGET_FILE:brw_test_gui>"
      -InterruptBrw "$<TARGET_FILE:brw_test_interrupt>"
      -Brw "$<TARGET_FILE:brw>" -Fixture "$<TARGET_FILE:brw_test_payload>"
      -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  foreach(layout same_volume cross_volume)
    add_test(NAME brw_recovery_${layout}
      COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/../tests/brw-recovery.ps1" -Layout "${layout}"
        -InterruptBrw "$<TARGET_FILE:brw_test_interrupt>"
        -Brw "$<TARGET_FILE:brw>" -Fixture "$<TARGET_FILE:brw_test_payload>"
        -Scratch "${CMAKE_CURRENT_BINARY_DIR}")
  endforeach()
  set_tests_properties(brw_recovery_cross_volume PROPERTIES SKIP_RETURN_CODE 77)
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
