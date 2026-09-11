#pragma once
// The compiler's private door into the compiled artefacts. M8.c task 2.1.
//
// A friend rather than a setter per member, for the reason `cy::graph::camera`'s `RigProgramAccess`
// gives: a compiled artefact is immutable to everybody but its compiler, and a public setter is a
// second way to build one that nothing keeps consistent. Two translation units need it — the
// lowering and the Slang emitter — so it is a header rather than a static in one of them.

#include <cy/vfx/compile.h>

#include <utility>

namespace cy::vfx {

/// Fills the private members of `VfxKernel`, `CompiledEmitter` and `CompiledSystem`.
///
/// A friend rather than a setter per member, for the reason `cy::graph::camera`'s
/// `RigProgramAccess` gives: a compiled artefact is immutable to everybody but its compiler, and a
/// public setter is a second way to build one that nothing keeps consistent.
class KernelAccess {
public:
    static void set_name(VfxKernel& kernel, Name name) noexcept { kernel.name_ = name; }
    static void add_stage(VfxKernel& kernel, Stage stage) noexcept {
        kernel.stages_ = static_cast<StageMask>(kernel.stages_ | stage_bit(stage));
    }
    static void set_module(VfxKernel& kernel, Module&& module) noexcept {
        kernel.expressions_ = std::move(module);
    }
    static Array<KernelWrite>& writes(VfxKernel& kernel) noexcept { return kernel.writes_; }
    static Array<KernelEvent>& events(VfxKernel& kernel) noexcept { return kernel.events_; }
    static Array<u16>& event_slots(VfxKernel& kernel) noexcept { return kernel.event_slots_; }
    static Array<KernelStep>& program(VfxKernel& kernel) noexcept { return kernel.program_; }
    static Array<u16>& write_slots(VfxKernel& kernel) noexcept { return kernel.write_slots_; }
    static void set_slots(VfxKernel& kernel, u32 slots) noexcept { kernel.slots_ = slots; }
    static void set_kill_slot(VfxKernel& kernel, u16 slot) noexcept { kernel.kill_slot_ = slot; }
    static void set_spawn_slot(VfxKernel& kernel, u16 slot) noexcept { kernel.spawn_slot_ = slot; }
    static void set_digest(VfxKernel& kernel, u64 digest) noexcept { kernel.digest_ = digest; }
};

class CompiledAccess {
public:
    static void set_name(CompiledEmitter& emitter, Name name) noexcept { emitter.name_ = name; }
    static AttributeLayout& layout(CompiledEmitter& emitter) noexcept { return emitter.layout_; }
    static Array<VfxKernel>& kernels(CompiledEmitter& emitter) noexcept { return emitter.kernels_; }
    static Array<GeneratedSource>& sources(CompiledEmitter& emitter) noexcept {
        return emitter.sources_;
    }
    static void set_path(CompiledEmitter& emitter, SimulationPath path) noexcept {
        emitter.path_ = path;
    }
    static void set_capacity(CompiledEmitter& emitter, u32 capacity) noexcept {
        emitter.capacity_ = capacity;
    }
    static void set_digest(CompiledEmitter& emitter, u64 digest) noexcept {
        emitter.digest_ = digest;
    }

    static void set_name(CompiledSystem& system, Name name) noexcept { system.name_ = name; }
    static Array<CompiledEmitter>& emitters(CompiledSystem& system) noexcept {
        return system.emitters_;
    }
    static Array<ParameterDecl>& parameters(CompiledSystem& system) noexcept {
        return system.parameters_;
    }
    static Array<EventChannelDecl>& channels(CompiledSystem& system) noexcept {
        return system.channels_;
    }
    static void set_importance(CompiledSystem& system, ImportanceClass importance) noexcept {
        system.importance_ = importance;
    }
    static void set_scalability(CompiledSystem& system, const ScalabilityPolicy& policy) noexcept {
        system.scalability_ = policy;
    }
    static void set_cook_key(CompiledSystem& system, u64 key) noexcept { system.cook_key_ = key; }
};

}  // namespace cy::vfx
