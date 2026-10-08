// SPDX-License-Identifier: MIT
#ifndef CY_BACKENDS_TEXT_COMPLETE_TESTS_FONTS_H
#define CY_BACKENDS_TEXT_COMPLETE_TESTS_FONTS_H
// The suite's fixtures: the fonts in deps/fonts/ (tools/content/make_fonts.py made them, and
// deps/fonts/PROVENANCE.md records where from), a started backend and a server over it.

#include <cy/backends/text/complete_backend.h>
#include <cy/servers/text/server.h>
#include <cy/test/test.h>

#include <cstdio>
#include <string>

namespace cy::text::test {

/// A font's bytes, read whole. Empty — and a failed REQUIRE — when the file is missing, which is a
/// checkout problem rather than a backend one, and says so.
inline Array<u8> read_font(const char* name) {
    const std::string path = std::string(CY_TEXT_FONTS_DIR) + "/" + name;
    Array<u8> bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    CY_REQUIRE_MESSAGE(file != nullptr, "missing test font " << path);
    // Sized first and read in one call: a read loop that stops on a short read leaves the stream at
    // its end, which the analyser rightly calls a read with no effect.
    const bool sized = std::fseek(file, 0, SEEK_END) == 0;
    const long length = sized ? std::ftell(file) : -1L;
    const bool rewound = length > 0 && std::fseek(file, 0, SEEK_SET) == 0;
    CY_REQUIRE(rewound);
    CY_REQUIRE(bytes.resize(static_cast<usize>(length)).has_value());
    const usize read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    CY_REQUIRE_EQ(read, bytes.size());
    return bytes;
}

inline FontSource source_of(const Array<u8>& bytes, u32 face_index = 0) {
    return FontSource{Span<const u8>(bytes.data(), bytes.size()), face_index};
}

inline FontDesc desc_of(f32 size, RenderMode mode = RenderMode::Grayscale) {
    FontDesc desc;
    desc.family = "test";
    desc.size_pixels = size;
    desc.mode = mode;
    return desc;
}

inline TextServerConfig server_config() {
    TextServerConfig config;
    config.atlas.initial_extent = 256;
    config.atlas.maximum_extent = 2048;
    return config;
}

/// A started backend and a server over it, stopped in the right order on the way out.
struct Stack {
    CompleteTextBackend backend;
    TextServer server;

    Stack() {
        CY_REQUIRE(backend.start().has_value());
        CY_REQUIRE(server.start_with(server_config(), backend).has_value());
    }
    ~Stack() {
        server.stop();
        backend.stop();
    }
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;
    Stack(Stack&&) = delete;
    Stack& operator=(Stack&&) = delete;

    FontHandle face(const Array<u8>& bytes, const FontDesc& desc, u32 face_index = 0) {
        Expected<FontHandle, Error> made = server.create_face(desc, source_of(bytes, face_index));
        CY_REQUIRE_MESSAGE(made.has_value(), (made ? "" : made.error().message));
        return made.value();
    }
};

inline FallbackChain chain_of(FontHandle first, FontHandle second = {}) {
    FallbackChain chain;
    CY_REQUIRE(chain.push(first).has_value());
    if (!second.is_null()) {
        CY_REQUIRE(chain.push(second).has_value());
    }
    return chain;
}

}  // namespace cy::text::test

#endif  // CY_BACKENDS_TEXT_COMPLETE_TESTS_FONTS_H
