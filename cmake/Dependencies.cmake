# Third-party libraries are fetched at configure time so every platform builds
# against the same pinned versions.
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

if(MIDIPAD_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.12
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)
endif()

if(MIDIPAD_BUILD_APP)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.8
        GIT_SHALLOW TRUE
        SYSTEM)

    set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi_uninstall" CACHE STRING "" FORCE)
    FetchContent_Declare(rtmidi
        GIT_REPOSITORY https://github.com/thestk/rtmidi.git
        GIT_TAG 6.0.0
        GIT_SHALLOW TRUE
        SYSTEM)

    FetchContent_MakeAvailable(SDL3 rtmidi)
endif()
