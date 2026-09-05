// module.toml: the declaration that turns a shared library into a module. Task 2.4.
//
// `native-abi`: "The manifest (`module.toml`) SHALL declare: module name, entry symbol, minimum ABI
// version, per-platform library paths, and whether the module is hot-reloadable."
//
// THE SUBSET IS THE DESIGN, NOT A SHORTCUT. This parser reads top-level `key = value` pairs and
// `[platform.<name>]` tables, and nothing else. A module manifest is a handful of declarations read
// once, before the engine is up; a TOML library at layer 6 would be a dependency taken on for a
// file with six keys in it. What the subset does keep is the property that matters:
// **an unknown key is an error naming it**, exactly as cmake/modules.cmake treats the build-time
// manifests, and for the same reason — a typo that is skipped is a setting that silently did not
// apply, and the symptom arrives much later and somewhere else.

#include <cy/abi/module.h>

#include <cstring>

namespace cy::abi {
namespace {

bool is_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r';
}

// Trim in place and return the start. The text is the caller's and is modified deliberately: that
// is what lets every string in the manifest be a pointer into it rather than a copy.
char* trim(char* text) noexcept {
    while (*text != '\0' && is_space(*text)) {
        ++text;
    }
    char* end = text + std::strlen(text);
    while (end > text && is_space(end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

// A quoted string, unquoted in place. Null when the value is not one, so that `name = game` is an
// error rather than a name of `game` that would work until someone wrote `name = my game`.
char* unquote(char* value) noexcept {
    const usize length = std::strlen(value);
    if (length < 2 || value[0] != '"' || value[length - 1] != '"') {
        return nullptr;
    }
    value[length - 1] = '\0';
    return value + 1;
}

Expected<u32, Error> parse_u32(const char* value) noexcept {
    if (*value == '\0') {
        return fail(ErrorCode::InvalidArgument, "an integer key was given an empty value");
    }
    u64 result = 0;
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return fail(ErrorCode::InvalidArgument, "an integer key was given a non-integer");
        }
        result = (result * 10) + static_cast<u64>(*cursor - '0');
        if (result > 0xFFFF'FFFFu) {
            return fail(ErrorCode::OutOfRange, "an integer key overflowed 32 bits");
        }
    }
    return static_cast<u32>(result);
}

Expected<ModulePlatform, Error> platform_from_name(const char* name) noexcept {
    for (u8 index = 0; index < static_cast<u8>(ModulePlatform::Count); ++index) {
        const auto platform = static_cast<ModulePlatform>(index);
        if (std::strcmp(module_platform_name(platform), name) == 0) {
            return platform;
        }
    }
    return fail(ErrorCode::NotFound, "unknown platform in a [platform.<name>] table");
}

// The section a key belongs to. `Count` stands for "no platform section is open", which is the
// top-level table; a separate flag would be the same information said twice.
struct Section {
    ModulePlatform platform = ModulePlatform::Count;
    bool in_platform = false;
};

Status apply_top_level(ModuleManifest& manifest, const char* key, char* value) noexcept {
    if (std::strcmp(key, "name") == 0 || std::strcmp(key, "entry_symbol") == 0) {
        char* text = unquote(value);
        if (text == nullptr) {
            return fail(ErrorCode::InvalidArgument, "a string key needs a quoted value");
        }
        if (std::strcmp(key, "name") == 0) {
            manifest.name = text;
        } else {
            manifest.entry_symbol = text;
        }
        return ok();
    }
    if (std::strcmp(key, "min_abi_major") == 0 || std::strcmp(key, "min_abi_minor") == 0) {
        Expected<u32, Error> number = parse_u32(value);
        if (!number) {
            return make_unexpected(number.error());
        }
        if (std::strcmp(key, "min_abi_major") == 0) {
            manifest.min_abi_major = number.value();
        } else {
            manifest.min_abi_minor = number.value();
        }
        return ok();
    }
    if (std::strcmp(key, "hot_reload") == 0) {
        if (std::strcmp(value, "true") == 0) {
            manifest.hot_reload = true;
            return ok();
        }
        if (std::strcmp(value, "false") == 0) {
            manifest.hot_reload = false;
            return ok();
        }
        return fail(ErrorCode::InvalidArgument, "hot_reload is true or false");
    }
    return fail(ErrorCode::InvalidArgument, "unknown key in module.toml");
}

Status apply_line(ModuleManifest& manifest, Section& section, char* line) noexcept {
    if (line[0] == '[') {
        const usize length = std::strlen(line);
        if (length < 3 || line[length - 1] != ']') {
            return fail(ErrorCode::InvalidArgument, "a table header is [name]");
        }
        line[length - 1] = '\0';
        char* header = trim(line + 1);
        if (std::strncmp(header, "platform.", 9) != 0) {
            return fail(ErrorCode::InvalidArgument,
                        "the only table in module.toml is [platform.<name>]");
        }
        Expected<ModulePlatform, Error> platform = platform_from_name(header + 9);
        if (!platform) {
            return make_unexpected(platform.error());
        }
        section.platform = platform.value();
        section.in_platform = true;
        return ok();
    }

    char* equals = std::strchr(line, '=');
    if (equals == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a manifest line is a key = value pair");
    }
    *equals = '\0';
    const char* key = trim(line);
    char* value = trim(equals + 1);
    if (*key == '\0') {
        return fail(ErrorCode::InvalidArgument, "a manifest line is a key = value pair");
    }

    if (!section.in_platform) {
        return apply_top_level(manifest, key, value);
    }
    if (std::strcmp(key, "library") != 0) {
        return fail(ErrorCode::InvalidArgument, "a [platform.<name>] table declares `library`");
    }
    char* text = unquote(value);
    if (text == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a string key needs a quoted value");
    }
    manifest.library[static_cast<usize>(section.platform)] = text;
    return ok();
}

}  // namespace

const char* module_platform_name(ModulePlatform platform) noexcept {
    switch (platform) {
        case ModulePlatform::Linux:
            return "linux";
        case ModulePlatform::Windows:
            return "windows";
        case ModulePlatform::MacOS:
            return "macos";
        case ModulePlatform::Ios:
            return "ios";
        case ModulePlatform::Android:
            return "android";
        case ModulePlatform::VisionOs:
            return "visionos";
        case ModulePlatform::Web:
            return "web";
        case ModulePlatform::Count:
            break;
    }
    return "unknown";
}

ModulePlatform host_module_platform() noexcept {
#if defined(_WIN32)
    return ModulePlatform::Windows;
#elif defined(__APPLE__)
    return ModulePlatform::MacOS;
#elif defined(__ANDROID__)
    return ModulePlatform::Android;
#elif defined(__EMSCRIPTEN__)
    return ModulePlatform::Web;
#else
    return ModulePlatform::Linux;
#endif
}

const char* ModuleManifest::library_for(ModulePlatform platform) const noexcept {
    if (platform == ModulePlatform::Count) {
        return nullptr;
    }
    const char* path = library[static_cast<usize>(platform)];
    return (path != nullptr && path[0] != '\0') ? path : nullptr;
}

Expected<ModuleManifest, Error> parse_module_manifest(char* text) noexcept {
    if (text == nullptr) {
        return fail(ErrorCode::InvalidArgument, "module.toml is empty");
    }

    ModuleManifest manifest;
    Section section;
    char* cursor = text;
    while (*cursor != '\0') {
        char* line = cursor;
        char* newline = std::strchr(cursor, '\n');
        if (newline != nullptr) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = line + std::strlen(line);
        }

        // A comment runs to the end of the line. It is cut before trimming so that a trailing
        // comment on a value line does not become part of the value.
        if (char* comment = std::strchr(line, '#'); comment != nullptr) {
            *comment = '\0';
        }
        line = trim(line);
        if (*line == '\0') {
            continue;
        }
        if (Status applied = apply_line(manifest, section, line); !applied) {
            return make_unexpected(applied.error());
        }
    }

    if (manifest.name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "module.toml must declare a name");
    }
    if (manifest.entry_symbol[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "module.toml must declare an entry symbol");
    }
    return manifest;
}

}  // namespace cy::abi
