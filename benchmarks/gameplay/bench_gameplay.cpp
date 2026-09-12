// The gameplay framework's per-operation costs, as numbers a threshold defends. M10 task 6.3.
//
// ================================================================================================
// WHY THIS FILE EXISTS: `m9:gameplay-benchmarks`
// ================================================================================================
//
// `gameplay-framework` — "Performance contracts": the framework "SHALL meet these architectural
// targets on a high-end desktop target, and they SHALL be benchmarked rather than asserted". M9's
// closing gate found that `benchmarks/baseline.json` held six entries and every one of them was
// `ecs/*` or `harness/*` — the table was asserted in a specification and measured nowhere — and
// declared `m9:gameplay-benchmarks` with M10 as the rung that closes it. This is that rung.
//
// WHICH ROWS OF THE TABLE ARE HERE, AND WHICH ARE NOT, because a benchmark file that implies it
// covers a table it half covers is worse than one that says so:
//
//   Command submission, 100 000/s without a central lock  →  gameplay/command-record
//   (the per-command cost the simulation pays for one)    →  gameplay/command-commit
//   Tag tests are integer or set operations               →  gameplay/tag-test
//   Ownership, team and tag queries are indexed, not scans →  gameplay/owner-query
//   Batch spawn: 10 000 entities, not 10 000 allocations  →  gameplay/batch-admit
//
//   Active gameplay entities, 100 000     the population every body below is measured over, so it
//                                         is the benchmark's condition rather than its subject
//   Resident simple entities, 1 000 000   architectural, and an ECS property — `ecs/*` measures the
//                                         storage these numbers are about
//   No virtual dispatch per entity        structural. `tests/test_framework_core.cpp` asserts it by
//                                         construction; a benchmark cannot distinguish "no virtual
//                                         call" from "a virtual call the predictor got right"
//
// AND THE FRACTION OF FRAME TIME. "The framework's own cost SHALL be a reported fraction of
// simulation frame time." `gameplay/command-commit` and `gameplay/batch-admit` are the framework's
// per-entity-per-tick cost and `ecs/query-iterate` is the simulation's inner loop over the same
// entity, all three as ratios against the same calibration workload in the same committed
// `benchmarks/baseline.json` — so the fraction is a division a reader can do on the file, and a
// regression in it is attributable to whichever of the three moved. That is the reported fraction
// at benchmark scale; attributing a *running* frame is a profiler zone rather than a benchmark, and
// benchmarks/README.md says so.
//
// WHAT EACH BODY MEASURES IS ONE COMMAND, OR ONE ENTITY, for the reason bench_ecs.cpp gives: the
// runner's ns/op is then directly the number the requirement is written in, rather than one the
// reader has to divide by a batch size they have to know.
//
// THE FIXTURE IS BUILT ONCE AND EVERY BODY IS STEADY STATE. The runner calls a body several times
// at growing iteration counts and then five more times for the minimum, so a body that grew the
// session, the index or the command log would measure a different subject on every sample. Each one
// below leaves the structure it touched exactly as it found it.

#include <cy/bench/bench.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/gameplay/indexes.h>
#include <cy/gameplay/tags.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using cy::ecs::Entity;
using namespace cy::gameplay;

/// Stop the run rather than report a number produced by a fixture that failed to build. A benchmark
/// that swallows an error measures the error path, and does it very quickly.
void require(bool condition, const char* what) noexcept {
    if (!condition) {
        std::fprintf(stderr, "cy_bench_gameplay: %s failed; the measurement would be meaningless\n",
                     what);
        std::abort();
    }
}

/// `gameplay-framework`'s own number: "Active gameplay entities | 100 000 without the framework
/// itself dominating frame time". Every body below runs against a framework holding this many, so
/// a query that degraded into a scan shows up as a hundred-thousand-fold regression rather than as
/// a benchmark that happened to fit in cache.
constexpr cy::u32 kEntities = 100'000;

/// Owners to spread those entities across. Eight is a large session rather than a huge one, which
/// is the harder case for `owned_by`: a bucket holds twelve and a half thousand entities.
constexpr cy::u32 kOwners = 8;

/// How many entities the batch body admits and removes per call into the framework. Ten thousand is
/// the table's own "Batch spawn" figure.
constexpr cy::u32 kBatch = 10'000;

/// A movement delta — small, trivially copyable, and what a command payload has to be.
struct MoveIntent {
    cy::i32 dx = 0;
    cy::i32 dy = 0;
};

/// A session, a control registry, a command stream, a tag registry and the derived indexes, wired
/// the way a game wires them and populated to the requirement's scale.
///
/// NO `ecs::World`, deliberately, and it is not an omission. `gameplay-framework` — "Headless
/// operation": the framework is fully functional with no renderer, no audio, no interface and no
/// GPU, and this module links nothing that has one. The entities below are the identifiers an ECS
/// would have issued; what is being measured is the framework's own storage and its own queries.
struct Fixture {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    GameSession session{allocator, 0x5EEDULL};
    ControlRegistry control{allocator};
    CommandStream commands{allocator, control};
    TagRegistry tags{allocator};
    GameplayIndexes indexes{allocator};
    TagSet carried{allocator};

    ParticipantId owners[kOwners];
    ControlSourceId source;
    CommandTypeId move = kInvalidCommandType;
    cy::u32 producer = 0;
    /// The entity every command is addressed to: one binding, so the structural check has something
    /// to pass and the benchmark measures validation rather than a rejection path.
    Entity target = Entity::make(1, 1);

    /// The deep tag the hierarchical query walks up to, and the shallow one it is queried with.
    TagId harvester = kInvalidTag;
    TagId unit = kInvalidTag;

    /// Scratch for the query bodies, sized once so a body allocates nothing.
    cy::Array<Entity> results{allocator};
    cy::Array<Entity> batch{allocator};

    Fixture() {
        for (ParticipantId& owner : owners) {
            auto added =
                session.add_participant(ParticipantKind::LocalHuman, cy::Name::intern("owner"));
            require(added.has_value(), "adding a participant");
            owner = *added;
        }

        auto created =
            control.create_source(ControlSourceKind::Human, owners[0], cy::Name::intern("input"));
        require(created.has_value(), "creating a control source");
        source = *created;
        require(control.bind_entity(source, channels::movement(), target).has_value(),
                "binding the command target");

        CommandDeclaration declaration;
        declaration.name = cy::Name::intern("Move");
        declaration.stable_id = 11;
        declaration.channel = channels::movement();
        auto declared = commands.declare(declaration);
        require(declared.has_value(), "declaring the command type");
        move = *declared;

        auto opened = commands.open_producer(cy::Name::intern("bench"));
        require(opened.has_value(), "opening a producer");
        producer = *opened;

        // A hierarchy deep enough that a `starts_with` implementation would be visibly cheaper than
        // the parent walk, so the benchmark is measuring the thing the requirement asks for.
        auto declared_tag = tags.declare("Unit.Robot.Harvester.Mk4.Field");
        require(declared_tag.has_value(), "declaring the tag hierarchy");
        harvester = *declared_tag;
        unit = tags.find("Unit.Robot");
        require(unit != kInvalidTag, "the ancestor is declared as a side effect");
        require(carried.add(harvester).has_value(), "carrying the deep tag");

        // A hundred thousand entities into the derived indexes, spread across the owners.
        for (cy::u32 index = 0; index < kEntities; ++index) {
            const Entity entity = Entity::make(index + 2, 1);
            require(indexes.on_owner_changed(entity, owners[index % kOwners]).has_value(),
                    "indexing an owner");
        }

        require(results.resize(kEntities).has_value(), "sizing the query's output");
        require(batch.resize(kBatch).has_value(), "sizing the batch");
        for (cy::u32 index = 0; index < kBatch; ++index) {
            // Above the populated range, so admitting and removing the batch leaves the hundred
            // thousand the other bodies are measured against untouched.
            batch[index] = Entity::make(kEntities + index + 2, 1);
        }
    }

    [[nodiscard]] GameplayContext context() noexcept {
        GameplayContext ctx;
        ctx.session = &session;
        ctx.services = &session.services();
        ctx.commands = &commands;
        ctx.at.tick = 1;
        return ctx;
    }

    [[nodiscard]] Command move_command() const noexcept {
        Command command;
        command.type = move;
        command.participant = owners[0];
        command.source = source;
        command.target = target;
        (void)command.set_payload(MoveIntent{1, 0});
        return command;
    }
};

/// Built on first use rather than at static initialisation: the session and the indexes allocate,
/// and a constructor running before main() would run before the allocator's domains do.
Fixture& fixture() {
    static Fixture instance;
    return instance;
}

}  // namespace

CY_BENCHMARK(
    "gameplay/command-record",
    "One command recorded into a producer's own buffer — the submission half of "
    "`gameplay-framework`'s 100 000 commands a second, and the half that is required to take no "
    "lock: 'submission SHALL NOT serialise through a single lock, and commands SHALL be "
    "accumulated "
    "per worker and committed deterministically'. The buffer is cleared per batch so the body is "
    "steady state. A regression means the record path grew work — a validation moved earlier than "
    "the commit, a sequence number that started costing an atomic, or a copy of the 48-byte "
    "payload "
    "that stopped being a memcpy. At the target this number must stay far under 10 000 ns, which "
    "is "
    "a hundred thousand commands in a second on ONE producer; the requirement is about many.") {
    Fixture& state = fixture();
    CommandBuffer& buffer = state.commands.producer(state.producer);
    const Command command = state.move_command();

    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    while (remaining > 0) {
        const auto batch = static_cast<cy::u32>(remaining < 4096 ? remaining : 4096);
        for (cy::u32 index = 0; index < batch; ++index) {
            require(buffer.record(command).has_value(), "recording a command");
        }
        buffer.clear();
        remaining -= batch;
    }
    CY_BENCH_KEEP(buffer.size());
}

CY_BENCHMARK(
    "gameplay/command-commit",
    "One command through the merge: validated structurally, committed, appended to the log and "
    "handed to the record seam. This is the framework's own per-command cost inside a tick, and it "
    "is the number the 'framework overhead is a small, reported fraction of simulation time' "
    "scenario is about — divide it by `ecs/query-iterate`'s ratio in the same baseline file for a "
    "per-entity share. A regression means validation gained a step, the merge stopped being a walk "
    "in (producer, sequence) order and became a sort, or the log's append started reallocating. "
    "The log is cleared per batch, because it grows for the life of a session and a body that let "
    "it grow would measure the allocator.") {
    Fixture& state = fixture();
    CommandBuffer& buffer = state.commands.producer(state.producer);
    const GameplayContext context = state.context();
    const Command command = state.move_command();

    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    cy::u32 committed = 0;
    while (remaining > 0) {
        const auto batch = static_cast<cy::u32>(remaining < 4096 ? remaining : 4096);
        for (cy::u32 index = 0; index < batch; ++index) {
            require(buffer.record(command).has_value(), "recording a command");
        }
        state.commands.commit(context, 1);
        committed = state.commands.committed_count();
        // Every command must have been ACCEPTED. A benchmark measuring the rejection path would be
        // fast, stable and about nothing — and a rejection is what a mis-built fixture produces.
        require(committed == batch, "every command was committed rather than rejected");
        state.commands.log().clear();
        remaining -= batch;
    }
    CY_BENCH_KEEP(committed);
}

CY_BENCHMARK(
    "gameplay/tag-test",
    "One hierarchical gameplay tag test: does a set carrying `Unit.Robot.Harvester.Mk4.Field` "
    "match "
    "a query for `Unit.Robot`? `gameplay-framework` requires tags to be 'compared as integers', "
    "with hierarchical queries resolving 'through compact metadata, not string prefix comparison' "
    "— "
    "so this is a binary search over a set of integers plus a walk up three parent links, and "
    "nothing in it reads a character. A regression of any size means something in that path "
    "started "
    "touching text: the number here is single-digit nanoseconds and a string compare is not.") {
    Fixture& state = fixture();
    const TagRegistry& registry = state.tags;
    const TagSet& carried = state.carried;
    const TagId query = state.unit;

    // Accumulated as an integer rather than xor-ed into a bool: the answer is the same every
    // iteration, and folding it into a counter keeps the optimiser from hoisting the call while
    // leaving the loop body one call and one add.
    cy::u64 sink = 0;
    for (std::uint64_t index = 0; index < CY_BENCH_ITERATIONS; ++index) {
        sink += carried.has(registry, query) ? 1U : 0U;
    }
    CY_BENCH_KEEP(sink);
}

CY_BENCHMARK(
    "gameplay/owner-query",
    "One entity's worth of 'what does this participant own', answered from the derived index over "
    "a "
    "hundred thousand entities spread across eight owners. `gameplay-framework`: 'Answering what "
    "this participant owns SHALL NOT require scanning every entity' — so the cost per entity "
    "RETURNED is a bucket walk and a copy, and it does not depend on how many entities the world "
    "holds. That is what makes this benchmark a gate rather than a number: if the index is ever "
    "replaced by a scan of the world, this body still returns the same twelve and a half thousand "
    "entities and takes eight times as long per one of them.") {
    Fixture& state = fixture();
    Entity* out = state.results.data();
    const auto capacity = static_cast<cy::u32>(state.results.size());

    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    cy::u32 owner = 0;
    cy::u32 found = 0;
    while (remaining > 0) {
        found = state.indexes.owned_by(state.owners[owner % kOwners], out, capacity);
        require(found > 0, "the index answered with the owner's entities");
        remaining -= (remaining < found) ? remaining : found;
        ++owner;
    }
    CY_BENCH_KEEP(found);
}

CY_BENCHMARK(
    "gameplay/batch-admit",
    "One entity's worth of the batch-spawn round trip on the framework's side: ten thousand "
    "entities given an owner in the derived index and then removed again. `gameplay-framework`'s "
    "table asks for 'Batch spawn | 10 000 entities without 10 000 individual allocations', and the "
    "ECS half of that is `ecs/create-many`; this is the half the framework itself pays, because an "
    "entity that spawns without an owner, a team or a tag is not a gameplay entity yet. It is the "
    "round trip and not the admission alone, for the same steady-state reason `ecs/create-many` "
    "gives — the index must hold the same hundred thousand before the body as after it. "
    "READ THE NUMBER AGAINST THE OTHER FOUR RATHER THAN ALONE: it is two to three orders of "
    "magnitude above them because `on_entity_removed` is O(buckets) BY CONSTRUCTION — an entity's "
    "row records its owner and its team, not every bucket it is in, so withdrawal probes each "
    "bucket's position map in turn and each probe is a cache miss into a different twelve-thousand-"
    "entry table. That is the index's design and not a defect, and pinning it here is what makes a "
    "change to it visible. A regression means the bucket insert started reallocating per entity, "
    "the swap-remove stopped being O(1), or the number of axes grew.") {
    Fixture& state = fixture();
    const cy::Span<const Entity> entities = state.batch.span();

    std::uint64_t remaining = CY_BENCH_ITERATIONS;
    while (remaining > 0) {
        const auto rows = static_cast<cy::u32>(remaining < kBatch ? remaining : kBatch);
        for (cy::u32 index = 0; index < rows; ++index) {
            require(state.indexes.on_owner_changed(entities[index], state.owners[index % kOwners])
                        .has_value(),
                    "admitting an entity to the index");
        }
        for (cy::u32 index = 0; index < rows; ++index) {
            state.indexes.on_entity_removed(entities[index]);
        }
        remaining -= rows;
    }
    CY_BENCH_KEEP(state.indexes.tracked_entities());
}
