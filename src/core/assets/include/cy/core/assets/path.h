#ifndef CY_CORE_ASSETS_PATH_H
#define CY_CORE_ASSETS_PATH_H
// `VirtualPath` — the one spelling of a path inside the virtual filesystem. Task 3.3.2.
//
// `core-assets-and-io` — "Virtual filesystem": paths are normalised, case-sensitive, and
// forward-slash separated, and path traversal outside a mount is rejected. All four of those are
// properties of this type rather than of the code that uses it, so a mount implementation never
// receives a path it has to validate again.
//
// A VirtualPath is FIXED CAPACITY and allocates nothing. Two reasons: a path is resolved on the
// load path, where an allocation per lookup would be a cost nobody chose, and a bounded path is one
// a package directory entry can hold inline. `kMaxPathLength` is the bound; a longer path is an
// error at construction, which is where a caller can still do something about it.
//
// CASE IS SIGNIFICANT, deliberately. Two files differing only in case are two files, on every
// platform the engine targets, and a case-folding resolver is a resolver that works on the
// developer's machine and fails on the build server.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy::assets {

/// The longest path the virtual filesystem accepts, excluding the terminator. Chosen so that a
/// VirtualPath is 256 bytes and a package directory can hold one without indirection.
inline constexpr usize kMaxPathLength = 254;

/// A normalised, mount-relative path.
///
/// The canonical form has no leading slash, no trailing slash, no empty segment, no `.` segment and
/// no unresolved `..` segment. `normalise()` is the only way to make one that is not empty, so a
/// value of this type is a path that has already been checked.
class VirtualPath {
public:
    constexpr VirtualPath() noexcept = default;

    /// Normalise `raw` into the canonical form.
    ///
    /// Rejects, each with a message naming the reason: a path longer than `kMaxPathLength`, an
    /// embedded NUL or control character, a backslash (which is a legal filename character on the
    /// platforms the engine targets, so accepting it as a separator would make one path mean two
    /// things), and a `..` that would escape the mount root — the traversal rule, enforced here
    /// once rather than in every mount.
    [[nodiscard]] static Expected<VirtualPath, Error> normalise(std::string_view raw) noexcept;

    [[nodiscard]] constexpr const char* c_str() const noexcept { return text_; }
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {text_, size_}; }
    [[nodiscard]] constexpr usize size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    /// The last segment, or the whole path when it has one segment.
    [[nodiscard]] std::string_view file_name() const noexcept;

    /// Everything before the last separator, empty when the path has one segment.
    [[nodiscard]] std::string_view parent() const noexcept;

    /// The extension including the dot, or empty. A leading dot on the last segment is a hidden
    /// file and not an extension: `.gitignore` has none.
    [[nodiscard]] std::string_view extension() const noexcept;

    /// True when this path is `prefix` itself or lies beneath it. Segment-aware: `textures` is a
    /// prefix of `textures/stone.ktx2` and is not a prefix of `textures_old/stone.ktx2`.
    [[nodiscard]] bool is_within(const VirtualPath& prefix) const noexcept;

    /// `this / tail`, normalised. Fails for the same reasons `normalise` does, and `..` in `tail`
    /// may not escape past this path's own root.
    [[nodiscard]] Expected<VirtualPath, Error> join(std::string_view tail) const noexcept;

    friend bool operator==(const VirtualPath& a, const VirtualPath& b) noexcept {
        return a.view() == b.view();
    }
    friend bool operator!=(const VirtualPath& a, const VirtualPath& b) noexcept {
        return !(a == b);
    }
    /// A total order, so a directory listing sorts the same way on every platform.
    friend bool operator<(const VirtualPath& a, const VirtualPath& b) noexcept {
        return a.view() < b.view();
    }

private:
    char text_[kMaxPathLength + 1] = {};
    u8 size_ = 0;
};

static_assert(sizeof(VirtualPath) == 256, "a VirtualPath is one cache-friendly fixed block");

/// Hash for the standard associative containers and for cy::HashMap.
struct VirtualPathHash {
    [[nodiscard]] usize operator()(const VirtualPath& path) const noexcept;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_PATH_H
