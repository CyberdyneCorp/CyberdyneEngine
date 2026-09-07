// The cache key, the two tier implementations, and the tiered search with write-back. Task 3.4.

#include <cy/backends/shader/cache.h>

#include <cstring>
#include <string_view>

namespace cy::shader {
namespace {

/// A `const char*` that may be null, as the view the key builder wants.
///
/// The builder frames every contribution — a tag, the name's length, the name, the value's length,
/// the value — so the "vulkan"/"spirv" against "vulkans"/"pirv" collision this file used to guard
/// against by hand is now a property of the encoding rather than of this function remembering to
/// write a length prefix. See cy/core/assets/derivation.h.
[[nodiscard]] std::string_view view_of(const char* text) noexcept {
    return text != nullptr ? std::string_view(text) : std::string_view();
}

}  // namespace

Expected<assets::DerivationKey, Error> derive_shader_derivation_key(
    const CacheKeyInputs& inputs) noexcept {
    // THE TOOLCHAIN IS CONTRIBUTED BY CONSTRUCTION AND REFUSED WHEN INCOMPLETE. M7 tasks 1.1, 1.2.
    //
    // `compiler_name` and `compiler_version` name the tool this key's producer INVOKES — Slang —
    // and they are not the same thing as the toolchain that compiled the producer. The engine
    // binary writes the shader library encoding and the reflection records, so two engine builds
    // that disagree about float contraction can write different bytes for one Slang invocation.
    // M6's spike measured that exact shape on the importer: identical keys at -O2 and -O0, and a
    // shared cache serving either binary's artefact to the other.
    const assets::ToolchainFingerprint& toolchain = assets::current_toolchain();
    if (!assets::toolchain_is_complete(toolchain)) {
        return make_unexpected(Error{ErrorCode::Internal,
                                     "the toolchain fingerprint is incomplete, so a shader cache "
                                     "key would be blind to what compiled the engine",
                                     0});
    }

    assets::DerivationKeyBuilder builder;
    // The order is the specification's list, extended with the toolchain in the position
    // `cy::build::derivation_key` puts it. It is fixed because a key is only useful if two machines
    // derive the same one, and a reordering would silently invalidate every cache on the day it
    // landed.
    builder.producer(assets::DerivedKind::Shader, view_of(inputs.compiler_name),
                     inputs.artefact_version);
    toolchain.contribute(builder);
    builder.source("source", inputs.source_hash);
    builder.text("compiler.version", view_of(inputs.compiler_version));
    builder.text("target.platform", view_of(inputs.target_platform));
    builder.text("renderer.profile", view_of(inputs.renderer_profile));
    builder.number("features", inputs.feature_set.size());
    for (const char* feature : inputs.feature_set) {
        builder.text("feature", view_of(feature));
    }
    builder.text("entry", view_of(inputs.entry_point.c_str()));
    builder.number("permutation", inputs.permutation);
    builder.number("optimization", static_cast<u64>(inputs.optimization));
    builder.number("debug_info", static_cast<u64>(inputs.debug_info));
    builder.number("spirv.version", inputs.spirv_version);
    return builder.finish();
}

Expected<CacheKey, Error> derive_cache_key(const CacheKeyInputs& inputs) noexcept {
    Expected<assets::DerivationKey, Error> derived = derive_shader_derivation_key(inputs);
    if (!derived) {
        return make_unexpected(derived.error());
    }
    CacheKey key;
    key.hash = derived.value().digest;
    return key;
}

Expected<assets::VirtualPath, Error> CacheKey::to_path(
    const assets::VirtualPath& root) const noexcept {
    // Sharded two hex characters deep, so a cache with a hundred thousand entries is not one
    // directory with a hundred thousand files in it — which is a measurable difference on every
    // filesystem the engine runs on.
    char text[assets::ContentHash::kTextLength + 1] = {};
    hash.format(text);

    // Assembled character by character rather than with memcpy: the buffer is a length-tracked
    // fragment and never a C string, and a copy that leaves it unterminated is exactly what
    // bugprone-not-null-terminated-result reports — correctly, because the next reader of this code
    // would have to work out which of the two it is.
    constexpr std::string_view kExtension = ".cyshader";
    char relative[assets::ContentHash::kTextLength + 16] = {};
    usize length = 0;
    relative[length++] = text[0];
    relative[length++] = text[1];
    relative[length++] = '/';
    for (usize index = 2; index < assets::ContentHash::kTextLength; ++index) {
        relative[length++] = text[index];
    }
    for (const char character : kExtension) {
        relative[length++] = character;
    }

    if (root.empty()) {
        return assets::VirtualPath::normalise(std::string_view(relative, length));
    }
    return root.join(std::string_view(relative, length));
}

CacheTier::~CacheTier() = default;

// --- MemoryCacheTier -------------------------------------------------------------------------

MemoryCacheTier::MemoryCacheTier(Allocator& allocator, const char* name, bool writable) noexcept
    : allocator_(&allocator), name_(name), writable_(writable), entries_(allocator) {}

MemoryCacheTier::~MemoryCacheTier() = default;

usize MemoryCacheTier::find(const CacheKey& key) const noexcept {
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].key == key) {
            return index;
        }
    }
    return entries_.size();
}

Expected<bool, Error> MemoryCacheTier::load(const CacheKey& key, Array<u8>& out) noexcept {
    const usize index = find(key);
    if (index == entries_.size()) {
        return false;
    }
    out.clear();
    if (Status appended =
            out.append(Span<const u8>(entries_[index].bytes.data(), entries_[index].bytes.size()));
        !appended) {
        return make_unexpected(appended.error());
    }
    return true;
}

Status MemoryCacheTier::store(const CacheKey& key, Span<const u8> bytes) noexcept {
    if (!writable_) {
        return fail(ErrorCode::PermissionDenied, "this cache tier is read-only");
    }
    return publish(key, bytes);
}

Status MemoryCacheTier::publish(const CacheKey& key, Span<const u8> bytes) noexcept {
    const usize index = find(key);
    if (index != entries_.size()) {
        entries_[index].bytes.clear();
        return entries_[index].bytes.append(bytes);
    }
    Expected<Entry*, Error> slot = entries_.emplace_back(Entry{key, Array<u8>(*allocator_)});
    if (!slot) {
        return make_unexpected(slot.error());
    }
    return entries_.back().bytes.append(bytes);
}

void MemoryCacheTier::clear() noexcept {
    entries_.clear();
}

// --- VfsCacheTier ----------------------------------------------------------------------------

VfsCacheTier::VfsCacheTier(Allocator& /*allocator*/, const char* name,
                           assets::VirtualFileSystem& files, const assets::VirtualPath& root,
                           bool writable) noexcept
    : name_(name), files_(&files), root_(root), writable_(writable) {}

VfsCacheTier::~VfsCacheTier() = default;

Expected<bool, Error> VfsCacheTier::load(const CacheKey& key, Array<u8>& out) noexcept {
    Expected<assets::VirtualPath, Error> path = key.to_path(root_);
    if (!path) {
        return make_unexpected(path.error());
    }
    if (!files_->exists(path.value())) {
        return false;
    }
    if (Status read = files_->read(path.value(), out); !read) {
        // A file that exists and cannot be read is a broken cache entry, not a miss. Reporting it
        // rather than recompiling silently is what makes a corrupt cache visible.
        return make_unexpected(read.error());
    }
    return true;
}

Status VfsCacheTier::store(const CacheKey& key, Span<const u8> bytes) noexcept {
    if (!writable_) {
        return fail(ErrorCode::PermissionDenied, "this cache tier is read-only");
    }
    Expected<assets::VirtualPath, Error> path = key.to_path(root_);
    if (!path) {
        return make_unexpected(path.error());
    }
    return files_->write(path.value(), bytes.data(), bytes.size());
}

// --- ShaderCache -----------------------------------------------------------------------------

ShaderCache::ShaderCache(Allocator& allocator) noexcept : tiers_(allocator) {}

Status ShaderCache::add_tier(CacheTier& tier) noexcept {
    return tiers_.push_back(&tier);
}

const CacheTier* ShaderCache::tier_at(usize index) const noexcept {
    return index < tiers_.size() ? tiers_[index] : nullptr;
}

Expected<bool, Error> ShaderCache::load(const CacheKey& key, Array<u8>& out) noexcept {
    ++stats_.lookups;
    for (usize index = 0; index < tiers_.size(); ++index) {
        Expected<bool, Error> found = tiers_[index]->load(key, out);
        if (!found) {
            return found;
        }
        if (!found.value()) {
            continue;
        }
        ++stats_.hits;
        stats_.bytes_loaded += out.size();
        if (index != 0) {
            // A hit in a slower tier is written back into every writable tier in front of it, so
            // the second lookup on this machine does not reach the network.
            ++stats_.promotions;
            for (usize faster = 0; faster < index; ++faster) {
                if (!tiers_[faster]->writable()) {
                    continue;
                }
                // A tier that refuses the write-back costs a slower second lookup and nothing else,
                // so it is not worth failing a successful load over.
                (void)tiers_[faster]->store(key, Span<const u8>(out.data(), out.size()));
            }
        }
        return true;
    }
    ++stats_.misses;
    return false;
}

Status ShaderCache::store(const CacheKey& key, Span<const u8> bytes) noexcept {
    Status result = ok();
    bool stored = false;
    for (CacheTier* tier : tiers_) {
        if (!tier->writable()) {
            continue;
        }
        if (Status written = tier->store(key, bytes); !written) {
            // Keep going: a full local disk should not cost the shared tier its copy. The first
            // failure is what is reported.
            if (result) {
                result = written;
            }
            continue;
        }
        stored = true;
    }
    if (stored) {
        ++stats_.stores;
        stats_.bytes_stored += bytes.size();
    }
    return result;
}

}  // namespace cy::shader
