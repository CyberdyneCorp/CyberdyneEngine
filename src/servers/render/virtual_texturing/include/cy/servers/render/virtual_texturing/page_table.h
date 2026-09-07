#pragma once
// The page table: virtual pages to physical tiles, and the batched update that never round-trips
// per page. Task 5.1.
//
// `virtual-texturing` — "Page tables". An entry carries "at minimum: the physical tile, the
// resident mip actually available, state flags (resident, pending, pinned, fallback, invalid,
// runtime-produced), and a generation for validation". The implementation — flat or hierarchical —
// "SHALL be an internal decision hidden behind the lookup, chosen by address space size, since a
// flat table is faster for small spaces and untenable for large ones".
//
// --- THE INTERNAL DECISION, AND HOW IT IS KEPT INTERNAL
// ---------------------------------------------
//
// `configure()` picks: a flat array indexed by (mip, layer, y, x) when the whole space fits in
// `kFlatEntryLimit` entries, and a sparse hash keyed on the encoded address when it does not. A
// 16k x 16k terrain texture at 128-texel tiles is 16,384 tiles at mip 0 and about 21,845 over the
// pyramid — flat. A 512k-texel virtual space is 16 million, and a flat table for it would be 128 MB
// of mostly-invalid entries.
//
// `lookup()` is the same call either way and `is_flat()` exists only so that the test can run the
// SAME property suite over both representations. If the two ever disagree, that test says so.
//
// --- WHY UPDATES ARE STAGED
// --------------------------------------------------------------------------
//
// "Page table updates SHALL be applied on the GPU without a CPU round trip per page." The shape
// that makes that possible is the one here: `stage()` appends to an update list — the buffer that
// would be uploaded — and `apply_staged()` consumes the whole list in one pass. A design where
// becoming resident wrote the table directly would have no list to upload and no way to batch, and
// the difference is invisible until a frame makes a thousand pages resident at once.
//
// This module is LAYER 2 and may not name a device, a buffer or a command list (see
// `src/servers/CMakeLists.txt`). `apply_staged()` is therefore the CPU-side mirror the uploader
// copies from, and `staged()` is the span it would upload. That is the boundary, not a shortcut.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/render/virtual_texturing/address.h>

namespace cy::render::vt {

/// `virtual-texturing`'s six state flags. A bitmask rather than an enumeration because a page is
/// legitimately several of these at once — resident and pinned, or fallback and pending.
struct PageFlags {
    static constexpr u8 kInvalid = 0x01;   // nothing has been resolved for this page
    static constexpr u8 kResident = 0x02;  // the physical tile holds this page
    static constexpr u8 kPending = 0x04;   // requested, being fetched or produced
    static constexpr u8 kPinned = 0x08;    // mip tail, or held by the residency layer
    static constexpr u8 kFallback = 0x10;  // resolves to a coarser level than asked for
    static constexpr u8 kRuntimeProduced =
        0x20;  // written by a producer rather than read from disk
};

struct PageTableEntry {
    u32 physical_tile = kNoPhysicalTile;
    /// The mip actually available for this address — which may be coarser than the address's own
    /// mip, and is what makes the deficit measurable rather than inferred.
    u8 resident_mip = kNoResidentMip;
    u8 flags = PageFlags::kInvalid;
    /// Bumped every time the entry is rewritten. A shader that read an entry, then sampled, can
    /// compare generations and know whether the tile moved underneath it.
    u16 generation = 0;

    [[nodiscard]] constexpr bool resident() const noexcept {
        return (flags & PageFlags::kResident) != 0;
    }
    [[nodiscard]] constexpr bool pinned() const noexcept {
        return (flags & PageFlags::kPinned) != 0;
    }
    [[nodiscard]] constexpr bool pending() const noexcept {
        return (flags & PageFlags::kPending) != 0;
    }
};

/// One staged change: what the uploader would write, and where.
struct PageTableUpdate {
    u64 address = 0;  // encoded VirtualAddress
    PageTableEntry entry;
};

/// One virtual texture's page table.
class PageTable {
public:
    /// Above this many entries the table is sparse rather than flat. 64k entries is 512 KB of
    /// `PageTableEntry`, which is the largest allocation worth making eagerly for a table that is
    /// mostly invalid in the interesting cases.
    static constexpr usize kFlatEntryLimit = 1U << 16U;

    explicit PageTable(Allocator& allocator = current_allocator()) noexcept
        : flat_(allocator), sparse_(allocator), staged_(allocator), mip_offsets_(allocator) {}

    PageTable(const PageTable&) = delete;
    PageTable& operator=(const PageTable&) = delete;
    PageTable(PageTable&&) noexcept = default;
    PageTable& operator=(PageTable&&) noexcept = default;
    ~PageTable() = default;

    Status configure(const VirtualTextureDesc& desc) noexcept;
    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] const VirtualTextureDesc& description() const noexcept { return desc_; }

    /// The internal representation in force. Exposed ONLY so that the suite can run the same
    /// properties over both; no caller should branch on it.
    [[nodiscard]] bool is_flat() const noexcept { return flat_representation_; }

    /// Resolve one page. An address outside the description, or one never written, reads as an
    /// invalid entry rather than as a failure — a sampler cannot handle an `Expected`.
    [[nodiscard]] PageTableEntry lookup(const VirtualAddress& address) const noexcept;

    /// Queue a change. Nothing is visible to `lookup()` until `apply_staged()` runs.
    Status stage(const VirtualAddress& address, const PageTableEntry& entry) noexcept;
    [[nodiscard]] usize staged_count() const noexcept { return staged_.size(); }
    [[nodiscard]] const PageTableUpdate* staged() const noexcept { return staged_.data(); }

    /// Apply every staged change in one pass and clear the list. Returns how many were applied.
    /// THE BATCH IS THE POINT: a thousand pages becoming resident is one call, not a thousand.
    usize apply_staged() noexcept;
    [[nodiscard]] usize last_batch_size() const noexcept { return last_batch_; }
    [[nodiscard]] u64 batches_applied() const noexcept { return batches_; }

    /// Mark a page invalid, together with `finer_levels` levels of the pyramid below it. What a
    /// runtime producer calls when its inputs change: "so only affected pages are re-produced".
    ///
    /// THE DEPTH IS THE CALLER'S AND IS NOT A BOOLEAN, because each level below quadruples the
    /// pages touched: invalidating one coarse page all the way down is 4^n stages, and a terrain
    /// deformation that means to touch two levels must not accidentally stage a million. Zero
    /// invalidates the named page alone.
    Status invalidate(const VirtualAddress& address, u8 finer_levels) noexcept;

    [[nodiscard]] usize resident_entries() const noexcept;
    void clear() noexcept;

    // --- What a GPU page-table image is addressed by, and how big it is
    // ---------------------------
    //
    // A shader cannot call `lookup()`. What it can do is index a buffer, and the two must agree
    // about the arithmetic — so the arithmetic is published rather than reimplemented. M7 task 4.2.
    //
    // `linear_index` is the flat representation's own index, mip-major, and it is well defined for
    // every in-range address whether or not the table chose the flat form: it is a property of the
    // DESCRIPTION, not of the storage. `entry_count` is one past the largest of them.
    //
    // WHY THIS IS PUBLISHED RATHER THAN COPIED INTO THE UPLOADER.
    // `src/rendering/virtual_texturing/` builds the buffer a shader samples, and it has to write
    // each entry where the shader will look for it. A second implementation of `mip_offsets_ +
    // layer * tile_count + y * tiles_x + x` is a second place to get a rounding rule wrong, and the
    // symptom would be a sample resolving to the wrong page rather than to no page — which is a
    // picture that is subtly incorrect rather than a failure.

    /// Where `address` lives in a flat, mip-major table. `entry_count()` for an address outside the
    /// description, which is a value no in-range address takes and is therefore a usable sentinel.
    [[nodiscard]] usize linear_index(const VirtualAddress& address) const noexcept;
    /// How many entries the whole pyramid has, over every layer.
    [[nodiscard]] usize entry_count() const noexcept;

private:
    /// Stage one page as invalid, unless it is out of range or pinned. The pin check lives here so
    /// that every invalidation path gets it.
    Status clear_page(const VirtualAddress& address) noexcept;
    /// Stage every `finer`-level page covering `coarse`'s footprint as invalid.
    Status clear_footprint(const VirtualAddress& coarse, u8 finer) noexcept;
    [[nodiscard]] usize flat_index(const VirtualAddress& address) const noexcept;
    [[nodiscard]] bool in_range(const VirtualAddress& address) const noexcept;
    void write(const VirtualAddress& address, const PageTableEntry& entry) noexcept;

    VirtualTextureDesc desc_;
    /// The flat representation: one entry per addressable page, laid out mip-major so that the
    /// coarse levels — the ones every sample walks to — are contiguous.
    Array<PageTableEntry> flat_;
    HashMap<u64, PageTableEntry> sparse_;
    Array<PageTableUpdate> staged_;
    Array<usize> mip_offsets_;
    usize last_batch_ = 0;
    u64 batches_ = 0;
    bool flat_representation_ = false;
    bool configured_ = false;
};

}  // namespace cy::render::vt
