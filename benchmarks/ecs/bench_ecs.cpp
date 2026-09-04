// The ECS's per-entity costs, as numbers a threshold defends. Task 1.6.
//
// M2 closed with the bulk-copy claim checked as a *time-bounded correctness test*:
// `integration.ecs_scale` instantiates 100 000 rows and reads them back inside the integration
// suite's per-case budget. That catches a gross regression and nothing finer — the spike's figures,
// 3.7 ns/entity for a block activation against 11.2 for a spawn into partly full chunks, lived in
// prose. `tools/roadmap/milestones/m2.toml` records the gap and names the fix; this is it.
//
// WHAT EACH BENCHMARK MEASURES IS ONE ENTITY. Every body below consumes exactly
// CY_BENCH_ITERATIONS entities' worth of work, so the runner's ns/op *is* ns/entity and can be read
// against the spike's numbers directly rather than divided by a batch size the reader has to know.
//
// THE FIXTURE IS BUILT ONCE. The runner calls a body several times at growing iteration counts to
// find one that takes 20 ms, then five more times to take the minimum; a world built inside the
// body would put its construction into every one of those samples and measure the allocator. The
// two bodies that must create and destroy do so in steady state — the same number of entities
// exists before and after — so repeating them neither grows the world nor changes what they cost.

#include <cy/bench/bench.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/command_buffer.h>
#include <cy/ecs/query.h>
#include <cy/ecs/world.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using cy::ecs::ComponentTypeId;
using cy::ecs::Entity;

struct Position {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    cy::f32 z = 0.0F;
};

struct Velocity {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    cy::f32 z = 0.0F;
};

/// A tag, so that the command-buffer benchmark measures a real archetype transition rather than a
/// value write. Adding and removing one moves the entity between two archetypes, which is the cost
/// `ecs-core` defers to the flush point in the first place.
struct Marked {};

/// The descriptor shape generated code emits. The identifiers are local to this binary's own
/// `ComponentRegistry` and are never registered into `reflect::default_registry()`, so they are not
/// manifest identifiers; the range matches src/ecs/tests/fixtures.h's for the same reason.
template <class T>
const cy::reflect::TypeInfo& descriptor(const char* name, cy::u32 id) noexcept {
    static cy::reflect::TypeInfo info;
    info.name = name;
    info.id = cy::reflect::TypeId(id);
    info.size = static_cast<cy::u32>(sizeof(T));
    info.alignment = static_cast<cy::u32>(alignof(T));
    info.trivially_relocatable = true;
    return info;
}

/// Stop the run rather than report a number produced by a world that failed to build. A benchmark
/// that swallows an error measures the error path, and does it very quickly.
void require(bool condition, const char* what) noexcept {
    if (!condition) {
        std::fprintf(stderr, "cy_bench_ecs: %s failed; the measurement would be meaningless\n",
                     what);
        std::abort();
    }
}

/// The number of entities every body works over. A hundred thousand is the figure `ecs-core`'s
/// scale scenarios are written at, and at 24 bytes a row the two columns are 2.4 MB — past every
/// level of cache on a normal machine, which is deliberate: an ECS benchmark that fits in L2
/// measures the loop and not the layout.
constexpr cy::u32 kEntities = 100'000;

/// How many entities a bulk body creates or activates per call into the world. Large enough that
/// the per-call overhead is amortised the way a real cell activation amortises it, small enough
/// that the world's size is bounded whatever iteration count the runner settles on.
constexpr cy::u32 kBatch = 4096;

struct Fixture {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Ecs);
    cy::ecs::World world{allocator};
    ComponentTypeId position = cy::ecs::kInvalidComponent;
    ComponentTypeId velocity = cy::ecs::kInvalidComponent;
    ComponentTypeId marked = cy::ecs::kInvalidComponent;
    cy::Array<Entity> entities{allocator};
    /// A permutation of `entities`, so the random-access body defeats the prefetcher without
    /// paying for a modulo or a generator inside the measured loop.
    cy::Array<cy::u32> shuffled{allocator};

    Fixture() {
        require(world.initialize().has_value(), "World::initialize");

        auto position_id = world.components().register_reflected(
            descriptor<Position>("cy::bench::Position", 9001));
        require(position_id.has_value(), "registering Position");
        position = *position_id;

        auto velocity_id = world.components().register_reflected(
            descriptor<Velocity>("cy::bench::Velocity", 9002));
        require(velocity_id.has_value(), "registering Velocity");
        velocity = *velocity_id;

        cy::ecs::ComponentOptions tag;
        tag.kind = cy::ecs::ComponentKind::Tag;
        auto marked_id = world.components().register_reflected(
            descriptor<Marked>("cy::bench::Marked", 9003), tag);
        require(marked_id.has_value(), "registering Marked");
        marked = *marked_id;

        const ComponentTypeId set[] = {position, velocity};
        require(world.create_many(kEntities, cy::Span<const ComponentTypeId>(set, 2), entities)
                    .has_value(),
                "creating the fixture's entities");

        require(shuffled.resize(kEntities).has_value(), "sizing the access order");
        for (cy::u32 i = 0; i < kEntities; ++i) {
            shuffled[i] = i;
        }
        // A fixed-seed xorshift, so the order is the same on every machine and every run: a
        // benchmark whose access pattern varies is a benchmark whose baseline cannot be compared.
        std::uint64_t state = 0x243f'6a88'85a3'08d3ULL;
        for (cy::u32 i = kEntities - 1; i > 0; --i) {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            const auto pick = static_cast<cy::u32>((state * 0x2545'f491'4f6c'dd1dULL) % (i + 1U));
            const cy::u32 held = shuffled[i];
            shuffled[i] = shuffled[pick];
            shuffled[pick] = held;
        }
    }
};

/// Built on first use rather than at static initialisation: a world constructed before main() would
/// run before the allocator's domains are, and the order of static initialisation across
/// translation units is unspecified.
Fixture& fixture() {
    static Fixture instance;
    return instance;
}

}  // namespace

CY_BENCHMARK("ecs/query-iterate",
             "One entity of a two-column query, read and written through the chunk view. This is "
             "the inner loop every system in the frame runs, so a regression is felt by all of "
             "them at once: it means the chunk layout, the query's per-chunk setup, or a column "
             "resolution that stopped being hoisted out of the row loop.") {
    Fixture& state = fixture();
    cy::ecs::QueryDesc desc(state.allocator);
    require(desc.write(state.position).has_value(), "declaring the write");
    require(desc.read(state.velocity).has_value(), "declaring the read");
    cy::ecs::Query query(state.world, std::move(desc));

    const ComponentTypeId position = state.position;
    const ComponentTypeId velocity = state.velocity;
    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    cy::f32 sink = 0.0F;
    while (remaining > 0) {
        require(query
                    .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                        const cy::Span<Position> positions = chunk.write<Position>(position);
                        const cy::Span<const Velocity> velocities = chunk.read<Velocity>(velocity);
                        const auto rows = static_cast<cy::u32>(
                            remaining < chunk.count() ? remaining : chunk.count());
                        for (cy::u32 row = 0; row < rows; ++row) {
                            positions[row].x += velocities[row].x + 1.0F;
                            sink += positions[row].x;
                        }
                        remaining -= rows;
                    })
                    .has_value(),
                "iterating the query");
    }
    CY_BENCH_KEEP(sink);
}

CY_BENCHMARK("ecs/random-access",
             "One `World::get` for a component of an entity chosen in a fixed shuffled order — the "
             "entity-table lookup and the chunk's column binary search, with the prefetcher "
             "defeated. It is what a gameplay system pays when it follows a reference instead of "
             "iterating, so a regression means the entity table, the archetype's column index, or "
             "a location lookup that grew an indirection.") {
    Fixture& state = fixture();
    const ComponentTypeId position = state.position;
    cy::f32 sink = 0.0F;
    const auto count = static_cast<cy::u32>(state.entities.size());
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        const auto index = state.shuffled[static_cast<cy::u32>(i % count)];
        const auto* value = state.world.get<Position>(state.entities[index], position);
        sink += (value == nullptr) ? 0.0F : value->x;
    }
    CY_BENCH_KEEP(sink);
}

CY_BENCHMARK("ecs/create-many",
             "One entity through the bulk spawn round trip: created by create_many and destroyed "
             "by destroy_many, in batches of 4096 over a world that already holds a hundred "
             "thousand. It is the round trip and not the spawn alone, because a body must leave "
             "the world as it found it or the runner's repetitions each measure a different world "
             "— so it is NOT directly comparable to the M2 spike's 11.2 ns/entity for a spawn into "
             "partly full chunks. A regression means the chunk allocator, the entity table's "
             "recycling, or a per-entity step that crept back into a path that is supposed to "
             "resolve the component set once for the whole batch.") {
    Fixture& state = fixture();
    const ComponentTypeId set[] = {state.position, state.velocity};
    const cy::Span<const ComponentTypeId> components(set, 2);
    cy::Array<Entity> created(state.allocator);

    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    while (remaining > 0) {
        const auto batch = static_cast<cy::u32>(remaining < kBatch ? remaining : kBatch);
        created.clear();
        require(state.world.create_many(batch, components, created).has_value(), "create_many");
        require(state.world.destroy_many(created.span()).has_value(), "destroy_many");
        remaining -= batch;
    }
    CY_BENCH_KEEP(created.size());
}

CY_BENCHMARK(
    "ecs/instantiate-block",
    "One entity through the streaming round trip: activated from a cooked archetype "
    "block — one memcpy per column per chunk — and destroyed again, for the same "
    "steady-state reason as ecs/create-many, so the number is the round trip rather than "
    "the 3.7 ns/entity the M2 spike measured for the activation alone. What it defends is "
    "the shape: this body and ecs/create-many differ only in how the entities arrive, so "
    "an activation that stopped being a bulk copy and became a loop over rows shows up as "
    "this benchmark converging on that one. `serialization-and-prefabs` designed the block "
    "format to prevent exactly that, and it is felt as a hitch when a cell crosses the "
    "streaming boundary rather than as a lower frame rate.") {
    Fixture& state = fixture();
    cy::Array<Position> positions(state.allocator);
    require(positions.resize(kBatch).has_value(), "sizing the block's column");
    for (cy::u32 row = 0; row < kBatch; ++row) {
        positions[row].x = static_cast<cy::f32>(row);
    }

    const ComponentTypeId components[] = {state.position};
    const void* columns[] = {static_cast<const void*>(positions.data())};
    cy::Array<Entity> created(state.allocator);

    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    while (remaining > 0) {
        const auto rows = static_cast<cy::u32>(remaining < kBatch ? remaining : kBatch);
        cy::ecs::World::ArchetypeBlock block;
        block.components = cy::Span<const ComponentTypeId>(components, 1);
        block.columns = cy::Span<const void* const>(columns, 1);
        block.count = rows;

        created.clear();
        require(state.world.instantiate(block, created).has_value(), "instantiate");
        require(state.world.destroy_many(created.span()).has_value(), "destroy_many");
        remaining -= rows;
    }
    CY_BENCH_KEEP(created.size());
}

CY_BENCHMARK(
    "ecs/command-buffer-flush",
    "One deferred archetype transition: a tag recorded into a command buffer and applied "
    "at the flush point, in batches of 4096. Structural change is deferred for "
    "correctness rather than for speed — a system that mutated a chunk it was iterating "
    "would be reading freed memory — so this measures the price of the mechanism nothing "
    "can opt out of. A regression means the buffer's record path, the placeholder "
    "resolution, or a flush that stopped batching entities moving to the same archetype.") {
    Fixture& state = fixture();
    cy::ecs::CommandBuffer commands(state.world);
    require(state.world.attach(commands).has_value(), "attaching the command buffer");

    const auto count = static_cast<cy::u32>(state.entities.size());
    cy::u32 cursor = 0;
    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    while (remaining > 0) {
        // Each entity is marked and unmarked within one batch, so the world is in the same shape
        // after the body as before it however many times the runner repeats it.
        const auto batch = static_cast<cy::u32>(remaining < kBatch ? remaining : kBatch);
        for (cy::u32 i = 0; i < batch; ++i) {
            const Entity entity = state.entities[(cursor + i) % count];
            require(commands.add(entity, state.marked).has_value(), "recording add");
        }
        require(state.world.flush().has_value(), "flushing the marks");
        for (cy::u32 i = 0; i < batch; ++i) {
            const Entity entity = state.entities[(cursor + i) % count];
            require(commands.remove(entity, state.marked).has_value(), "recording remove");
        }
        require(state.world.flush().has_value(), "flushing the unmarks");
        cursor = (cursor + batch) % count;
        remaining -= batch;
    }
    state.world.detach(commands);
    CY_BENCH_KEEP(cursor);
}
