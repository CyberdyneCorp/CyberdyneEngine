// The pipeline state key, the manifest, and the central state cache. Task 3.7.

#include <cy/backends/shader/pipeline.h>

#include <cstring>

namespace cy::shader {
namespace {

void hash_bytes(assets::ContentHasher& hasher, const void* data, usize size) noexcept {
    const u64 length = size;
    hasher.update(&length, sizeof(length));
    if (size != 0) {
        hasher.update(data, size);
    }
}

void hash_u32(assets::ContentHasher& hasher, u32 value) noexcept {
    hasher.update(&value, sizeof(value));
}

/// A colour attachment's state, field by field.
///
/// Field by field rather than as a struct: `hash_bytes` over the struct would fold in whatever the
/// compiler put in its padding, and two identical states could then hash differently on two builds.
/// A key that is not reproducible is a manifest that warms nothing.
void hash_color_attachment(assets::ContentHasher& hasher,
                           const rhi::ColorAttachmentState& state) noexcept {
    hash_u32(hasher, static_cast<u32>(state.format));
    hash_u32(hasher, state.blend_enable ? 1U : 0U);
    hash_u32(hasher, static_cast<u32>(state.source_color));
    hash_u32(hasher, static_cast<u32>(state.destination_color));
    hash_u32(hasher, static_cast<u32>(state.color_op));
    hash_u32(hasher, static_cast<u32>(state.source_alpha));
    hash_u32(hasher, static_cast<u32>(state.destination_alpha));
    hash_u32(hasher, static_cast<u32>(state.alpha_op));
    hash_u32(hasher, static_cast<u32>(state.write_mask));
}

void hash_depth_stencil(assets::ContentHasher& hasher,
                        const rhi::DepthStencilState& state) noexcept {
    hash_u32(hasher, static_cast<u32>(state.format));
    hash_u32(hasher, state.depth_test_enable ? 1U : 0U);
    hash_u32(hasher, state.depth_write_enable ? 1U : 0U);
    hash_u32(hasher, static_cast<u32>(state.depth_compare));
    hash_u32(hasher, state.stencil_test_enable ? 1U : 0U);
}

void hash_rasterisation(assets::ContentHasher& hasher,
                        const rhi::RasterisationState& state) noexcept {
    hash_u32(hasher, static_cast<u32>(state.polygon_mode));
    hash_u32(hasher, static_cast<u32>(state.cull_mode));
    hash_u32(hasher, static_cast<u32>(state.front_face));
    hash_u32(hasher, state.depth_clamp_enable ? 1U : 0U);
    // The float fields are hashed as their bits: a depth bias is part of the state, and two states
    // differing only in it are two pipelines.
    hasher.update(&state.depth_bias_constant, sizeof(state.depth_bias_constant));
    hasher.update(&state.depth_bias_slope, sizeof(state.depth_bias_slope));
    hasher.update(&state.line_width, sizeof(state.line_width));
}

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

const char* pipeline_kind_name(PipelineKind kind) noexcept {
    switch (kind) {
        case PipelineKind::Graphics:
            return "graphics";
        case PipelineKind::Compute:
            return "compute";
    }
    return "unknown";
}

const char* pipeline_status_name(PipelineStatus status) noexcept {
    switch (status) {
        case PipelineStatus::Missing:
            return "missing";
        case PipelineStatus::Compiling:
            return "compiling";
        case PipelineStatus::Ready:
            return "ready";
        case PipelineStatus::Failed:
            return "failed";
    }
    return "unknown";
}

PipelineStateKey derive_pipeline_state_key(const PipelineStateInputs& inputs) noexcept {
    assets::ContentHasher hasher;
    hash_u32(hasher, static_cast<u32>(inputs.kind));
    hash_u32(hasher, static_cast<u32>(inputs.programs.size()));
    for (const assets::ContentHash& program : inputs.programs) {
        hasher.update(program.bytes, sizeof(program.bytes));
    }
    hash_u32(hasher, static_cast<u32>(inputs.color_attachments.size()));
    for (const rhi::ColorAttachmentState& attachment : inputs.color_attachments) {
        hash_color_attachment(hasher, attachment);
    }
    hash_depth_stencil(hasher, inputs.depth_stencil);
    hash_rasterisation(hasher, inputs.rasterisation);
    hash_u32(hasher, static_cast<u32>(inputs.vertex_bindings.size()));
    for (const rhi::VertexBinding& binding : inputs.vertex_bindings) {
        hash_u32(hasher, binding.binding);
        hash_u32(hasher, binding.stride);
        hash_u32(hasher, static_cast<u32>(binding.input_rate));
    }
    hash_u32(hasher, static_cast<u32>(inputs.vertex_attributes.size()));
    for (const rhi::VertexAttribute& attribute : inputs.vertex_attributes) {
        hash_u32(hasher, attribute.location);
        hash_u32(hasher, attribute.binding);
        hash_u32(hasher, static_cast<u32>(attribute.format));
        hash_u32(hasher, attribute.offset);
    }
    hash_u32(hasher, static_cast<u32>(inputs.topology));
    hash_u32(hasher, inputs.sample_count);
    hash_u32(hasher, inputs.view_mask);
    hasher.update(&inputs.permutation.value, sizeof(inputs.permutation.value));

    PipelineStateKey key;
    key.hash = hasher.finish();
    return key;
}

Expected<assets::VirtualPath, Error> pipeline_cache_path(const assets::VirtualPath& root,
                                                         const PipelineDiskCacheKey& key) noexcept {
    assets::ContentHasher hasher;
    hash_bytes(hasher, key.device_name.data(), key.device_name.size());
    hash_u32(hasher, key.vendor_id);
    hash_u32(hasher, key.device_id);
    hash_u32(hasher, key.driver_version);
    hash_bytes(hasher, key.engine_version.data(), key.engine_version.size());
    const assets::ContentHash digest = hasher.finish();

    // Sixteen hex digits is enough to name a file per (device, driver, engine version) on one
    // machine, and short enough to read in a bug report.
    constexpr std::string_view kExtension = ".pcache";
    char name[32] = {};
    usize length = 0;
    for (usize index = 0; index < 8; ++index) {
        name[length++] = kHexDigits[digest.bytes[index] >> 4U];
        name[length++] = kHexDigits[digest.bytes[index] & 0x0FU];
    }
    for (const char character : kExtension) {
        name[length++] = character;
    }
    const std::string_view file(name, length);

    if (root.empty()) {
        return assets::VirtualPath::normalise(file);
    }
    return root.join(file);
}

// --- PipelineManifest ------------------------------------------------------------------------

PipelineManifest::PipelineManifest(Allocator& allocator) noexcept : entries_(allocator) {}

usize PipelineManifest::find(const PipelineStateKey& key) const noexcept {
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].key == key) {
            return index;
        }
    }
    return entries_.size();
}

bool PipelineManifest::contains(const PipelineStateKey& key) const noexcept {
    return find(key) != entries_.size();
}

const PipelineManifestEntry* PipelineManifest::entry_at(usize index) const noexcept {
    return index < entries_.size() ? &entries_[index] : nullptr;
}

Status PipelineManifest::record(const PipelineStateKey& key,
                                Span<const assets::ContentHash> programs) noexcept {
    if (programs.empty() || programs.size() > 2) {
        return fail(ErrorCode::InvalidArgument,
                    "a pipeline state names one program (compute) or two (graphics)");
    }
    const usize index = find(key);
    if (index != entries_.size()) {
        ++entries_[index].uses;
        return ok();
    }
    PipelineManifestEntry entry;
    entry.key = key;
    entry.program_count = static_cast<u8>(programs.size());
    for (usize slot = 0; slot < programs.size(); ++slot) {
        entry.programs[slot] = programs[slot];
    }
    entry.uses = 1;
    return entries_.push_back(entry);
}

void PipelineManifest::sort() noexcept {
    // Descending use count, then by key. Warming then does the states a level leans on first, and a
    // manifest cooked twice from the same session is byte-identical — the same "stable inputs only"
    // rule design.md §6 puts on draw order.
    for (usize index = 1; index < entries_.size(); ++index) {
        PipelineManifestEntry value = entries_[index];
        usize position = index;
        while (position > 0) {
            const PipelineManifestEntry& previous = entries_[position - 1];
            const bool later = previous.uses < value.uses ||
                               (previous.uses == value.uses && value.key.hash < previous.key.hash);
            if (!later) {
                break;
            }
            entries_[position] = previous;
            --position;
        }
        entries_[position] = value;
    }
}

Status PipelineManifest::serialize(Array<u8>& out) const noexcept {
    out.clear();
    const auto write_u32 = [&out](u32 value) noexcept -> Status {
        return out.append(Span<const u8>(reinterpret_cast<const u8*>(&value), sizeof(value)));
    };
    if (Status written = write_u32(kPipelineManifestMagic); !written) {
        return written;
    }
    if (Status written = write_u32(kPipelineManifestVersion); !written) {
        return written;
    }
    if (Status written = write_u32(static_cast<u32>(entries_.size())); !written) {
        return written;
    }
    if (Status written = write_u32(0); !written) {
        return written;
    }
    for (const PipelineManifestEntry& entry : entries_) {
        if (Status written =
                out.append(Span<const u8>(entry.key.hash.bytes, sizeof(entry.key.hash.bytes)));
            !written) {
            return written;
        }
        for (const assets::ContentHash& program : entry.programs) {
            if (Status written = out.append(Span<const u8>(program.bytes, sizeof(program.bytes)));
                !written) {
                return written;
            }
        }
        if (Status written = write_u32(entry.program_count); !written) {
            return written;
        }
        if (Status written = write_u32(entry.uses); !written) {
            return written;
        }
    }
    return ok();
}

Expected<PipelineManifest, Error> PipelineManifest::parse(Allocator& allocator,
                                                          Span<const u8> bytes) noexcept {
    constexpr usize kHeaderBytes = 4 * sizeof(u32);
    constexpr usize kEntryBytes = (3 * assets::ContentHash::kByteLength) + (2 * sizeof(u32));
    if (bytes.size() < kHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "the pipeline manifest header is truncated");
    }
    u32 header[4] = {};
    std::memcpy(header, bytes.data(), kHeaderBytes);
    if (header[0] != kPipelineManifestMagic) {
        return fail(ErrorCode::InvalidArgument, "not a pipeline manifest: wrong magic number");
    }
    if (header[1] != kPipelineManifestVersion) {
        return fail(ErrorCode::Unsupported,
                    "the pipeline manifest was written by a different version of the format");
    }
    if (bytes.size() < kHeaderBytes + (static_cast<usize>(header[2]) * kEntryBytes)) {
        return fail(ErrorCode::InvalidArgument, "the pipeline manifest is shorter than it claims");
    }

    PipelineManifest manifest(allocator);
    usize cursor = kHeaderBytes;
    for (u32 index = 0; index < header[2]; ++index) {
        PipelineManifestEntry entry;
        std::memcpy(entry.key.hash.bytes, bytes.data() + cursor, assets::ContentHash::kByteLength);
        cursor += assets::ContentHash::kByteLength;
        for (auto& program : entry.programs) {
            std::memcpy(program.bytes, bytes.data() + cursor, assets::ContentHash::kByteLength);
            cursor += assets::ContentHash::kByteLength;
        }
        u32 fields[2] = {};
        std::memcpy(fields, bytes.data() + cursor, sizeof(fields));
        cursor += sizeof(fields);
        entry.program_count = static_cast<u8>(fields[0]);
        entry.uses = fields[1];
        if (Status pushed = manifest.entries_.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return manifest;
}

void PipelineManifest::clear() noexcept {
    entries_.clear();
}

// --- PipelineStateCache ----------------------------------------------------------------------

PipelineStateCache::PipelineStateCache(Allocator& allocator) noexcept : states_(allocator) {}

void PipelineStateCache::set_builder(PipelineBuilder builder, void* user) noexcept {
    builder_ = builder;
    builder_user_ = user;
}

usize PipelineStateCache::find(const PipelineStateKey& key) const noexcept {
    for (usize index = 0; index < states_.size(); ++index) {
        if (states_[index].key == key) {
            return index;
        }
    }
    return states_.size();
}

Expected<usize, Error> PipelineStateCache::intern(
    const PipelineStateKey& key, Span<const assets::ContentHash> programs) noexcept {
    const usize existing = find(key);
    if (existing != states_.size()) {
        return existing;
    }
    State state;
    state.key = key;
    state.program_count = static_cast<u8>(programs.size() > 2 ? 2 : programs.size());
    for (u8 slot = 0; slot < state.program_count; ++slot) {
        state.programs[slot] = programs[slot];
    }
    if (Status pushed = states_.push_back(state); !pushed) {
        return make_unexpected(pushed.error());
    }
    return states_.size() - 1;
}

PipelineStateCache::Request PipelineStateCache::request(
    const PipelineStateKey& key, Span<const assets::ContentHash> programs) noexcept {
    ++stats_.requests;
    Expected<usize, Error> index = intern(key, programs);
    Request out;
    if (!index) {
        // Out of memory recording the state. The fallback is still the right answer for the draw.
        out.pipeline = fallback_;
        out.is_fallback = true;
        ++stats_.fallbacks;
        return out;
    }

    State& state = states_[index.value()];
    if (state.status == PipelineStatus::Ready) {
        ++stats_.hits;
        out.pipeline = state.pipeline;
        out.status = PipelineStatus::Ready;
        return out;
    }
    if (state.status == PipelineStatus::Missing) {
        // Queued, not built: building here is the frame stall this requirement exists to remove.
        state.status = PipelineStatus::Compiling;
    }
    ++stats_.fallbacks;
    out.pipeline = fallback_;
    out.status = state.status;
    out.is_fallback = true;
    return out;
}

bool PipelineStateCache::build_at(usize index) noexcept {
    State& state = states_[index];
    if (builder_ == nullptr) {
        state.status = PipelineStatus::Failed;
        ++stats_.build_failures;
        return false;
    }
    Expected<PipelineObject, Error> built =
        builder_(builder_user_, state.key,
                 Span<const assets::ContentHash>(state.programs, state.program_count));
    if (!built) {
        // Recorded as failed rather than left pending, so the failure is reported once instead of
        // retried every frame. A hot reload of the shader clears it back to Missing.
        state.status = PipelineStatus::Failed;
        ++stats_.build_failures;
        return false;
    }
    state.pipeline = built.value();
    state.status = PipelineStatus::Ready;
    ++stats_.builds;
    return true;
}

Expected<bool, Error> PipelineStateCache::build_pending() noexcept {
    for (usize index = 0; index < states_.size(); ++index) {
        if (states_[index].status != PipelineStatus::Compiling) {
            continue;
        }
        (void)build_at(index);
        return true;
    }
    return false;
}

Status PipelineStateCache::warm(const PipelineManifest& manifest, PipelineWarmObserver observer,
                                void* user) noexcept {
    const usize total = manifest.size();
    for (usize index = 0; index < total; ++index) {
        const PipelineManifestEntry* entry = manifest.entry_at(index);
        if (entry == nullptr) {
            continue;
        }
        Expected<usize, Error> slot = intern(
            entry->key, Span<const assets::ContentHash>(entry->programs, entry->program_count));
        if (!slot) {
            return make_unexpected(slot.error());
        }
        bool built = states_[slot.value()].status == PipelineStatus::Ready;
        if (!built) {
            built = build_at(slot.value());
            if (built) {
                ++stats_.warmed;
            }
        }
        if (observer != nullptr) {
            observer(user, index + 1, total, entry->key, built);
        }
    }
    return ok();
}

PipelineStatus PipelineStateCache::status_of(const PipelineStateKey& key) const noexcept {
    const usize index = find(key);
    return index == states_.size() ? PipelineStatus::Missing : states_[index].status;
}

usize PipelineStateCache::pending_count() const noexcept {
    usize count = 0;
    for (const State& state : states_) {
        if (state.status == PipelineStatus::Compiling) {
            ++count;
        }
    }
    return count;
}

Status PipelineStateCache::invalidate(const PipelineStateKey& key) noexcept {
    const usize index = find(key);
    if (index == states_.size()) {
        return fail(ErrorCode::NotFound, "no such pipeline state");
    }
    states_[index].status = PipelineStatus::Missing;
    states_[index].pipeline = PipelineObject{};
    return ok();
}

u32 PipelineStateCache::invalidate_by_program(const assets::ContentHash& program) noexcept {
    u32 count = 0;
    for (State& state : states_) {
        bool names_it = false;
        for (u8 slot = 0; slot < state.program_count; ++slot) {
            names_it = names_it || state.programs[slot] == program;
        }
        if (!names_it) {
            continue;
        }
        state.status = PipelineStatus::Missing;
        state.pipeline = PipelineObject{};
        ++count;
    }
    return count;
}

void PipelineStateCache::clear() noexcept {
    states_.clear();
}

}  // namespace cy::shader
