// Virtual texturing on the device: feedback written by a shader, resolved on the GPU, and a page
// table sampled by a shader. M7 tasks 4.1 and 4.2.
//
// `virtual-texturing` — "GPU feedback": shaders record the virtual pages they would have sampled,
// and the results "are deduplicated, compacted, and prioritised on the GPU before any CPU
// involvement". **"A per-pixel request stream SHALL NOT reach the CPU."**
//
// `virtual-texturing` — "Page tables": "Page table updates SHALL be applied on the GPU without a CPU
// round trip per page", and the mip tail's guarantee — a surface is never missing, only blurry.
//
// ================================================================================================
// THE FEEDBACK BUFFER IS A COUNT PER PAGE, NOT A LIST OF PIXELS
// ================================================================================================
//
// That single decision is what makes "no per-pixel stream reaches the CPU" a structural fact rather
// than a promise about buffer sizes. `record_feedback` runs one thread per pixel and does ONE
// `InterlockedAdd` into a word indexed by the page the pixel would have sampled. A million pixels
// asking for one page is one word holding a million — deduplication is not a pass, it is the shape
// of the buffer. `resolve_feedback` then compacts the non-zero words into `(address, samples)`
// pairs, and that compact list is the only thing the CPU ever maps.
//
// It is the same arrangement `FeedbackBuffer::resolve()` produces on the CPU, and the C++ side
// checks the two against each other — which is what makes this a second implementation of a known
// answer rather than a first implementation of an unknown one.
//
// THE LIMIT, STATED. The count array is one word per addressable page, so it is the flat page-table
// form's cost and it has the flat form's ceiling. `PageTable::kFlatEntryLimit` is 65,536 entries;
// `VirtualTextureFeedbackPass` refuses a texture whose pyramid is larger, naming the limit, rather
// than allocating 128 MB of mostly-zero words. A hash on the device is what a 512k-texel virtual
// space needs and it is not here.
//
// ================================================================================================
// THE COMPACTION IS ORDERED, AND THAT IS DELIBERATE
// ================================================================================================
//
// `resolve_feedback` is ONE workgroup walking the page array in ascending index order with a
// shared-memory prefix scan, exactly as `compact_draws` does in gpu_cull.slang. An atomic-append
// compaction would be shorter and its output order would be a property of the scheduler; the CPU
// mirror emits in a defined order and the two could then only be compared as sets. Ascending page
// index is ascending mip-major address, which is also the order a request list is easiest to read.
//
// PRIORITISATION IS THE SAMPLE COUNT, AND SCORING IS `residency`'S. The compacted entry carries how
// many samples asked for the page; `residency` turns that into a score. This module does not sort by
// priority, because `src/servers/render/virtual_texturing/README.md` states the division: "Policy is
// the residency layer's; storage is this module's."
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc vt_frame.slang -target spirv -profile spirv_1_5 \
//          -entry record_feedback  -stage compute -o vt_record.spv
//   slangc vt_frame.slang -target spirv -profile spirv_1_5 \
//          -entry resolve_feedback -stage compute -o vt_resolve.spv
//   slangc vt_frame.slang -target spirv -profile spirv_1_5 \
//          -entry sample_pages     -stage compute -o vt_sample.spv
//   python3 embed_spirv.py vt_frame_spirv.h \
//          kVtRecordSpirv=vt_record.spv kVtResolveSpirv=vt_resolve.spv kVtSampleSpirv=vt_sample.spv
//   clang-format -i vt_frame_spirv.h

/// One mip level of the texture, as the C++ published it. `x` tiles across, `y` tiles down, `z` the
/// level's first entry in the flat mip-major table, `w` how many entries one layer of it has.
///
/// These four numbers ARE `PageTable::linear_index`'s arithmetic. They are published by the C++
/// rather than recomputed here for the reason that header states: a second implementation of the
/// rounding is a sample that resolves to the wrong page, which is a picture that is subtly incorrect
/// rather than a failure.
struct MipLevel {
    uint tiles_x;
    uint tiles_y;
    uint first_entry;
    uint entries_per_layer;
};

/// One compacted request. `cy::render::vt::FeedbackRequest` is `{u64 address, u32 samples}`; the
/// address is split in two words here because a storage buffer of 64-bit scalars needs a capability
/// this engine does not require of a device.
struct FeedbackRequest {
    uint address_low;
    uint address_high;
    uint samples;
    uint reserved;
};

/// What one thread of `sample_pages` resolved. Mirrors `cy::render::vt::SampleResult`'s four
/// answerable fields; the address it resolved to is recoverable from the level and the thread's own
/// index, so it is not stored.
struct SampleOut {
    uint missing;
    uint resident_mip;
    uint physical_tile;
    uint deficit;
};

struct VtView {
    /// The virtual texture's id, which is the top of the encoded address.
    uint texture_id;
    uint mip_count;
    uint layers;
    /// One past the largest linear page index. The count array's length.
    uint entry_count;

    /// `FeedbackBuffer::samples_pixel`'s modulus: one sample per `density` pixels. The quality lever
    /// `virtual-texturing` requires, applied where the samples are taken rather than where they are
    /// counted.
    uint density;
    uint grid_width;
    uint grid_height;
    uint request_capacity;

    /// Added to the mip a pixel would otherwise ask for. Positive asks for coarser pages, which is
    /// what texture pressure does to the feedback stream.
    int mip_bias;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

// `PageTableEntry`'s six flags, as `page_table.h` declares them.
static const uint kPageInvalid = 0x01u;
static const uint kPageResident = 0x02u;
static const uint kPagePending = 0x04u;
static const uint kPagePinned = 0x08u;
static const uint kPageFallback = 0x10u;
static const uint kPageRuntimeProduced = 0x20u;

static const uint kNoPhysicalTile = 0xFFFFFFFFu;
static const uint kNoResidentMip = 0xFFu;

static const uint kResolveGroupSize = 256u;

[[vk::binding(0, 0)]] ConstantBuffer<VtView> view;
[[vk::binding(1, 0)]] StructuredBuffer<MipLevel> mips;
/// The feedback buffer: one word per addressable page, atomically incremented by the shading pass.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> page_samples;
/// [0] how many requests were compacted, [1] how many samples they account for, [2] how many
/// requests did not fit. The third is `FeedbackBuffer::dropped()`'s counterpart and it is reported,
/// never silently zero.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> resolve_header;
[[vk::binding(4, 0)]] RWStructuredBuffer<FeedbackRequest> requests;
/// The page table, as `PageTable::linear_index` addresses it. `x` is the physical tile; `y` packs
/// the resident mip in bits 0-7, the flags in 8-15 and the generation in 16-31.
[[vk::binding(5, 0)]] StructuredBuffer<uint2> page_table;
[[vk::binding(6, 0)]] RWStructuredBuffer<SampleOut> sample_out;

/// `VirtualAddress::encode`, in the shader. The layout is `address.h`'s and the field widths are
/// fixed by `cy::residency::PageKey`'s 56 opaque bits.
void encode_address(uint texture, uint mip, uint layer, uint tile_y, uint tile_x, out uint low,
                    out uint high) {
    low = ((tile_y & 0xFFFu) << 12u) | (tile_x & 0xFFFu) | ((layer & 0x7u) << 24u) |
          ((mip & 0x1Fu) << 27u);
    // Bit 32 upwards: the texture id, and above it the four-bit address kind. `Planar` is zero, so
    // a planar address's high word is the texture id alone.
    high = texture & 0xFFFFFu;
}

uint entry_index(uint mip, uint layer, uint tile_y, uint tile_x) {
    const MipLevel level = mips[mip];
    return level.first_entry + (layer * level.entries_per_layer) + (tile_y * level.tiles_x) +
           tile_x;
}

// --- Pass one: the shading pass records what it would have sampled -----------------------------
//
// A real forward or visibility-buffer pass computes the address from the interpolated UV and the
// derivative-derived mip; this dispatch computes it from the pixel's position over the same grid,
// because what is being demonstrated is the FEEDBACK PATH and not the texturing. The C++ mirror
// computes the identical address for the identical pixel, so "the two agree" is a claim about the
// path rather than about the formula.

[shader("compute")]
[numthreads(8, 8, 1)]
void record_feedback(uint3 thread: SV_DispatchThreadID) {
    if (thread.x >= view.grid_width || thread.y >= view.grid_height) {
        return;
    }
    const uint pixel = (thread.y * view.grid_width) + thread.x;

    // THE DENSITY LEVER, and it is applied here rather than at the count. `virtual-texturing`:
    // density "SHALL be adjustable — one sample per pixel block rather than per pixel". A pixel that
    // does not report costs one modulo and no memory traffic at all.
    if ((pixel % view.density) != 0u) {
        return;
    }

    // Which page this pixel would have sampled. Distance from the grid's centre stands in for the
    // derivative: the middle of the frame asks for fine mips and the edges for coarse ones, which is
    // what a real frame's feedback looks like and what makes the resolved set span the pyramid.
    const int half_width = int(view.grid_width) / 2;
    const int half_height = int(view.grid_height) / 2;
    const int dx = int(thread.x) - half_width;
    const int dy = int(thread.y) - half_height;
    const int away = (abs(dx) > abs(dy) ? abs(dx) : abs(dy));
    int mip = (away * int(view.mip_count)) / (half_width > 0 ? half_width : 1);
    mip += view.mip_bias;
    if (mip < 0) {
        mip = 0;
    }
    if (mip > int(view.mip_count) - 1) {
        mip = int(view.mip_count) - 1;
    }

    const MipLevel level = mips[uint(mip)];
    const uint tile_x = (thread.x * level.tiles_x) / view.grid_width;
    const uint tile_y = (thread.y * level.tiles_y) / view.grid_height;
    const uint index = entry_index(uint(mip), 0u, tile_y, tile_x);
    if (index >= view.entry_count) {
        return;
    }
    InterlockedAdd(page_samples[index], 1u);
}

// --- Pass two: deduplicate and compact, on the GPU ------------------------------------------------

groupshared uint gs_scan[256];
groupshared uint gs_base;

uint inclusive_scan(uint lane, uint value) {
    GroupMemoryBarrierWithGroupSync();
    gs_scan[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < kResolveGroupSize; offset <<= 1u) {
        uint addend = 0u;
        if (lane >= offset) {
            addend = gs_scan[lane - offset];
        }
        GroupMemoryBarrierWithGroupSync();
        gs_scan[lane] += addend;
        GroupMemoryBarrierWithGroupSync();
    }
    return gs_scan[lane];
}

/// The inverse of `entry_index`: which mip, layer and tile a flat index names.
///
/// A loop over the levels rather than arithmetic, because the pyramid's offsets are not a closed
/// form once a level's tile count is rounded up. `mip_count` is at most 32.
void decode_entry(uint index, out uint mip, out uint layer, out uint tile_y, out uint tile_x) {
    mip = 0u;
    for (uint level = 0u; level < view.mip_count; ++level) {
        if (mips[level].first_entry <= index) {
            mip = level;
        }
    }
    const MipLevel found = mips[mip];
    const uint within = index - found.first_entry;
    layer = found.entries_per_layer > 0u ? within / found.entries_per_layer : 0u;
    const uint rest = found.entries_per_layer > 0u ? within % found.entries_per_layer : 0u;
    tile_y = found.tiles_x > 0u ? rest / found.tiles_x : 0u;
    tile_x = found.tiles_x > 0u ? rest % found.tiles_x : 0u;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void resolve_feedback(uint3 thread: SV_GroupThreadID) {
    const uint lane = thread.x;
    if (lane == 0u) {
        gs_base = 0u;
        resolve_header[0] = 0u;
        resolve_header[1] = 0u;
        resolve_header[2] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint total_samples = 0u;
    uint dropped = 0u;
    for (uint chunk = 0u; chunk < view.entry_count; chunk += kResolveGroupSize) {
        const uint index = chunk + lane;
        const uint samples = index < view.entry_count ? page_samples[index] : 0u;
        const uint wanted = samples > 0u ? 1u : 0u;
        const uint inclusive = inclusive_scan(lane, wanted);
        const uint chunk_total = gs_scan[kResolveGroupSize - 1u];
        const uint slot = gs_base + inclusive - wanted;

        if (wanted != 0u) {
            if (slot < view.request_capacity) {
                uint mip = 0u;
                uint layer = 0u;
                uint tile_y = 0u;
                uint tile_x = 0u;
                decode_entry(index, mip, layer, tile_y, tile_x);
                FeedbackRequest request;
                encode_address(view.texture_id, mip, layer, tile_y, tile_x, request.address_low,
                               request.address_high);
                request.samples = samples;
                request.reserved = 0u;
                requests[slot] = request;
            } else {
                ++dropped;
            }
            total_samples += samples;
        }

        GroupMemoryBarrierWithGroupSync();
        if (lane == 0u) {
            gs_base += chunk_total;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // The two totals are sums over lanes, so they are accumulated atomically rather than scanned:
    // an order-independent quantity does not need an ordered reduction.
    if (total_samples != 0u) {
        InterlockedAdd(resolve_header[1], total_samples);
    }
    if (dropped != 0u) {
        InterlockedAdd(resolve_header[2], dropped);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        resolve_header[0] =
            gs_base < view.request_capacity ? gs_base : view.request_capacity;
    }
}

// --- Pass three: the page table, sampled by a shader ----------------------------------------------
//
// One thread per addressable page, so the mip tail's guarantee is asserted over the WHOLE address
// space rather than at three sampled points — which is the shape `tests/test_system.cpp` already
// uses on the CPU, moved onto the device.

[shader("compute")]
[numthreads(64, 1, 1)]
void sample_pages(uint3 thread: SV_DispatchThreadID) {
    const uint index = thread.x;
    if (index >= view.entry_count) {
        return;
    }
    uint mip = 0u;
    uint layer = 0u;
    uint tile_y = 0u;
    uint tile_x = 0u;
    decode_entry(index, mip, layer, tile_y, tile_x);

    SampleOut result;
    result.missing = 1u;
    result.resident_mip = kNoResidentMip;
    result.physical_tile = kNoPhysicalTile;
    result.deficit = 0u;

    // THE WALK, and it is `VirtualTextureSystem::sample()`'s walk word for word: from the level that
    // was asked for towards the coarsest, stopping at the first resident entry. Because the mip tail
    // is pinned and `PageTable::invalidate` refuses to clear a pinned entry, this terminates on a
    // resident level for every address of a texture whose tail was made resident — which is the
    // whole of "a surface is never missing, only blurry", now held ON THE DEVICE.
    for (uint level = mip; level < view.mip_count; ++level) {
        const uint shift = level - mip;
        const uint probe =
            entry_index(level, layer, tile_y >> shift, tile_x >> shift);
        const uint2 entry = page_table[probe];
        const uint flags = (entry.y >> 8u) & 0xFFu;
        if ((flags & kPageResident) == 0u) {
            continue;
        }
        result.missing = 0u;
        result.resident_mip = level;
        result.physical_tile = entry.x;
        result.deficit = shift;
        break;
    }
    sample_out[index] = result;
}
