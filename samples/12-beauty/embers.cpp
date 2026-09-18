// The air in the beauty shot. M11.c task 6.3. See embers.h for why it is authored here.

#include "embers.h"

#include <cmath>
#include <utility>

namespace cy::sample::beauty {
namespace {

using cy::graph::Graph;
using cy::graph::Literal;
using cy::graph::NodeKey;
using cy::graph::NodeRegistry;
using cy::vfx::AttributeDecl;
using cy::vfx::Emitter;
using cy::vfx::ParameterDecl;
using cy::vfx::Stage;

[[nodiscard]] Literal number(f32 x, f32 y, f32 z, f32 w, const char* type) noexcept {
    Literal literal;
    literal.type = Name::intern(type);
    literal.value = graph::Immediate{x, y, z, w, 0};
    return literal;
}

[[nodiscard]] Literal text(const char* value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

/// A terse way to author one stage graph. The same shape `src/vfx/tests/effects.cpp`'s
/// `StageBuilder` has and deliberately not a second copy of all of it: this file authors three
/// graphs and needs eleven of its twenty methods.
///
/// EVERY CALL FOLDS ITS SUCCESS INTO `ok_`. A builder that swallowed a failure would author a graph
/// missing a wire, the compiler would refuse it, and the refusal would name the compiler.
class Author {
public:
    Author(Allocator& allocator, const char* name) noexcept
        : graph_(allocator, Name::intern(name)) {
        graph_.grant(graph::Capability::ReadWorld | graph::Capability::Randomness);
    }

    [[nodiscard]] NodeKey constant(f32 x, f32 y = 0.0F, f32 z = 0.0F, f32 w = 0.0F,
                                   const char* type = "float") noexcept {
        const NodeKey key = add("vfx.constant");
        ok_ = ok_ &&
              graph_.set_property(key, Name::intern("value"), number(x, y, z, w, type)).has_value();
        return key;
    }

    [[nodiscard]] NodeKey attribute(const char* name) noexcept {
        return named("vfx.attribute", "attribute", name);
    }
    [[nodiscard]] NodeKey parameter(const char* name) noexcept {
        return named("vfx.parameter", "parameter", name);
    }
    [[nodiscard]] NodeKey input(const char* name) noexcept {
        return named("vfx.input", "input", name);
    }
    [[nodiscard]] NodeKey random() noexcept { return add("vfx.random"); }

    [[nodiscard]] NodeKey unary(const char* type, NodeKey x) noexcept {
        const NodeKey key = add(type);
        wire(x, key, "x");
        return key;
    }
    [[nodiscard]] NodeKey binary(const char* type, NodeKey a, NodeKey b) noexcept {
        const NodeKey key = add(type);
        wire(a, key, "a");
        wire(b, key, "b");
        return key;
    }
    /// `vfx.lerp` only, which is the one ternary this file uses: a, b, t.
    [[nodiscard]] NodeKey lerp(NodeKey a, NodeKey b, NodeKey t) noexcept {
        const NodeKey key = add("vfx.lerp");
        wire(a, key, "a");
        wire(b, key, "b");
        wire(t, key, "t");
        return key;
    }
    [[nodiscard]] NodeKey make3(NodeKey x, NodeKey y, NodeKey z) noexcept {
        const NodeKey key = add("vfx.make_float3");
        wire(x, key, "x");
        wire(y, key, "y");
        wire(z, key, "z");
        return key;
    }
    [[nodiscard]] NodeKey make4(NodeKey x, NodeKey y, NodeKey z, NodeKey w) noexcept {
        const NodeKey key = add("vfx.make_float4");
        wire(x, key, "x");
        wire(y, key, "y");
        wire(z, key, "z");
        wire(w, key, "w");
        return key;
    }

    void write(const char* attribute_name, NodeKey value) noexcept {
        const NodeKey key = named("vfx.set_attribute", "attribute", attribute_name);
        wire(value, key, "value");
    }
    void kill_if(NodeKey predicate) noexcept { wire(predicate, add("vfx.kill_if"), "value"); }
    void spawn_count(NodeKey value) noexcept { wire(value, add("vfx.spawn_count"), "value"); }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] Graph&& take() noexcept { return std::move(graph_); }

private:
    [[nodiscard]] NodeKey add(const char* type) noexcept {
        const NodeKey key = graph_.allocate_key();
        ok_ = ok_ && graph_.add_node(key, Name::intern(type)).has_value();
        return key;
    }
    [[nodiscard]] NodeKey named(const char* type, const char* property,
                                const char* value) noexcept {
        const NodeKey key = add(type);
        ok_ = ok_ && graph_.set_property(key, Name::intern(property), text(value)).has_value();
        return key;
    }
    void wire(NodeKey from, NodeKey to, const char* pin) noexcept {
        ok_ = ok_ && graph_.connect(from, Name::intern("out"), to, Name::intern(pin)).has_value();
    }

    Graph graph_;
    bool ok_ = true;
};

/// What the motes are made of, as the compiler sees it.
///
/// `color` declares a tolerance of 1/255 over [0, 1] and is therefore stored as `Unorm8` — four
/// bytes a particle rather than sixteen — while `emission` keeps its full range in `Float32`,
/// because a mote seen through eleven and a half stops of exposure is a radiance in the thousands
/// and eight bits of it would band. That split is `vfx-system`'s precision selection doing its job
/// on this effect rather than on a test's.
[[nodiscard]] Status declare_attributes(Emitter& emitter) noexcept {
    struct Row {
        const char* name;
        const char* type;
        f32 minimum;
        f32 maximum;
        f32 tolerance;
    };
    static constexpr Row kRows[] = {
        {"position", "float3", -64.0F, 64.0F, 0.0F}, {"velocity", "float3", -8.0F, 8.0F, 0.0F},
        {"age", "float", 0.0F, 16.0F, 0.0F},         {"lifetime", "float", 0.0F, 16.0F, 0.0F},
        {"size", "float", 0.0F, 1.0F, 0.0F},         {"color", "float4", 0.0F, 1.0F, 1.0F / 255.0F},
        {"emission", "float", 0.0F, 40000.0F, 0.0F},
    };
    for (const Row& row : kRows) {
        AttributeDecl decl;
        decl.name = Name::intern(row.name);
        decl.type = Name::intern(row.type);
        decl.minimum = row.minimum;
        decl.maximum = row.maximum;
        decl.tolerance = row.tolerance;
        if (Status declared = emitter.declare_attribute(decl); !declared) {
            return declared;
        }
    }
    return ok();
}

/// Two motes a step, scaled by the exposed `density`.
///
/// MEASURED RATHER THAN INTENDED: the effect is `ImportanceClass::Ambient`, whose default
/// simulation frequency is 30 Hz and not 60, so two a step is sixty a second — and against a
/// four-and-a-half to seven second life that settles at about three hundred and twenty motes an
/// emitter, 972 over the three. Dense enough to read as air, sparse enough that the colonnade is
/// still the subject, and comfortably inside the 512-particle block each emitter is given.
[[nodiscard]] Status build_spawn(Allocator& allocator, Emitter& emitter) noexcept {
    Author stage(allocator, "embers.spawn");
    const NodeKey rate = stage.constant(2.0F);
    stage.spawn_count(stage.binary("vfx.mul", rate, stage.parameter("density")));
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "beauty embers: the spawn graph did not author");
    }
    return emitter.set_stage(Stage::Spawn, stage.take());
}

[[nodiscard]] Status build_initialise(Allocator& allocator, Emitter& emitter) noexcept {
    Author stage(allocator, "embers.initialise");
    const NodeKey one = stage.constant(1.0F);
    const NodeKey two = stage.constant(2.0F);

    // 2r - 1. Each call allocates its OWN `vfx.random`, which is a separate stream — `ir.cpp`'s
    // `MergeClass::Never` — so the components below do not correlate and the slab is a slab rather
    // than a diagonal.
    const auto signed_draw = [&stage, one, two]() noexcept {
        return stage.binary("vfx.sub", stage.binary("vfx.mul", stage.random(), two), one);
    };

    // A SLAB OF AIR rather than a point source: ±3.4 m across the courtyard, ±2.6 m along it and
    // 2.3 m of height. An emitter is a volume here because embers that all left one point read as a
    // fountain, and this shot wants air.
    stage.write("position",
                stage.make3(stage.binary("vfx.mul", signed_draw(), stage.constant(3.4F)),
                            stage.binary("vfx.mul", stage.random(), stage.constant(2.3F)),
                            stage.binary("vfx.mul", signed_draw(), stage.constant(2.6F))));
    // They RISE, slowly and at their own speeds, with a lateral drift. 0.10 to 0.34 m/s is a mote
    // of ash in still air; the shot is a sixtieth of a second and the field is photographed after
    // six seconds of it.
    stage.write(
        "velocity",
        stage.make3(stage.binary("vfx.mul", signed_draw(), stage.constant(0.075F)),
                    stage.binary("vfx.add", stage.constant(0.10F),
                                 stage.binary("vfx.mul", stage.random(), stage.constant(0.24F))),
                    stage.binary("vfx.mul", signed_draw(), stage.constant(0.075F))));
    stage.write("age", stage.constant(0.0F));
    stage.write("lifetime",
                stage.binary("vfx.add", stage.constant(4.5F),
                             stage.binary("vfx.mul", stage.random(), stage.constant(2.5F))));
    // 18 to 40 mm. At 1920 across and this camera that is between one and three pixels of core with
    // the sprite's falloff around it, which is what an ember looks like and not what a sprite sheet
    // looks like.
    stage.write("size",
                stage.binary("vfx.add", stage.constant(0.018F),
                             stage.binary("vfx.mul", stage.random(), stage.constant(0.022F))));
    // A RADIANCE, not a colour: `emission` carries the magnitude and these four are the chroma and
    // the opacity the sprite's falloff premultiplies. Warm, because the sun at 15.5 degrees is, and
    // barely opaque at all, because `embers.h` measured what happens to a sky when it is not.
    stage.write("color",
                stage.make4(stage.constant(1.0F), stage.constant(0.62F), stage.constant(0.31F),
                            stage.binary("vfx.add", stage.constant(kEmberOpacityFloor),
                                         stage.binary("vfx.mul", stage.random(),
                                                      stage.constant(kEmberOpacitySpan)))));
    stage.write("emission", stage.constant(kEmberBirthRadiance));
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "beauty embers: the initialise graph did not author");
    }
    return emitter.set_stage(Stage::Initialise, stage.take());
}

[[nodiscard]] Status build_update(Allocator& allocator, Emitter& emitter) noexcept {
    Author stage(allocator, "embers.update");
    const NodeKey dt = stage.input(vfx::input::kDeltaTime);
    const NodeKey velocity = stage.attribute("velocity");
    const NodeKey position = stage.attribute("position");
    const NodeKey age = stage.attribute("age");
    const NodeKey lifetime = stage.attribute("lifetime");

    // `buoyancy` is UNEXPOSED, so the compiler folds it into the generated code and
    // `EmitterReport::folded_parameters` counts one. It is an updraught rather than a fall: ash off
    // a warm courtyard floor accelerates upward, which is why these motes never come back down
    // inside their lifetime.
    const NodeKey next_velocity =
        stage.binary("vfx.add", velocity, stage.binary("vfx.mul", stage.parameter("buoyancy"), dt));
    stage.write("velocity", next_velocity);
    stage.write("position",
                stage.binary("vfx.add", position, stage.binary("vfx.mul", next_velocity, dt)));

    const NodeKey next_age = stage.binary("vfx.add", age, dt);
    stage.write("age", next_age);
    const NodeKey fraction =
        stage.unary("vfx.saturate", stage.binary("vfx.div", next_age, lifetime));
    // A MOTE COOLS. The chroma stays where the initialise put it and the magnitude falls by the
    // factor of twenty `embers.h` fixes, which is what makes the far end of the field dimmer than
    // the near end without a single per-particle branch.
    stage.write("emission", stage.lerp(stage.constant(kEmberBirthRadiance),
                                       stage.constant(kEmberDeathRadiance), fraction));
    stage.kill_if(stage.binary("vfx.greater", next_age, lifetime));
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "beauty embers: the update graph did not author");
    }
    return emitter.set_stage(Stage::Update, stage.take());
}

}  // namespace

Expected<vfx::VfxSystemAsset, Error> build_embers(Allocator& allocator) noexcept {
    vfx::VfxSystemAsset asset(allocator, Name::intern("courtyard_embers"));
    // AMBIENT, and the class is the honest one: this is atmosphere. It is the class the budget
    // controller degrades FIRST, which is the correct answer for air in a frame that is tight —
    // and the beauty shot is not tight, so the field is drawn whole.
    asset.set_importance(vfx::ImportanceClass::Ambient);
    vfx::ScalabilityPolicy policy;
    policy.min_spawn_scale = 0.25F;
    policy.min_simulation_hz = 15.0F;
    asset.set_scalability(policy);

    ParameterDecl density;
    density.name = Name::intern("density");
    density.type = Name::intern("float");
    density.value[0] = 1.0F;
    density.exposed = true;
    if (Status declared = asset.declare_parameter(density); !declared) {
        return make_unexpected(declared.error());
    }
    ParameterDecl buoyancy;
    buoyancy.name = Name::intern("buoyancy");
    buoyancy.type = Name::intern("float3");
    buoyancy.value[0] = 0.0F;
    buoyancy.value[1] = 0.055F;
    buoyancy.value[2] = 0.0F;
    buoyancy.exposed = false;
    if (Status declared = asset.declare_parameter(buoyancy); !declared) {
        return make_unexpected(declared.error());
    }

    Emitter emitter(allocator, Name::intern("motes"));
    emitter.set_capacity(kEmberCapacity);
    if (Status declared = declare_attributes(emitter); !declared) {
        return make_unexpected(declared.error());
    }
    if (Status built = build_spawn(allocator, emitter); !built) {
        return make_unexpected(built.error());
    }
    if (Status built = build_initialise(allocator, emitter); !built) {
        return make_unexpected(built.error());
    }
    if (Status built = build_update(allocator, emitter); !built) {
        return make_unexpected(built.error());
    }
    if (Status added = asset.add_emitter(std::move(emitter)); !added) {
        return make_unexpected(added.error());
    }
    return asset;
}

Expected<vfx::CompiledSystem, Error> cook_embers(Allocator& allocator, graph::DiagnosticSink& sink,
                                                 vfx::CompileReport& report) noexcept {
    NodeRegistry registry(allocator);
    vfx::DataInterfaceRegistry interfaces(allocator);
    if (Status registered = vfx::register_vfx_nodes(registry); !registered) {
        return make_unexpected(registered.error());
    }
    if (Status registered = vfx::register_builtin_interfaces(interfaces); !registered) {
        return make_unexpected(registered.error());
    }
    Expected<vfx::VfxSystemAsset, Error> asset = build_embers(allocator);
    if (!asset) {
        return make_unexpected(asset.error());
    }
    // CyberGraph's load-time step. An unresolved graph validates as a page of "type is not
    // registered", which blames the author for the caller's omission.
    asset->resolve(registry);
    const vfx::CompileOptions options;
    return compile_system(*asset, registry, interfaces, options, sink, report);
}

EmberField::EmberField(Allocator& allocator) noexcept
    : allocator_(&allocator),
      sink_(allocator),
      cook_(allocator),
      world_(allocator),
      records_(allocator) {}

Status EmberField::build() noexcept {
    Expected<vfx::CompiledSystem, Error> compiled = cook_embers(*allocator_, sink_, cook_);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    system_ = Expected<vfx::CompiledSystem, Error>(std::move(compiled.value()));

    vfx::WorldDescription description;
    description.pool_bytes = 4ULL * 1024ULL * 1024ULL;
    description.max_instances = 16;
    if (Status made = world_.initialize(description); !made) {
        return made;
    }
    for (const EmberEmitter& emitter : kEmberEmitters) {
        vfx::EffectSpawn spawn;
        spawn.position = emitter.position;
        spawn.scale = 1.0F;
        if (Expected<vfx::EffectHandle, Error> played = world_.play(system_.value(), spawn);
            !played.has_value()) {
            return make_unexpected(played.error());
        }
    }
    built_ = true;
    return ok();
}

Status EmberField::publish(const Vec3& camera_position) noexcept {
    return publish_sprites(world_, camera_position, kEmberRing, records_, published_);
}

Status EmberField::settle(const Vec3& camera_position) noexcept {
    if (!built_) {
        return fail(ErrorCode::Unavailable, "beauty embers: build() was not called");
    }
    // `lround` rather than `+ 0.5F` and a cast: the two agree here — the quotient is 360
    // exactly — and clang-tidy is right that they do not agree in general.
    const auto steps = static_cast<u32>(std::lround(kEmberWarmup / kEmberStep));
    for (u32 step = 0; step < steps; ++step) {
        if (Status stepped = world_.step(kEmberStep, stepped_); !stepped) {
            return stepped;
        }
    }
    return publish(camera_position);
}

Status EmberField::advance(const Vec3& camera_position, f32 dt) noexcept {
    if (!built_) {
        return fail(ErrorCode::Unavailable, "beauty embers: build() was not called");
    }
    if (Status stepped = world_.step(dt, stepped_); !stepped) {
        return stepped;
    }
    return publish(camera_position);
}

}  // namespace cy::sample::beauty
