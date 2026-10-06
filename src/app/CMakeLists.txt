# Platform glue (SDL3 input, RtMidi output), shared by the app and its tests.
add_library(midipad_platform STATIC
    GamepadInput.cpp
    MidiOutput.cpp
)
target_include_directories(midipad_platform PUBLIC .)
target_link_libraries(midipad_platform
    PUBLIC midipad::core SDL3::SDL3 rtmidi
    PRIVATE midipad_warnings)

add_executable(midipad main.cpp)
target_link_libraries(midipad PRIVATE midipad_platform midipad_warnings)
