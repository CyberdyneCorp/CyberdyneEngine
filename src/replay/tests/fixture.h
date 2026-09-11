#pragma once
// The shared fixture for CyberReplay's suites. M9 section 1.
//
// WHAT IS ABSENT IS THE SUBJECT, exactly as in src/gameplay/tests/fixture.h. There is no renderer,
// no audio device and no GPU here, and `replay-and-rollback` says why: "Reconstruction needs no
// renderer ... it SHALL require only recorded authoritative data." These suites cannot reach a
// device because this module links nothing that has one.

#include <cy/core/memory/system_allocator.h>
#include <cy/replay/log.h>
#include <cy/replay/record.h>
#include <cy/test/test.h>

namespace cy::replay_test {

using namespace cy::replay;

inline cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// A manifest with every field set to something distinguishable, so that a comparison which
/// accidentally compared defaults would pass for the wrong reason.
[[nodiscard]] inline CompatibilityManifest manifest() noexcept {
    CompatibilityManifest recorded;
    recorded.engine_build = 0xE461'2024ULL;
    recorded.project_build = 0x9401'1999ULL;
    recorded.plugin_lockfile_hash = 0xABCD'1234ULL;
    recorded.content_manifest_hash = 0x5678'FEDCULL;
    recorded.session_seed = 0x5EED'5EEDULL;
    recorded.tick_rate_numerator = 60;
    recorded.tick_rate_denominator = 1;
    recorded.command_schema_version = 3;
    recorded.state_schema_version = 2;
    recorded.profile = cy::determinism::DeterminismProfile::SamePlatform;
    return recorded;
}

/// A command record with enough set that a field lost in encoding is a failure rather than a
/// coincidence.
[[nodiscard]] inline LogRecord command_record(u64 tick, u32 sequence, cy::i32 dx) noexcept {
    LogRecord record;
    record.kind = RecordKind::Command;
    record.tick = tick;
    record.sequence = sequence;
    record.command.type = 11;
    record.command.tick = tick;
    record.command.sequence = sequence;
    record.command.participant = cy::gameplay::ParticipantId::from_slot(2, 1);
    record.command.source = cy::gameplay::ControlSourceId::from_slot(5, 1);
    record.command.target = cy::ecs::Entity::make(7, 1);
    record.command.provenance = cy::gameplay::Provenance{cy::gameplay::ControlSourceKind::Human, 5};
    (void)record.command.set_payload(dx);
    return record;
}

[[nodiscard]] inline LogRecord checkpoint_record(u64 tick, cy::u64 address) noexcept {
    LogRecord record;
    record.kind = RecordKind::Checkpoint;
    record.tick = tick;
    record.value = address;
    return record;
}

[[nodiscard]] inline LogRecord hash_record(u64 tick, cy::u64 state_hash) noexcept {
    LogRecord record;
    record.kind = RecordKind::StateHash;
    record.tick = tick;
    record.value = state_hash;
    return record;
}

}  // namespace cy::replay_test
