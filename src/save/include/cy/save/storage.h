#pragma once
// Where a save's bytes go. Task 6.2.
//
// `save-and-persistence` — "Storage backends and the cloud boundary": storage is behind a backend
// interface — "local filesystem, platform storage, cloud service, server database, and an in-memory
// backend for tests" — while the save model stays the same. Cloud transport, quotas and account
// association are NOT owned here; this capability produces artefacts and the metadata a platform
// service needs.
//
// THE ONE THING EVERY BACKEND MUST PROMISE. `write()` is ATOMIC per key: after it, the key holds
// either all of the new bytes or all of the previous ones, and never a prefix. Everything the
// archive builds on top — generations that survive `kill -9`, a manifest switch that is a single
// observable event — rests on that one property, and it is why a backend that cannot promise it is
// not a backend this engine can use. The filesystem implementation gets it from
// `assets::fs::write_atomic` (write to a temporary, flush, rename); the in-memory one gets it
// because a swap under no concurrency is atomic by construction.
//
// KEYS, NOT PATHS. A key is a '/'-separated name inside the save's own namespace —
// "chunks/<hash>.cychunk", "generations/00000007.cymanifest", "current". A backend maps it to
// whatever it stores in; a caller never builds a filesystem path, so a cloud backend is a class
// rather than a rewrite.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::save {

/// The longest key a backend must accept. Long enough for a content hash under two directories,
/// short enough that a caller can build one on the stack.
inline constexpr usize kMaxSaveKeyLength = 255;

/// Where a save's bytes live. Implementations are not thread-safe unless they say so; the archive
/// serialises its own writes and the service performs them on one thread.
class SaveBackend {
public:
    SaveBackend() noexcept = default;
    virtual ~SaveBackend();

    SaveBackend(const SaveBackend&) = delete;
    SaveBackend& operator=(const SaveBackend&) = delete;
    SaveBackend(SaveBackend&&) = delete;
    SaveBackend& operator=(SaveBackend&&) = delete;

    /// For a diagnostic. Never null.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Replace the object at `key` atomically. See the note above: this is the whole contract.
    [[nodiscard]] virtual Status write(std::string_view key, Span<const u8> bytes) noexcept = 0;

    /// Read the whole object at `key`, replacing `out`. `NotFound` when there is no such key.
    [[nodiscard]] virtual Status read(std::string_view key, Array<u8>& out) const noexcept = 0;

    [[nodiscard]] virtual bool exists(std::string_view key) const noexcept = 0;

    /// Remove an object. Removing what is not there succeeds: a collector that raced another
    /// collector has done its job, not failed at it.
    [[nodiscard]] virtual Status remove(std::string_view key) noexcept = 0;

    /// Called once per key under `prefix`, in ascending key order. Returning false stops the walk.
    using KeyVisitor = bool (*)(void* user, std::string_view key) noexcept;

    [[nodiscard]] virtual Status list(std::string_view prefix, KeyVisitor visitor,
                                      void* user) const noexcept = 0;
};

/// The local filesystem, rooted at a directory. What a single-player game on a desktop uses.
class FilesystemBackend final : public SaveBackend {
public:
    FilesystemBackend() noexcept = default;
    ~FilesystemBackend() override;

    /// Take `root` as the directory every key is relative to, creating it if it does not exist.
    ///
    /// Also discards the temporaries an interrupted write left behind, and reports how many —
    /// `core-assets-and-io` requires exactly that of a start-up on the user mount, and a repeated
    /// crash should be visible rather than silent.
    [[nodiscard]] Expected<usize, Error> open(const char* root) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return "filesystem"; }
    [[nodiscard]] const char* root() const noexcept { return root_; }

    [[nodiscard]] Status write(std::string_view key, Span<const u8> bytes) noexcept override;
    [[nodiscard]] Status read(std::string_view key, Array<u8>& out) const noexcept override;
    [[nodiscard]] bool exists(std::string_view key) const noexcept override;
    [[nodiscard]] Status remove(std::string_view key) noexcept override;
    [[nodiscard]] Status list(std::string_view prefix, KeyVisitor visitor,
                              void* user) const noexcept override;

private:
    static constexpr usize kMaxRootLength = 3072;
    char root_[kMaxRootLength + 1] = {};
};

/// An in-memory store. `save-and-persistence` names it as one of the backends rather than as a test
/// double, because a dedicated server that persists to a database wants the same seam.
///
/// It also carries the fault injection the specification's transactional tests need: "simulating
/// failure after every write phase and verifying the previous save remains valid" needs a store
/// that can be made to fail at a chosen write, and putting that here rather than in a test-only
/// subclass keeps the failure on the path the real backend takes.
class MemoryBackend final : public SaveBackend {
public:
    explicit MemoryBackend(Allocator& allocator = current_allocator()) noexcept
        : objects_(allocator), allocator_(&allocator) {}
    ~MemoryBackend() override;

    [[nodiscard]] const char* name() const noexcept override { return "memory"; }

    [[nodiscard]] Status write(std::string_view key, Span<const u8> bytes) noexcept override;
    [[nodiscard]] Status read(std::string_view key, Array<u8>& out) const noexcept override;
    [[nodiscard]] bool exists(std::string_view key) const noexcept override;
    [[nodiscard]] Status remove(std::string_view key) noexcept override;
    [[nodiscard]] Status list(std::string_view prefix, KeyVisitor visitor,
                              void* user) const noexcept override;

    /// Fail every write after this many more have succeeded. `kNoLimit` disables it.
    static constexpr u32 kNoLimit = 0xFFFF'FFFFU;
    void set_write_budget(u32 writes) noexcept { write_budget_ = writes; }
    [[nodiscard]] u32 writes() const noexcept { return writes_; }

    /// Corrupt an object in place, so a load meets a chunk whose bytes do not match its name.
    /// Nothing but a test has a reason to call it, and a test has a very good one.
    [[nodiscard]] Status corrupt(std::string_view key) noexcept;

    [[nodiscard]] usize object_count() const noexcept { return objects_.size(); }
    [[nodiscard]] u64 total_bytes() const noexcept;

private:
    struct Object {
        Array<char> key;
        Array<u8> bytes;

        explicit Object(Allocator& allocator) noexcept : key(allocator), bytes(allocator) {}
        [[nodiscard]] std::string_view name() const noexcept { return {key.data(), key.size()}; }
    };

    [[nodiscard]] usize index_of(std::string_view key) const noexcept;

    /// Sorted by key, so `list` is in order without sorting and two stores with the same content
    /// enumerate identically.
    Array<Object> objects_;
    Allocator* allocator_ = nullptr;
    u32 writes_ = 0;
    u32 write_budget_ = kNoLimit;
};

}  // namespace cy::save
