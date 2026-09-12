// The unified simulation world, the shared particle pool and the global scheduler. M8.c task 2.4.
// See world.h for the requirement behind every decision here.

#include <cy/vfx/world.h>

#include <cy/vfx/runtime.h>

#include <algorithm>
#include <utility>

namespace cy::vfx {
namespace {

/// The default simulation frequency of each importance class, before the budget controller and the
/// effect's own policy have their say. `vfx-system`'s "Distant smoke is cheap" scenario is the
/// bottom row.
constexpr f32 kDefaultHz[kImportanceCount] = {60.0F, 60.0F, 30.0F, 15.0F};

/// The largest number of sub-steps one frame may run for one instance. A frame that stalled for a
/// second must not then simulate sixty steps of every effect and stall again — `vfx-system` does
/// not name this and every fixed-step loop needs it.
constexpr u32 kMaxSubsteps = 4;

}  // namespace

// --- ParticlePool
// ---------------------------------------------------------------------------------

Status ParticlePool::initialize(u64 bytes, const f32 reserved_fraction[kImportanceCount]) noexcept {
    if (bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "vfx: a particle pool of no bytes holds nothing");
    }
    if (Status sized = memory_.resize(static_cast<usize>(bytes)); !sized) {
        return sized;
    }
    for (u8& byte : memory_) {
        byte = 0;
    }
    report_ = PoolReport{};
    report_.total_bytes = bytes;
    f32 total_fraction = 0.0F;
    for (u32 which = 0; which < kImportanceCount; ++which) {
        total_fraction += reserved_fraction[which];
    }
    if (total_fraction > 1.0F) {
        return fail(
            ErrorCode::InvalidArgument,
            "vfx: the per-importance reservations sum to more than the pool — a reservation "
            "nothing can honour is worse than none");
    }
    for (u32 which = 0; which < kImportanceCount; ++which) {
        report_.reserved_bytes[which] =
            static_cast<u64>(static_cast<f64>(bytes) * static_cast<f64>(reserved_fraction[which]));
    }
    free_list_.clear();
    high_water_ = 0;
    return ok();
}

u64 ParticlePool::free_bytes() const noexcept {
    u64 free = report_.total_bytes - high_water_;
    for (const FreeBlock& block : free_list_) {
        free += block.bytes;
    }
    return free;
}

u64 ParticlePool::available_to(ImportanceClass importance) const noexcept {
    // THE RESERVATION RULE, IN ONE PLACE. A class may take what is free MINUS whatever every OTHER
    // class still has unclaimed of its own reservation. So a decorative request cannot reach into
    // a critical reservation even when the rest of the pool is empty, which is the whole of
    // "a high-volume decorative effect cannot starve a critical one".
    const u64 free = free_bytes();
    u64 protected_bytes = 0;
    for (u32 which = 0; which < kImportanceCount; ++which) {
        if (which == static_cast<u32>(importance)) {
            continue;
        }
        const u64 reserved = report_.reserved_bytes[which];
        const u64 used = report_.used_by_class[which];
        protected_bytes += used >= reserved ? 0 : reserved - used;
    }
    return free > protected_bytes ? free - protected_bytes : 0;
}

Expected<PoolBlock, Error> ParticlePool::acquire(ImportanceClass importance, u32 wanted,
                                                 u32 bytes_per_particle) noexcept {
    if (bytes_per_particle == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "vfx: an emitter with a zero-byte layout has no particles to allocate");
    }
    const u64 available = available_to(importance);
    u32 granted = wanted;
    const u64 asked = static_cast<u64>(wanted) * bytes_per_particle;
    if (asked > available) {
        // REDUCED, NOT REFUSED. "spawn requests SHALL be reduced by importance rank and the
        // shortfall reported, rather than overwriting live particles or failing the frame."
        granted = static_cast<u32>(available / bytes_per_particle);
        report_.shortfall_particles += wanted - granted;
        ++report_.reduced_requests;
    }
    PoolBlock block;
    block.importance = importance;
    block.particles = granted;
    block.bytes = static_cast<u64>(granted) * bytes_per_particle;
    if (granted == 0) {
        return block;
    }

    for (usize index = 0; index < free_list_.size(); ++index) {
        if (free_list_[index].bytes < block.bytes) {
            continue;
        }
        block.offset = free_list_[index].offset;
        free_list_[index].offset += block.bytes;
        free_list_[index].bytes -= block.bytes;
        if (free_list_[index].bytes == 0) {
            free_list_.remove_unordered(index);
        }
        report_.used_bytes += block.bytes;
        report_.used_by_class[static_cast<u32>(importance)] += block.bytes;
        return block;
    }

    if (high_water_ + block.bytes > report_.total_bytes) {
        // The free list is fragmented past the point where this fits. Reduce rather than fail, for
        // the same reason the reservation check does.
        const u64 room = report_.total_bytes - high_water_;
        granted = static_cast<u32>(room / bytes_per_particle);
        report_.shortfall_particles += block.particles - granted;
        ++report_.reduced_requests;
        block.particles = granted;
        block.bytes = static_cast<u64>(granted) * bytes_per_particle;
        if (granted == 0) {
            return block;
        }
    }
    block.offset = high_water_;
    high_water_ += block.bytes;
    report_.used_bytes += block.bytes;
    report_.used_by_class[static_cast<u32>(importance)] += block.bytes;
    return block;
}

void ParticlePool::release(const PoolBlock& block) noexcept {
    if (block.bytes == 0) {
        return;
    }
    report_.used_bytes -= block.bytes;
    const u32 which = static_cast<u32>(block.importance);
    report_.used_by_class[which] = report_.used_by_class[which] >= block.bytes
                                       ? report_.used_by_class[which] - block.bytes
                                       : 0;
    if (block.offset + block.bytes == high_water_) {
        high_water_ = block.offset;
        return;
    }
    (void)free_list_.push_back(FreeBlock{block.offset, block.bytes});
}

Span<u8> ParticlePool::bytes_at(const PoolBlock& block) noexcept {
    if (block.offset + block.bytes > memory_.size()) {
        return {};
    }
    return {memory_.data() + block.offset, static_cast<usize>(block.bytes)};
}

Span<const u8> ParticlePool::bytes_at(const PoolBlock& block) const noexcept {
    if (block.offset + block.bytes > memory_.size()) {
        return {};
    }
    return {memory_.data() + block.offset, static_cast<usize>(block.bytes)};
}

// --- SimulationWorld
// ------------------------------------------------------------------------------

SimulationWorld::SimulationWorld(Allocator& allocator) noexcept
    : pool_(allocator),
      events_(allocator),
      readback_(allocator),
      instances_(allocator),
      blocks_(allocator),
      alive_(allocator),
      alive_offset_(allocator),
      parameters_(allocator),
      parameter_offset_(allocator) {}

Status SimulationWorld::initialize(const WorldDescription& description) noexcept {
    description_ = description;
    if (Status made = pool_.initialize(description.pool_bytes, description.reserved_fraction);
        !made) {
        return made;
    }
    readback_.set_budget(description.readback_bytes_per_frame);
    instances_.clear();
    blocks_.clear();
    alive_.clear();
    alive_offset_.clear();
    parameters_.clear();
    parameter_offset_.clear();
    last_step_ = StepReport{};
    next_handle_ = 1;
    ready_ = true;
    return ok();
}

void SimulationWorld::shutdown() noexcept {
    for (const PoolBlock& block : blocks_) {
        pool_.release(block);
    }
    instances_.clear();
    blocks_.clear();
    alive_.clear();
    alive_offset_.clear();
    parameters_.clear();
    parameter_offset_.clear();
    ready_ = false;
}

Span<u8> SimulationWorld::alive_flags(u32 block) noexcept {
    if (block >= blocks_.size() || block >= alive_offset_.size()) {
        return {};
    }
    const u32 offset = alive_offset_[block];
    const u32 count = blocks_[block].particles;
    if (static_cast<usize>(offset) + count > alive_.size()) {
        return {};
    }
    return {alive_.data() + offset, count};
}

Span<const u8> SimulationWorld::alive_flags(u32 block) const noexcept {
    if (block >= blocks_.size() || block >= alive_offset_.size()) {
        return {};
    }
    const u32 offset = alive_offset_[block];
    const u32 count = blocks_[block].particles;
    if (static_cast<usize>(offset) + count > alive_.size()) {
        return {};
    }
    return {alive_.data() + offset, count};
}

namespace {

/// A retired instance whose block range is the right size, or null. Reusing one is what stops a
/// fire-and-forget effect played every frame from growing four arrays without bound.
[[nodiscard]] EffectInstance* reusable(Array<EffectInstance>& instances, usize emitters) noexcept {
    for (EffectInstance& candidate : instances) {
        if (!candidate.active && candidate.block_count == emitters) {
            return &candidate;
        }
    }
    return nullptr;
}

}  // namespace

Status SimulationWorld::acquire_blocks(const CompiledSystem& system, u32 first_block,
                                       bool reusing) noexcept {
    const Span<const CompiledEmitter> emitters = system.emitters();
    for (u32 which = 0; which < emitters.size(); ++which) {
        const u32 bytes = emitters[which].layout().bytes_per_particle();
        auto block = pool_.acquire(system.importance(), emitters[which].capacity(), bytes);
        if (!block) {
            return make_unexpected(block.error());
        }
        if (reusing) {
            const u32 slot = first_block + which;
            pool_.release(blocks_[slot]);
            blocks_[slot] = *block;
            continue;
        }
        if (Status pushed = blocks_.push_back(*block); !pushed) {
            return pushed;
        }
        if (Status pushed = alive_offset_.push_back(static_cast<u32>(alive_.size())); !pushed) {
            return pushed;
        }
        if (Status sized = alive_.resize(alive_.size() + block->particles); !sized) {
            return sized;
        }
    }
    return ok();
}

void SimulationWorld::write_parameters(const EffectInstance& instance,
                                       Span<const f32> overrides) noexcept {
    const Span<const ParameterDecl> declared = instance.system->parameters();
    const usize base = instance.parameter_base;
    for (usize index = 0; index < declared.size(); ++index) {
        for (u32 component = 0; component < 4U; ++component) {
            const usize word = base + (index * 4U) + component;
            if (word < parameters_.size()) {
                parameters_[word] = declared[index].value[component];
            }
        }
    }
    for (usize index = 0; index < overrides.size(); ++index) {
        if (base + index < parameters_.size()) {
            parameters_[base + index] = overrides[index];
        }
    }
}

Expected<EffectHandle, Error> SimulationWorld::play(const CompiledSystem& system,
                                                    const EffectSpawn& spawn) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "vfx: the simulation world is not initialized");
    }
    const Span<const CompiledEmitter> emitters = system.emitters();
    if (emitters.empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx: a system with no emitters plays nothing");
    }

    EffectInstance* reused = reusable(instances_, emitters.size());
    if (reused == nullptr && instances_.size() >= description_.max_instances) {
        return fail(ErrorCode::OutOfRange,
                    "vfx: the simulation world holds its declared maximum number of instances");
    }

    EffectInstance instance;
    instance.handle = next_handle_++;
    instance.system = &system;
    instance.position = spawn.position;
    instance.scale = spawn.scale;
    instance.importance = system.importance();
    instance.release_on_completion = spawn.release_on_completion;
    instance.simulation_hz = spawn.simulation_hz > 0.0F
                                 ? spawn.simulation_hz
                                 : kDefaultHz[static_cast<u32>(system.importance())];
    instance.block_count = static_cast<u32>(emitters.size());
    instance.first_block =
        reused != nullptr ? reused->first_block : static_cast<u32>(blocks_.size());

    // THE SYSTEM'S CHANNELS ARE DECLARED WHEN IT IS PLAYED, not by the caller. A compiled system
    // carries its own channel declarations — both bounds included — and a kernel that raises on one
    // the router has never heard of would fail the whole step. `AlreadyExists` is the ordinary case
    // for the second instance of an effect and is not an error.
    for (const EventChannelDecl& channel : system.channels()) {
        if (Status declared = events_.declare(channel);
            !declared && declared.error().code != ErrorCode::AlreadyExists) {
            return make_unexpected(declared.error());
        }
    }

    if (Status acquired = acquire_blocks(system, instance.first_block, reused != nullptr);
        !acquired) {
        return make_unexpected(acquired.error());
    }
    for (u32 which = 0; which < emitters.size(); ++which) {
        for (u8& flag : alive_flags(instance.first_block + which)) {
            flag = 0;
        }
    }

    if (reused != nullptr) {
        instance.parameter_base = reused->parameter_base;
    } else {
        instance.parameter_base = static_cast<u32>(parameters_.size());
        if (Status pushed = parameter_offset_.push_back(instance.parameter_base); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status sized =
                parameters_.resize(parameters_.size() + (system.parameters().size() * 4U));
            !sized) {
            return make_unexpected(sized.error());
        }
    }

    if (reused != nullptr) {
        *reused = instance;
    } else if (Status pushed = instances_.push_back(instance); !pushed) {
        return make_unexpected(pushed.error());
    }
    EffectInstance& stored = reused != nullptr ? *reused : instances_[instances_.size() - 1];
    write_parameters(stored, spawn.parameter_values);
    return stored.handle;
}

const EffectInstance* SimulationWorld::find(EffectHandle handle) const noexcept {
    for (const EffectInstance& instance : instances_) {
        if (instance.handle == handle && instance.active) {
            return &instance;
        }
    }
    return nullptr;
}

Status SimulationWorld::stop(EffectHandle handle, bool allow_completion) noexcept {
    for (EffectInstance& instance : instances_) {
        if (instance.handle != handle || !instance.active) {
            continue;
        }
        // GRACEFUL STOP. "WHEN an effect is stopped by allowing completion THEN spawning SHALL
        // cease and existing particles SHALL live out their lifetimes."
        instance.spawning = false;
        if (!allow_completion) {
            return release_instance(instance);
        }
        return ok();
    }
    return fail(ErrorCode::NotFound, "vfx: no playing effect has that handle");
}

Status SimulationWorld::release_instance(EffectInstance& instance) noexcept {
    for (u32 which = 0; which < instance.block_count; ++which) {
        const u32 block = instance.first_block + which;
        if (block < blocks_.size()) {
            pool_.release(blocks_[block]);
            blocks_[block].bytes = 0;
            blocks_[block].particles = 0;
        }
    }
    instance.active = false;
    instance.spawning = false;
    instance.live_particles = 0;
    instance.system = nullptr;
    return ok();
}

Status SimulationWorld::set_parameter(EffectHandle handle, Name parameter,
                                      Span<const f32> value) noexcept {
    for (EffectInstance& instance : instances_) {
        if (instance.handle != handle || !instance.active || instance.system == nullptr) {
            continue;
        }
        const Span<const ParameterDecl> declared = instance.system->parameters();
        for (usize index = 0; index < declared.size(); ++index) {
            if (declared[index].name != parameter) {
                continue;
            }
            for (usize component = 0; component < value.size() && component < 4U; ++component) {
                const usize word = instance.parameter_base + (index * 4U) + component;
                if (word < parameters_.size()) {
                    parameters_[word] = value[component];
                }
            }
            return ok();
        }
        return fail(ErrorCode::NotFound, "vfx: this system declares no parameter of that name");
    }
    return fail(ErrorCode::NotFound, "vfx: no playing effect has that handle");
}

Status SimulationWorld::set_transform(EffectHandle handle, const Vec3& position) noexcept {
    for (EffectInstance& instance : instances_) {
        if (instance.handle == handle && instance.active) {
            instance.position = position;
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "vfx: no playing effect has that handle");
}

f32 SimulationWorld::read_attribute(const EffectInstance& instance, u32 emitter, u32 particle,
                                    Name attribute, u32 component) const noexcept {
    if (instance.system == nullptr || emitter >= instance.block_count) {
        return 0.0F;
    }
    const Span<const CompiledEmitter> emitters = instance.system->emitters();
    if (emitter >= emitters.size()) {
        return 0.0F;
    }
    const u32 block = instance.first_block + emitter;
    if (block >= blocks_.size() || particle >= blocks_[block].particles) {
        return 0.0F;
    }
    const u32 offset = alive_offset_[block];
    if (static_cast<usize>(offset) + particle >= alive_.size() || alive_[offset + particle] == 0) {
        return 0.0F;
    }
    const AttributeSlot* slot = emitters[emitter].layout().find(attribute);
    if (slot == nullptr || slot->elided) {
        return 0.0F;
    }
    return load_component(pool_.bytes_at(blocks_[block]), *slot, particle, component);
}

namespace {

[[nodiscard]] Status note_dispatch(Array<SimulationWorld::DispatchGroup>& groups, u64 kernel_digest,
                                   u64 layout_digest) noexcept {
    for (SimulationWorld::DispatchGroup& group : groups) {
        if (group.kernel_digest == kernel_digest && group.layout_digest == layout_digest) {
            ++group.members;
            return ok();
        }
    }
    return groups.push_back(SimulationWorld::DispatchGroup{kernel_digest, layout_digest, 1});
}

}  // namespace

Status SimulationWorld::step(f32 dt, StepReport& report) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "vfx: the simulation world is not initialized");
    }
    report = StepReport{};
    events_.begin_frame();
    if (Status advanced = readback_.begin_frame(); !advanced) {
        return advanced;
    }

    Array<DispatchGroup> groups(instances_.allocator());
    for (EffectInstance& instance : instances_) {
        if (!instance.active || instance.system == nullptr) {
            continue;
        }
        ++report.instances;
        if (Status simulated = simulate_instance(instance, dt, report, groups); !simulated) {
            return simulated;
        }
        if (instance.live_particles == 0 && !instance.spawning && instance.release_on_completion) {
            // FIRE AND FORGET: "it SHALL play to completion and release itself with no handle
            // retained."
            if (Status released = release_instance(instance); !released) {
                return released;
            }
        }
    }

    // MERGED DISPATCHES, over the WHOLE world. `vfx-system`'s scenario is four hundred instances of
    // one explosion simulated "by a small number of merged dispatches, not 400 separate ones", so
    // the grouping has to span instances or the number it reports is the one it set out to reduce.
    report.dispatches_merged = static_cast<u32>(groups.size());
    last_step_ = report;
    return ok();
}

namespace {

/// Everything one emitter's sub-step needs. A structure rather than eleven arguments, because the
/// three halves below — spawn, initialise, update — each need most of it and a parameter list that
/// long is a parameter list that gets reordered wrongly once.
struct EmitterPass {
    const CompiledEmitter* emitter = nullptr;
    KernelContext context;
    Span<u8> alive;
    KernelRegisters* registers = nullptr;
    Array<SimulationWorld::DispatchGroup>* groups = nullptr;
    StepReport* report = nullptr;
    /// The block's own particle count, and the cap the budget controller set within it.
    u32 block_capacity = 0;
    u32 capacity = 0;
    f32 spawn_scale = 1.0F;
    bool spawning = true;
};

/// Note a dispatch and record its group in one place, so "counted but not grouped" cannot happen.
[[nodiscard]] Status dispatch(EmitterPass& pass, const VfxKernel& kernel) noexcept {
    ++pass.report->dispatches_unmerged;
    return note_dispatch(*pass.groups, kernel.digest(), pass.emitter->layout().digest());
}

/// How many particles the Spawn stage asked for, scaled by the controller's spawn lever.
[[nodiscard]] Expected<u32, Error> run_spawn(EmitterPass& pass) noexcept {
    if (!pass.spawning) {
        return 0U;
    }
    const VfxKernel* spawn = pass.emitter->kernel_for(Stage::Spawn);
    if (spawn == nullptr) {
        return 0U;
    }
    if (Status noted = dispatch(pass, *spawn); !noted) {
        return make_unexpected(noted.error());
    }
    KernelResult result;
    if (Status ran = execute_kernel(*spawn, pass.context, 0, *pass.registers, result); !ran) {
        return make_unexpected(ran.error());
    }
    return static_cast<u32>(static_cast<f32>(result.spawn_count) * pass.spawn_scale);
}

/// Initialise `wanted` free slots. With fusion on, this kernel also advances them by one sub-step,
/// which is why they are marked 2 rather than 1 — see the update loop.
[[nodiscard]] Status run_initialise(EmitterPass& pass, u32 wanted) noexcept {
    const VfxKernel* initialise = pass.emitter->kernel_for(Stage::Initialise);
    if (wanted == 0 || initialise == nullptr) {
        return ok();
    }
    if (Status noted = dispatch(pass, *initialise); !noted) {
        return noted;
    }
    u32 spawned = 0;
    for (u32 particle = 0; particle < pass.capacity && spawned < wanted; ++particle) {
        if (pass.alive[particle] != 0) {
            continue;
        }
        pass.context.spawn_index = spawned;
        pass.context.normalised_age = 0.0F;
        KernelResult result;
        if (Status ran =
                execute_kernel(*initialise, pass.context, particle, *pass.registers, result);
            !ran) {
            return ran;
        }
        // TWO, NOT ONE. A particle initialised by the FUSED kernel has already been advanced by
        // this sub-step, and running the update kernel over it as well would apply gravity twice on
        // its first frame. The marker is cleared in the loop below, so it costs one byte and no
        // branch anywhere else.
        pass.alive[particle] = 2;
        ++spawned;
        ++pass.report->spawned;
    }
    return ok();
}

/// Advance every live particle, and return how many survived.
[[nodiscard]] Expected<u32, Error> run_update(EmitterPass& pass) noexcept {
    const VfxKernel* update = pass.emitter->kernel_for(Stage::Update);
    if (update != nullptr) {
        if (Status noted = dispatch(pass, *update); !noted) {
            return make_unexpected(noted.error());
        }
    }
    u32 live = 0;
    for (u32 particle = 0; particle < pass.block_capacity; ++particle) {
        if (pass.alive[particle] == 0) {
            continue;
        }
        if (particle >= pass.capacity) {
            // Above the cap the controller set. Killed rather than left simulating, because a cap
            // that only stopped new spawns is a cap that does nothing to a full block.
            pass.alive[particle] = 0;
            ++pass.report->killed;
            continue;
        }
        if (pass.alive[particle] == 2) {
            pass.alive[particle] = 1;
            ++live;
            continue;
        }
        if (update == nullptr) {
            ++live;
            continue;
        }
        KernelResult result;
        if (Status ran = execute_kernel(*update, pass.context, particle, *pass.registers, result);
            !ran) {
            return make_unexpected(ran.error());
        }
        if (result.killed) {
            pass.alive[particle] = 0;
            ++pass.report->killed;
            continue;
        }
        ++live;
    }
    return live;
}

/// The particle cap this emitter runs under: the controller's fraction of the block, floored by the
/// effect's own declared reservation and never above the block itself.
[[nodiscard]] u32 capped(u32 block_capacity, f32 count_cap_scale, u32 reserved) noexcept {
    const auto scaled = static_cast<u32>(static_cast<f32>(block_capacity) * count_cap_scale);
    u32 capacity = std::max(scaled, reserved);
    capacity = std::min(capacity, block_capacity);
    return capacity == 0 ? 1U : capacity;
}

}  // namespace

Status SimulationWorld::simulate_emitter(EffectInstance& instance, u32 which,
                                         const BudgetLevers& levers, f32 substep,
                                         KernelRegisters& registers, StepReport& report,
                                         Array<DispatchGroup>& groups, u32& live) noexcept {
    live = 0;
    const CompiledEmitter& emitter = instance.system->emitters()[which];
    const u32 block = instance.first_block + which;
    if (block >= blocks_.size() || blocks_[block].particles == 0) {
        return ok();
    }
    ++report.active_emitters;

    // THE PATH IS DECIDED AND COUNTED EVERY SUB-STEP, never assumed. A fallback is a number in the
    // report rather than a silence — `runtime.h` is where the reason lives.
    // THIS WORLD HAS NO DEVICE, so `gpu_path_enabled` is not what is false here — the dispatch
    // exists (M10 task 5.1 built it, in `cy::vfx-gpu`) and this object cannot reach it. Declaring
    // it rather than fabricating a capable device is what keeps the count true: every emitter
    // stepped by a `SimulationWorld` runs the CPU executor below, and a report that said
    // `gpu_emitters` on a frame this function integrated by hand would be a false green of exactly
    // the shape `StepReport::cpu_fallbacks` exists to prevent.
    DeviceCapability capability;
    capability.indirect_dispatch = false;
    PathDecision decision = decide_path(emitter, capability);
    if (decision.reason == FallbackReason::DeviceLacksIndirectDispatch) {
        decision.reason = FallbackReason::NoDeviceInThisWorld;
        decision.explanation = fallback_explanation(decision.reason);
    }
    ++report.cpu_emitters;
    report.cpu_fallbacks += decision.is_fallback ? 1U : 0U;

    EmitterPass pass;
    pass.emitter = &emitter;
    pass.context.layout = &emitter.layout();
    pass.context.storage = pool_.bytes_at(blocks_[block]);
    pass.context.parameters = {parameters_.data() + instance.parameter_base,
                               instance.system->parameters().size() * 4U};
    pass.context.parameter_decls = instance.system->parameters();
    pass.context.dt = substep;
    pass.context.emitter_age = instance.age;
    pass.context.events = &events_;
    pass.alive = alive_flags(block);
    pass.registers = &registers;
    pass.groups = &groups;
    pass.report = &report;
    pass.block_capacity = blocks_[block].particles;
    // THE PARTICLE COUNT CAP, one of the six levers `vfx-system` names. Without it the controller
    // can only slow spawning, and an effect that has already filled its block stays full however
    // overloaded the frame is — so "cost is bounded by configuration" would be a claim about the
    // ramp rather than about the steady state.
    pass.capacity = capped(pass.block_capacity, levers.count_cap_scale,
                           instance.system->scalability().reserved_particles);
    pass.spawn_scale = levers.spawn_scale;
    pass.spawning = instance.spawning;

    auto wanted = run_spawn(pass);
    if (!wanted) {
        return make_unexpected(wanted.error());
    }
    if (Status initialised = run_initialise(pass, *wanted); !initialised) {
        return initialised;
    }
    auto surviving = run_update(pass);
    if (!surviving) {
        return make_unexpected(surviving.error());
    }
    live = *surviving;
    return ok();
}

Status SimulationWorld::simulate_instance(EffectInstance& instance, f32 dt, StepReport& report,
                                          Array<DispatchGroup>& groups) noexcept {
    const BudgetLevers levers =
        budget_.levers_for(instance.importance, instance.system->scalability());

    // DECOUPLED SIMULATION FREQUENCY. The instance's own hertz, reduced by the budget controller
    // and floored by the effect's policy, decides how many sub-steps this frame runs — which may be
    // none, and that is the whole point of the decoupling.
    const f32 hz = std::min(levers.simulation_hz, instance.simulation_hz);
    const f32 substep = hz > 0.0F ? 1.0F / hz : dt;
    instance.accumulator += dt;
    instance.age += dt;

    KernelRegisters registers(instances_.allocator());
    const u32 emitters = instance.block_count;

    u32 substeps = 0;
    while (instance.accumulator >= substep && substeps < kMaxSubsteps) {
        instance.accumulator -= substep;
        ++substeps;
        ++report.substeps;
        u32 live_this_substep = 0;
        for (u32 which = 0; which < emitters; ++which) {
            u32 live = 0;
            if (Status simulated = simulate_emitter(instance, which, levers, substep, registers,
                                                    report, groups, live);
                !simulated) {
                return simulated;
            }
            live_this_substep += live;
        }
        instance.live_particles = live_this_substep;
    }

    // THE POPULATION IS REPORTED WHETHER OR NOT A SUB-STEP RAN, and that is not bookkeeping: an
    // 8 Hz effect at 120 frames a second simulates on one frame in fifteen, and a report that said
    // "zero live particles" on the other fourteen would make every population number a function of
    // which frame it was read on. `instance.live_particles` persists across the frames that
    // simulate nothing, which is exactly what the renderer draws on those frames too.
    report.live_particles += instance.live_particles;
    report.particles_by_importance[static_cast<u32>(instance.importance)] +=
        instance.live_particles;

    instance.interpolation_alpha = substep > 0.0F ? instance.accumulator / substep : 0.0F;
    if (instance.accumulator > substep * static_cast<f32>(kMaxSubsteps)) {
        // A long stall does not become a burst of sub-steps next frame either.
        instance.accumulator = substep;
    }
    return ok();
}

}  // namespace cy::vfx
