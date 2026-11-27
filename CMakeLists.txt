cmake_minimum_required(VERSION 3.10...3.25)
project(burrow LANGUAGES C)

include(CTest)
if(NOT BRW_MINIMAL)
  option(BRW_BLUEPRINT "Build the offline QuickJS Blueprint CLI" ON)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_subdirectory(brw native)
