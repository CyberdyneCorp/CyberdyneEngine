#pragma once
// The slice's internals: the level, the compiled programs, the gameplay kit, and the hosts the
// programs call out through. Included by slice.cpp and systems.cpp and by nothing else.
//
// It is a header rather than one translation unit because the game divides in two along a line
// worth keeping — building it, and running it — and both halves need the same three structures.

#include <cy/ai/knowledge.h>
#include <cy/ai/perception.h>
#include <cy/ai/runtime.h>
#include <cy/core/base/expected.h>
#include <cy/navigation/build.h>
#include <cy/navigation/query.h>
#include <cy/rendering/scene/asset_binding.h>

#include "slice.h"

#include <ctime>

#include <chrono>
#include <cmath>
#include <new>
#include <utility>

namespace cy::sample::slice {

using cy::ecs::Entity;

// --- The arena ----------------------------------------------------------------------------------

/// The smallest arena: a 48 m square, which is what a person watching a few hundred characters
/// wants to look at.
constexpr f32 kMinHalfExtent = 24.0F;
/// Square metres of floor per character. THE DENSITY IS WHAT IS HELD CONSTANT, NOT THE SIZE.
///
/// Eight thousand characters in a 48 m square is 0.29 m² each, and a character is 0.8 m across:
/// they cannot physically stand that close, so every neighbour query, every avoidance solve and
/// every perception request in the tick would be measuring the overcrowding rather than the
/// engine. Four square metres each is a dense crowd a person would recognise, and it makes the
/// scale act a linearity test instead — the same simulation, four times as much of it.
constexpr f32 kMetresPerCharacter = 4.0F;

/// The crowd grid's cell, for a given population and arena.
///
/// A UNIFORM GRID HAS TWO COSTS AND THEY PULL IN OPPOSITE DIRECTIONS: building it costs a pass over
/// the occupied cells per agent (`Crowd::rebuild_grid` finds an agent's cell by scanning the cell
/// table, which is a linear scan), and querying it costs a pass over the agents in the block of
/// cells the neighbour distance covers. Small cells make the first expensive, large cells the
/// second. The sum is least at roughly 3·sqrt(agents) cells, which is what this returns, floored at
/// the neighbour distance because a cell smaller than the query radius buys nothing.
///
/// MEASURED, on this machine, 8,000 agents over a 179 m arena: 2 m cells cost 72 ms a tick and
/// 11 m cells cost 9 ms. The difference is entirely the linear scan, and it is reported to
/// `src/navigation/`'s owner as a finding rather than worked around silently — a hash of the cell
/// key would make the build O(agents) and make this function unnecessary.
[[nodiscard]] inline f32 crowd_cell_size(u32 agents, f32 half_extent) noexcept {
    const f32 cells = 3.0F * std::sqrt(static_cast<f32>(agents < 1U ? 1U : agents));
    const f32 size = (2.0F * half_extent) / std::sqrt(cells);
    return size < 4.0F ? 4.0F : size;
}

/// Half the arena's side for a given population.
[[nodiscard]] inline f32 arena_half_extent(u32 agents) noexcept {
    const f32 wanted = 0.5F * std::sqrt(static_cast<f32>(agents) * kMetresPerCharacter);
    return wanted < kMinHalfExtent ? kMinHalfExtent : wanted;
}
constexpr f32 kCharacterRadius = 0.4F;
/// The squad: the characters that fight each other, and the only ones registered as perception
/// targets. Everything past this is crowd — it thinks, steers and animates, and it is not something
/// the squad's sensors are asked about. `PerceptionTarget`'s own header puts that decision in
/// gameplay's hands rather than the runtime's, and this is gameplay making it.
constexpr u32 kSquad = 32;

/// A value pinned to [0, 1]. Named rather than a nested conditional, which reads worse and which
/// the lint rules refuse.
[[nodiscard]] inline f32 clamp_unit(f32 value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}
constexpr f32 kDeltaTime = 1.0F / 60.0F;
/// The joints of the slice's biped. Twelve, which is what a bone level of detail needs to have
/// something to drop.
constexpr cy::u16 kJointCount = 12;
constexpr cy::u16 kJointRoot = 0;
constexpr cy::u16 kJointShoulder = 5;
/// How many characters have their pose EVALUATED per tick. Root motion is integrated for every one
/// of them — `advance()` is the deterministic half and a tier may not skip it — but a pose nothing
/// looks at is a pose nothing needs, which is the whole of `animation-and-skinning`'s LOD.
constexpr u32 kEvaluatedPoses = 128;

[[nodiscard]] inline f64 cpu_micros() noexcept {
    // CPU TIME AND NOT WALL CLOCK, for the reason `src/ai/tests/test_scale.cpp` gives at length: a
    // budget asserted on elapsed time measures whatever else the machine is running, and a figure
    // that fails on a busy host is a figure somebody switches off.
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0) {
        return (static_cast<f64>(now.tv_sec) * 1e6) + (static_cast<f64>(now.tv_nsec) / 1e3);
    }
#endif
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<f64, std::micro>(since).count();
}

[[nodiscard]] inline cy::graph::Literal text_literal(const char* value) noexcept {
    cy::graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] inline cy::graph::Literal number_literal(f32 value) noexcept {
    cy::graph::Literal literal;
    literal.type = Name::intern("float");
    literal.value = cy::graph::Immediate::scalar(value);
    return literal;
}

[[nodiscard]] inline cy::graph::Literal integer_literal(u32 value) noexcept {
    cy::graph::Literal literal;
    literal.type = Name::intern("int");
    literal.value.mask = value;
    return literal;
}

/// A graph written as a list of calls. The first failure is kept and the rest are no-ops, so the
/// caller checks once — the shape `src/animation/tests/test_evaluate.cpp` settled for the same
/// problem.
class GraphWriter {
public:
    explicit GraphWriter(cy::graph::Graph& graph) noexcept : graph_(&graph) {}

    void node(cy::graph::NodeKey key, const char* type) noexcept {
        keep(graph_->add_node(key, Name::intern(type)));
    }
    void prop(cy::graph::NodeKey key, const char* name, const cy::graph::Literal& value) noexcept {
        keep(graph_->set_property(key, Name::intern(name), value));
    }
    void link(cy::graph::NodeKey from, const char* out, cy::graph::NodeKey to,
              const char* in) noexcept {
        keep(graph_->connect(from, Name::intern(out), to, Name::intern(in)));
    }
    [[nodiscard]] Status result() const noexcept { return status_; }

private:
    void keep(const Status& step) noexcept {
        if (status_ && !step) {
            status_ = step;
        }
    }

    cy::graph::Graph* graph_ = nullptr;
    Status status_ = cy::ok();
};

/// FNV-1a over eight bytes. The fold every other digest in this tree uses.
[[nodiscard]] inline u64 fold_into(u64 digest, u64 value) noexcept {
    u64 result = digest;
    for (u32 byte = 0; byte < 8U; ++byte) {
        result ^= (value >> (byte * 8U)) & 0xFFULL;
        result *= 0x100000001B3ULL;
    }
    return result;
}

/// A float folded at a fixed quantisation, so a digest is a statement about the simulation rather
/// than about the last bit of a float.
[[nodiscard]] inline u64 quantise(f32 value) noexcept {
    const f64 scaled = static_cast<f64>(value) * 4096.0;
    const f64 rounded = scaled < 0.0 ? scaled - 0.5 : scaled + 0.5;
    return static_cast<u64>(static_cast<cy::i64>(rounded));
}

[[nodiscard]] inline f64 median_of(Array<f64>& samples) noexcept {
    if (samples.empty()) {
        return 0.0;
    }
    for (cy::usize i = 0; i < samples.size(); ++i) {
        for (cy::usize j = i + 1; j < samples.size(); ++j) {
            if (samples[j] < samples[i]) {
                const f64 swap = samples[i];
                samples[i] = samples[j];
                samples[j] = swap;
            }
        }
    }
    return samples[samples.size() / 2];
}

// --- The hosts the compiled programs call out through --------------------------------------------

/// The behaviour tree's boundary: `ai-system`'s tasks and conditions are the game's, and this is
/// where the game answers them.
///
/// A FINDING, RECORDED WHERE IT WAS FOUND: `BehaviourHost` takes no agent. `AiRuntime::think()`
/// owns the loop and hands the host a task name and a delta and nothing that says which character
/// is asking, so a host cannot answer a per-character question. What IS per character is the
/// agent's own execution state, its blackboard and its knowledge store — so this slice's per
/// character decisions come from `KnowledgeStore::best_target()` after the perception pass, and the
/// tree decides the MODE. README.md, "What building this found".
class SquadHost final : public cy::graph::behaviour::BehaviourHost {
public:
    cy::graph::behaviour::BtStatus run_task(Name task, f32 /*dt*/) override {
        ++tasks;
        last_task = task;
        return task == Name::intern("hold") ? cy::graph::behaviour::BtStatus::Success
                                            : cy::graph::behaviour::BtStatus::Running;
    }
    [[nodiscard]] bool test_condition(Name condition) override {
        ++conditions;
        return condition == Name::intern("objective_live") ? objective_live : contact;
    }
    [[nodiscard]] f32 score(Name task) override {
        ++scores;
        return task == Name::intern("engage") ? engage_score : 0.35F;
    }

    u64 tasks = 0;
    u64 conditions = 0;
    u64 scores = 0;
    Name last_task;
    bool contact = false;
    bool objective_live = true;
    f32 engage_score = 0.5F;
};

/// The perception pass's batch, answered against the level's own pillars. A project answers from
/// the physics server; this level's occluders are cylinders, so the answer is arithmetic and the
/// slice needs no collision world to have line of sight mean something.
class ArenaVisibility final : public cy::ai::VisibilityHost {
public:
    ArenaVisibility(Span<const Vec3> pillars, f32 radius) noexcept
        : pillars_(pillars), radius_(radius) {}

    void trace_batch(Span<const cy::ai::VisibilityQuery> queries, Span<bool> results) override {
        ++calls;
        traced += static_cast<u64>(queries.size());
        for (cy::usize index = 0; index < results.size() && index < queries.size(); ++index) {
            results[index] = !blocked(queries[index].from, queries[index].to);
        }
    }

    u64 calls = 0;
    u64 traced = 0;

private:
    [[nodiscard]] bool blocked(Vec3 from, Vec3 to) const noexcept {
        const f32 span_x = to.x - from.x;
        const f32 span_z = to.z - from.z;
        const f32 length_squared = (span_x * span_x) + (span_z * span_z);
        if (length_squared < 1e-4F) {
            return false;
        }
        for (const Vec3& pillar : pillars_) {
            const f32 offset_x = pillar.x - from.x;
            const f32 offset_z = pillar.z - from.z;
            f32 along = ((offset_x * span_x) + (offset_z * span_z)) / length_squared;
            along = clamp_unit(along);
            const f32 dx = offset_x - (span_x * along);
            const f32 dz = offset_z - (span_z * along);
            if ((dx * dx) + (dz * dz) < radius_ * radius_) {
                return true;
            }
        }
        return false;
    }

    Span<const Vec3> pillars_;
    f32 radius_ = 1.0F;
};

/// The camera rig's query batch. `camera-system` forbids "scattered synchronous casts from
/// individual rig nodes", and the compiled rig's phase boundary is what makes this ONE call.
class RigQueries final : public cy::graph::camera::RigQueryBatch {
public:
    void resolve(Span<const cy::graph::camera::RigQuery> queries,
                 Span<cy::graph::camera::RigQueryResult> results) override {
        ++calls;
        resolved += static_cast<u32>(queries.size());
        for (cy::usize index = 0; index < results.size() && index < queries.size(); ++index) {
            for (u32 channel = 0; channel < 3U; ++channel) {
                results[index].hit[channel] = queries[index].target[channel];
            }
        }
    }

    u32 calls = 0;
    u32 resolved = 0;
};

/// The objective script's boundary. `visual-scripting`'s "the pure interface every external effect
/// goes through", implemented for one script.
class ObjectiveHost final : public cy::graph::script::ScriptHost {
public:
    cy::graph::script::Value call(const cy::graph::script::ExternalRef& /*callee*/,
                                  Span<const cy::graph::script::Value> /*arguments*/) override {
        ++calls;
        return cy::graph::script::Value::from_float(0.0F);
    }
    cy::graph::script::Value query(const cy::graph::script::ExternalRef& /*query*/,
                                   Span<const cy::graph::script::Value> /*arguments*/) override {
        ++queries;
        return cy::graph::script::Value::from_float(progress);
    }
    void emit_event(const cy::graph::script::ExternalRef& /*event*/,
                    Span<const cy::graph::script::Value> /*arguments*/) override {
        ++events;
    }
    void emit_command(const cy::graph::script::ExternalRef& command,
                      Span<const cy::graph::script::Value> /*arguments*/) override {
        ++commands;
        last_command = command.name;
    }
    cy::graph::script::Value get_field(const cy::graph::script::ExternalRef& /*field*/,
                                       const cy::graph::script::Value& /*subject*/) override {
        return cy::graph::script::Value::from_float(0.0F);
    }
    void set_field(const cy::graph::script::ExternalRef& /*field*/,
                   const cy::graph::script::Value& /*subject*/,
                   const cy::graph::script::Value& /*value*/) override {
        ++writes;
    }
    [[nodiscard]] bool wait_satisfied(const cy::graph::script::SuspendPoint& /*point*/) override {
        return true;
    }

    f32 progress = 0.0F;
    u32 calls = 0;
    u32 queries = 0;
    u32 events = 0;
    u32 commands = 0;
    u32 writes = 0;
    Name last_command;
};

/// THE NEGATIVE CONTROL'S SUBJECT: a per-entity object with a virtual `tick()`, which is precisely
/// what `docs/ROADMAP.md`'s "an audit finds no per-entity virtual tick in any graph consumer"
/// forbids. Compiled unconditionally and instantiated only under `--interpret-control`, because a
/// control that is not in the binary is a control nobody can run.
class InterpretedNode {
public:
    InterpretedNode() = default;
    virtual ~InterpretedNode() = default;
    InterpretedNode(const InterpretedNode&) = delete;
    InterpretedNode& operator=(const InterpretedNode&) = delete;
    InterpretedNode(InterpretedNode&&) = delete;
    InterpretedNode& operator=(InterpretedNode&&) = delete;

    virtual void tick(f32 dt) noexcept = 0;
};

class WanderNode final : public InterpretedNode {
public:
    void tick(f32 dt) noexcept override { phase += dt; }
    f32 phase = 0.0F;
};

inline void add_quad(Array<Vec3>& vertices, Array<u32>& indices,
                     Array<cy::navigation::AreaType>& areas, Vec3 a, Vec3 b, Vec3 c, Vec3 d,
                     Status& status) noexcept {
    const auto base = static_cast<u32>(vertices.size());
    const Vec3 corners[4] = {a, b, c, d};
    for (const Vec3& corner : corners) {
        if (Status pushed = vertices.push_back(corner); !pushed && status) {
            status = pushed;
        }
    }
    // Wound so the normal points up: `cross(b - a, c - a).y > 0`. The other winding is a ceiling,
    // and every triangle of it is steeper than any slope limit.
    const u32 order[6] = {0, 2, 1, 0, 3, 2};
    for (const u32 offset : order) {
        if (Status pushed = indices.push_back(base + offset); !pushed && status) {
            status = pushed;
        }
    }
    for (u32 triangle = 0; triangle < 2U; ++triangle) {
        if (Status pushed = areas.push_back(cy::navigation::kAreaGround); !pushed && status) {
            status = pushed;
        }
    }
}

/// Two mesh bounds that are the same box. Exact equality is right here: both sides come from the
/// same `from_center_extents` on the same authored numbers, so a tolerance would only let two
/// genuinely different meshes share one asset.
[[nodiscard]] inline bool same_extent(const Aabb& left, const Aabb& right) noexcept {
    return left.min.x == right.min.x && left.min.y == right.min.y && left.min.z == right.min.z &&
           left.max.x == right.max.x && left.max.y == right.max.y && left.max.z == right.max.z;
}

// --- The level -----------------------------------------------------------------------------------

struct Slice::Level {
    Level(Allocator& allocator, f32 arena_half) noexcept
        : half_extent(arena_half),
          ground_cells(static_cast<u32>(arena_half)),
          world(allocator),
          tree(world),
          buffer(allocator),
          meshes(allocator),
          vertices(allocator),
          indices(allocator),
          areas(allocator),
          props(allocator),
          prop_kind(allocator),
          prop_bounds(allocator),
          pillars(allocator),
          mesh(allocator, Name::intern("arena"), 2.0F * arena_half) {}

    ~Level() {
        if (extractor != nullptr) {
            extractor->~SnapshotExtractor();
            extractor = nullptr;
        }
    }

    Level(const Level&) = delete;
    Level& operator=(const Level&) = delete;

    [[nodiscard]] Status initialize() noexcept {
        if (Status started = world.initialize(); !started) {
            return started;
        }
        if (Status built = tree.initialize(); !built) {
            return built;
        }
        scene = tree.components();
        Expected<cy::rendering::RenderComponents, Error> registered =
            cy::rendering::RenderComponents::register_all(world);
        if (!registered) {
            return Status{cy::make_unexpected(registered.error())};
        }
        render = *registered;
        extractor = new (static_cast<void*>(storage))
            cy::rendering::SnapshotExtractor(allocator(), world, render, scene, buffer);
        return cy::ok();
    }

    [[nodiscard]] Allocator& allocator() const noexcept { return vertices.allocator(); }

    // ==========================================================================================
    // THE SLICE IS ITS OWN ASSET SYSTEM, AND THAT IS WHAT MAKES 11.3 DEMONSTRABLE
    // ==========================================================================================
    //
    // `bind_render_assets` takes two LOOKUPS and loads nothing — where a mesh comes from is
    // `core-assets-and-io`'s. So something has to answer, and here it is this table: authoring
    // interns a (kind, half extent) pair as one mesh asset and the reference the component carries
    // names it, exactly as a cooked reference would. The resolver below turns that reference into
    // the slot this process gave it.
    //
    // THE TWO WALL SHAPES ARE TWO MESHES, not one mesh placed twice, and the table is what forced
    // it: `bind_mesh` writes the RESOLVER's bounds over the component's, because the mesh's own
    // bounds are the mesh's. A north wall and a west wall referencing one asset would both end up
    // whichever shape was interned first — which is the same class of mistake as M8.a's box, one
    // link further along.
    struct MeshAsset {
        u64 reference = 0;
        MeshKind kind = MeshKind::Ground;
        Aabb bounds = Aabb::empty();
    };

    /// The asset reference for this (kind, size), interning it on first use.
    [[nodiscard]] Expected<cy::rendering::AssetRef, Error> intern_mesh(MeshKind kind,
                                                                       Vec3 half) noexcept {
        const Aabb bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, half);
        for (const MeshAsset& asset : meshes) {
            if (asset.kind == kind && same_extent(asset.bounds, bounds)) {
                return cy::rendering::AssetRef{0, asset.reference};
            }
        }
        MeshAsset asset;
        asset.reference = 0x4D45534800000000ULL + static_cast<u64>(meshes.size());
        asset.kind = kind;
        asset.bounds = bounds;
        if (Status pushed = meshes.push_back(asset); !pushed) {
            return cy::make_unexpected(pushed.error());
        }
        return cy::rendering::AssetRef{0, asset.reference};
    }

    /// The lookups `bind_render_assets` calls. Plain functions with the level as their datum,
    /// which is the shape every engine callback takes.
    [[nodiscard]] static cy::rendering::MeshResolution resolve_mesh(cy::rendering::AssetRef ref,
                                                                    void* user) noexcept {
        const Level& level = *static_cast<const Level*>(user);
        cy::rendering::MeshResolution resolution;
        for (cy::usize index = 0; index < level.meshes.size(); ++index) {
            if (level.meshes[index].reference == ref.low) {
                // Generation 1: a zero generation IS the null handle, so a slot's first live
                // generation is one and a resolver that answered zero would be answering "no mesh".
                resolution.mesh = cy::render::MeshHandle::from_slot(static_cast<u32>(index), 1);
                resolution.local_bounds = level.meshes[index].bounds;
                resolution.found = true;
                return resolution;
            }
        }
        return resolution;
    }

    [[nodiscard]] static cy::rendering::MaterialResolution resolve_material(
        cy::rendering::AssetRef ref, void* user) noexcept {
        const Level& level = *static_cast<const Level*>(user);
        (void)level;
        cy::rendering::MaterialResolution resolution;
        const u64 slot = ref.low - 0x4D41540000000000ULL;
        if (slot >= static_cast<u64>(MaterialKind::Count)) {
            return resolution;
        }
        resolution.material = cy::render::MaterialHandle::from_slot(static_cast<u32>(slot), 1);
        resolution.found = true;
        return resolution;
    }

    /// Turn every authored reference in the world into a handle. Idempotent, and run once here
    /// because nothing in this slice streams — a game runs it again after a load or a stream step.
    [[nodiscard]] Status bind_assets(cy::rendering::BindReport& out) noexcept {
        cy::rendering::AssetResolver resolver;
        resolver.mesh = &Level::resolve_mesh;
        resolver.material = &Level::resolve_material;
        resolver.user = this;
        return cy::rendering::bind_render_assets(world, render, resolver, out);
    }

    /// One authored, drawable thing: a transform, a mesh REFERENCE and the bounds that travel with
    /// it. The handle is filled in later by `bind_render_assets` — task 11.3's two halves, in the
    /// order a game performs them.
    [[nodiscard]] Expected<Entity, Error> place(Vec3 at, MeshKind kind, MaterialKind paint,
                                                Vec3 half) noexcept {
        cy::ecs::ComponentTypeId components[2] = {scene.world_transform, render.mesh_renderer};
        Expected<Entity, Error> entity =
            world.create(Span<const cy::ecs::ComponentTypeId>(components, 2));
        if (!entity) {
            return entity;
        }
        cy::scene::WorldTransform transform;
        transform.value = Transform::from_translation(at);
        if (Status placed = world.set(*entity, scene.world_transform, transform); !placed) {
            return cy::make_unexpected(placed.error());
        }
        Expected<cy::rendering::AssetRef, Error> mesh_reference = intern_mesh(kind, half);
        if (!mesh_reference) {
            return cy::make_unexpected(mesh_reference.error());
        }
        cy::rendering::MeshRenderer renderer;
        renderer.mesh = *mesh_reference;
        renderer.material =
            cy::rendering::AssetRef{0, 0x4D41540000000000ULL + static_cast<u64>(paint)};
        renderer.local_bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, half);
        if (Status drawn = world.set(*entity, render.mesh_renderer, renderer); !drawn) {
            return cy::make_unexpected(drawn.error());
        }
        return entity;
    }

    [[nodiscard]] Status move(Entity entity, Vec3 to) noexcept {
        cy::scene::WorldTransform transform;
        transform.value = Transform::from_translation(to);
        return world.set(entity, scene.world_transform, transform);
    }

    /// Half the arena's side, and how many quads its floor is divided into per side. Both derived
    /// from the population — see `arena_half_extent`. Declared FIRST because they are initialised
    /// first: `mesh`'s tile size is computed from the same number.
    f32 half_extent = kMinHalfExtent;
    u32 ground_cells = 24;
    cy::ecs::World world;
    cy::scene::SceneTree tree;
    cy::scene::SceneComponents scene;
    cy::rendering::RenderComponents render;
    cy::render::SnapshotBuffer buffer;
    alignas(cy::rendering::SnapshotExtractor) unsigned char storage[sizeof(
        cy::rendering::SnapshotExtractor)]{};
    cy::rendering::SnapshotExtractor* extractor = nullptr;

    Array<MeshAsset> meshes;
    Array<Vec3> vertices;
    Array<u32> indices;
    Array<cy::navigation::AreaType> areas;
    Array<Entity> props;
    Array<MeshKind> prop_kind;
    Array<Aabb> prop_bounds;
    Array<Vec3> pillars;
    cy::navigation::NavMesh mesh;
};

// --- The compiled programs and the runtimes that execute them
// -------------------------------------

struct Slice::Brain {
    Brain(Allocator& allocator, f32 crowd_cell) noexcept
        : registry(allocator),
          sink(allocator),
          behaviour_graph(allocator, Name::intern("squad")),
          pose_graph(allocator, Name::intern("locomotion")),
          ability_graph(allocator, Name::intern("volley")),
          rig_graph(allocator, Name::intern("shoulder")),
          objective_graph(allocator, Name::intern("objective")),
          skeleton(allocator),
          walk(allocator),
          aim(allocator),
          clip_table(allocator),
          rig(allocator),
          scratch(allocator),
          poses(allocator),
          agents(allocator),
          states(allocator),
          blackboards(allocator),
          sensors(allocator),
          importance(allocator),
          tiers(allocator),
          knowledge(allocator),
          stores(allocator),
          targets(allocator),
          crowd(allocator, crowd_cell),
          interpreted(allocator) {}

    ~Brain() {
        delete objective_state;
        delete objective;
        delete rig_program;
        delete ability;
        delete posed;
        delete batch;
        delete behaviour;
        delete perception;
        delete runtime;
        for (InterpretedNode* node : interpreted.span()) {
            delete node;
        }
    }

    Brain(const Brain&) = delete;
    Brain& operator=(const Brain&) = delete;

    cy::graph::NodeRegistry registry;
    cy::graph::DiagnosticSink sink;

    cy::graph::Graph behaviour_graph;
    cy::graph::Graph pose_graph;
    cy::graph::Graph ability_graph;
    cy::graph::Graph rig_graph;
    cy::graph::Graph objective_graph;

    cy::graph::behaviour::BehaviourProgram* behaviour = nullptr;
    cy::graph::pose::PoseProgram* posed = nullptr;
    cy::graph::script::AbilityProgram* ability = nullptr;
    cy::graph::camera::CameraRigProgram* rig_program = nullptr;
    cy::graph::script::ScriptProgram* objective = nullptr;
    cy::graph::script::ScriptState* objective_state = nullptr;

    cy::animation::Skeleton skeleton;
    cy::animation::Clip walk;
    cy::animation::Clip aim;
    Array<const cy::animation::Clip*> clip_table;
    cy::animation::AnimationRig rig;
    cy::animation::PoseScratch scratch;
    Array<Transform> poses;
    cy::animation::AnimationBatch* batch = nullptr;

    Array<cy::ai::AIAgent> agents;
    Array<cy::ai::AIState> states;
    Array<cy::ai::Blackboard> blackboards;
    Array<cy::ai::PerceptionSensors> sensors;
    Array<f32> importance;
    Array<cy::ai::AiTier> tiers;
    Array<cy::ai::KnowledgeStore> knowledge;
    Array<cy::ai::KnowledgeStore*> stores;
    Array<cy::ai::PerceptionTarget> targets;
    cy::ai::AiRuntime* runtime = nullptr;
    cy::ai::PerceptionScheduler* perception = nullptr;
    cy::navigation::Crowd crowd;

    SquadHost host;
    ObjectiveHost objective_host;
    RigQueries rig_queries;
    cy::graph::camera::RigInstance rig_instance;
    cy::graph::camera::RigOutput rig_output;

    /// Empty unless `--interpret-control` asked for it. See `InterpretedNode`.
    Array<InterpretedNode*> interpreted;
};

// --- The gameplay framework and the ability kit
// ---------------------------------------------------

struct Slice::Kit {
    Kit(Allocator& allocator, u64 seed) noexcept
        : tags(allocator),
          tag_store(allocator),
          relationships(allocator),
          phases(allocator),
          time(allocator),
          schema(allocator),
          attributes(allocator, schema),
          effects(allocator, attributes, tags),
          abilities(allocator),
          costs(allocator, attributes),
          buffer(allocator),
          random(seed),
          pipeline(allocator, abilities, effects, attributes, costs, tag_store, tags, relationships,
                   random) {}

    Kit(const Kit&) = delete;
    Kit& operator=(const Kit&) = delete;

    cy::gameplay::TagRegistry tags;
    cy::gameplay::EntityTagStore tag_store;
    cy::gameplay::RelationshipService relationships;
    cy::gameplay::PhaseController phases;
    cy::gameplay::TimeDomains time;

    cy::gameplay::abilities::AttributeSchema schema;
    cy::gameplay::abilities::AttributeStore attributes;
    cy::gameplay::abilities::EffectSystem effects;
    cy::gameplay::abilities::AbilityRegistry abilities;
    cy::gameplay::abilities::CostLedger costs;
    cy::gameplay::abilities::TargetBuffer buffer;
    cy::gameplay::GameplayRandom random;
    cy::gameplay::abilities::ActivationPipeline pipeline;
    cy::gameplay::abilities::TargetContext target_context;

    cy::gameplay::abilities::AttributeId health = cy::gameplay::abilities::kInvalidAttribute;
    cy::gameplay::abilities::AttributeId energy = cy::gameplay::abilities::kInvalidAttribute;
    cy::gameplay::abilities::EffectDefId burn = cy::gameplay::abilities::kInvalidEffectDef;
    cy::gameplay::abilities::AbilityId volley = cy::gameplay::abilities::kInvalidAbility;
    cy::gameplay::TagId cue = cy::gameplay::kInvalidTag;
    cy::gameplay::TagId suppressed = cy::gameplay::kInvalidTag;
    cy::gameplay::TagId phase_play = cy::gameplay::kInvalidTag;
    cy::gameplay::TeamId blue = 0;
    cy::gameplay::TeamId red = 0;
};

}  // namespace cy::sample::slice
