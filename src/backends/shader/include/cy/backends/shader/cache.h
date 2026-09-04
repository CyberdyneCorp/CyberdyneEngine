#pragma once
// The tiered, content-addressed shader cache. Task 3.4.
//
// `shader-system` — "Shader compilation service and tiered cache": compiled outputs are stored in a
// tiered content-addressed cache — local, a shared read-only remote, and a writable remote
// populated by continuous integration — "the same tiering as the cook cache in
// `asset-import-pipeline`". And: "Identical compilation work SHALL NOT be repeated when a cache
// tier already holds the result."
//
// THE KEY IS THE DESIGN. The specification lists exactly what goes into it — the source, the
// material graph where applicable, the compiler version, the IR version, the target platform, the
// renderer profile and the feature set — "so a compiler change invalidates derived data without
// invalidating authored assets". `CacheKeyInputs` below is that list, field for field, and
// `derive_cache_key` is the only way to produce a key. There is deliberately no constructor that
// takes a hash: a key assembled from anything other than the full input set is a key that collides
// across a compiler upgrade, and that class of bug reproduces as "it works on my machine" for a
// week before anybody suspects the cache.
//
// TIER ORDER IS SEARCH ORDER, AND A HIT PROMOTES. Tiers are consulted in the order they were added
// — local first, remote last — and a hit in a slower tier is written back into every writable tier
// in front of it. That is what turns `shader-system`'s "CI populates, developers consume" scenario
// into one line of behaviour: the developer's first build fetches, and the second one does not.
//
// A TIER IS AN INTERFACE, NOT A DIRECTORY. `MemoryCacheTier` and `VfsCacheTier` ship here; an HTTP
// tier or an object-store tier is somebody's later afternoon and needs nothing from this file to
// change. Nothing above this line knows how many tiers there are, which is what lets a build with
// no network configure the same code with one tier instead of three.

#include <cy/backends/shader/compiler.h>
#include <cy/core/assets/hash.h>
#include <cy/core/assets/path.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::shader {

/// The version of the engine's own derived-artefact format. Bumped whenever the shader library
/// encoding or the reflection record layout changes, so an old cache entry is a miss rather than a
/// misparse. `shader-system` calls this "the IR version".
inline constexpr u32 kShaderArtefactVersion = 1;

/// Everything the specification requires a cache key to include.
///
/// Every field is a decision about what invalidates what. `source_hash` covers the authored or
/// generated text; `compiler_name` and `compiler_version` cover the toolchain, so upgrading Slang
/// invalidates the derived binaries and touches no asset; `target_platform` and `renderer_profile`
/// keep a Vulkan build and a Metal build, or a forward-clustered profile and a visibility-buffer
/// one, from sharing an entry that is only correct for one of them.
struct CacheKeyInputs {
    assets::ContentHash source_hash;
    const char* compiler_name = "";
    const char* compiler_version = "";
    u32 artefact_version = kShaderArtefactVersion;
    /// "vulkan-spirv", "metal-msl", "d3d12-dxil". The interchange form the entry holds.
    const char* target_platform = "vulkan-spirv";
    /// The renderer's own profile, so two pipelines that lower differently do not collide.
    const char* renderer_profile = "";
    /// The build's feature set, in a stable order. Sorted by the caller; `derive_cache_key` hashes
    /// it in the order given, because sorting it here would hide an unstable caller.
    Span<const char* const> feature_set;
    Name entry_point;
    u64 permutation = 0;
    OptimizationLevel optimization = OptimizationLevel::Performance;
    DebugInfoLevel debug_info = DebugInfoLevel::None;
    u32 spirv_version = kSpirv1_5;
};

/// A derived key. Content-addressed: equal keys mean equal inputs, so equal outputs.
struct CacheKey {
    assets::ContentHash hash;

    /// The file name this key is stored under in a directory-shaped tier: the hex digest, sharded
    /// two characters deep so a cache with a hundred thousand entries is not one directory with a
    /// hundred thousand files in it.
    [[nodiscard]] Expected<assets::VirtualPath, Error> to_path(
        const assets::VirtualPath& root) const noexcept;

    friend bool operator==(const CacheKey& a, const CacheKey& b) noexcept {
        return a.hash == b.hash;
    }
    friend bool operator!=(const CacheKey& a, const CacheKey& b) noexcept { return !(a == b); }
};

/// The only way to make a key. See the header for why there is no other.
[[nodiscard]] CacheKey derive_cache_key(const CacheKeyInputs& inputs) noexcept;

/// One level of the cache.
class CacheTier {
public:
    CacheTier() noexcept = default;
    CacheTier(const CacheTier&) = delete;
    CacheTier& operator=(const CacheTier&) = delete;
    virtual ~CacheTier();

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    /// A read-only tier is never written to, and a hit in one promotes into the writable tiers in
    /// front of it. The shared remote is read-only to a developer and writable to CI.
    [[nodiscard]] virtual bool writable() const noexcept = 0;

    /// Replace `out` with the entry's bytes. Returns false for a miss, which is not an error.
    [[nodiscard]] virtual Expected<bool, Error> load(const CacheKey& key,
                                                     Array<u8>& out) noexcept = 0;
    [[nodiscard]] virtual Status store(const CacheKey& key, Span<const u8> bytes) noexcept = 0;
};

/// A tier in memory. The process's own cache, and the one a test uses to stand in for a remote.
class MemoryCacheTier final : public CacheTier {
public:
    MemoryCacheTier(Allocator& allocator, const char* name, bool writable) noexcept;
    ~MemoryCacheTier() override;

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] bool writable() const noexcept override { return writable_; }
    [[nodiscard]] Expected<bool, Error> load(const CacheKey& key, Array<u8>& out) noexcept override;
    [[nodiscard]] Status store(const CacheKey& key, Span<const u8> bytes) noexcept override;

    /// Insert regardless of `writable()`. How a read-only tier is populated in a test, and how CI
    /// populates the shared tier it publishes.
    [[nodiscard]] Status publish(const CacheKey& key, Span<const u8> bytes) noexcept;
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    void clear() noexcept;

private:
    struct Entry {
        CacheKey key;
        Array<u8> bytes;
    };

    [[nodiscard]] usize find(const CacheKey& key) const noexcept;

    Allocator* allocator_;
    const char* name_;
    bool writable_;
    Array<Entry> entries_;
};

/// A tier over the virtual filesystem: the local on-disk cache, and — over a remote mount — the
/// shared one. The same class serves both because `cy::assets::VirtualFileSystem` already made
/// "where the bytes are" somebody else's problem.
class VfsCacheTier final : public CacheTier {
public:
    VfsCacheTier(Allocator& allocator, const char* name, assets::VirtualFileSystem& files,
                 const assets::VirtualPath& root, bool writable) noexcept;
    ~VfsCacheTier() override;

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] bool writable() const noexcept override { return writable_; }
    [[nodiscard]] Expected<bool, Error> load(const CacheKey& key, Array<u8>& out) noexcept override;
    [[nodiscard]] Status store(const CacheKey& key, Span<const u8> bytes) noexcept override;

private:
    Allocator* allocator_;
    const char* name_;
    assets::VirtualFileSystem* files_;
    assets::VirtualPath root_;
    bool writable_;
};

struct ShaderCacheStats {
    u64 lookups = 0;
    u64 hits = 0;
    u64 misses = 0;
    /// Hits served by a tier other than the first, and therefore written back. The number that says
    /// whether the remote tier is earning its keep.
    u64 promotions = 0;
    u64 stores = 0;
    u64 bytes_loaded = 0;
    u64 bytes_stored = 0;
};

/// The tiers, searched in order, with write-back.
///
/// Holds pointers and owns nothing: a tier outlives the cache, which is what lets the same memory
/// tier be shared by two caches in a test and lets a host decide the lifetime of a mount.
class ShaderCache {
public:
    explicit ShaderCache(Allocator& allocator) noexcept;

    ShaderCache(const ShaderCache&) = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;

    /// Append a tier. Order is search order: fastest first.
    [[nodiscard]] Status add_tier(CacheTier& tier) noexcept;
    [[nodiscard]] usize tier_count() const noexcept { return tiers_.size(); }
    [[nodiscard]] const CacheTier* tier_at(usize index) const noexcept;

    /// Search every tier in order. On a hit past the first, write the bytes back into every
    /// writable tier in front of it before returning. False is a miss.
    [[nodiscard]] Expected<bool, Error> load(const CacheKey& key, Array<u8>& out) noexcept;
    /// Store into every writable tier. A tier that refuses is reported; the others still get it,
    /// because a full local disk should not cost the shared tier its copy.
    [[nodiscard]] Status store(const CacheKey& key, Span<const u8> bytes) noexcept;

    [[nodiscard]] const ShaderCacheStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = ShaderCacheStats{}; }

private:
    Array<CacheTier*> tiers_;
    ShaderCacheStats stats_;
};

}  // namespace cy::shader
