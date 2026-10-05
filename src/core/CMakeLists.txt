add_library(midipad_core STATIC
    src/Chords.cpp
    src/Controls.cpp
    src/Engine.cpp
    src/LegacyImport.cpp
    src/Notes.cpp
    src/PresetIO.cpp
)
add_library(midipad::core ALIAS midipad_core)

target_include_directories(midipad_core PUBLIC include)
target_link_libraries(midipad_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE midipad_warnings)
