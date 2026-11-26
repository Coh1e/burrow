cmake_minimum_required(VERSION 3.25)
project(devkit_probe LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(c_probe probe.c)
target_compile_features(c_probe PRIVATE c_std_99)

add_executable(cxx_probe probe.cpp)
target_compile_features(cxx_probe PRIVATE cxx_std_23)
