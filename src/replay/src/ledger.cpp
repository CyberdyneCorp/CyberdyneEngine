// The side-effect ledger: what must not be applied twice when a rollback re-simulates. M9 task 1.4.

#include <cy/replay/ledger.h>

#include <algorithm>

namespace cy::replay {

const char* speculation_name(Speculation value) noexcept {
    return value == Speculation::Speculative ? "Speculative" : "ConfirmedOnly";
}

const char* reconciliation_name(Reconciliation value) noexcept {
    switch (value) {
        case Reconciliation::Cancel:
            return "Cancel";
        case Reconciliation::AllowToFinish:
            return "AllowToFinish";
        case Reconciliation::Correct:
            return "Correct";
    }
    return "Cancel";
}

const char* effect_verdict_name(EffectVerdict verdict) noexcept {
    switch (verdict) {
        case EffectVerdict::Realise:
            return "Realise";
        case EffectVerdict::SuppressedAlreadyRealised:
            return "SuppressedAlreadyRealised";
        case EffectVerdict::DeferredUntilConfirmed:
            return "DeferredUntilConfirmed";
        case EffectVerdict::Undeclared:
            return "Undeclared";
    }
    return "Undeclared";
}

Status SideEffectLedger::declare(const EffectDeclaration& declaration) noexcept {
    if (declaration.kind == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: zero is the null effect identity and cannot be declared");
    }
    if (find(declaration.kind) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "replay: this effect kind is already declared");
    }
    if (Status added = declarations_.push_back(declaration); !added) {
        return added;
    }
    ++report_.kinds_declared;
    return ok();
}

const EffectDeclaration* SideEffectLedger::find(u64 kind) const noexcept {
    for (const EffectDeclaration& declaration : declarations_) {
        if (declaration.kind == kind) {
            return &declaration;
        }
    }
    return nullptr;
}

EffectEntry* SideEffectLedger::find_entry(u64 kind, u64 instance, u64 tick) noexcept {
    // The key is (kind, instance, tick) and NOT the (epoch, tick) pair. See the header: a rollback
    // advances the epoch, so a pair key would never match the entry the first simulation wrote and
    // every explosion would play twice.
    for (EffectEntry& entry : entries_) {
        if (entry.kind == kind && entry.instance == instance && entry.tick == tick) {
            return &entry;
        }
    }
    return nullptr;
}

EffectVerdict SideEffectLedger::realise(u64 kind, u64 instance, determinism::SimulationPoint at,
                                        bool predicted) noexcept {
    const EffectDeclaration* declaration = find(kind);
    if (declaration == nullptr) {
        ++report_.undeclared;
        return EffectVerdict::Undeclared;
    }

    if (EffectEntry* existing = find_entry(kind, instance, at.tick); existing != nullptr) {
        if (existing->confirmed || !predicted) {
            // Already realised at this tick. **The explosion that does not play twice.**
            ++report_.suppressed;
            return EffectVerdict::SuppressedAlreadyRealised;
        }
        if (declaration->speculation == Speculation::ConfirmedOnly) {
            ++report_.deferred;
            return EffectVerdict::DeferredUntilConfirmed;
        }
        ++report_.suppressed;
        return EffectVerdict::SuppressedAlreadyRealised;
    }

    if (predicted && declaration->speculation == Speculation::ConfirmedOnly) {
        // Held rather than dropped: `confirm()` lets it through when the authority catches up. An
        // achievement waits; it does not vanish.
        EffectEntry entry;
        entry.kind = kind;
        entry.instance = instance;
        entry.tick = at.tick;
        entry.first_epoch = at.epoch;
        entry.speculative = true;
        entry.confirmed = false;
        if (Status added = entries_.push_back(entry); !added) {
            drop_oldest();
            (void)entries_.push_back(entry);
        }
        ++report_.deferred;
        return EffectVerdict::DeferredUntilConfirmed;
    }

    EffectEntry entry;
    entry.kind = kind;
    entry.instance = instance;
    entry.tick = at.tick;
    entry.first_epoch = at.epoch;
    entry.speculative = predicted;
    entry.confirmed = !predicted;
    if (entries_.size() >= capacity_) {
        drop_oldest();
    }
    if (Status added = entries_.push_back(entry); !added) {
        // Out of memory rather than out of capacity. The effect still plays — refusing to play it
        // would turn an allocation failure into a gameplay difference — and the entry is lost,
        // which is what `dropped_for_capacity` counts.
        ++report_.dropped_for_capacity;
    }
    ++report_.realised;
    return EffectVerdict::Realise;
}

u32 SideEffectLedger::confirm(u64 tick) noexcept {
    u32 released = 0;
    for (EffectEntry& entry : entries_) {
        if (entry.tick <= tick && entry.speculative && !entry.confirmed && !entry.invalidated) {
            entry.confirmed = true;
            ++released;
        }
    }
    return released;
}

Reconciliation SideEffectLedger::invalidate(u64 kind, u64 instance, u64 tick) noexcept {
    const EffectDeclaration* declaration = find(kind);
    const Reconciliation policy =
        declaration != nullptr ? declaration->reconciliation : Reconciliation::Cancel;
    if (EffectEntry* entry = find_entry(kind, instance, tick); entry != nullptr) {
        if (!entry->invalidated) {
            entry->invalidated = true;
            ++report_.invalidated;
        }
    }
    return policy;
}

void SideEffectLedger::prune_before(u64 tick) noexcept {
    usize kept = 0;
    for (const EffectEntry& entry : entries_) {
        if (entry.tick >= tick) {
            entries_[kept++] = entry;
        }
    }
    (void)entries_.resize(kept);
}

void SideEffectLedger::drop_oldest() noexcept {
    const usize held = entries_.size();
    if (held == 0) {
        return;
    }
    // The oldest by tick, which is the front: entries are appended in simulation order and
    // `prune_before` preserves it. `pop_back()` rather than `resize(size() - 1)` for the reason
    // SnapshotRing::evict_to_budget() spells out: the subtraction is a wrapping one as far as GCC's
    // -O2 analysis is concerned, and it fails the Profile build.
    for (usize index = 1; index < held; ++index) {
        entries_[index - 1] = entries_[index];
    }
    entries_.pop_back();
    ++report_.dropped_for_capacity;
}

u64 SideEffectLedger::oldest_tick() const noexcept {
    if (entries_.empty()) {
        return 0;
    }
    u64 oldest = entries_[0].tick;
    for (const EffectEntry& entry : entries_) {
        oldest = std::min(oldest, entry.tick);
    }
    return oldest;
}

void SideEffectLedger::clear() noexcept {
    entries_.clear();
    report_ = LedgerReport{};
    report_.kinds_declared = static_cast<u32>(declarations_.size());
}

}  // namespace cy::replay
