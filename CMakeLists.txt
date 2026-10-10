cmake_minimum_required(VERSION 3.25)
project(MidiPad VERSION 0.14 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MIDIPAD_BUILD_TESTS "Build the unit tests" ON)
option(MIDIPAD_BUILD_APP "Build the midipad application (needs SDL3 and RtMidi)" ON)

include(cmake/Dependencies.cmake)

add_library(midipad_warnings INTERFACE)
if(MSVC)
    target_compile_options(midipad_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(midipad_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_subdirectory(src/core)

if(MIDIPAD_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(MIDIPAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
