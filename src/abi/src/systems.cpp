// SPDX-License-Identifier: MIT
// A module's systems, in the engine's scheduler. ABI 1.5, `add-swift-m12-gaps`. See
// cy/abi/systems.h.

#include <cy/abi/systems.h>

#include <cy/abi/game/services.h>
#include <cy/core/jobs/access.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/world.h>

#include <atomic>
#include <cstring>

namespace cy::abi {
namespace {

/// `CyAccessMode` in the scheduler's terms. The registry already refused any other value.
Status declare(jobs::AccessSet& access, const CySystemAccess& term) noexcept {
    switch (term.mode) {
        case CY_ACCESS_READ:
            return access.read(term.component);
        case CY_ACCESS_WRITE:
            return access.write(term.component);
        default:
            return access.exclude(term.component);
    }
}

}  // namespace

ScriptSystems::ScriptSystems(Allocator& allocator, Host& host) noexcept
    : allocator_(allocator), host_(host), slots_(allocator) {}

ScriptSystems::~ScriptSystems() {
    for (Slot* slot : slots_) {
        slot->~Slot();
        allocator_.deallocate(static_cast<void*>(slot), sizeof(Slot), alignof(Slot));
    }
}

const ScriptSystems::Slot* ScriptSystems::find(const char* name) const noexcept {
    for (const Slot* slot : slots_) {
        if (std::strcmp(slot->name, name) == 0) {
            return slot;
        }
    }
    return nullptr;
}

Status ScriptSystems::add(ecs::Schedule& schedule, const SystemRecord& record) noexcept {
    ecs::SystemDesc desc;
    desc.name = record.name;
    desc.body = &ScriptSystems::body;
    for (const CySystemAccess& term : record.access) {
        if (Status declared = declare(desc.access, term); !declared) {
            return declared;
        }
    }
    Expected<UniquePtr<Slot>, Error> allocated = make_unique<Slot>(allocator_);
    if (!allocated) {
        return make_unexpected(allocated.error());
    }
    if (Status reserved = slots_.reserve(slots_.size() + 1); !reserved) {
        return reserved;
    }
    Slot* slot = allocated.value().get();
    slot->owner = this;
    slot->name = record.name;
    slot->stage = static_cast<ecs::Stage>(record.stage);
    desc.user = slot;
    Expected<ecs::SystemId, Error> added = schedule.add(slot->stage, desc);
    if (!added) {
        return make_unexpected(added.error());
    }
    slot->id = added.value();
    (void)slots_.push_back(allocated.value().release());
    return ok();
}

Expected<u32, Error> ScriptSystems::install(ecs::Schedule& schedule) noexcept {
    u32 added = 0;
    for (const SystemRecord* record : host_.systems) {
        if (record->generation != host_.generation || find(record->name) != nullptr) {
            continue;
        }
        if (Status installed = add(schedule, *record); !installed) {
            return make_unexpected(installed.error());
        }
        ++added;
    }
    if (Status built = schedule.build(); !built) {
        return make_unexpected(built.error());
    }
    return added;
}

void ScriptSystems::body(const ecs::SystemContext& context) noexcept {
    auto* slot = static_cast<Slot*>(context.user);
    Host& host = slot->owner->host_;
    // THE CURRENT GENERATION'S CODE, resolved by name on every run. See the header: a reload
    // replaces what this slot calls, never the slot.
    const SystemRecord* record = host.find_system(slot->name);
    if (record == nullptr || host.world == nullptr || context.world == nullptr) {
        return;
    }
    // Relaxed: a counter for tests and diagnostics, and two runs of one slot never overlap.
    slot->runs.fetch_add(1, std::memory_order_relaxed);
    const ecs::World::IterationGuard iterating(*context.world);
    record->run(&host, host.world, record->user_data);
}

Status ScriptSystems::run(ecs::Schedule& schedule, ecs::Stage stage,
                          jobs::JobSystem* jobs) noexcept {
    const game::PhaseScope phase(host_.game.clock,
                                 game::phase_of_stage(static_cast<CyStage>(stage)));
    return jobs != nullptr ? schedule.run(stage, *jobs) : schedule.run_serial(stage);
}

ecs::SystemId ScriptSystems::id_of(const char* name) const noexcept {
    const Slot* slot = name == nullptr ? nullptr : find(name);
    return slot == nullptr ? ecs::kInvalidSystem : slot->id;
}

u64 ScriptSystems::runs(const char* name) const noexcept {
    const Slot* slot = name == nullptr ? nullptr : find(name);
    return slot == nullptr ? 0U : slot->runs.load(std::memory_order_relaxed);
}

}  // namespace cy::abi
