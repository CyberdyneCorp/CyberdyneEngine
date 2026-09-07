#include "run.h"

#include <cy/core/assets/hash.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/serialize/value_record.h>
#include <cy/save/overlay.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace cy::sample::openworld {
namespace {

using render::vt::FormatClass;
using render::vt::TileCacheDesc;
using render::vt::VirtualAddress;
using render::vt::VirtualTextureDesc;

/// The two virtual textures the world is surfaced with. Terrain covers the ground everywhere;
/// structures covers what stands on it. Two rather than one because the mip tail is per texture and
/// a run that registered one would not show that.
constexpr u32 kTerrainTexture = 1;
constexpr u32 kStructureTexture = 2;
constexpr u32 kBytesPerTile = 8192;  // 128x128 BC1 with a border, near enough
constexpr u32 kCacheTiles = 192;
constexpr u32 kTerrainTilesAtMip0 = 32;
constexpr u32 kStructureTilesAtMip0 = 16;
/// The neighbourhood of cells whose surfaces are asked for each tick. Seven by seven at 128 m cells
/// is a little under a kilometre — more than the streaming radius, because a surface is asked for
/// before its cell is walked into.
constexpr i32 kFeedbackRadiusCells = 3;

/// The route: a fixed diagonal that passes every landmark the content declares.
constexpr f64 kRouteStartX = 576.0;  // cell (4, 3), the north gate
constexpr f64 kRouteStartZ = 448.0;
constexpr f64 kRouteEndX = 5568.0;  // cell (43, 41), the south yard
constexpr f64 kRouteEndZ = 5312.0;

/// The route's length in metres. A constant of the content, so a partial run can say how far along
/// it actually got.
[[nodiscard]] f64 route_length_metres() noexcept {
    const f64 dx = kRouteEndX - kRouteStartX;
    const f64 dz = kRouteEndZ - kRouteStartZ;
    return std::sqrt((dx * dx) + (dz * dz));
}

/// What the streaming budget is, and it is deliberately smaller than the world.
///
/// THE THREE NUMBERS MEASURE THREE DIFFERENT THINGS, and confusing them is how a budget comes to
/// bound nothing:
///
///   io_bytes_per_tick     the CELL PAYLOADS — geometry, textures, physics, navigation — which are
///                         where a cell's bulk is. The route below reads tens of megabytes through
///                         it while never holding more than a fraction of the world.
///   entity_memory_bytes   the STAGED ECS ROWS ONLY, which is what `CellActivation::staged_bytes()`
///                         counts: 41 rows of 36 bytes is under two kilobytes per cell. Sixty-four
///                         kilobytes is therefore about thirty-five cells' worth — a few per cent
///                         of the world — so cells the traveller has left are reclaimed
///                         continuously rather than accumulating behind it. A budget of megabytes
///                         here would bound nothing and the route would never evict, which is what
///                         the first draft of this sample measured and reported as success.
///   activation_time       MODELLED work, spent against the cost model rather than a clock, so the
///                         same route prepares the same cells on the same tick on two machines.
[[nodiscard]] world::StreamingBudget route_budget() noexcept {
    world::StreamingBudget budget;
    budget.io_bytes_per_tick = 4ULL * 1024 * 1024;
    budget.entity_memory_bytes = 64ULL * 1024;
    budget.activation_time_per_tick = 2'000'000;  // 2 ms of MODELLED work
    return budget;
}

/// This thread's own CPU time in nanoseconds, or the monotonic clock where there is no per-thread
/// one. Per-thread rather than wall clock, for the reason tests/harness/src/budget.cpp records: a
/// 0.2 ms case stretched to 4.1 ms of wall clock under twenty-four spinning threads, and a
/// measurement that changes with what else the machine is doing is a measurement of the machine.
[[nodiscard]] f64 cpu_now_ns() noexcept {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0) {
        return (static_cast<f64>(now.tv_sec) * 1e9) + static_cast<f64>(now.tv_nsec);
    }
#endif
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<f64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

[[nodiscard]] f64 percentile(Array<f64>& samples, f64 fraction) noexcept {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.data(), samples.data() + samples.size());
    const auto index = static_cast<usize>(fraction * static_cast<f64>(samples.size() - 1));
    return samples[index];
}

/// The producer behind both textures: it fills a page with its own mip level, so a tile's bytes say
/// which page they came from. A cooked project's producer reads from the package instead; this one
/// is here because the artefact is about the paging and not about the pixels.
class SurfaceProducer final : public render::vt::PageProducer {
public:
    [[nodiscard]] const char* producer_name() const noexcept override { return "open-world"; }
    [[nodiscard]] f32 declared_cost_ms() const noexcept override { return 0.2F; }
    [[nodiscard]] render::vt::PersistenceClass persistence() const noexcept override {
        return render::vt::PersistenceClass::Derived;
    }

    Status produce(const render::vt::ProductionRequest& request) noexcept override {
        std::memset(request.destination, static_cast<int>(request.address.mip), request.bytes);
        return ok();
    }
};

SurfaceProducer& surface_producer() noexcept {
    static SurfaceProducer producer;
    return producer;
}

[[nodiscard]] VirtualTextureDesc texture_desc(u32 id, u32 width, u8 mips, u8 tail) noexcept {
    VirtualTextureDesc desc;
    desc.id = id;
    desc.width = width;
    desc.height = width;
    desc.tile_size = 128;
    desc.border = 4;
    desc.mip_count = mips;
    desc.layers = 1;
    desc.mip_tail_levels = tail;
    desc.semantic = render::vt::TextureSemantic::Colour;
    desc.model = render::vt::ResidencyModel::VirtualStreamed;
    desc.bytes_per_tile = kBytesPerTile;
    return desc;
}

[[nodiscard]] VirtualAddress address_of(u32 texture, u32 tiles_at_mip0, i32 x, i32 z,
                                        u8 mip) noexcept {
    const i32 tiles = static_cast<i32>(tiles_at_mip0 >> mip);
    const i32 wrapped_x = ((x % tiles) + tiles) % tiles;
    const i32 wrapped_z = ((z % tiles) + tiles) % tiles;
    VirtualAddress address;
    address.texture = texture;
    address.mip = mip;
    address.tile_x = static_cast<u16>(wrapped_x);
    address.tile_y = static_cast<u16>(wrapped_z);
    return address;
}

/// The residency policy virtual texturing competes under. One subsystem, because this sample pages
/// one kind of thing; the point of `residency` is that a second one would join the same policy.
[[nodiscard]] residency::SubsystemPolicy texture_policy() noexcept {
    residency::SubsystemPolicy policy;
    policy.domain = MemoryDomain::Gpu;
    policy.budget_bytes = static_cast<u64>(kCacheTiles) * kBytesPerTile;
    policy.budget_kind = BudgetKind::Hard;
    policy.min_residency_frames = 2;
    return policy;
}

// --- What a run records about itself, as a save fragment
// ------------------------------------------

/// The session's own bookkeeping: where the run got to, and what content it was playing.
///
/// A `Scope::Session` fragment rather than a file beside the save, because "the save is the
/// overlay" and a number a resumed run needs is part of the state being saved.
struct RunState {
    u32 tick = 0;
    u32 landmarks_passed = 0;
    u32 entities_removed = 0;
    u32 entities_modified = 0;
    u64 content_digest = 0;
};

constexpr u32 kRunStateTypeId = 9605;

[[nodiscard]] reflect::FieldInfo state_field(const char* name, u32 id, reflect::FieldKind kind,
                                             u32 offset, u32 size) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes.declared = reflect::AttributeKind::Persistence;
    field.attributes.persistence = reflect::PersistenceKind::PersistentState;
    return field;
}

[[nodiscard]] const reflect::TypeInfo& run_state_type() noexcept {
    using reflect::FieldKind;
    static const reflect::FieldInfo fields[] = {
        state_field("tick", 1, FieldKind::U32, static_cast<u32>(offsetof(RunState, tick)),
                    sizeof(u32)),
        state_field("landmarks_passed", 2, FieldKind::U32,
                    static_cast<u32>(offsetof(RunState, landmarks_passed)), sizeof(u32)),
        state_field("entities_removed", 3, FieldKind::U32,
                    static_cast<u32>(offsetof(RunState, entities_removed)), sizeof(u32)),
        state_field("entities_modified", 4, FieldKind::U32,
                    static_cast<u32>(offsetof(RunState, entities_modified)), sizeof(u32)),
        state_field("content_digest", 5, FieldKind::U64,
                    static_cast<u32>(offsetof(RunState, content_digest)), sizeof(u64)),
    };
    static reflect::TypeInfo info;
    info.name = "cy::sample::openworld::RunState";
    info.id = reflect::TypeId(kRunStateTypeId);
    info.size = static_cast<u32>(sizeof(RunState));
    info.alignment = static_cast<u32>(alignof(RunState));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

[[nodiscard]] assets::ContentHash content_version_of(u64 digest) noexcept {
    return assets::content_hash(&digest, sizeof(digest));
}

}  // namespace

Session::Session(Allocator& allocator, const WorldContent& content) noexcept
    : allocator_(&allocator),
      content_(&content),
      ecs_(allocator),
      grid_(content.partition()),
      overlay_(allocator),
      streaming_(allocator, grid_, ecs_),
      residency_(allocator),
      texturing_(allocator) {}

Session::~Session() {
    stop();
}

void Session::stop() noexcept {
    // ORDER, AND IT IS THE POINT. The texture system quiesces its production workers before a
    // single staging byte is freed; the streaming withdraws every published cell from the ECS world
    // before the world is touched. Doing either the other way round is the defect M5.5's gate found
    // in a different module, one run in forty.
    texturing_.reset();
    streaming_.shutdown();
    started_ = false;
}

Status Session::start(const RouteOptions& options) noexcept {
    if (Status initialised = ecs_.initialize(); !initialised) {
        return initialised;
    }
    const Expected<Components, Error> components = register_components(ecs_);
    if (!components) {
        return make_unexpected(components.error());
    }
    components_ = *components;
    streaming_.set_overlay(&overlay_);
    overlay_.set_content_version(content_->digest());

    if (Status cooked = cook_world(); !cooked) {
        return cooked;
    }
    if (Status configured = configure_texturing(options); !configured) {
        return configured;
    }

    world::StreamingSource traveller;
    traveller.position = along_route(options, 0);
    traveller.radius = options.radius;
    traveller.prediction_horizon = 2.0F;
    traveller.activates = true;
    traveller.klass = world::RequestClass::Gameplay;
    traveller.importance = 1.0F;
    const Expected<world::SourceId, Error> source = streaming_.sources().add(traveller);
    if (!source) {
        return make_unexpected(source.error());
    }
    traveller_ = *source;
    started_ = true;
    return ok();
}

Status Session::cook_world() noexcept {
    for (i32 z = 0; z < content_->extent(); ++z) {
        for (i32 x = 0; x < content_->extent(); ++x) {
            world::CookedCell cell =
                cook_cell(*allocator_, *content_, grid_, world::CellCoord{x, 0, z, 0}, components_);
            if (Status added = streaming_.add_cell(std::move(cell)); !added) {
                return added;
            }
        }
    }
    return ok();
}

Status Session::configure_texturing(const RouteOptions& options) noexcept {
    TileCacheDesc cache;
    cache.format = FormatClass::BlockColour;
    cache.tile_size = 128;
    cache.border = 4;
    cache.bytes_per_tile = kBytesPerTile;
    cache.tile_capacity = kCacheTiles;
    if (Status configured = texturing_.configure_cache(cache); !configured) {
        return configured;
    }
    if (Status registered =
            residency_.register_subsystem(residency::Subsystem::Texture, texture_policy());
        !registered) {
        return registered;
    }

    const VirtualTextureDesc textures[] = {
        texture_desc(kTerrainTexture, 4096, 6, 3),
        texture_desc(kStructureTexture, 2048, 5, 2),
    };
    for (const VirtualTextureDesc& desc : textures) {
        if (Status registered = texturing_.register_texture(desc); !registered) {
            return registered;
        }
        if (Status attached = texturing_.producers().register_producer(desc.id, surface_producer());
            !attached) {
            return attached;
        }
        // THE MIP TAIL, BEFORE ANY SAMPLE. It is what makes `sample()` unable to answer "missing",
        // and a run that skipped it would report holes that are a configuration error rather than a
        // streaming state.
        if (Status pinned = texturing_.make_mip_tail_resident(desc.id); !pinned) {
            return pinned;
        }
    }
    if (Status configured = texturing_.feedback().configure(4096); !configured) {
        return configured;
    }
    return texturing_.start_production(options.production_workers);
}

world::WorldPosition Session::along_route(const RouteOptions& options, u32 tick) const noexcept {
    // FIXED IN SPACE AND FIXED IN SPEED. `--ticks` decides how far along the route a run gets,
    // never how fast the traveller moves: a short run and a long one must traverse the same cells
    // in the same order at the same speed, or their reports are not comparable and the shorter one
    // is measuring a teleport rather than a traversal.
    const f64 metres = static_cast<f64>(tick) * static_cast<f64>(options.metres_per_tick);
    const f64 travelled = std::min(1.0, metres / route_length_metres());
    const world::WorldVec3d point{kRouteStartX + ((kRouteEndX - kRouteStartX) * travelled), 0.0,
                                  kRouteStartZ + ((kRouteEndZ - kRouteStartZ) * travelled)};
    return world::from_absolute(content_->partition(), point, 0);
}

void Session::page_textures(const RouteOptions& options, world::CellCoord here, u32 tick) noexcept {
    (void)options;
    // ONE FEEDBACK RECORD PER VISIBLE CELL, and the level asked for falls off with distance. This
    // is the shape a GPU feedback buffer's resolve produces; `FeedbackBuffer::record` is wait-free
    // and drops rather than waiting, which is the half of the exit criterion that says feedback
    // never blocks a frame.
    for (i32 dz = -kFeedbackRadiusCells; dz <= kFeedbackRadiusCells; ++dz) {
        for (i32 dx = -kFeedbackRadiusCells; dx <= kFeedbackRadiusCells; ++dx) {
            const i32 distance = std::max(dx < 0 ? -dx : dx, dz < 0 ? -dz : dz);
            const auto mip = static_cast<u8>(std::min(2, distance / 2));
            texturing_.feedback().record(
                address_of(kTerrainTexture, kTerrainTilesAtMip0, here.x + dx, here.z + dz, mip)
                    .encode());
        }
    }
    texturing_.feedback().record(
        address_of(kStructureTexture, kStructureTilesAtMip0, here.x, here.z, 0).encode());

    const f64 now = static_cast<f64>(tick) / 60.0;
    (void)texturing_.submit_feedback_requests(residency_, now);
    residency::Schedule schedule;
    if (residency_.schedule(residency::ScheduleOptions{now, 8}, schedule)) {
        (void)texturing_.apply(schedule, residency_, now);
    }
    (void)texturing_.collect_production(residency_, now);
    residency_.end_frame(now);
    texturing_.end_frame();
}

Status Session::pass_landmark(const Landmark& landmark, Telemetry& telemetry) noexcept {
    // WHAT A PLAYER DID, RECORDED IN THE OVERLAY AND NOWHERE ELSE. `world-partition-and-streaming`
    // requires cooked cells to stay immutable and the change to be applied during the next
    // activation, so nothing here reaches into the live ECS world: a destroyed landmark must never
    // briefly exist after a reload, and correcting it after instantiation is what that forbids.
    const world::CellId cell = grid_.id_of(world::CellCoord{landmark.x, 0, landmark.z, 0});
    const world::PersistentId landmark_id =
        identity_of(*content_, landmark.x, landmark.z, content_->props_per_cell());
    if (Status removed = overlay_.record_removed(cell, landmark_id); !removed) {
        return removed;
    }
    ++telemetry.entities_removed;

    for (u32 row = 0; row < 2; ++row) {
        const world::PersistentId id = identity_of(*content_, landmark.x, landmark.z, row);
        const Structure damaged{landmark.kind, 100U - (17U * (row + 1U))};
        const Span<const u8> bytes{reinterpret_cast<const u8*>(&damaged), sizeof(Structure)};
        if (Status recorded = overlay_.record_component(cell, id, components_.structure, bytes);
            !recorded) {
            return recorded;
        }
        ++telemetry.entities_modified;
    }
    ++telemetry.landmarks_passed;
    return ok();
}

u32 Session::count_resident_not_activated() const noexcept {
    return streaming_.stats().by_state[static_cast<u32>(world::CellState::Resident)];
}

Status Session::traverse(const RouteOptions& options, Telemetry& out) noexcept {
    if (!started_) {
        return make_unexpected(Error{ErrorCode::Internal, "the session was not started", 0});
    }
    const world::StreamingBudget budget = route_budget();
    const Nanoseconds modelled_ceiling =
        budget.activation_time_per_tick +
        world::estimate_activation_time(content_->props_per_cell() + 1, 0);

    Array<f64> ticks(*allocator_);
    if (Status reserved = ticks.reserve(options.ticks); !reserved) {
        return reserved;
    }

    const u32 last =
        (options.abort_at != 0) ? std::min(options.abort_at, options.ticks) : options.ticks;
    for (u32 tick = 0; tick < last; ++tick) {
        const world::WorldPosition here = along_route(options, tick);
        const world::WorldPosition next = along_route(options, tick + 1);
        const world::WorldVec3d from = world::to_absolute(content_->partition(), here);
        const world::WorldVec3d to = world::to_absolute(content_->partition(), next);
        const Vec3 velocity{static_cast<f32>((to.x - from.x) * 60.0), 0.0F,
                            static_cast<f32>((to.z - from.z) * 60.0)};
        (void)streaming_.sources().move_to(traveller_, here, velocity);

        const f64 started = cpu_now_ns();
        const Expected<world::TickReport, Error> report = streaming_.tick(budget);
        if (!report) {
            return make_unexpected(report.error());
        }
        page_textures(options, here.cell, tick);
        const f64 finished = cpu_now_ns();
        (void)ticks.push_back((finished - started) / 1000.0);

        out.cells_activated += report->cells_activated;
        out.cells_deactivated += report->cells_deactivated;
        out.cells_evicted += report->cells_evicted;
        out.cells_made_resident += report->cells_made_resident;
        out.io_bytes += report->io_bytes_spent;
        out.published_peak = std::max(out.published_peak, streaming_.stats().published_entities);
        out.worst_modelled_tick = std::max(out.worst_modelled_tick, report->activation_time_spent);
        out.worst_cost_divergence =
            std::max(out.worst_cost_divergence, report->worst_cost_divergence);
        out.ticks_over_budget += (report->activation_time_spent > modelled_ceiling) ? 1U : 0U;
        out.memory_shortfall_peak = std::max(out.memory_shortfall_peak, report->memory_shortfall);

        if (const Landmark* landmark = content_->landmark_at(here.cell.x, here.cell.z);
            landmark != nullptr) {
            const auto index = static_cast<u64>(landmark - content_->landmarks().data());
            if ((landmark_seen_ & (1ULL << index)) == 0) {
                landmark_seen_ |= 1ULL << index;
                if (Status passed = pass_landmark(*landmark, out); !passed) {
                    return passed;
                }
                std::printf("    landmark  %-12s cell %d,%d  kind %u\n", landmark->name,
                            landmark->x, landmark->z, landmark->kind);
            }
        }
    }

    out.ticks = last;
    out.metres_travelled = std::min(
        route_length_metres(), static_cast<f64>(last) * static_cast<f64>(options.metres_per_tick));
    out.metres_per_second =
        (last == 0) ? 0.0 : out.metres_travelled / (static_cast<f64>(last) / 60.0);
    out.median_tick_us = percentile(ticks, 0.5);
    out.p99_tick_us = percentile(ticks, 0.99);
    out.worst_tick_us = percentile(ticks, 1.0);
    for (const f64 sample : ticks.span()) {
        out.hitches += (sample > options.hitch_threshold_ms * 1000.0) ? 1U : 0U;
    }

    const render::vt::VirtualTextureStats pages = texturing_.stats();
    out.feedback_recorded = pages.feedback_recorded;
    out.feedback_dropped = pages.feedback_dropped;
    out.page_requests = pages.requests_submitted;
    out.pages_produced = pages.pages_produced;
    out.page_evictions = pages.evictions_applied;
    out.resident_tiles_peak = pages.resident_tiles;
    out.missing_samples = pages.missing_samples;
    out.fallback_rate = pages.fallback_rate();
    out.overlay_cells = static_cast<u32>(overlay_.cell_count());
    out.resident_not_activated = probe_prefetch();
    return ok();
}

u32 Session::probe_prefetch() noexcept {
    // "RESIDENCY AND ACTIVATION ARE SEPARATE AXES", reached from the gameplay API rather than from
    // a test: `WorldStreaming::prefetch` registers a source with `activates` false, so the cells it
    // names come resident — their bytes are in memory, their assets are paid for — and nothing
    // about them is published into the ECS world. The corner asked for here is one the route never
    // approaches, so what the count reports cannot be a cell on its way to being activated.
    const world::PartitionConfig config = content_->partition();
    const f64 metres = static_cast<f64>(content_->cell_size());
    const world::WorldVec3d corner{(static_cast<f64>(content_->extent()) - 4.5) * metres, 0.0,
                                   4.5 * metres};
    const world::ChannelMask channels = world::ChannelMask::of(world::Channel::Geometry) |
                                        world::ChannelMask::of(world::Channel::Textures);
    const Expected<world::SourceId, Error> source = streaming_.prefetch(
        world::from_absolute(config, corner, 0), 200.0F, world::RequestClass::Background, channels);
    if (!source) {
        return 0;
    }
    const world::StreamingBudget budget = route_budget();
    for (u32 tick = 0; tick < 120; ++tick) {
        if (!streaming_.tick(budget)) {
            break;
        }
    }
    return count_resident_not_activated();
}

// --- The save, and the conversion this sample pays for
// --------------------------------------------
//
// THERE ARE TWO OVERLAY MODELS IN THIS TREE AND THIS IS WHERE A PROGRAM MEETS BOTH.
// `world::PersistenceOverlay` is what the world maintains — keyed by `CellId`, holding raw
// component bytes addressed by the runtime's dense `ecs::ComponentTypeId`. `save::Overlay` is what
// a save is — keyed by `RegionKey`, holding per-field `serialize::ValueRecord`s addressed by
// `reflect::TypeId`. Both headers correctly cite the same requirement.
//
// The two functions below are the conversion, and they are the honest cost of that duplication:
// this sample knows that the only component it ever overrides is `Structure`, so it can name the
// descriptor. A game with a hundred component types could not, and would need the world's overlay
// to carry `reflect::TypeInfo` at the point of recording — which is what src/save/README.md's
// recommended resolution says. Recorded here rather than hidden, because an artefact that quietly
// papered over it would be the wrong kind of evidence.

Status Session::to_save_overlay(save::Overlay& out) const noexcept {
    Array<world::CellId> cells(*allocator_);
    if (Status listed = overlay_.cells(cells); !listed) {
        return listed;
    }
    for (const world::CellId cell : cells.span()) {
        const world::CellOverlay* entry = overlay_.find(cell);
        if (entry == nullptr) {
            continue;
        }
        const save::RegionKey region{cell.value};
        for (const world::PersistentId removed : entry->removed.span()) {
            if (Status destroyed = out.destroy_entity(region, save::PersistentId{0, removed.value});
                !destroyed) {
                return destroyed;
            }
        }
        for (const world::ComponentOverride& change : entry->overrides.span()) {
            const Span<const u8> bytes =
                overlay_.component_override(cell, change.entity, change.component);
            if (bytes.size() != sizeof(Structure) || change.component != components_.structure) {
                return make_unexpected(
                    Error{ErrorCode::Internal, "an override this sample cannot describe", 0});
            }
            Structure structure;
            std::memcpy(&structure, bytes.data(), sizeof(Structure));
            if (Status recorded =
                    out.record_component(region, save::PersistentId{0, change.entity.value},
                                         structure_type(), &structure, 1);
                !recorded) {
                return recorded;
            }
        }
    }
    return ok();
}

Status Session::from_save_overlay(const save::Overlay& saved, ResumeReport& report) noexcept {
    reflect::FieldIndex fields;
    if (Status built = fields.build(structure_type()); !built) {
        return built;
    }
    for (const save::Region& region : saved.regions()) {
        const world::CellId cell{region.key.value()};
        ++report.regions;
        for (const save::Entry& entry : region.entries.span()) {
            const world::PersistentId id{entry.id.low()};
            if (entry.kind == save::EntryKind::Tombstone) {
                if (Status removed = overlay_.record_removed(cell, id); !removed) {
                    return removed;
                }
                continue;
            }
            for (const save::ComponentDelta& delta : entry.components.span()) {
                if (delta.type != structure_type().id) {
                    continue;
                }
                Structure structure;
                if (Status applied = serialize::record_to_object(delta.record, fields, &structure);
                    !applied) {
                    return applied;
                }
                const Span<const u8> bytes{reinterpret_cast<const u8*>(&structure),
                                           sizeof(Structure)};
                if (Status recorded =
                        overlay_.record_component(cell, id, components_.structure, bytes);
                    !recorded) {
                    return recorded;
                }
            }
        }
    }
    return ok();
}

Status Session::verify_region(const save::Region& region, ResumeReport& report) noexcept {
    reflect::FieldIndex fields;
    if (Status built = fields.build(structure_type()); !built) {
        return built;
    }
    const world::CellId cell{region.key.value()};
    const Span<const ecs::Entity> live = streaming_.entities_of(cell);

    for (const save::Entry& entry : region.entries.span()) {
        ecs::Entity found;
        bool present = false;
        for (const ecs::Entity candidate : live) {
            const auto* ident = ecs_.get<Ident>(candidate, components_.ident);
            if (ident != nullptr && ident->value == entry.id.low()) {
                found = candidate;
                present = true;
                break;
            }
        }

        if (entry.kind == save::EntryKind::Tombstone) {
            // "A destroyed building must never briefly exist": the overlay was applied during
            // activation, so the row was never published rather than published and corrected.
            report.removals_honoured += present ? 0U : 1U;
            report.mismatches += present ? 1U : 0U;
            continue;
        }
        const Structure* live_value =
            present ? ecs_.get<Structure>(found, components_.structure) : nullptr;
        for (const save::ComponentDelta& delta : entry.components.span()) {
            if (delta.type != structure_type().id) {
                continue;
            }
            Structure expected;
            if (Status applied = serialize::record_to_object(delta.record, fields, &expected);
                !applied) {
                return applied;
            }
            const bool matches = live_value != nullptr &&
                                 live_value->material == expected.material &&
                                 live_value->integrity == expected.integrity;
            report.overrides_matched += matches ? 1U : 0U;
            report.mismatches += matches ? 0U : 1U;
        }
    }
    return ok();
}

Expected<u32, Error> Session::checkpoint(const char* directory,
                                         const Telemetry& telemetry) noexcept {
    save::Overlay overlay(*allocator_);
    if (Status converted = to_save_overlay(overlay); !converted) {
        return make_unexpected(converted.error());
    }
    RunState state;
    state.tick = telemetry.ticks;
    state.landmarks_passed = telemetry.landmarks_passed;
    state.entities_removed = telemetry.entities_removed;
    state.entities_modified = telemetry.entities_modified;
    state.content_digest = content_->digest();
    if (Status recorded =
            overlay.record_fragment(save::Scope::Session, run_state_type(), &state, 1);
        !recorded) {
        return make_unexpected(recorded.error());
    }
    overlay.set_content_version(content_version_of(state.content_digest));

    save::FilesystemBackend store;
    const Expected<usize, Error> opened = store.open(directory);
    if (!opened) {
        return make_unexpected(opened.error());
    }
    save::SaveArchive archive(*allocator_);
    if (Status attached = archive.open(store); !attached) {
        return make_unexpected(attached.error());
    }
    save::SaveIdentity identity;
    identity.build_id = "open-world";
    identity.content_version = content_version_of(state.content_digest);
    return archive.commit(overlay, identity);
}

Status Session::resume(const char* directory, ResumeReport& out) noexcept {
    save::FilesystemBackend store;
    const Expected<usize, Error> opened = store.open(directory);
    if (!opened) {
        return make_unexpected(opened.error());
    }
    save::SaveArchive archive(*allocator_);
    if (Status attached = archive.open(store); !attached) {
        return attached;
    }
    save::Overlay saved(*allocator_);
    save::LoadReport load_report;
    save::LoadPolicy policy;
    policy.build_id = "open-world";
    // THE CONTENT VERSION IS REPORTED RATHER THAN ENFORCED HERE, deliberately. A save written
    // before a patch and loaded after it is the interesting case, not an error:
    // `save-and-persistence` requires a mismatch to be DETECTED and reported, and the two digests
    // below are that report.
    if (Status loaded = archive.load(policy, saved, load_report); !loaded) {
        return loaded;
    }
    if (const Expected<u32, Error> generation = archive.active_generation(); generation) {
        out.generation = *generation;
    }

    RunState state;
    if (const save::Fragment* fragment =
            saved.find_fragment(save::Scope::Session, run_state_type().id);
        fragment != nullptr) {
        reflect::FieldIndex fields;
        if (Status built = fields.build(run_state_type()); !built) {
            return built;
        }
        if (Status applied = serialize::record_to_object(fragment->record, fields, &state);
            !applied) {
            return applied;
        }
    }
    out.tick = state.tick;
    out.landmarks_passed = state.landmarks_passed;
    out.saved_content_digest = state.content_digest;
    out.installed_content_digest = content_->digest();

    if (Status applied = from_save_overlay(saved, out); !applied) {
        return applied;
    }

    // ACTIVATE EXACTLY THE CELLS THE SAVE HOLDS STATE FOR. A direct request rather than a route:
    // what is being checked is that the state came back, and settling a camera over each of them in
    // turn would make the check depend on the streaming plan as well as on the save.
    for (const save::Region& region : saved.regions()) {
        if (Status requested = streaming_.request(world::CellId{region.key.value()},
                                                  world::ChannelMask::all(), true);
            !requested) {
            return requested;
        }
    }
    world::StreamingBudget budget = route_budget();
    budget.entity_memory_bytes = 64ULL * 1024 * 1024;
    for (u32 tick = 0; tick < 600; ++tick) {
        if (Expected<world::TickReport, Error> report = streaming_.tick(budget); !report) {
            return make_unexpected(report.error());
        }
    }
    for (const save::Region& region : saved.regions()) {
        if (streaming_.state_of(world::CellId{region.key.value()}) == world::CellState::Activated) {
            ++out.cells_reactivated;
        }
        if (Status verified = verify_region(region, out); !verified) {
            return verified;
        }
    }
    return ok();
}

void print_telemetry(const WorldContent& content, const Telemetry& telemetry) {
    std::printf("\n  the world\n");
    std::printf("    %s: %d x %d cells of %.0f m — %.2f km square, %llu persistent entities\n",
                content.name(), content.extent(), content.extent(),
                static_cast<f64>(content.cell_size()), content.span_metres() / 1000.0,
                static_cast<unsigned long long>(content.entity_count()));
    std::printf("    content digest %016llx\n", static_cast<unsigned long long>(content.digest()));

    std::printf("\n  the route\n");
    std::printf("    %u ticks, %.0f m travelled at %.0f m/s\n", telemetry.ticks,
                telemetry.metres_travelled, telemetry.metres_per_second);
    std::printf("    cells   activated %llu  deactivated %llu  evicted %llu  resident %llu\n",
                static_cast<unsigned long long>(telemetry.cells_activated),
                static_cast<unsigned long long>(telemetry.cells_deactivated),
                static_cast<unsigned long long>(telemetry.cells_evicted),
                static_cast<unsigned long long>(telemetry.cells_made_resident));
    std::printf("    peak    %u entities published of %llu in the world  (%.1f%%)\n",
                telemetry.published_peak, static_cast<unsigned long long>(content.entity_count()),
                100.0 * static_cast<f64>(telemetry.published_peak) /
                    static_cast<f64>(content.entity_count()));
    std::printf("    io      %llu MiB of payload read; peak staged-row shortfall %llu bytes\n",
                static_cast<unsigned long long>(telemetry.io_bytes / (1024ULL * 1024)),
                static_cast<unsigned long long>(telemetry.memory_shortfall_peak));
    std::printf("    tick    median %.1f us  p99 %.1f us  worst %.1f us  hitches %u\n",
                telemetry.median_tick_us, telemetry.p99_tick_us, telemetry.worst_tick_us,
                telemetry.hitches);
    std::printf(
        "    budget  worst modelled tick %llu ns, over budget %u times, "
        "worst cost divergence %.2fx\n",
        static_cast<unsigned long long>(telemetry.worst_modelled_tick), telemetry.ticks_over_budget,
        static_cast<f64>(telemetry.worst_cost_divergence));

    std::printf("\n  the pages\n");
    std::printf("    feedback recorded %llu  dropped %llu  requests %llu\n",
                static_cast<unsigned long long>(telemetry.feedback_recorded),
                static_cast<unsigned long long>(telemetry.feedback_dropped),
                static_cast<unsigned long long>(telemetry.page_requests));
    std::printf("    produced %llu  evicted %llu  resident %llu tiles  fallback %.1f%%\n",
                static_cast<unsigned long long>(telemetry.pages_produced),
                static_cast<unsigned long long>(telemetry.page_evictions),
                static_cast<unsigned long long>(telemetry.resident_tiles_peak),
                telemetry.fallback_rate * 100.0);
    std::printf(
        "    missing samples %llu   (the mip tail's guarantee: this is zero or the tail "
        "was never made resident)\n",
        static_cast<unsigned long long>(telemetry.missing_samples));

    std::printf("\n  residency against activation\n");
    std::printf("    %u cells resident with nothing simulating in them\n",
                telemetry.resident_not_activated);

    std::printf("\n  what the player changed\n");
    std::printf("    landmarks passed %u  entities removed %u  modified %u  in %u cells\n",
                telemetry.landmarks_passed, telemetry.entities_removed, telemetry.entities_modified,
                telemetry.overlay_cells);
}

void print_resume(const ResumeReport& report) {
    std::printf("\n  the save\n");
    std::printf("    generation %u, %u regions, saved at tick %u after %u landmarks\n",
                report.generation, report.regions, report.tick, report.landmarks_passed);
    std::printf("    content    saved %016llx  installed %016llx  %s\n",
                static_cast<unsigned long long>(report.saved_content_digest),
                static_cast<unsigned long long>(report.installed_content_digest),
                report.saved_content_digest == report.installed_content_digest
                    ? "(the same content)"
                    : "(DIFFERENT CONTENT — the save is being replayed against a patched world)");
    std::printf(
        "    reactivated %u cells: %u removals honoured, %u overrides matched, "
        "%u mismatches\n",
        report.cells_reactivated, report.removals_honoured, report.overrides_matched,
        report.mismatches);
}

}  // namespace cy::sample::openworld
