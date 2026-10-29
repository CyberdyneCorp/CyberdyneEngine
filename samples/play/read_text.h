// SPDX-License-Identifier: MIT
// read_text.h — a small text file (an input script, a bindings file) read whole.

#pragma once

#include <cy/core/base/types.h>

#include <cstdio>
#include <string>

namespace cy::play {

/// The whole of the file at `path` into `out`; false when it cannot be opened or read.
[[nodiscard]] inline bool read_text(const char* path, std::string& out) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    bool read = std::fseek(file, 0, SEEK_END) == 0;
    const long length = read ? std::ftell(file) : -1;
    read = length >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (read) {
        out.assign(static_cast<usize>(length), '\0');
        read = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    (void)std::fclose(file);
    return read;
}

}  // namespace cy::play
