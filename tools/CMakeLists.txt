add_executable(midipad-import midipad_import.cpp)
target_link_libraries(midipad-import PRIVATE midipad::core midipad_warnings)
