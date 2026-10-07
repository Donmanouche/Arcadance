// midipad: the window by default, the command-line player with --headless.

#include "Headless.hpp"
#include "gui/GuiApp.hpp"

#include <SDL3/SDL_main.h>

#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const bool headless = std::erase(args, std::string("--headless")) > 0;
    return headless ? runHeadless(args) : runGui(args);
}
