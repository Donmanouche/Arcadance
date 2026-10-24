cmake_minimum_required(VERSION 3.25)
project(Arcadance VERSION 0.2.46 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The unit tests are kept out of the published sources: built only where they are present.
if(EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
    set(MIDIPAD_TESTS_PRESENT ON)
else()
    set(MIDIPAD_TESTS_PRESENT OFF)
endif()
option(MIDIPAD_BUILD_TESTS "Build the unit tests (when the tests folder is present)" ${MIDIPAD_TESTS_PRESENT})
option(MIDIPAD_BUILD_APP "Build the arcadance application (needs SDL3 and RtMidi)" ON)

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

if(MIDIPAD_BUILD_TESTS AND MIDIPAD_TESTS_PRESENT)
    enable_testing()
    add_subdirectory(tests)
endif()
