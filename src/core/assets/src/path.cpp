// VirtualPath normalisation. Task 3.3.2.
//
// One pass, no allocation: segments are copied into the output buffer as they are accepted and the
// buffer is rewound when a `..` pops one. The rewind is what makes traversal detection exact rather
// than textual — a path is outside the mount exactly when a `..` has nothing left to pop.

#include <cy/core/assets/path.h>

#include <cy/core/assets/diagnostics.h>

#include <cstring>

namespace cy::assets {
namespace {

/// A character a path segment may not contain. NUL and the C0 controls would make a path that means
/// one thing to the engine and another to the platform's own APIs.
bool is_forbidden(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7F;
}

}  // namespace

Expected<VirtualPath, Error> VirtualPath::normalise(std::string_view raw) noexcept {
    if (raw.size() > kMaxPathLength) {
        counters::record_path_rejection();
        return fail(ErrorCode::InvalidArgument,
                    "path is longer than the virtual filesystem's maximum");
    }

    VirtualPath out;
    // The offset each accepted segment begins at, so that `..` can rewind to the previous one.
    usize starts[(kMaxPathLength / 2) + 1] = {};
    usize depth = 0;

    usize cursor = 0;
    while (cursor <= raw.size()) {
        const usize separator = raw.find('/', cursor);
        const usize end = separator == std::string_view::npos ? raw.size() : separator;
        const std::string_view segment = raw.substr(cursor, end - cursor);

        if (segment.empty() || segment == ".") {
            // A leading, trailing or doubled separator, and the no-op segment. Both are spellings
            // of the same path, and the canonical form has neither.
        } else if (segment == "..") {
            if (depth == 0) {
                counters::record_path_rejection();
                return fail(ErrorCode::PermissionDenied,
                            "path traverses above its mount root with '..'");
            }
            --depth;
            out.size_ = static_cast<u8>(starts[depth]);
        } else {
            for (const char c : segment) {
                if (is_forbidden(c)) {
                    counters::record_path_rejection();
                    return fail(ErrorCode::InvalidArgument,
                                "path contains a NUL or control character");
                }
                if (c == '\\') {
                    counters::record_path_rejection();
                    return fail(ErrorCode::InvalidArgument,
                                "path contains a backslash; virtual paths are forward-slash "
                                "separated and a backslash is an ordinary filename character");
                }
            }
            starts[depth] = out.size_;
            ++depth;
            if (out.size_ != 0) {
                out.text_[out.size_++] = '/';
            }
            std::memcpy(out.text_ + out.size_, segment.data(), segment.size());
            out.size_ = static_cast<u8>(out.size_ + segment.size());
        }

        if (separator == std::string_view::npos) {
            break;
        }
        cursor = separator + 1;
    }

    out.text_[out.size_] = '\0';
    return out;
}

std::string_view VirtualPath::file_name() const noexcept {
    const std::string_view whole = view();
    const usize separator = whole.rfind('/');
    return separator == std::string_view::npos ? whole : whole.substr(separator + 1);
}

std::string_view VirtualPath::parent() const noexcept {
    const std::string_view whole = view();
    const usize separator = whole.rfind('/');
    return separator == std::string_view::npos ? std::string_view() : whole.substr(0, separator);
}

std::string_view VirtualPath::extension() const noexcept {
    const std::string_view name = file_name();
    const usize dot = name.rfind('.');
    // `dot == 0` is a hidden file, not an extension; see the declaration.
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return name.substr(dot);
}

bool VirtualPath::is_within(const VirtualPath& prefix) const noexcept {
    if (prefix.empty()) {
        return true;  // the mount root contains everything
    }
    const std::string_view whole = view();
    const std::string_view head = prefix.view();
    if (!whole.starts_with(head)) {
        return false;
    }
    return whole.size() == head.size() || whole[head.size()] == '/';
}

Expected<VirtualPath, Error> VirtualPath::join(std::string_view tail) const noexcept {
    if (size_ + 1 + tail.size() > kMaxPathLength) {
        return fail(ErrorCode::InvalidArgument, "joined path exceeds the maximum path length");
    }
    char buffer[kMaxPathLength + 2] = {};
    usize written = 0;
    std::memcpy(buffer, text_, size_);
    written = size_;
    if (written != 0 && !tail.empty()) {
        buffer[written++] = '/';
    }
    std::memcpy(buffer + written, tail.data(), tail.size());
    written += tail.size();
    return normalise(std::string_view(buffer, written));
}

usize VirtualPathHash::operator()(const VirtualPath& path) const noexcept {
    // FNV-1a. Paths are short and the table is small; the mixing quality that matters here is
    // between paths sharing a long prefix, which a byte-at-a-time multiply gives.
    u64 hash = 0xcbf2'9ce4'8422'2325ULL;
    for (const char c : path.view()) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(c));
        hash *= 0x0000'0100'0000'01B3ULL;
    }
    return static_cast<usize>(hash ^ (hash >> 32));
}

}  // namespace cy::assets
