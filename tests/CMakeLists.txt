add_executable(midipad_tests
    test_main.cpp
    test_chords.cpp
    test_engine.cpp
    test_labels.cpp
    test_legacy_import.cpp
    test_notes.cpp
    test_preset_io.cpp
    test_preset_library.cpp
    test_scales.cpp
    test_settings.cpp
    test_spsc_ring.cpp
)
target_link_libraries(midipad_tests PRIVATE midipad::core doctest::doctest midipad_warnings)
target_compile_definitions(midipad_tests PRIVATE MIDIPAD_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME midipad_tests COMMAND midipad_tests)

if(MIDIPAD_BUILD_APP)
    add_executable(midipad_platform_tests test_main.cpp test_gamepad_input.cpp test_midi_output.cpp)
    target_link_libraries(midipad_platform_tests PRIVATE midipad_platform doctest::doctest midipad_warnings)
    add_test(NAME midipad_platform_tests COMMAND midipad_platform_tests)
endif()
