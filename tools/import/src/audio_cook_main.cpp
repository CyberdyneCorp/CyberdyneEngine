// SPDX-License-Identifier: MIT
// cy_audio_cook: one source file in, one cooked track out. Issue #87.
//
//     cy_audio_cook --source music/theme.ogg --out theme.cyaudio [--option name=value ...]
//
// The options are the audio importer's own schema (`cy::import::audio_options()`), set by name, so
// a sample's build cooks a track exactly as the import pipeline would and a person reading a CMake
// file reads the same option names a sidecar uses. What the pipeline adds — the cache, the sidecar,
// the bundle — is not wanted here: the output is the bare cooked track a player streams from disk.

#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/audio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using namespace cy;

[[nodiscard]] Expected<import::OptionValue, Error> value_for(const import::OptionSpec& spec,
                                                             std::string_view text) noexcept {
    switch (spec.type) {
        case import::OptionType::Bool:
            return import::OptionValue::of_bool(text == "true" || text == "1" || text == "on");
        case import::OptionType::Int: {
            // A copy, NUL-terminated: `text` is a view into the argument after the '='.
            const std::string number(text);
            return import::OptionValue::of_int(std::strtoll(number.c_str(), nullptr, 10));
        }
        case import::OptionType::Float: {
            const std::string number(text);
            return import::OptionValue::of_float(std::strtod(number.c_str(), nullptr));
        }
        case import::OptionType::Enumeration:
            return import::OptionValue::of_enumeration(text);
        case import::OptionType::Text:
            return import::OptionValue::of_text(text);
    }
    return fail(ErrorCode::InvalidArgument, "an option of an unknown type");
}

/// Apply one `name=value` to `options`, checked against the importer's schema.
[[nodiscard]] Status apply(const char* assignment, import::ImportOptions& options) noexcept {
    const std::string_view text(assignment);
    const usize equals = text.find('=');
    if (equals == std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "an --option is name=value");
    }
    const import::OptionsSchema schema = import::audio_options();
    const std::string_view name = text.substr(0, equals);
    const import::OptionSpec* spec = schema.find(name);
    if (spec == nullptr) {
        return fail(ErrorCode::NotFound, "the audio importer has no option of that name");
    }
    const Expected<import::OptionValue, Error> value = value_for(*spec, text.substr(equals + 1));
    if (!value) {
        return make_unexpected(value.error());
    }
    return options.set(schema, name, *value);
}

int usage() {
    std::fprintf(stderr,
                 "usage: cy_audio_cook --source <file> --out <file> [--option name=value ...]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const char* source = nullptr;
    const char* out = nullptr;
    import::ImportOptions options;
    for (int index = 1; index < argc; ++index) {
        const bool valued = index + 1 < argc;
        if (std::strcmp(argv[index], "--source") == 0 && valued) {
            source = argv[++index];
        } else if (std::strcmp(argv[index], "--out") == 0 && valued) {
            out = argv[++index];
        } else if (std::strcmp(argv[index], "--option") == 0 && valued) {
            if (Status applied = apply(argv[++index], options); !applied) {
                std::fprintf(stderr, "cy_audio_cook: %s: %s\n", argv[index],
                             applied.error().message);
                return 2;
            }
        } else {
            return usage();
        }
    }
    if (source == nullptr || out == nullptr) {
        return usage();
    }

    Array<u8> bytes(system_allocator(MemoryDomain::Assets));
    if (Status read = assets::fs::read_whole(source, bytes); !read) {
        std::fprintf(stderr, "cy_audio_cook: cannot read %s: %s\n", source, read.error().message);
        return 1;
    }
    import::AudioImporter importer;
    import::ImportRequest request;
    request.source = assets::VirtualPath::normalise("source/track").value();
    request.bytes = bytes.span();
    request.options = &options;
    import::ImportResult result;
    if (Status imported = importer.import(request, result); !imported || result.has_errors()) {
        for (const import::ImportDiagnostic& diagnostic : result.diagnostics()) {
            std::fprintf(stderr, "cy_audio_cook: %s: %s\n", diagnostic.code, diagnostic.detail);
        }
        return 1;
    }
    const import::SubAsset* track = result.primary();
    if (Status written =
            assets::fs::write_atomic(out, track->payload.data(), track->payload.size());
        !written) {
        std::fprintf(stderr, "cy_audio_cook: cannot write %s: %s\n", out, written.error().message);
        return 1;
    }
    std::printf("cy_audio_cook: %s -> %s (%zu bytes)\n", source, out, track->payload.size());
    return 0;
}
