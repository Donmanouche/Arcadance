#pragma once

#include <string>
#include <vector>

// The command-line player; `args` excludes the program name and --headless.
int runHeadless(const std::vector<std::string>& args);
