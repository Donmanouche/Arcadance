# Platform glue (SDL3 input, RtMidi output, real-time thread), shared by the
# app and its tests.
add_library(midipad_platform STATIC
    GamepadInput.cpp
    MidiOutput.cpp
    Runtime.cpp
)
target_include_directories(midipad_platform PUBLIC .)
target_link_libraries(midipad_platform
    PUBLIC midipad::core SDL3::SDL3 rtmidi
    PRIVATE midipad_warnings)

include(${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake)
set(MIDIPAD_FONT_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/RobotoMedium.cpp)
midipad_embed_file(${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf ${MIDIPAD_FONT_SOURCE} kRobotoMedium)
set(MIDIPAD_FONT_BOLD_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/RobotoBold.cpp)
midipad_embed_file(${PROJECT_SOURCE_DIR}/assets/fonts/Roboto-Bold.ttf ${MIDIPAD_FONT_BOLD_SOURCE} kRobotoBold)
set(MIDIPAD_FONT_CONDENSED_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/RobotoCondensedBold.cpp)
midipad_embed_file(${PROJECT_SOURCE_DIR}/assets/fonts/RobotoCondensed-Bold.ttf ${MIDIPAD_FONT_CONDENSED_SOURCE} kRobotoCondensedBold)

add_executable(midipad
    main.cpp
    Headless.cpp
    gui/GuiApp.cpp
    gui/MappingEditor.cpp
    gui/PerformanceView.cpp
    ${MIDIPAD_FONT_SOURCE}
    ${MIDIPAD_FONT_BOLD_SOURCE}
    ${MIDIPAD_FONT_CONDENSED_SOURCE}
)
target_link_libraries(midipad PRIVATE midipad_platform imgui midipad_warnings)
