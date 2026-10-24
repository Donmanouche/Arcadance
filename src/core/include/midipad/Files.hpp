#pragma once

#include <filesystem>
#include <string_view>

namespace midipad {

// Writes `text` to `file` through a temporary file next to it, renamed over
// it once every byte is written: a crash or a full disk never leaves half a
// file, and the previous one stays as it was. Creates the folder if needed.
// Throws std::runtime_error (or std::filesystem::filesystem_error) on failure.
void writeFileAtomically(const std::filesystem::path& file, std::string_view text);

}  // namespace midipad
