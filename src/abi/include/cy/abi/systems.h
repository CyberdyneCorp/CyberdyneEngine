// cy/abi/systems.h — a module's systems, in the engine's scheduler. ABI 1.5, `add-swift-m12-gaps`.
//
// `swift-scripting`: a Swift system declares its access "in the signature so the scheduler can
// parallelise them exactly as it does native systems". `register_system` records the declaration on
// the host; this is the half that puts it in an `ecs::Schedule`, beside the native systems of the
// same stage, ordered by the same conflict rules (`jobs::AccessSet::conflicts_with`).
//
// --- ONE SCHEDULE ENTRY PER NAME, RESOLVED ON EVERY RUN
// -------------------------------------------
//
// `ecs::Schedule` has no removal: a schedule is extended, never re-ordered under a running frame. A
// hot reload therefore cannot replace a scheduled system's entry; it replaces what the entry CALLS.
// Each installed system is a slot holding its name, and the slot's body asks the host for the
// current generation's registration of that name every time it runs — so after a reload the new
// image's code runs with no reinstall, a system the new image dropped runs nothing, and a retired
// image's code is never called. What a reload may not do is change a system's stage or access,
// because that would need a different place in the schedule; `register_system` refuses it and the
// reload is refused with it (`ReloadFailure::SystemChanged`).
//
// --- THE PHASE AND THE WORLD WHILE A BODY RUNS
// ----------------------------------------------------
//
// `run()` sets the host clock's phase for the whole stage (`phase_of_stage`), once, on the calling
// thread, before any job is dispatched — never per system, because two systems of one batch run at
// once on different workers and the clock is not written from a worker. While a body runs the world
// is held as iterating (`ecs::World::IterationGuard`), so every structural ABI entry answers
// UNAVAILABLE instead of moving the chunk a parallel system is reading.

#pragma once

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/system.h>

namespace cy::jobs {
class JobSystem;
}  // namespace cy::jobs

namespace cy::abi {

/// The script systems of one host, installed into one schedule.
///
/// NOT THREAD-SAFE to install, like `BehaviourRuntime`: `install()` runs at the frame boundary. The
/// slots' bodies run wherever the schedule puts them, and read only the host's registry.
class ScriptSystems {
public:
    ScriptSystems(Allocator& allocator, Host& host) noexcept;
    ~ScriptSystems();

    ScriptSystems(const ScriptSystems&) = delete;
    ScriptSystems& operator=(const ScriptSystems&) = delete;
    ScriptSystems(ScriptSystems&&) = delete;
    ScriptSystems& operator=(ScriptSystems&&) = delete;

    /// Add every system the current generation registered that is not in `schedule` yet, and
    /// rebuild it. Call after loading a module and after every reload; an unchanged module adds
    /// nothing. Returns how many were added. The schedule must outlive this object.
    [[nodiscard]] Expected<u32, Error> install(ecs::Schedule& schedule) noexcept;

    /// Run one stage under its phase: on `jobs` when it is non-null, serially otherwise.
    [[nodiscard]] Status run(ecs::Schedule& schedule, ecs::Stage stage,
                             jobs::JobSystem* jobs) noexcept;

    /// The scheduler's id for an installed system, or `ecs::kInvalidSystem`. For a caller asking
    /// the schedule whether it ordered a script system against a native one.
    [[nodiscard]] ecs::SystemId id_of(const char* name) const noexcept;
    /// Systems installed so far.
    [[nodiscard]] u32 installed() const noexcept { return static_cast<u32>(slots_.size()); }
    /// Times `name`'s body was called, across every generation. Zero for an unknown name.
    [[nodiscard]] u64 runs(const char* name) const noexcept;

private:
    struct Slot {
        ScriptSystems* owner = nullptr;
        const char* name = "";
        ecs::Stage stage = ecs::Stage::Simulation;
        ecs::SystemId id = ecs::kInvalidSystem;
        u64 runs = 0;
    };

    static void body(const ecs::SystemContext& context) noexcept;
    [[nodiscard]] const Slot* find(const char* name) const noexcept;
    [[nodiscard]] Status add(ecs::Schedule& schedule, const SystemRecord& record) noexcept;

    Allocator& allocator_;
    Host& host_;
    /// Individually allocated: the schedule holds each slot's address as its body's `user`.
    Array<Slot*> slots_;
};

}  // namespace cy::abi
