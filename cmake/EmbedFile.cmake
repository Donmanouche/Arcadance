# Generates a C++ source holding the bytes of a file:
#   extern const unsigned char <symbol>[];  extern const std::size_t <symbol>_size;
function(midipad_embed_file input output symbol)
    file(READ "${input}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE "${output}"
        "// Generated from ${input}\n#include <cstddef>\n"
        "extern const unsigned char ${symbol}[] = {${bytes}};\n"
        "extern const std::size_t ${symbol}_size = sizeof(${symbol});\n")
endfunction()
