// The execution-path decision, the CPU kernel executor, and the seam to the renderer.
// M8.c tasks 2.3 and 2.5. See runtime.h for what each half owes its specification.

#include <cy/vfx/runtime.h>

#include <algorithm>
#include <cmath>

namespace cy::vfx {
namespace {

/// Four floats a slot. A vector op writes all four; a scalar op writes the first and the rest are
/// read only by an op that asked for a wider type, which `Builder::make` refused to produce.
inline constexpr u32 kSlotWidth = 4;

[[nodiscard]] f32* slot_of(KernelRegisters& registers, u16 slot) noexcept {
    return registers.data() + (static_cast<usize>(slot) * kSlotWidth);
}

[[nodiscard]] const f32* slot_at(const KernelRegisters& registers, u16 slot) noexcept {
    return registers.data() + (static_cast<usize>(slot) * kSlotWidth);
}

/// The per-particle random draw. The same sequence the generated Slang's `cyVfxRandom` computes —
/// two spellings of one hash is how a CPU fallback and a GPU path come to disagree about a
/// deterministic test, so this is written beside the shader's version and checked against it.
[[nodiscard]] f32 random_draw(u32 particle, u32 stream) noexcept {
    u32 state = (particle * 747796405U) + (stream * 2891336453U) + 1U;
    state ^= state >> 16U;
    state *= 2246822519U;
    state ^= state >> 13U;
    state *= 3266489917U;
    state ^= state >> 16U;
    return static_cast<f32>(state >> 8U) * (1.0F / 16777216.0F);
}

[[nodiscard]] f32 value_noise(f32 x) noexcept {
    const f32 index = std::floor(x);
    const f32 fraction = x - index;
    const auto hash = [](f32 at) noexcept {
        const f32 raw = std::sin(at * 12.9898F) * 43758.5453F;
        return raw - std::floor(raw);
    };
    const f32 a = hash(index);
    const f32 b = hash(index + 1.0F);
    const f32 smooth = fraction * fraction * (3.0F - (2.0F * fraction));
    return a + ((b - a) * smooth);
}

[[nodiscard]] f32 saturate(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

void read_attribute(const KernelContext& context, u32 particle, Name attribute,
                    f32* destination) noexcept {
    for (u32 component = 0; component < kSlotWidth; ++component) {
        destination[component] = 0.0F;
    }
    const AttributeSlot* slot = context.layout->find(attribute);
    if (slot == nullptr || slot->elided) {
        return;
    }
    for (u32 component = 0; component < slot->components; ++component) {
        destination[component] =
            load_component(Span<const u8>(context.storage.data(), context.storage.size()), *slot,
                           particle, component);
    }
}

void read_parameter(const KernelContext& context, Name parameter, f32* destination) noexcept {
    for (u32 component = 0; component < kSlotWidth; ++component) {
        destination[component] = 0.0F;
    }
    for (usize index = 0; index < context.parameter_decls.size(); ++index) {
        if (context.parameter_decls[index].name != parameter) {
            continue;
        }
        const usize base = index * kSlotWidth;
        if (base + kSlotWidth > context.parameters.size()) {
            return;
        }
        for (u32 component = 0; component < kSlotWidth; ++component) {
            destination[component] = context.parameters[base + component];
        }
        return;
    }
}

void read_input(const KernelContext& context, Name symbol, u32 particle,
                f32* destination) noexcept {
    const std::string_view text = symbol.text();
    f32 value = 0.0F;
    if (text == input::kDeltaTime) {
        value = context.dt;
    } else if (text == input::kEmitterAge) {
        value = context.emitter_age;
    } else if (text == input::kParticleIndex) {
        value = static_cast<f32>(particle);
    } else if (text == input::kSpawnIndex) {
        value = static_cast<f32>(context.spawn_index);
    } else if (text == input::kNormalisedAge) {
        value = context.normalised_age;
    }
    for (u32 component = 0; component < kSlotWidth; ++component) {
        destination[component] = value;
    }
}

/// A leaf: everything with no operands. Split out because the operator switch is long enough
/// without it and because a leaf is where the outside world reaches into a kernel.
void evaluate_leaf(const KernelStep& step, const KernelContext& context, u32 particle,
                   f32* destination) noexcept {
    switch (step.op) {
        case Constant:
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] = immediate_component(step.value, component);
            }
            // A scalar constant broadcasts, so `float3 * 2.0` multiplies every component.
            if (vfx_type_components(step.type) == 1) {
                for (u32 component = 1; component < kSlotWidth; ++component) {
                    destination[component] = destination[0];
                }
            }
            return;
        case Parameter:
            read_parameter(context, step.symbol, destination);
            return;
        case Attribute:
            read_attribute(context, particle, step.symbol, destination);
            return;
        case EmitterInput:
            read_input(context, step.symbol, particle, destination);
            return;
        case Random: {
            const f32 draw = random_draw(particle, step.value.mask);
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] = draw;
            }
            return;
        }
        default:
            break;
    }
    for (u32 component = 0; component < kSlotWidth; ++component) {
        destination[component] = 0.0F;
    }
}

/// The elementwise operations, through the SAME function the compiler's constant folder uses. Two
/// spellings of `add` is how a folded parameter changes an effect.
[[nodiscard]] bool evaluate_elementwise(const KernelStep& step, const f32* a, const f32* b,
                                        f32* destination) noexcept {
    if (step.op < Add || step.op > Max) {
        return false;
    }
    Immediate left;
    Immediate right;
    for (u32 component = 0; component < kSlotWidth; ++component) {
        set_immediate_component(left, component, a[component]);
        set_immediate_component(right, component, b[component]);
    }
    Immediate result;
    if (!fold_elementwise_public(step.op, left, right, kSlotWidth, false, false, result)) {
        // A division by zero declines rather than inventing a value, and the destination keeps the
        // numerator — which is the same answer `x / 1` would give and never a NaN in a position.
        for (u32 component = 0; component < kSlotWidth; ++component) {
            destination[component] = a[component];
        }
        return true;
    }
    for (u32 component = 0; component < kSlotWidth; ++component) {
        destination[component] = immediate_component(result, component);
    }
    return true;
}

void evaluate_transcendental(const KernelStep& step, const f32* a, const f32* b,
                             f32* destination) noexcept {
    switch (step.op) {
        case Dot: {
            // THREE COMPONENTS. `dot` returns a scalar, so `step.type` is `Float` and says nothing
            // about the operands' width; three is what this domain's `dot` means, and a `float2`
            // operand's third component is zero from the register file's own initialisation.
            f32 sum = 0.0F;
            for (u32 component = 0; component < 3U; ++component) {
                sum += a[component] * b[component];
            }
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] = sum;
            }
            return;
        }
        case Cross:
            destination[0] = (a[1] * b[2]) - (a[2] * b[1]);
            destination[1] = (a[2] * b[0]) - (a[0] * b[2]);
            destination[2] = (a[0] * b[1]) - (a[1] * b[0]);
            destination[3] = 0.0F;
            return;
        case Length: {
            const f32 length = std::sqrt((a[0] * a[0]) + (a[1] * a[1]) + (a[2] * a[2]));
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] = length;
            }
            return;
        }
        case Normalize: {
            const f32 length = std::sqrt((a[0] * a[0]) + (a[1] * a[1]) + (a[2] * a[2]));
            const f32 inverse = length > 1.0e-8F ? 1.0F / length : 0.0F;
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] = a[component] * inverse;
            }
            return;
        }
        default:
            break;
    }
    for (u32 component = 0; component < kSlotWidth; ++component) {
        const f32 value = a[component];
        switch (step.op) {
            case Sin:
                destination[component] = std::sin(value);
                break;
            case Cos:
                destination[component] = std::cos(value);
                break;
            case Pow:
                destination[component] = std::pow(value < 0.0F ? 0.0F : value, b[component]);
                break;
            // `saturate` and an unbound `curve` are the same function, and that is not a
            // coincidence to be tidied away: the generated Slang's `cyVfxCurve` is `saturate` too,
            // until a cooked curve resource is bound. Two paths, one answer, one branch.
            case Saturate:
            case Curve:
                destination[component] = saturate(value);
                break;
            case Noise:
                destination[component] = value_noise(value);
                break;
            case Sample:
                // A data interface with no bound resource reads zero on the CPU path. The cook-time
                // gate in `compile.cpp` is what stops an effect DEPENDING on one it cannot have;
                // this is what a bound-but-empty interface produces, and it is not an error.
                destination[component] = 0.0F;
                break;
            default:
                destination[component] = value;
                break;
        }
    }
}

void evaluate_composite(const KernelStep& step, const f32* a, const f32* b, const f32* c,
                        const f32* d, f32* destination) noexcept {
    switch (step.op) {
        case Lerp:
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] =
                    a[component] + ((b[component] - a[component]) * c[component]);
            }
            return;
        case Select:
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] = a[0] != 0.0F ? b[component] : c[component];
            }
            return;
        case Less:
        case Greater:
        case LogicalAnd:
        case LogicalOr:
        case LogicalNot: {
            bool truth = false;
            switch (step.op) {
                case Less:
                    truth = a[0] < b[0];
                    break;
                case Greater:
                    truth = a[0] > b[0];
                    break;
                case LogicalAnd:
                    truth = a[0] != 0.0F && b[0] != 0.0F;
                    break;
                case LogicalOr:
                    truth = a[0] != 0.0F || b[0] != 0.0F;
                    break;
                default:
                    truth = a[0] == 0.0F;
                    break;
            }
            for (u32 component = 0; component < kSlotWidth; ++component) {
                destination[component] = truth ? 1.0F : 0.0F;
            }
            return;
        }
        case MakeVec3:
            destination[0] = a[0];
            destination[1] = b[0];
            destination[2] = c[0];
            destination[3] = 0.0F;
            return;
        case MakeVec4:
            destination[0] = a[0];
            destination[1] = b[0];
            destination[2] = c[0];
            destination[3] = d[0];
            return;
        default:
            break;
    }
    for (u32 component = 0; component < kSlotWidth; ++component) {
        destination[component] = a[component];
    }
}

/// Step through the kernel's flat program, filling the register file.
void evaluate_program(const VfxKernel& kernel, const KernelContext& context, u32 particle,
                      KernelRegisters& registers) noexcept {
    static constexpr f32 kZero[kSlotWidth] = {0.0F, 0.0F, 0.0F, 0.0F};
    for (const KernelStep& step : kernel.program()) {
        f32* destination = slot_of(registers, step.dst);
        if (step.a == 0xFFFFU) {
            evaluate_leaf(step, context, particle, destination);
            continue;
        }
        const f32* a = slot_of(registers, step.a);
        const f32* b = step.b == 0xFFFFU ? kZero : slot_of(registers, step.b);
        const f32* c = step.c == 0xFFFFU ? kZero : slot_of(registers, step.c);
        const f32* d = step.d == 0xFFFFU ? kZero : slot_of(registers, step.d);
        if (evaluate_elementwise(step, a, b, destination)) {
            continue;
        }
        switch (step.op) {
            case Dot:
            case Cross:
            case Length:
            case Normalize:
            case Sin:
            case Cos:
            case Pow:
            case Saturate:
            case Curve:
            case Noise:
            case Sample:
                evaluate_transcendental(step, a, b, destination);
                break;
            default:
                evaluate_composite(step, a, b, c, d, destination);
                break;
        }
    }
}

/// Apply the ordered write list. THIS is the half `cy::graph::expr.h` cannot express and the reason
/// `ir.h` exists.
void apply_writes(const VfxKernel& kernel, const KernelContext& context, u32 particle,
                  const KernelRegisters& registers) noexcept {
    const Span<const KernelWrite> writes = kernel.writes();
    const Span<const u16> slots = kernel.write_slots();
    for (usize index = 0; index < writes.size() && index < slots.size(); ++index) {
        if (slots[index] == 0xFFFFU) {
            continue;
        }
        const AttributeSlot* attribute = context.layout->find(writes[index].attribute);
        if (attribute == nullptr || attribute->elided) {
            continue;
        }
        const f32* value = slot_at(registers, slots[index]);
        for (u32 component = 0; component < attribute->components; ++component) {
            store_component(context.storage, *attribute, particle, component, value[component]);
        }
    }
}

/// Raise every event whose predicate came out non-zero. After the writes and before the kill: an
/// event raised by a particle that is about to die is still that particle's event, and
/// `vfx-system`'s "collision raises an event" response is exactly a raise beside a kill.
[[nodiscard]] Status raise_events(const VfxKernel& kernel, const KernelContext& context,
                                  u32 particle, const KernelRegisters& registers,
                                  u32& raised) noexcept {
    const Span<const KernelEvent> raises = kernel.events();
    const Span<const u16> slots = kernel.event_slots();
    for (usize index = 0; index < raises.size() && index < slots.size(); ++index) {
        if (slots[index] == 0xFFFFU) {
            continue;
        }
        const f32* predicate = slot_at(registers, slots[index]);
        if (predicate[0] == 0.0F) {
            continue;
        }
        ++raised;
        if (context.events == nullptr) {
            continue;
        }
        EventRecord record;
        record.source = particle;
        record.depth = 0;
        // THE RANK IS THE PREDICATE'S OWN VALUE. A graph that raises with `1.0` gets a flat rank
        // and the channel drops by arrival order; a graph that raises with an impact speed gets the
        // hardest impacts kept, which is what a deterministic rank is for.
        record.rank = predicate[0];
        for (u32 channel = 0; channel < 4U; ++channel) {
            record.payload[channel] = predicate[channel];
        }
        if (Status put = context.events->raise(raises[index].channel, record); !put) {
            return put;
        }
    }
    return ok();
}

}  // namespace

const char* fallback_reason_name(FallbackReason reason) noexcept {
    switch (reason) {
        case FallbackReason::None:
            return "None";
        case FallbackReason::EffectRequiresCpu:
            return "EffectRequiresCpu";
        case FallbackReason::DeviceLacksCompute:
            return "DeviceLacksCompute";
        case FallbackReason::DeviceLacksIndirectDispatch:
            return "DeviceLacksIndirectDispatch";
        case FallbackReason::DisabledByHost:
            return "DisabledByHost";
        case FallbackReason::NoDeviceInThisWorld:
            return "NoDeviceInThisWorld";
    }
    return "?";
}

const char* fallback_explanation(FallbackReason reason) noexcept {
    switch (reason) {
        case FallbackReason::None:
            return "GPU compute simulation, which is the default path.";
        case FallbackReason::EffectRequiresCpu:
            return "The effect declares SimulationPath::CpuRequired because gameplay reads its "
                   "per-particle results synchronously. This is not a fallback; its particle count "
                   "is bounded by that choice.";
        case FallbackReason::DeviceLacksCompute:
            return "The device cannot run compute. Budgets are reduced and the features the CPU "
                   "path does not support were reported at cook time.";
        case FallbackReason::DeviceLacksIndirectDispatch:
            return "The device has no indirect dispatch, so dispatch size could not follow the "
                   "live "
                   "particle count without a CPU readback of it.";
        case FallbackReason::DisabledByHost:
            return "The host disabled the GPU path — a capture, a profile or a bisection.";
        case FallbackReason::NoDeviceInThisWorld:
            return "This build has a compute dispatch for a VFX kernel — cy::vfx-gpu's VfxGpuPass "
                   "— and this SimulationWorld has no device to run it on. A host that wants the "
                   "GPU path drives a VfxGpuPass from its frame.";
    }
    return "";
}

bool device_dispatch_available() noexcept {
    // ONE LINE, ONE FACT, and the fact changed at M10 task 5.1: `src/vfx/gpu/` is the dispatch.
    // Everything above it — the decision, the report, the counts — already read it, which is why
    // this is still one line.
    return true;
}

PathDecision decide_path(const CompiledEmitter& emitter,
                         const DeviceCapability& capability) noexcept {
    PathDecision decision;
    if (emitter.path() == SimulationPath::CpuRequired) {
        decision.path = ExecutionPath::Cpu;
        decision.reason = FallbackReason::EffectRequiresCpu;
        decision.explanation = fallback_explanation(decision.reason);
        decision.is_fallback = false;
        return decision;
    }
    FallbackReason reason = FallbackReason::None;
    if (!capability.gpu_path_enabled) {
        reason = FallbackReason::DisabledByHost;
    } else if (!capability.compute) {
        reason = FallbackReason::DeviceLacksCompute;
    } else if (!capability.indirect_dispatch) {
        reason = FallbackReason::DeviceLacksIndirectDispatch;
    } else if (!device_dispatch_available()) {
        reason = FallbackReason::NoDeviceInThisWorld;
    }
    if (reason == FallbackReason::None) {
        decision.path = ExecutionPath::Gpu;
        decision.explanation = fallback_explanation(FallbackReason::None);
        return decision;
    }
    decision.path = ExecutionPath::Cpu;
    decision.reason = reason;
    decision.explanation = fallback_explanation(reason);
    decision.is_fallback = true;
    return decision;
}

Status execute_kernel(const VfxKernel& kernel, const KernelContext& context, u32 particle,
                      KernelRegisters& registers, KernelResult& out) noexcept {
    out = KernelResult{};
    if (context.layout == nullptr) {
        return fail(ErrorCode::InvalidArgument, "vfx: a kernel evaluation needs a layout");
    }
    const usize wanted = static_cast<usize>(kernel.slot_count()) * kSlotWidth;
    if (registers.size() < wanted) {
        if (Status sized = registers.resize(wanted); !sized) {
            return sized;
        }
    }

    evaluate_program(kernel, context, particle, registers);
    apply_writes(kernel, context, particle, registers);
    if (Status raised = raise_events(kernel, context, particle, registers, out.events_raised);
        !raised) {
        return raised;
    }

    if (kernel.kill_slot() != 0xFFFFU) {
        out.killed = slot_of(registers, kernel.kill_slot())[0] != 0.0F;
    }
    if (kernel.spawn_slot() != 0xFFFFU) {
        const f32 count = slot_of(registers, kernel.spawn_slot())[0];
        out.spawn_count = count > 0.0F ? static_cast<u32>(count) : 0U;
    }
    return ok();
}

// --- Publication ---------------------------------------------------------------------------------

namespace {

/// One live particle's presentation attributes, read through the layout so a quantised attribute
/// arrives quantised.
struct Presentation {
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    f32 size = 0.0F;
    /// Chroma and opacity. Often quantised — the plume declares eight bits for it — which is only
    /// sound because the WIDE-RANGE half of the colour is `emission` below.
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    /// The radiance scale. `ParticleInstance::color` is a LINEAR RADIANCE, not a colour in [0, 1]:
    /// the frame's colour target is scene-referred and the tonemap is downstream, so an emissive
    /// particle is in the same physical range as a light. Splitting the two lets the chroma be
    /// eight bits and the magnitude be a float — which is what makes the derived layout's precision
    /// choice a saving rather than a clamp. An emitter with no `emission` attribute publishes at 1.
    f32 emission = 1.0F;
};

void read_presentation(const SimulationWorld& world, const EffectInstance& instance, u32 emitter,
                       u32 particle, Presentation& out) noexcept {
    static const Name kPosition = Name::intern("position");
    static const Name kSize = Name::intern("size");
    static const Name kColor = Name::intern("color");
    static const Name kEmission = Name::intern("emission");
    for (u32 component = 0; component < 3U; ++component) {
        out.position[component] =
            world.read_attribute(instance, emitter, particle, kPosition, component);
    }
    out.size = world.read_attribute(instance, emitter, particle, kSize, 0);
    for (u32 component = 0; component < 4U; ++component) {
        out.color[component] = world.read_attribute(instance, emitter, particle, kColor, component);
    }
    const AttributeLayout& layout = instance.system->emitters()[emitter].layout();
    const AttributeSlot* emission = layout.find(kEmission);
    out.emission = emission != nullptr && !emission->elided
                       ? world.read_attribute(instance, emitter, particle, kEmission, 0)
                       : 1.0F;
}

}  // namespace

namespace {

/// One emitter's live particles, as sprite records. Split out of `publish_sprites` because the
/// importance-ordered walk and the per-particle conversion are two different things and the sum of
/// them is harder to read than either.
[[nodiscard]] Status publish_emitter_sprites(const SimulationWorld& world,
                                             const EffectInstance& instance, u32 emitter,
                                             const Vec3& camera_position, u32 capacity,
                                             Array<rendering::particles::ParticleInstance>& out,
                                             PublishReport& report) noexcept {
    const u32 block = instance.first_block + emitter;
    if (block >= world.blocks().size()) {
        return ok();
    }
    const u32 count = world.blocks()[block].particles;
    for (u32 particle = 0; particle < count; ++particle) {
        Presentation presentation;
        read_presentation(world, instance, emitter, particle, presentation);
        if (presentation.size <= 0.0F) {
            continue;
        }
        if (out.size() >= capacity) {
            ++report.dropped;
            continue;
        }
        rendering::particles::ParticleInstance record;
        record.position[0] = presentation.position[0] + instance.position.x - camera_position.x;
        record.position[1] = presentation.position[1] + instance.position.y - camera_position.y;
        record.position[2] = presentation.position[2] + instance.position.z - camera_position.z;
        record.size = presentation.size * instance.scale;
        for (u32 channel = 0; channel < 3U; ++channel) {
            record.color[channel] = presentation.color[channel] * presentation.emission;
        }
        record.color[3] = presentation.color[3];
        if (Status pushed = out.push_back(record); !pushed) {
            return pushed;
        }
        ++report.particles;
    }
    return ok();
}

}  // namespace

Status publish_sprites(const SimulationWorld& world, const Vec3& camera_position, u32 capacity,
                       Array<rendering::particles::ParticleInstance>& out,
                       PublishReport& report) noexcept {
    out.clear();
    report = PublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }

    // IMPORTANCE ORDER. When the ring cannot hold the world, the tail that is dropped is the
    // decorative one — which is the same rank the budget controller and the pool reservations use,
    // rather than a third order nobody can reason about together.
    for (u32 rank = 0; rank < kImportanceCount; ++rank) {
        const auto importance = static_cast<ImportanceClass>(rank);
        for (const EffectInstance& instance : world.instances()) {
            if (!instance.active || instance.system == nullptr ||
                instance.importance != importance) {
                continue;
            }
            const u32 emitters = static_cast<u32>(instance.system->emitters().size());
            for (u32 emitter = 0; emitter < emitters; ++emitter) {
                ++report.emitters;
                if (Status published = publish_emitter_sprites(
                        world, instance, emitter, camera_position, capacity, out, report);
                    !published) {
                    return published;
                }
            }
        }
    }
    return ok();
}

Status publish_mesh_instances(const SimulationWorld& world, const Vec3& camera_position,
                              u32 capacity, Array<MeshParticleInstance>& out,
                              PublishReport& report) noexcept {
    out.clear();
    report = PublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }
    static const Name kMesh = Name::intern("mesh");
    static const Name kMaterial = Name::intern("material");

    for (const EffectInstance& instance : world.instances()) {
        if (!instance.active || instance.system == nullptr) {
            continue;
        }
        const Span<const CompiledEmitter> emitters = instance.system->emitters();
        for (u32 emitter = 0; emitter < emitters.size(); ++emitter) {
            // AN EMITTER PUBLISHES MESH INSTANCES ONLY IF ITS GRAPHS PRODUCED A MESH REFERENCE.
            // The layout is the compiler's answer to "what does this emitter have", so asking it is
            // asking the graphs rather than a flag somebody set beside them.
            if (emitters[emitter].layout().find(kMesh) == nullptr) {
                continue;
            }
            ++report.emitters;
            const u32 block = instance.first_block + emitter;
            if (block >= world.blocks().size()) {
                continue;
            }
            const u32 count = world.blocks()[block].particles;
            for (u32 particle = 0; particle < count; ++particle) {
                Presentation presentation;
                read_presentation(world, instance, emitter, particle, presentation);
                if (presentation.size <= 0.0F) {
                    continue;
                }
                if (out.size() >= capacity) {
                    ++report.dropped;
                    continue;
                }
                MeshParticleInstance record;
                const f32 scale = presentation.size * instance.scale;
                record.rows[0] = scale;
                record.rows[5] = scale;
                record.rows[10] = scale;
                record.rows[3] = presentation.position[0] + instance.position.x - camera_position.x;
                record.rows[7] = presentation.position[1] + instance.position.y - camera_position.y;
                record.rows[11] =
                    presentation.position[2] + instance.position.z - camera_position.z;
                record.mesh =
                    static_cast<u32>(world.read_attribute(instance, emitter, particle, kMesh, 0));
                record.material = static_cast<u32>(
                    world.read_attribute(instance, emitter, particle, kMaterial, 0));
                record.radius = scale;
                if (Status pushed = out.push_back(record); !pushed) {
                    return pushed;
                }
                ++report.particles;
            }
        }
    }
    return ok();
}

}  // namespace cy::vfx
