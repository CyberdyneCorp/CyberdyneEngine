// The spark plume every VFX suite compiles, simulates and draws. M8.c section 2. See effects.h for
// why each part of it is there.

#include "effects.h"

#include <utility>

namespace cy::vfx_test {
namespace {

[[nodiscard]] Literal float_literal(f32 x, f32 y, f32 z, f32 w, const char* type) noexcept {
    Literal literal;
    literal.type = Name::intern(type);
    literal.value = graph::Immediate{x, y, z, w, 0};
    return literal;
}

[[nodiscard]] Literal text_literal(const char* text) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(text);
    return literal;
}

}  // namespace

StageBuilder::StageBuilder(Allocator& allocator, const char* name) noexcept
    : graph_(allocator, Name::intern(name)) {
    graph_.grant(graph::Capability::ReadWorld | graph::Capability::Randomness);
}

NodeKey StageBuilder::add(const char* type) noexcept {
    const NodeKey key = graph_.allocate_key();
    ok_ = ok_ && graph_.add_node(key, Name::intern(type)).has_value();
    return key;
}

void StageBuilder::set_text(NodeKey key, const char* property, const char* text) noexcept {
    ok_ = ok_ && graph_.set_property(key, Name::intern(property), text_literal(text)).has_value();
}

void StageBuilder::wire(NodeKey from, NodeKey to, const char* pin) noexcept {
    ok_ = ok_ && graph_.connect(from, Name::intern("out"), to, Name::intern(pin)).has_value();
}

NodeKey StageBuilder::constant(f32 x, f32 y, f32 z, f32 w, const char* type) noexcept {
    const NodeKey key = add("vfx.constant");
    ok_ = ok_ && graph_.set_property(key, Name::intern("value"), float_literal(x, y, z, w, type))
                     .has_value();
    return key;
}

NodeKey StageBuilder::attribute(const char* name) noexcept {
    const NodeKey key = add("vfx.attribute");
    set_text(key, "attribute", name);
    return key;
}

NodeKey StageBuilder::parameter(const char* name) noexcept {
    const NodeKey key = add("vfx.parameter");
    set_text(key, "parameter", name);
    return key;
}

NodeKey StageBuilder::input(const char* name) noexcept {
    const NodeKey key = add("vfx.input");
    set_text(key, "input", name);
    return key;
}

NodeKey StageBuilder::random() noexcept {
    return add("vfx.random");
}

NodeKey StageBuilder::sample(const char* interface_name, const char* field,
                             NodeKey argument) noexcept {
    const NodeKey key = add("vfx.sample");
    set_text(key, "interface", interface_name);
    set_text(key, "field", field);
    wire(argument, key, "x");
    return key;
}

NodeKey StageBuilder::unary(const char* type, NodeKey x) noexcept {
    const NodeKey key = add(type);
    wire(x, key, "x");
    return key;
}

NodeKey StageBuilder::binary(const char* type, NodeKey a, NodeKey b) noexcept {
    const NodeKey key = add(type);
    wire(a, key, "a");
    wire(b, key, "b");
    return key;
}

NodeKey StageBuilder::ternary(const char* type, NodeKey a, NodeKey b, NodeKey c) noexcept {
    const NodeKey key = add(type);
    const bool is_select = Name::intern(type) == Name::intern("vfx.select");
    wire(a, key, is_select ? "condition" : "a");
    wire(b, key, is_select ? "a" : "b");
    wire(c, key, is_select ? "b" : "t");
    return key;
}

NodeKey StageBuilder::make3(NodeKey x, NodeKey y, NodeKey z) noexcept {
    const NodeKey key = add("vfx.make_float3");
    wire(x, key, "x");
    wire(y, key, "y");
    wire(z, key, "z");
    return key;
}

NodeKey StageBuilder::make4(NodeKey x, NodeKey y, NodeKey z, NodeKey w) noexcept {
    const NodeKey key = add("vfx.make_float4");
    wire(x, key, "x");
    wire(y, key, "y");
    wire(z, key, "z");
    wire(w, key, "w");
    return key;
}

void StageBuilder::write(const char* attribute_name, NodeKey value) noexcept {
    const NodeKey key = add("vfx.set_attribute");
    set_text(key, "attribute", attribute_name);
    wire(value, key, "value");
}

void StageBuilder::kill_if(NodeKey predicate) noexcept {
    const NodeKey key = add("vfx.kill_if");
    wire(predicate, key, "value");
}

void StageBuilder::emit_event(const char* channel, NodeKey predicate) noexcept {
    const NodeKey key = add("vfx.emit_event");
    set_text(key, "channel", channel);
    wire(predicate, key, "value");
}

void StageBuilder::spawn_count(NodeKey value) noexcept {
    const NodeKey key = add("vfx.spawn_count");
    wire(value, key, "value");
}

Graph&& StageBuilder::take() noexcept {
    return std::move(graph_);
}

namespace {

[[nodiscard]] Status declare_attributes(Emitter& emitter) noexcept {
    struct Row {
        const char* name;
        const char* type;
        f32 minimum;
        f32 maximum;
        f32 tolerance;
    };
    // THE TOLERANCES ARE THE WHOLE OF THE PRECISION CLAIM. `color` accepts 1/255 over [0, 1], which
    // is exactly what an eight-bit encoding gives, so the compiler picks `Unorm8`. `position` and
    // `velocity` accept nothing and stay at `Float32` — an author who says nothing gets nothing
    // quantised.
    static constexpr Row kRows[] = {
        {"position", "float3", -64.0F, 64.0F, 0.0F},
        {"velocity", "float3", -64.0F, 64.0F, 0.0F},
        {"age", "float", 0.0F, 8.0F, 0.0F},
        {"lifetime", "float", 0.0F, 8.0F, 0.0F},
        {"size", "float", 0.0F, 1.0F, 0.0F},
        {"color", "float4", 0.0F, 1.0F, 1.0F / 255.0F},
        // THE WIDE-RANGE HALF OF THE COLOUR. A physically-lit frame views a 22 000 lux sun through
        // about eleven stops of exposure, so an emissive particle's radiance is in the thousands —
        // and that is exactly why `color` above can be eight bits: the magnitude is not in it.
        {"emission", "float", 0.0F, 40000.0F, 0.0F},
        {"scratch", "float", 0.0F, 1.0F, 0.0F},
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

[[nodiscard]] Status build_spawn(Allocator& allocator, Emitter& emitter) noexcept {
    StageBuilder stage(allocator, "spawn");
    // Sixteen a step, scaled by the exposed `intensity` parameter — so a parameter reaches a stage
    // that is not the update, which is what "readable by every stage graph in the system" means.
    const NodeKey rate = stage.constant(16.0F);
    const NodeKey intensity = stage.parameter("intensity");
    stage.spawn_count(stage.binary("vfx.mul", rate, intensity));
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "vfx test: the spawn graph did not author");
    }
    return emitter.set_stage(Stage::Spawn, stage.take());
}

[[nodiscard]] Status build_initialise(Allocator& allocator, Emitter& emitter) noexcept {
    StageBuilder stage(allocator, "initialise");
    const NodeKey one = stage.constant(1.0F);
    const NodeKey two = stage.constant(2.0F);
    const NodeKey spread = stage.constant(0.16F);

    const auto signed_random = [&stage, one, two]() noexcept {
        // 2r - 1: two `vfx.random` nodes are two streams, so the three components below do not
        // correlate. That is the `MergeClass::Never` policy in `ir.cpp` being load-bearing.
        return stage.binary("vfx.sub", stage.binary("vfx.mul", stage.random(), two), one);
    };

    const NodeKey offset = stage.binary(
        "vfx.mul", stage.make3(signed_random(), signed_random(), signed_random()), spread);
    stage.write("position", offset);

    // THE SPEED AND THE LIFETIME ARE BOTH DRAWN, and that is what makes this a plume rather than a
    // block: a population whose particles all left at one speed and all die at one moment moves in
    // lockstep, which photographs as a solid rectangle. Two more `vfx.random` nodes, two more
    // streams — `MergeClass::Never` in `ir.cpp` is what keeps them independent.
    const NodeKey rise =
        stage.binary("vfx.add", stage.constant(2.6F),
                     stage.binary("vfx.mul", stage.random(), stage.constant(3.6F)));
    const NodeKey lateral = stage.constant(0.62F);
    stage.write("velocity", stage.make3(stage.binary("vfx.mul", signed_random(), lateral), rise,
                                        stage.binary("vfx.mul", signed_random(), lateral)));
    stage.write("age", stage.constant(0.0F));
    stage.write("lifetime",
                stage.binary("vfx.add", stage.constant(0.55F),
                             stage.binary("vfx.mul", stage.random(), stage.constant(0.9F))));
    stage.write("size", stage.constant(0.22F));
    // A RADIANCE, NOT A COLOUR — see src/rendering/particles/README.md. The alpha is the opacity
    // the sprite's falloff premultiplies; the three channels are quantised to `Unorm8` and scaled
    // by the renderer at publication.
    stage.write("color", stage.make4(stage.constant(1.0F), stage.constant(0.42F),
                                     stage.constant(0.12F), stage.constant(0.9F)));
    stage.write("emission", stage.constant(14000.0F));
    // WRITTEN AND NEVER READ. Attribute liveness elides it, its store is not emitted into the
    // generated Slang, and the per-particle byte size is four bytes smaller for it.
    stage.write("scratch", stage.constant(7.0F));
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "vfx test: the initialise graph did not author");
    }
    return emitter.set_stage(Stage::Initialise, stage.take());
}

[[nodiscard]] Status build_update(Allocator& allocator, Emitter& emitter) noexcept {
    StageBuilder stage(allocator, "update");
    const NodeKey dt = stage.input(input::kDeltaTime);
    const NodeKey gravity = stage.parameter("gravity");
    const NodeKey velocity = stage.attribute("velocity");
    const NodeKey position = stage.attribute("position");
    const NodeKey age = stage.attribute("age");
    const NodeKey lifetime = stage.attribute("lifetime");

    // v += g * dt. `gravity` is UNEXPOSED, so the compiler folds it into the generated code and
    // `EmitterReport::folded_parameters` counts one.
    const NodeKey next_velocity =
        stage.binary("vfx.add", velocity, stage.binary("vfx.mul", gravity, dt));
    stage.write("velocity", next_velocity);
    // p += v * dt, reading the velocity the FUSED initialise already has in a register.
    stage.write("position",
                stage.binary("vfx.add", position, stage.binary("vfx.mul", next_velocity, dt)));
    const NodeKey next_age = stage.binary("vfx.add", age, dt);
    stage.write("age", next_age);

    const NodeKey fraction =
        stage.unary("vfx.saturate", stage.binary("vfx.div", next_age, lifetime));
    stage.write("size",
                stage.ternary("vfx.lerp", stage.constant(0.22F), stage.constant(0.02F), fraction));
    stage.write("emission", stage.ternary("vfx.lerp", stage.constant(14000.0F),
                                          stage.constant(300.0F), fraction));
    const NodeKey expired = stage.binary("vfx.greater", next_age, lifetime);
    stage.kill_if(expired);
    // A GPU-TO-GPU EVENT RAISE, on the channel the asset declared with both of its bounds. The
    // predicate is the particle's HEIGHT rather than a constant, so the rank the channel drops by
    // is a real number and the surviving set is the highest sparks — which is what a deterministic
    // rank is for. `dot(position, (0, 1, 0))` is how this node library spells a component read.
    const NodeKey height =
        stage.binary("vfx.dot", position, stage.constant(0.0F, 1.0F, 0.0F, 0.0F, "float3"));
    stage.emit_event("collision", height);
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "vfx test: the update graph did not author");
    }
    return emitter.set_stage(Stage::Update, stage.take());
}

}  // namespace

Expected<VfxSystemAsset, Error> build_plume(Allocator& allocator, u32 emitters,
                                            u32 capacity) noexcept {
    VfxSystemAsset asset(allocator, Name::intern("spark_plume"));
    asset.set_importance(ImportanceClass::Important);
    ScalabilityPolicy policy;
    policy.min_spawn_scale = 0.2F;
    policy.min_simulation_hz = 10.0F;
    asset.set_scalability(policy);

    ParameterDecl intensity;
    intensity.name = Name::intern("intensity");
    intensity.type = Name::intern("float");
    intensity.value[0] = 1.0F;
    intensity.exposed = true;
    if (Status declared = asset.declare_parameter(intensity); !declared) {
        return make_unexpected(declared.error());
    }
    ParameterDecl gravity;
    gravity.name = Name::intern("gravity");
    gravity.type = Name::intern("float3");
    gravity.value[0] = 0.0F;
    gravity.value[1] = -9.81F;
    gravity.value[2] = 0.0F;
    // NOT EXPOSED, so the compiler folds it. Flipping this one boolean is what makes
    // `folded_parameters` move, which is how the folding claim is checked rather than asserted.
    gravity.exposed = false;
    if (Status declared = asset.declare_parameter(gravity); !declared) {
        return make_unexpected(declared.error());
    }

    EventChannelDecl collision;
    collision.name = Name::intern("collision");
    collision.max_events_per_frame = 64;
    collision.max_chain_depth = 3;
    collision.readback = true;
    if (Status declared = asset.declare_channel(collision); !declared) {
        return make_unexpected(declared.error());
    }

    for (u32 which = 0; which < emitters; ++which) {
        char name[32] = "sparks0";
        name[6] = static_cast<char>('0' + static_cast<char>(which % 10U));
        Emitter emitter(allocator, Name::intern(name));
        emitter.set_capacity(capacity);
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
    }
    return asset;
}

Status prepare(NodeRegistry& registry, DataInterfaceRegistry& interfaces) noexcept {
    if (Status registered = register_vfx_nodes(registry); !registered) {
        return registered;
    }
    return register_builtin_interfaces(interfaces);
}

Expected<CompiledSystem, Error> cook_plume(Allocator& allocator, graph::DiagnosticSink& sink,
                                           CompileReport& report, const CompileOptions& options,
                                           u32 emitters, u32 capacity) noexcept {
    NodeRegistry registry(allocator);
    DataInterfaceRegistry interfaces(allocator);
    if (Status prepared = prepare(registry, interfaces); !prepared) {
        return make_unexpected(prepared.error());
    }
    auto asset = build_plume(allocator, emitters, capacity);
    if (!asset) {
        return make_unexpected(asset.error());
    }
    // CyberGraph's load-time step. An unresolved graph validates as a page of "type is not
    // registered", which blames the author for the caller's omission.
    asset->resolve(registry);
    return compile_system(*asset, registry, interfaces, options, sink, report);
}

}  // namespace cy::vfx_test
