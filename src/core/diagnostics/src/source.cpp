// The source-location table and the path rule.
//
// Two separate things live here on purpose and the distinction is the whole finding M0's gate left
// open. SANITISATION removes what no artefact may carry — an absolute path, which names the machine
// a build came from — and it happens whatever the policy is. CLASSIFICATION decides whether the
// sanitised remainder is carried at all, and it is the writer's, comparing the entry against the
// artefact's declared ceiling. One is a rule about the string; the other is a rule about the
// artefact. A compiler flag can approximate the first on some toolchains and can do nothing at all
// about the second.

#include <cy/core/diagnostics/source.h>

#include <cy/core/diagnostics/field.h>

#include <cstring>
#include <mutex>

namespace cy::diag {
namespace {

struct LocationEntry {
    const char* file = nullptr;
    u32 line = 0;
};

struct LocationTable {
    std::mutex mutex;
    LocationEntry entries[kMaxSourceLocations] = {};
    u32 count = 0;
};

LocationTable& table() noexcept {
    static LocationTable instance;
    return instance;
}

bool same_text(const char* a, const char* b) noexcept {
    return a == b || (a != nullptr && b != nullptr && std::strcmp(a, b) == 0);
}

/// Absolute in the only sense that matters here: it begins somewhere other than where the build
/// ran, so it names a machine. Both separators and a Windows drive letter, because the artefact a
/// Windows build writes is read by the same tools.
bool is_absolute(const char* path, usize length) noexcept {
    if (length == 0) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    return length >= 2 && path[1] == ':';
}

usize last_separator(const char* path, usize length) noexcept {
    for (usize index = length; index > 0; --index) {
        const char character = path[index - 1];
        if (character == '/' || character == '\\') {
            return index;  // one past the separator
        }
    }
    return 0;
}

void copy_out(const char* source, usize length, char* out, u32 capacity, u32* written) noexcept {
    // Truncate from the LEFT: the tail names the file, the head names the machine, and a path that
    // does not fit should lose the half that is not the answer.
    usize start = 0;
    if (length >= capacity) {
        start = length - (capacity - 1);
    }
    const usize kept = length - start;
    std::memcpy(out, source + start, kept);
    out[kept] = '\0';
    if (written != nullptr) {
        *written = static_cast<u32>(kept);
    }
}

}  // namespace

const char* source_root() noexcept {
#if defined(CY_DIAG_SOURCE_ROOT)
    return CY_DIAG_SOURCE_ROOT;
#else
    return "";
#endif
}

LocationId register_source_location(const char* file, u32 line) noexcept {
    if (file == nullptr) {
        return kInvalidLocation;
    }
    LocationTable& locations = table();
    const std::lock_guard<std::mutex> guard(locations.mutex);
    for (u32 index = 0; index < locations.count; ++index) {
        if (locations.entries[index].line == line &&
            same_text(locations.entries[index].file, file)) {
            return index + 1;
        }
    }
    if (locations.count == kMaxSourceLocations) {
        return kInvalidLocation;  // counted by the registry's own rejection counter below
    }
    locations.entries[locations.count] = LocationEntry{file, line};
    ++locations.count;
    return locations.count;
}

bool lookup_source_location(LocationId id, SourceLocation& out) noexcept {
    LocationTable& locations = table();
    const std::lock_guard<std::mutex> guard(locations.mutex);
    if (id == kInvalidLocation || id > locations.count) {
        return false;
    }
    const LocationEntry& entry = locations.entries[id - 1];
    out = SourceLocation{entry.file, entry.line, Privacy::Developer};
    return true;
}

u32 source_location_count() noexcept {
    LocationTable& locations = table();
    const std::lock_guard<std::mutex> guard(locations.mutex);
    return locations.count;
}

PathForm sanitise_source_path(const char* file, char* out, u32 capacity, u32* length) noexcept {
    if (out == nullptr || capacity == 0) {
        return PathForm::Empty;
    }
    out[0] = '\0';
    if (length != nullptr) {
        *length = 0;
    }
    if (file == nullptr || file[0] == '\0') {
        return PathForm::Empty;
    }

    const char* cursor = file;
    usize remaining = std::strlen(cursor);

    // 1. The root this build declared, when the compiler did not already remove it.
    const char* root = source_root();
    const usize root_length = std::strlen(root);
    if (root_length != 0 && remaining >= root_length &&
        std::memcmp(cursor, root, root_length) == 0) {
        cursor += root_length;
        remaining -= root_length;
        while (remaining != 0 && (cursor[0] == '/' || cursor[0] == '\\')) {
            ++cursor;
            --remaining;
        }
        if (remaining == 0) {
            return PathForm::Empty;
        }
        copy_out(cursor, remaining, out, capacity, length);
        return PathForm::Relative;
    }

    // 2. Still absolute: it belongs to a tree this build knows nothing about, so only the file's
    // own
    //    name survives. This is the step that does not need a compiler flag to exist.
    if (is_absolute(cursor, remaining)) {
        const usize separator = last_separator(cursor, remaining);
        if (separator >= remaining) {
            return PathForm::Empty;  // a path that is only separators names nothing
        }
        copy_out(cursor + separator, remaining - separator, out, capacity, length);
        return PathForm::Basename;
    }

    // 3. Already relative.
    copy_out(cursor, remaining, out, capacity, length);
    return PathForm::Relative;
}

}  // namespace cy::diag
