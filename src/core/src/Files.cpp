#include "midipad/Files.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace midipad {

void writeFileAtomically(const std::filesystem::path& file, std::string_view text) {
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path());
    std::filesystem::path temp = file;
    temp += ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error(temp.string() + ": cannot write");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();  // the last bytes are written here: checked before the rename
    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw std::runtime_error(temp.string() + ": write failed");
    }
    std::filesystem::rename(temp, file);
}

}  // namespace midipad
