#pragma once
// Water streaming: one logical body for the whole world, a few resident segments, and a server
// profile that keeps the queries and drops the surfaces. M10 task 2.3.
//
// `water` — "Water streaming": "A water body SHALL exist LOGICALLY FOR THE WHOLE WORLD while only
// its nearby SEGMENTS are resident: surface geometry, simulation state, foam, shoreline data,
// physics representation, and audio. A river SHALL NOT be split into unrelated per-cell objects;
// its network and identity SHALL be GLOBAL while its runtime data is segmented. Water payloads
// SHALL be a CELL CHANNEL, so a server profile can omit surface rendering data while retaining the
// queries and physics representation it needs."
//
// ================================================================================================
// IDENTITY IS GLOBAL, RUNTIME DATA IS SEGMENTED, AND THE DIFFERENCE IS IN THE TYPES
// ================================================================================================
//
// A `WaterBodyId` is a hash of a name and exists whether or not anything is resident; a
// `WaterSegmentKey` is (body, x, z) and exists only while a cell that overlaps it is bound. Nothing
// in this module can produce a per-cell body, because there is no type for one — which is the
// specification's "A continental river" scenario made structural rather than reviewed.
//
// ================================================================================================
// NO SECOND STREAMING MECHANISM, AND NO BUDGET OF ITS OWN
// ================================================================================================
//
// This is a registered consumer of `world::CellEventQueue`, at a declared order, like navigation,
// audio, illumination and `environment::FieldStreaming`. A segment is resident exactly while at
// least one bound cell overlaps it, so "evicted with them" falls out of consuming the same events
// rather than out of a second lifetime kept in step.
//
// It does NOT take a residency budget. Field tiles go through `residency::ResidencyServer` because
// their bytes are cooked data that a policy chooses to admit or refuse; a water segment is DERIVED
// — the ocean's cascades are evaluated around the camera and a river's segment is a range of a
// spline already in memory — so there is nothing to admit. A budget here would be a policy deciding
// whether water exists in a cell whose water it has already agreed to stream.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/water/body.h>
#include <cy/world/activation.h>
#include <cy/world/cell.h>

namespace cy::water {

/// What a resident segment carries. `water`'s own list: "surface geometry, simulation state, foam,
/// shoreline data, physics representation, and audio".
enum class WaterPayload : u8 {
    /// The query and simulation state: the body's backend evaluated here. Without it a query in
    /// this segment falls back to the mean level.
    Query = 0,
    /// The collision and buoyancy representation.
    Physics,
    /// Surface geometry for rendering. THE ONE A SERVER DROPS.
    Surface,
    /// The foam field's coverage over this segment.
    Foam,
    /// Shoreline data: the fields' published rectangle for this segment.
    Shoreline,
    /// Audio emitters along the shore and the rapids.
    Audio,
    kCount,
};

[[nodiscard]] const char* water_payload_name(WaterPayload payload) noexcept;

/// A set of payloads. A bitmask with names, so a mask is copied, compared and stored without a
/// container — the shape `world::ChannelMask` has, for the same reasons.
struct WaterPayloadMask {
    u8 bits = 0;

    [[nodiscard]] static constexpr WaterPayloadMask none() noexcept { return WaterPayloadMask{}; }
    [[nodiscard]] static constexpr WaterPayloadMask all() noexcept {
        return WaterPayloadMask{
            static_cast<u8>((1U << static_cast<u32>(WaterPayload::kCount)) - 1U)};
    }
    [[nodiscard]] static constexpr WaterPayloadMask of(WaterPayload payload) noexcept {
        return WaterPayloadMask{static_cast<u8>(1U << static_cast<u32>(payload))};
    }
    [[nodiscard]] constexpr bool has(WaterPayload payload) const noexcept {
        return (bits & of(payload).bits) != 0;
    }
    constexpr void set(WaterPayload payload) noexcept { bits |= of(payload).bits; }
    constexpr void clear(WaterPayload payload) noexcept {
        bits = static_cast<u8>(bits & ~of(payload).bits);
    }
    friend constexpr bool operator==(WaterPayloadMask, WaterPayloadMask) noexcept = default;
};

/// The payloads a world profile carries. A dedicated server keeps everything a simulation needs and
/// drops the surface geometry and the foam — which is the specification's "Server keeps queries,
/// drops surfaces" scenario as a function rather than as a convention.
[[nodiscard]] WaterPayloadMask profile_payloads(world::WorldProfile profile) noexcept;

/// One segment of one body. Segments are square in plan, `WaterBodyDesc::segment_metres` on a side,
/// on a grid anchored at the world origin so that two bodies' segments do not have to agree about
/// anything but the size they each declared.
struct WaterSegmentKey {
    WaterBodyId body;
    i32 x = 0;
    i32 z = 0;

    friend constexpr bool operator==(const WaterSegmentKey&,
                                     const WaterSegmentKey&) noexcept = default;
};

/// A resident segment.
struct WaterSegment {
    WaterSegmentKey key;
    WaterPayloadMask payloads;
    /// How many bound cells overlap it. A segment is dropped when this reaches zero, which is what
    /// makes "evicted with them" true for a segment straddling two cells.
    u32 references = 0;
    /// Estimated bytes. Derived from the payload mask, so a server's segment is measurably smaller
    /// than a client's rather than merely documented as such.
    u64 bytes = 0;
};

/// What one tick did.
struct WaterStreamingReport {
    u32 cells_bound = 0;
    u32 cells_released = 0;
    u32 segments_created = 0;
    u32 segments_dropped = 0;
    /// Cells carrying the water channel whose footprint overlapped no body at all. Counted rather
    /// than ignored: a world whose every cell reports this has water bounds that do not match its
    /// terrain, which is an authoring error nothing else would surface.
    u32 cells_without_water = 0;
};

/// Binds a body's segments to the world cells that overlap them.
class WaterStreaming {
public:
    WaterStreaming(Allocator& allocator, const WaterRegistry& registry,
                   const world::PartitionConfig& partition) noexcept;

    WaterStreaming(const WaterStreaming&) = delete;
    WaterStreaming& operator=(const WaterStreaming&) = delete;

    /// The profile whose payloads new segments carry. Changing it does not rewrite the segments
    /// already resident: a profile is chosen when a world is opened, and a mid-session change would
    /// be a different world.
    void set_profile(world::WorldProfile profile) noexcept { profile_ = profile; }
    [[nodiscard]] world::WorldProfile profile() const noexcept { return profile_; }

    /// Register as a consumer of a world's cell events, at a declared order.
    [[nodiscard]] Status attach(world::CellEventQueue& events, u32 order) noexcept;

    /// Tell the streamer where a cell is. A cell event carries an identifier and a channel mask and
    /// nothing spatial — `world::CellId` is opaque by requirement — so the footprint has to come
    /// from whoever added the cell to the world. `environment::FieldStreaming` takes the same
    /// declaration for the same reason, and a consumer of both learns one model.
    [[nodiscard]] Status declare_cell(world::CellId cell, const world::CellCoord& coord) noexcept;

    /// Drain the cell events and bind or release segments accordingly.
    [[nodiscard]] Expected<WaterStreamingReport, Error> tick() noexcept;

    [[nodiscard]] const WaterSegment* find(const WaterSegmentKey& key) const noexcept;
    [[nodiscard]] bool is_resident(const WaterSegmentKey& key) const noexcept {
        return find(key) != nullptr;
    }
    /// Whether a payload is resident at a position for a body. What `WaterSystem::query()` asks to
    /// decide between `Simulated` and the mean-level fallback.
    [[nodiscard]] bool payload_resident(WaterBodyId body, const world::WorldVec3d& at,
                                        WaterPayload payload) const noexcept;

    /// The segment a position falls in, for a body with the given segment size.
    [[nodiscard]] static WaterSegmentKey segment_of(WaterBodyId body, f32 segment_metres,
                                                    const world::WorldVec3d& at) noexcept;

    [[nodiscard]] usize resident_segments() const noexcept { return segments_.size(); }
    [[nodiscard]] usize bound_cells() const noexcept { return bindings_.size(); }
    [[nodiscard]] u64 bytes() const noexcept;
    [[nodiscard]] Span<const WaterSegment> segments() const noexcept { return segments_.span(); }

private:
    struct Binding {
        world::CellId cell;
        Array<WaterSegmentKey> segments;

        explicit Binding(Allocator& allocator) noexcept : segments(allocator) {}
    };

    /// A cell's footprint in absolute metres, and the test against a body's bounds. Two rectangles
    /// and the comparison between them, named so that `bind_cell()` reads as "which bodies reach
    /// into this cell" rather than as four coordinate comparisons.
    struct Footprint {
        f64 min_x = 0.0;
        f64 min_z = 0.0;
        f64 max_x = 0.0;
        f64 max_z = 0.0;

        [[nodiscard]] bool overlaps(const WaterBounds& bounds) const noexcept {
            return bounds.max_x > min_x && bounds.min_x < max_x && bounds.max_z > min_z &&
                   bounds.min_z < max_z;
        }
    };

    [[nodiscard]] Status bind_cell(world::CellId cell, WaterStreamingReport& report) noexcept;
    /// The segments one body contributes to one cell's footprint. Separate from `bind_cell()`
    /// because "which segments does this body reach into" is the arithmetic a reader checks, and it
    /// should be readable without the loop over bodies around it.
    [[nodiscard]] Status bind_body(const WaterBodyRecord& record, const Footprint& footprint,
                                   Binding& binding, WaterStreamingReport& report) noexcept;
    [[nodiscard]] Status release_cell(world::CellId cell, WaterStreamingReport& report) noexcept;
    [[nodiscard]] Status reference_segment(const WaterSegmentKey& key,
                                           WaterStreamingReport& report) noexcept;
    void release_segment(const WaterSegmentKey& key, WaterStreamingReport& report) noexcept;
    [[nodiscard]] Binding* find_binding(world::CellId cell) noexcept;
    [[nodiscard]] usize find_segment(const WaterSegmentKey& key) const noexcept;

    Allocator* allocator_;
    const WaterRegistry* registry_;
    const world::PartitionConfig* partition_;
    world::CellEventQueue* events_ = nullptr;
    world::CellEventQueue::ConsumerId consumer_ = 0;
    world::WorldProfile profile_ = world::WorldProfile::Client;

    Array<WaterSegment> segments_;
    Array<Binding> bindings_;
    HashMap<world::CellId, world::CellCoord> cells_;
    Array<world::CellEvent> drained_;
};

}  // namespace cy::water
