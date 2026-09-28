// SPDX-License-Identifier: MIT
// The unwrap, kept by geometry key. See `Uv2Cache` in mesh.h.
//
// NO XATLAS HERE. The cache stores and copies what `generate_uv2` produced; unwrap.cpp is still the
// only translation unit that names an xatlas symbol.

#include <cy/import/mesh.h>

#include "unwrap_counter.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <new>

namespace cy::import {
namespace {

/// Bumped by unwrap.cpp through `note_unwrap`, read by `uv2_unwrap_count`.
std::atomic<u64> g_unwraps{0};

/// A version for the key itself, so a change to what an unwrap computes — a new xatlas, a new
/// option — can retire every cached entry by changing one number.
constexpr u32 kKeyVersion = 1;

template <class T>
void hash_array(assets::ContentHasher& hasher, const Array<T>& values) noexcept {
    const u64 count = values.size();
    hasher.update(&count, sizeof(count));
    if (!values.empty()) {
        hasher.update(values.data(), values.size() * sizeof(T));
    }
}

template <class T>
[[nodiscard]] Status copy_array(const Array<T>& from, Array<T>& to) noexcept {
    to.clear();
    if (from.empty()) {
        return ok();
    }
    return to.append(Span<const T>(from.data(), from.size()));
}

[[nodiscard]] Status copy_mesh(const MeshData& from, MeshData& to) noexcept {
    if (Status copied = copy_array(from.positions, to.positions); !copied) {
        return copied;
    }
    if (Status copied = copy_array(from.normals, to.normals); !copied) {
        return copied;
    }
    if (Status copied = copy_array(from.uvs, to.uvs); !copied) {
        return copied;
    }
    if (Status copied = copy_array(from.uv2, to.uv2); !copied) {
        return copied;
    }
    if (Status copied = copy_array(from.tangents, to.tangents); !copied) {
        return copied;
    }
    if (Status copied = copy_array(from.skin, to.skin); !copied) {
        return copied;
    }
    if (Status copied = copy_array(from.indices, to.indices); !copied) {
        return copied;
    }
    return copy_array(from.sections, to.sections);
}

}  // namespace

void note_uv2_unwrap() noexcept {
    g_unwraps.fetch_add(1, std::memory_order_relaxed);
}

u64 uv2_unwrap_count() noexcept {
    return g_unwraps.load(std::memory_order_relaxed);
}

assets::ContentHash uv2_geometry_key(const MeshData& mesh, const Uv2Options& options) noexcept {
    assets::ContentHasher hasher;
    hasher.update(&kKeyVersion, sizeof(kKeyVersion));
    hash_array(hasher, mesh.positions);
    hash_array(hasher, mesh.normals);
    hash_array(hasher, mesh.uvs);
    hash_array(hasher, mesh.tangents);
    hash_array(hasher, mesh.skin);
    hash_array(hasher, mesh.indices);
    hash_array(hasher, mesh.sections);
    // Field by field rather than the struct's bytes: padding is not a property of the options.
    hasher.update(&options.texel_density, sizeof(options.texel_density));
    hasher.update(&options.padding, sizeof(options.padding));
    hasher.update(&options.max_distortion, sizeof(options.max_distortion));
    hasher.update(&options.resolution, sizeof(options.resolution));
    hasher.update(&options.max_chart_angle, sizeof(options.max_chart_angle));
    return hasher.finish();
}

struct Uv2Cache::State {
    struct Entry {
        assets::ContentHash key;
        MeshData mesh;
        Uv2Report report;
    };
    mutable std::mutex mutex;
    std::deque<Entry> entries;
    u64 hits = 0;
};

Uv2Cache::Uv2Cache() noexcept : state_(new (std::nothrow) State) {}

Uv2Cache::~Uv2Cache() {
    delete state_;
}

bool Uv2Cache::find(const assets::ContentHash& key, MeshData& out, Uv2Report& report) noexcept {
    if (state_ == nullptr) {
        return false;
    }
    const std::lock_guard lock(state_->mutex);
    for (const State::Entry& entry : state_->entries) {
        if (entry.key == key) {
            MeshData copy;
            if (!copy_mesh(entry.mesh, copy).has_value()) {
                return false;
            }
            out = std::move(copy);
            report = entry.report;
            state_->hits += 1;
            return true;
        }
    }
    return false;
}

Status Uv2Cache::store(const assets::ContentHash& key, const MeshData& unwrapped,
                       const Uv2Report& report) noexcept {
    if (state_ == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the unwrap cache could not be created");
    }
    State::Entry entry;
    entry.key = key;
    entry.report = report;
    if (Status copied = copy_mesh(unwrapped, entry.mesh); !copied) {
        return copied;
    }
    const std::lock_guard lock(state_->mutex);
    for (const State::Entry& existing : state_->entries) {
        if (existing.key == key) {
            return ok();
        }
    }
    if (state_->entries.size() >= kCapacity) {
        state_->entries.pop_front();
    }
    state_->entries.push_back(std::move(entry));
    return ok();
}

usize Uv2Cache::size() const noexcept {
    if (state_ == nullptr) {
        return 0;
    }
    const std::lock_guard lock(state_->mutex);
    return state_->entries.size();
}

u64 Uv2Cache::hits() const noexcept {
    if (state_ == nullptr) {
        return 0;
    }
    const std::lock_guard lock(state_->mutex);
    return state_->hits;
}

void Uv2Cache::clear() noexcept {
    if (state_ == nullptr) {
        return;
    }
    const std::lock_guard lock(state_->mutex);
    state_->entries.clear();
    state_->hits = 0;
}

Uv2Cache& Uv2Cache::process() noexcept {
    static Uv2Cache cache;
    return cache;
}

Expected<Uv2Report, Error> generate_uv2_cached(MeshData& mesh, const Uv2Options& options,
                                               Uv2Cache* cache) noexcept {
    if (cache == nullptr) {
        return generate_uv2(mesh, options);
    }
    const assets::ContentHash key = uv2_geometry_key(mesh, options);
    Uv2Report report;
    if (cache->find(key, mesh, report)) {
        return report;
    }
    Expected<Uv2Report, Error> unwrapped = generate_uv2(mesh, options);
    if (!unwrapped) {
        return unwrapped;
    }
    if (Status stored = cache->store(key, mesh, unwrapped.value()); !stored) {
        return make_unexpected(stored.error());
    }
    return unwrapped;
}

}  // namespace cy::import
