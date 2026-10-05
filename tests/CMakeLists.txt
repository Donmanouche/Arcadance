add_executable(midipad_tests
    test_main.cpp
    test_chords.cpp
    test_engine.cpp
    test_legacy_import.cpp
    test_notes.cpp
    test_preset_io.cpp
)
target_link_libraries(midipad_tests PRIVATE midipad::core doctest::doctest midipad_warnings)
target_compile_definitions(midipad_tests PRIVATE MIDIPAD_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME midipad_tests COMMAND midipad_tests)
