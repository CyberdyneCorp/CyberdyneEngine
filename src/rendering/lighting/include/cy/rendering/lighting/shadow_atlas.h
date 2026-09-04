#pragma once
// The conventional shadow atlas: tile sizes, allocation, retention and caching. Task 4.4.2.
//
// `rendering-lighting-and-shadows` — "Shadow atlas allocation". The requirement is careful to say
// that this path "SHALL remain fully supported… it is the correct answer for constrained hardware
// and the required fallback; it is not deprecated by `virtual-shadows`". So this is not a stopgap
// for M7 to delete.
//
// Four properties, each of which is a scenario in the specification and a decision here:
//
//   TILE SIZE FOLLOWS SCREEN COVERAGE, WITH HYSTERESIS. A light covering more of the screen gets a
//   bigger tile. Without hysteresis a light sitting on a size boundary changes tile every frame,
//   and every change is a re-render of its shadow — so the band is not a polish item, it is what
//   makes caching work at all.
//
//   ALLOCATION PREFERS A FREE TILE, THEN THE LEAST RECENTLY USED ONE PAST A RETENTION PERIOD. The
//   retention period is what stops two lights that both want the last tile from evicting each other
//   every frame, which costs two re-renders per frame forever and looks like nothing is wrong.
//
//   SHADOWS ARE CACHED ACROSS FRAMES, tracked "by a version per light and per caster set". A
//   request whose light version and caster version both match what the tile already holds comes
//   back with `needs_render` false, and that is the entire caching mechanism — there is no second
//   place where a shadow decides whether to re-render.
//
//   OVERSUBSCRIPTION IS REPORTED, NOT HIDDEN. A light that cannot get a tile renders unshadowed and
//   `ShadowAtlasStatistics::shortfall` counts it.
//
// ================================================================================================
// THE ALLOCATOR IS A GRID, NOT A QUADTREE, AND THAT IS A SIZE DECISION
// ================================================================================================
//
// The atlas is divided into cells of the smallest tile size, and a tile of size 2^k cells is placed
// at a 2^k-aligned cell. A 4096² atlas with 256² cells is a 16×16 grid: 256 cells, which a linear
// scan searches in microseconds. A quadtree would be the right structure at ten times that, and the
// interface below does not change if one arrives — `request()` and `release()` are the whole
// surface.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

namespace cy::rendering {

/// Where a shadow was rendered, in atlas texels. What a shader turns into a UV rectangle.
struct ShadowTile {
    u32 x = 0;
    u32 y = 0;
    u32 size = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return size != 0; }
};

struct ShadowAtlasConfig {
    /// The atlas is square and a power of two.
    u32 size = 4096;
    /// The grid cell, and therefore the smallest tile.
    u32 min_tile = 256;
    /// The largest tile a single light may take.
    u32 max_tile = 1024;
    /// How many frames a tile is protected from eviction after its last use. The number that stops
    /// two lights thrashing over one tile.
    u32 retention_frames = 8;
    /// The fraction a light's coverage must change by before its tile size does. See the header
    /// comment for why this is load-bearing rather than polish.
    f32 size_hysteresis = 0.35F;
    /// Coverage at or above which a light gets `max_tile`. Below it, size halves per halving of
    /// coverage, down to `min_tile`.
    f32 coverage_for_max_tile = 0.25F;
};

/// What a light asks for.
struct ShadowRequest {
    /// The light's stable identity — `render::LightDescription::stable_id`. NEVER an index into a
    /// frame's light list: a tile is remembered across frames and a frame's ordering is not.
    u64 light_id = 0;
    /// The fraction of the screen the light's bounds cover. Drives the tile size.
    f32 screen_coverage = 0.0F;
    /// Screen coverage × intensity, from `light_importance()`. Breaks ties when the atlas is full.
    f32 importance = 0.0F;
    /// Bumped by whoever moves the light or changes its parameters.
    u64 light_version = 0;
    /// Bumped when the set of casters inside the light's volume changes, or any of them moves. The
    /// "static light with only static casters" scenario is this staying equal.
    u64 caster_version = 0;
};

/// What the atlas answered.
struct ShadowAssignment {
    ShadowTile tile;
    /// False when the tile already holds a valid shadow for these versions — the cache hit, and the
    /// only thing that makes a static light free.
    bool needs_render = true;
    /// The tile's grid cell index, which is what `GpuLight::shadow_slot` carries. Stable while the
    /// tile is held — see the implementation for why it is not the entry's array index.
    u32 slot = 0xFFFFFFFFU;
};

struct ShadowAtlasStatistics {
    u32 cells = 0;
    u32 cells_used = 0;
    u32 tiles_live = 0;
    /// Requests served from an unchanged tile this frame.
    u32 cache_hits = 0;
    u32 renders = 0;
    /// Tiles taken from another light because nothing was free.
    u32 evictions = 0;
    /// Requests that could not be served at all. These lights render unshadowed, and the number is
    /// the "shortfall SHALL be reported" half of the oversubscription scenario.
    u32 shortfall = 0;
    /// Tiles whose size changed this frame, each of which forces a re-render. The number that says
    /// whether the hysteresis band is wide enough.
    u32 resizes = 0;
};

/// The atlas. Not thread-safe: allocation happens once per frame on the frame thread, between the
/// light cull and the shadow passes.
class ShadowAtlas {
public:
    explicit ShadowAtlas(Allocator& allocator) noexcept;

    ShadowAtlas(const ShadowAtlas&) = delete;
    ShadowAtlas& operator=(const ShadowAtlas&) = delete;

    /// Refuses a configuration whose sizes are not powers of two or whose maximum exceeds the
    /// atlas, because either produces a grid whose arithmetic silently does not tile.
    [[nodiscard]] Status initialize(const ShadowAtlasConfig& config) noexcept;

    /// Start a frame. Resets the per-frame counters and advances the clock the retention period is
    /// measured against; it does NOT release anything, which is what makes a tile survive a frame
    /// in which its light was not visible.
    void begin_frame(u64 frame_index) noexcept;

    /// Ask for a tile.
    ///
    /// REQUESTS MUST ARRIVE IN DESCENDING IMPORTANCE, and that is the caller's job rather than this
    /// class's. Sorting inside would mean buffering every request and answering none until the last
    /// one arrived, which is a worse interface; and the caller already has the lights in a list it
    /// sorted for its own reasons. The consequence is stated rather than hidden: under
    /// oversubscription the lights that miss out are the ones at the end of the caller's order.
    [[nodiscard]] Expected<ShadowAssignment, Error> request(const ShadowRequest& request) noexcept;

    /// Give a light's tile back. A light that has been destroyed, or whose shadows were switched to
    /// the virtual path — `rendering-lighting-and-shadows`: a `Virtual` light "SHALL consume no
    /// atlas tile".
    void release(u64 light_id) noexcept;

    /// The tile a light currently holds, or an invalid one.
    [[nodiscard]] ShadowTile tile_of(u64 light_id) const noexcept;

    /// The tile size this coverage asks for, before hysteresis. Public because a caller sizing a
    /// budget wants to know what a light will cost before asking for it.
    [[nodiscard]] u32 tile_size_for(f32 screen_coverage) const noexcept;

    [[nodiscard]] const ShadowAtlasConfig& config() const noexcept { return config_; }
    [[nodiscard]] ShadowAtlasStatistics statistics() const noexcept;

    void reset() noexcept;

private:
    /// One live allocation. `cells` is the tile's size in grid cells, which is what the placement
    /// arithmetic is in.
    struct Entry {
        u64 light_id = 0;
        ShadowTile tile;
        u32 cell_x = 0;
        u32 cell_y = 0;
        u32 cells = 0;
        u64 last_used_frame = 0;
        u64 light_version = ~0ULL;
        u64 caster_version = ~0ULL;
        bool rendered = false;
    };

    [[nodiscard]] usize find_entry(u64 light_id) const noexcept;
    /// Whether every cell of one candidate square is unoccupied.
    [[nodiscard]] bool square_is_free(u32 cell_x, u32 cell_y, u32 cells) const noexcept;
    /// Find a free, correctly aligned square of `cells`×`cells`. Returns false when there is none.
    [[nodiscard]] bool find_free_square(u32 cells, u32& out_x, u32& out_y) const noexcept;
    void occupy(u32 cell_x, u32 cell_y, u32 cells, bool used) noexcept;
    /// Evict the least recently used entry that is past the retention period and is at least
    /// `cells` wide. Returns false when nothing may be evicted, which is the shortfall.
    [[nodiscard]] bool evict_for(u32 cells) noexcept;
    [[nodiscard]] u32 resolve_size(const Entry* existing, f32 coverage) const noexcept;
    /// The coverage a tile of this size is the right answer for, before hysteresis. The inverse of
    /// `tile_size_for`, and what the hysteresis band is measured in.
    [[nodiscard]] f32 coverage_for_size(u32 size) const noexcept;

    ShadowAtlasConfig config_{};
    /// One byte per grid cell: occupied or not. A bitset would be four times smaller and a great
    /// deal harder to read in a debugger, over 256 cells.
    Array<u8> cells_;
    Array<Entry> entries_;
    u32 grid_ = 0;
    u64 frame_ = 0;
    ShadowAtlasStatistics stats_{};
};

}  // namespace cy::rendering
