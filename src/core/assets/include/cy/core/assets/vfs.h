#ifndef CY_CORE_ASSETS_VFS_H
#define CY_CORE_ASSETS_VFS_H
// The virtual filesystem: one namespace over layered mounts. Task 3.3.2.
//
// `core-assets-and-io` — "Virtual filesystem": a single namespace over layered mount points,
// resolved in priority order, with package, project, user, memory and remote mounts; paths
// normalised, case-sensitive, forward-slash separated; traversal outside a mount rejected.
//
// HOW RESOLUTION WORKS, in one paragraph, because everything else here follows from it. Mounts are
// held in descending priority order. A lookup walks them from the highest and stops at the first
// that either SERVES the path or MASKS it. Masking is what makes a patch work: a patch package
// mounted above a base carries deleted-entry markers, and an entry marked deleted in the patch
// hides the base's copy entirely rather than falling through to it. Equal priorities resolve in
// mount order, most recently mounted first, so a project that mounts two packages at the same
// priority still gets a defined answer.
//
// PATH SAFETY IS THE TYPE'S, NOT THE MOUNT'S. Every entry point takes a `VirtualPath`, which cannot
// be constructed without normalisation and cannot hold an unresolved `..`. A mount implementation
// therefore never validates a path, and a mount written by a project cannot forget to.

#include <cy/core/assets/file.h>
#include <cy/core/assets/path.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>

namespace cy::assets {

/// The five mount kinds the specification names. The kind is reported in diagnostics and chooses
/// nothing: behaviour is the Mount implementation's, and priority is the mounter's.
enum class MountKind : u8 {
    /// A cooked content package (`.cypak`), including a patch.
    Package = 0,
    /// The project directory, in editor and development builds.
    Project = 1,
    /// The writable per-user location for saves, logs and caches.
    User = 2,
    /// In-memory files, for tests and generated content.
    Memory = 3,
    /// Development-time file serving from a host machine.
    Remote = 4,
};

const char* mount_kind_name(MountKind kind) noexcept;

/// The priorities the engine mounts its own layers at. Higher wins. Spelled here so that a project
/// adding a mount picks a number relative to something rather than inventing a scale.
namespace mount_priority {
inline constexpr i32 kBasePackage = 100;
inline constexpr i32 kPatchPackage = 200;
inline constexpr i32 kRemote = 300;
inline constexpr i32 kProject = 400;
inline constexpr i32 kUser = 500;
inline constexpr i32 kMemory = 600;
}  // namespace mount_priority

using MountId = u32;
inline constexpr MountId kInvalidMount = 0;

class PackageMount;

/// One entry a walk reports. `path` is mount-relative and valid only for the duration of the call.
struct VirtualEntry {
    const VirtualPath* path = nullptr;
    bool is_directory = false;
    u64 size = 0;
    MountId mount = kInvalidMount;
};

/// Returning false stops the walk.
using VirtualVisitor = bool (*)(void* user, const VirtualEntry& entry) noexcept;

/// One layer of the namespace.
///
/// Everything but `read`, `size_of`, `contains`, `enumerate`, `kind` and `name` has a default, so a
/// project's own mount implements six functions and inherits the rest.
class Mount {
public:
    Mount() noexcept = default;
    virtual ~Mount();

    Mount(const Mount&) = delete;
    Mount& operator=(const Mount&) = delete;

    [[nodiscard]] virtual MountKind kind() const noexcept = 0;
    /// A short label for diagnostics — the package name, the directory, "memory". Never null.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// True when this mount has the path.
    [[nodiscard]] virtual bool contains(const VirtualPath& path) const noexcept = 0;

    /// True when this mount deliberately HIDES the path from everything below it. Only a patch
    /// package answers true; the default is what every other mount wants.
    [[nodiscard]] virtual bool is_deleted(const VirtualPath& path) const noexcept;

    [[nodiscard]] virtual Expected<u64, Error> size_of(const VirtualPath& path) const noexcept = 0;

    /// Read `size` bytes at `offset` within the file. A read past the end is OutOfRange rather than
    /// a short read: a caller here always knows the size it asked the mount for.
    [[nodiscard]] virtual Status read(const VirtualPath& path, u64 offset, void* destination,
                                      usize size) const noexcept = 0;

    /// Map the file, where the mount can. The default reports Unsupported, which every caller
    /// handles by copying — mapping is an optimisation and never a requirement.
    [[nodiscard]] virtual Expected<MappedFile, Error> map(const VirtualPath& path) const noexcept;

    [[nodiscard]] virtual Status enumerate(const VirtualPath& directory, bool recursive,
                                           VirtualVisitor visitor, void* user) const noexcept = 0;

    /// True for a mount that accepts writes — the user mount, and a memory mount.
    [[nodiscard]] virtual bool writable() const noexcept;
    /// Write a whole file. The default refuses, which is what a read-only mount means.
    [[nodiscard]] virtual Status write(const VirtualPath& path, const void* data,
                                       usize size) noexcept;

    /// This mount as a package mount, or null.
    ///
    /// The one place the namespace exposes a mount's richer identity, and it exists because the
    /// asset system needs a package ENTRY — its kind, its dependencies, its content hash — and the
    /// Mount interface deliberately speaks only in bytes. The engine is built with -fno-rtti, so a
    /// dynamic_cast is not available and a hand-rolled type tag would be the same thing with more
    /// ceremony; a named hook says what it is for. Any other mount inherits the null.
    [[nodiscard]] virtual PackageMount* as_package() noexcept;
};

/// A mount over a real directory. The project mount and the user mount are both this.
///
/// The directory is the root: a `VirtualPath` cannot escape it, so nothing under this mount can
/// reach a file the mount was not given.
class DirectoryMount final : public Mount {
public:
    /// `root` is a native path. Fails when it is not a directory, because a mount that silently
    /// serves nothing is a configuration error that surfaces as a missing asset much later.
    [[nodiscard]] static Expected<UniquePtr<DirectoryMount>, Error> create(const char* root,
                                                                           MountKind kind,
                                                                           bool writable) noexcept;

    [[nodiscard]] MountKind kind() const noexcept override { return kind_; }
    [[nodiscard]] const char* name() const noexcept override { return root_; }
    [[nodiscard]] bool contains(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Expected<u64, Error> size_of(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Status read(const VirtualPath& path, u64 offset, void* destination,
                              usize size) const noexcept override;
    [[nodiscard]] Expected<MappedFile, Error> map(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Status enumerate(const VirtualPath& directory, bool recursive,
                                   VirtualVisitor visitor, void* user) const noexcept override;
    [[nodiscard]] bool writable() const noexcept override { return writable_; }
    [[nodiscard]] Status write(const VirtualPath& path, const void* data,
                               usize size) noexcept override;

    /// The native path a virtual one resolves to. Public because the package mount opens a real
    /// file through it, and because a diagnostic that says which file it meant is worth having.
    [[nodiscard]] Expected<usize, Error> native_path(const VirtualPath& path, char* out,
                                                     usize capacity) const noexcept;

    /// Public only because `make_unique` constructs it; `create()` is how one is made, because a
    /// default-constructed mount serves nothing and would be a configuration error nobody sees.
    DirectoryMount() noexcept = default;

private:
    char root_[kMaxPathLength + 1] = {};
    MountKind kind_ = MountKind::Project;
    bool writable_ = false;
};

/// A mount whose files are in memory. For tests, for generated content, and for a save game held
/// before it is written.
class MemoryMount final : public Mount {
public:
    explicit MemoryMount(const char* label = "memory") noexcept;

    [[nodiscard]] MountKind kind() const noexcept override { return MountKind::Memory; }
    [[nodiscard]] const char* name() const noexcept override { return label_; }
    [[nodiscard]] bool contains(const VirtualPath& path) const noexcept override;
    [[nodiscard]] bool is_deleted(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Expected<u64, Error> size_of(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Status read(const VirtualPath& path, u64 offset, void* destination,
                              usize size) const noexcept override;
    [[nodiscard]] Status enumerate(const VirtualPath& directory, bool recursive,
                                   VirtualVisitor visitor, void* user) const noexcept override;
    [[nodiscard]] bool writable() const noexcept override { return true; }
    [[nodiscard]] Status write(const VirtualPath& path, const void* data,
                               usize size) noexcept override;

    /// Add a file, copying the bytes. Replaces an existing one.
    [[nodiscard]] Status add(const VirtualPath& path, const void* data, usize size) noexcept;

    /// Mask a path from every lower-priority mount, the way a patch package's deleted-entry marker
    /// does. This is how the patch-masking rule is exercised without building a patch.
    [[nodiscard]] Status mark_deleted(const VirtualPath& path) noexcept;

    [[nodiscard]] usize file_count() const noexcept { return files_.size(); }

private:
    struct Entry {
        VirtualPath path;
        Array<u8> bytes;
        bool deleted = false;
    };

    [[nodiscard]] const Entry* find(const VirtualPath& path) const noexcept;

    char label_[32] = {};
    Array<Entry> files_;
};

/// Where a remote mount's bytes actually come from.
///
/// `core-assets-and-io` — "Development file serving": a device with a remote mount fetches assets
/// from the host machine on demand, so iteration does not require repackaging. THE TRANSPORT IS NOT
/// HERE: there is no socket, no protocol and no host discovery at M1, because none of them can be
/// tested on one machine and a protocol nobody has spoken is a protocol that is wrong. What is here
/// is the seam — the interface a transport implements, and a mount that is already correct against
/// it. `tests/test_vfs.cpp` supplies an in-process provider and asserts the on-demand property by
/// counting fetches.
class RemoteFileProvider {
public:
    RemoteFileProvider() noexcept = default;
    virtual ~RemoteFileProvider();

    RemoteFileProvider(const RemoteFileProvider&) = delete;
    RemoteFileProvider& operator=(const RemoteFileProvider&) = delete;

    /// The size of a file on the host, or NotFound.
    [[nodiscard]] virtual Expected<u64, Error> stat(const VirtualPath& path) noexcept = 0;
    [[nodiscard]] virtual Status fetch(const VirtualPath& path, u64 offset, void* destination,
                                       usize size) noexcept = 0;
    [[nodiscard]] virtual Status list(const VirtualPath& directory, bool recursive,
                                      VirtualVisitor visitor, void* user) noexcept = 0;
};

/// A mount served by a host machine over a `RemoteFileProvider`.
class RemoteMount final : public Mount {
public:
    explicit RemoteMount(RemoteFileProvider& provider) noexcept : provider_(&provider) {}

    [[nodiscard]] MountKind kind() const noexcept override { return MountKind::Remote; }
    [[nodiscard]] const char* name() const noexcept override { return "remote"; }
    [[nodiscard]] bool contains(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Expected<u64, Error> size_of(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Status read(const VirtualPath& path, u64 offset, void* destination,
                              usize size) const noexcept override;
    [[nodiscard]] Status enumerate(const VirtualPath& directory, bool recursive,
                                   VirtualVisitor visitor, void* user) const noexcept override;

private:
    RemoteFileProvider* provider_;
};

/// The layered namespace itself.
class VirtualFileSystem {
public:
    VirtualFileSystem() noexcept = default;

    VirtualFileSystem(const VirtualFileSystem&) = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

    /// Take ownership of a mount. Higher priority resolves first; among equal priorities the most
    /// recently mounted wins.
    ///
    /// THE DELETER IS NOT DECORATION. `UniquePtr<Mount>` would destroy the object correctly — the
    /// destructor is virtual — and then hand the allocator `sizeof(Mount)` rather than the concrete
    /// size, which is a wrong free rather than a wrong destructor and which no test of behaviour
    /// would catch. `mount_owned` captures the concrete type at the call site and gives the
    /// namespace a function that knows how big the object really is.
    template <class T>
    [[nodiscard]] Expected<MountId, Error> mount_owned(UniquePtr<T> mount, i32 priority) noexcept {
        if (!mount) {
            return fail(ErrorCode::InvalidArgument, "a null mount cannot be mounted");
        }
        Allocator* allocator = mount.allocator();
        T* raw = mount.release();
        return adopt(raw, &destroy_mount<T>, *allocator, priority);
    }

    /// How a mount is released. Written by `mount_owned`; a caller with its own allocation scheme
    /// may supply one.
    using MountDeleter = void (*)(Mount* mount, Allocator* allocator) noexcept;

    /// Take ownership of a mount that is already allocated.
    [[nodiscard]] Expected<MountId, Error> adopt(Mount* mount, MountDeleter deleter,
                                                 Allocator& allocator, i32 priority) noexcept;
    [[nodiscard]] Status unmount(MountId id) noexcept;
    void unmount_all() noexcept;

    [[nodiscard]] usize mount_count() const noexcept { return mounts_.size(); }
    /// The mount, or null. Also how a caller reaches a package reader it mounted.
    [[nodiscard]] Mount* find_mount(MountId id) noexcept;

    /// Which mount serves a path, and how large the file is.
    struct Resolution {
        MountId mount = kInvalidMount;
        Mount* source = nullptr;
        u64 size = 0;
    };
    [[nodiscard]] Expected<Resolution, Error> resolve(const VirtualPath& path) const noexcept;

    [[nodiscard]] bool exists(const VirtualPath& path) const noexcept;
    [[nodiscard]] Expected<u64, Error> size_of(const VirtualPath& path) const noexcept;

    /// Read a whole file into `out`, replacing its contents.
    [[nodiscard]] Status read(const VirtualPath& path, Array<u8>& out) const noexcept;
    [[nodiscard]] Status read_range(const VirtualPath& path, u64 offset, void* destination,
                                    usize size) const noexcept;
    [[nodiscard]] Expected<MappedFile, Error> map(const VirtualPath& path) const noexcept;

    /// Walk the union of every mount's view of a directory, highest priority first. A path served
    /// by two mounts is reported once, by the higher; a path a higher mount marks deleted is not
    /// reported at all. Results are sorted, so a listing is reproducible.
    [[nodiscard]] Status enumerate(const VirtualPath& directory, bool recursive,
                                   VirtualVisitor visitor, void* user) const noexcept;

    /// Write through the highest-priority writable mount. Fails with PermissionDenied when there is
    /// none, which is the correct answer for a shipping build that mounted only packages.
    [[nodiscard]] Status write(const VirtualPath& path, const void* data, usize size) noexcept;

private:
    template <class T>
    static void destroy_mount(Mount* mount, Allocator* allocator) noexcept {
        T* concrete = static_cast<T*>(mount);
        concrete->~T();
        allocator->deallocate(static_cast<void*>(concrete), sizeof(T), alignof(T));
    }

    /// One mounted layer. Move-only, and it releases its mount through the deleter the concrete
    /// type supplied — see `mount_owned`.
    struct Entry {
        Entry() noexcept = default;
        ~Entry() { release(); }

        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

        Entry(Entry&& other) noexcept
            : id(other.id),
              priority(other.priority),
              mount(other.mount),
              deleter(other.deleter),
              allocator(other.allocator) {
            other.mount = nullptr;
        }

        Entry& operator=(Entry&& other) noexcept {
            if (this != &other) {
                release();
                id = other.id;
                priority = other.priority;
                mount = other.mount;
                deleter = other.deleter;
                allocator = other.allocator;
                other.mount = nullptr;
            }
            return *this;
        }

        void release() noexcept {
            if (mount != nullptr && deleter != nullptr) {
                deleter(mount, allocator);
            }
            mount = nullptr;
        }

        MountId id = kInvalidMount;
        i32 priority = 0;
        Mount* mount = nullptr;
        MountDeleter deleter = nullptr;
        Allocator* allocator = nullptr;
    };

    /// Descending priority; see the file comment.
    Array<Entry> mounts_;
    MountId next_id_ = 1;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_VFS_H
