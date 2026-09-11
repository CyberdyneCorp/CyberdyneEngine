// The artefact's four acts. M9 tasks 6.1 to 6.4.

#include "acts.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/replay/crash.h>
#include <cy/replay/playback.h>

#include <cstdio>
#include <cstring>

namespace cy::mp {
namespace {

[[nodiscard]] Allocator& act_allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A world that replays somebody else's log, with its own ledger so the effects it reproduces are
/// offered through the same door the live session offered them through.
struct Playback {
    replay::RecordLog* log = nullptr;
    GameWorld world;
    replay::SideEffectLedger ledger;
    replay::SnapshotRing ring;
    determinism::EpochCounter epochs;
    replay::RollbackEngine engine;
    /// Where the replay's own record goes. The live session recorded its effects, so a replay that
    /// did not would produce a shorter log and the two digests could never be compared.
    replay::SessionRecorder* recorder = nullptr;
    u32 explosions = 0;

    explicit Playback(replay::RecordLog& source) noexcept
        : log(&source),
          ledger(act_allocator()),
          ring(act_allocator(), 8ULL * 1024 * 1024),
          engine(act_allocator(), source, ring, ledger, epochs) {}

    [[nodiscard]] bool build() noexcept;
    static void on_effect(void* user, u64 kind, u64 instance) noexcept;
};

}  // namespace

void emit(const char* key, u64 value) noexcept {
    std::printf("%s = %llu\n", key, static_cast<unsigned long long>(value));
}

void emit_text(const char* key, const char* value) noexcept {
    std::printf("%s = %s\n", key, value);
}

// ================================================================================================
// ACT ONE'S REPORT
// ================================================================================================

namespace {

[[nodiscard]] bool write_trace(NetworkedSession& session, const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return true;
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    (void)std::fprintf(file,
                       "# tick x0 y0 x1 y1 x2 y2 x3 y3 substituted_mask host_hash "
                       "r0 d0 r1 d1 r2 d2 r3 d3\n");
    for (const TraceSample& sample : session.trace()) {
        (void)std::fprintf(file, "tick %llu", static_cast<unsigned long long>(sample.tick));
        for (u32 player = 0; player < kPlayers; ++player) {
            (void)std::fprintf(file, " %.1f %.1f", static_cast<double>(sample.x[player]),
                               static_cast<double>(sample.y[player]));
        }
        (void)std::fprintf(file, " %u %llu", sample.substituted,
                           static_cast<unsigned long long>(sample.host_hash));
        for (u32 player = 0; player < kPlayers; ++player) {
            (void)std::fprintf(file, " %u %u", sample.rollbacks[player], sample.depth[player]);
        }
        (void)std::fprintf(file, "\n");
    }
    for (u32 player = 0; player < kPlayers; ++player) {
        const ClientReport& client = session.report().clients[player];
        (void)std::fprintf(file, "client %u %u %u %u %u %u\n", player, client.rollbacks,
                           client.ticks_resimulated, client.explosions_played,
                           client.duplicate_effects, client.converged ? 1U : 0U);
    }
    (void)std::fprintf(file, "network %llu %llu %llu %u\n",
                       static_cast<unsigned long long>(session.report().datagrams_offered),
                       static_cast<unsigned long long>(session.report().datagrams_dropped),
                       static_cast<unsigned long long>(session.report().datagrams_duplicated),
                       session.report().substitutions);
    (void)std::fclose(file);
    return true;
}

}  // namespace

bool report_session(NetworkedSession& session, const Artefacts& artefacts) noexcept {
    const SessionReport& report = session.report();

    // TASK 1.4b's STARTUP LINE, PRINTED BY AN ARTEFACT. `guarded` beside `underived`, because a
    // firewall that guards a tenth of the world and one that guards all of it read identically
    // otherwise.
    char arming[gameplay::kArmingReportBuffer] = {};
    const usize written = gameplay::format_arming_report(arming, sizeof(arming), session.arming());
    if (written == 0) {
        return false;
    }
    emit_text("firewall_line", arming);
    emit("firewall_armed", session.arming().armed ? 1U : 0U);
    emit("firewall_components", session.arming().components_registered);
    emit("firewall_guarded", session.arming().guarded);
    emit("firewall_underived", session.arming().underived);
    emit("firewall_percent", session.arming().guarded_percent());

    emit("session_players", kPlayers);
    emit("session_ticks", report.ticks_simulated);
    emit("session_last_tick", session.last_simulated_tick());
    emit("session_datagrams_offered", report.datagrams_offered);
    emit("session_datagrams_dropped", report.datagrams_dropped);
    emit("session_datagrams_duplicated", report.datagrams_duplicated);
    emit("session_datagrams_delivered", report.datagrams_delivered);
    emit("session_substitutions", report.substitutions);
    emit("session_rollbacks", report.rollbacks());
    emit("session_duplicate_effects", report.duplicate_effects());
    emit("session_converged_clients", report.converged_clients());
    emit("session_host_explosions", report.host_explosions);
    emit("session_host_hash", report.host_final_hash);
    emit("session_records", report.records_written);
    emit("session_records_dropped", report.records_dropped);
    emit("session_checkpoints", report.checkpoints);
    emit("session_crash_ring_capacity", report.crash_ring_capacity);
    emit("session_crash_ring_size", report.crash_ring_size);
    emit("session_crash_ring_overwritten", report.crash_ring_overwritten);

    for (u32 player = 0; player < kPlayers; ++player) {
        const ClientReport& client = report.clients[player];
        char key[64] = {};
        (void)std::snprintf(key, sizeof(key), "client%u_rollbacks", player);
        emit(key, client.rollbacks);
        (void)std::snprintf(key, sizeof(key), "client%u_resimulated", player);
        emit(key, client.ticks_resimulated);
        (void)std::snprintf(key, sizeof(key), "client%u_mispredicted", player);
        emit(key, client.mispredicted_ticks);
        (void)std::snprintf(key, sizeof(key), "client%u_effects_offered", player);
        emit(key, client.effects_offered);
        (void)std::snprintf(key, sizeof(key), "client%u_effects_suppressed", player);
        emit(key, client.effects_suppressed);
        (void)std::snprintf(key, sizeof(key), "client%u_explosions", player);
        emit(key, client.explosions_played);
        (void)std::snprintf(key, sizeof(key), "client%u_duplicate_effects", player);
        emit(key, client.duplicate_effects);
        (void)std::snprintf(key, sizeof(key), "client%u_frontier", player);
        emit(key, client.frontier);
        (void)std::snprintf(key, sizeof(key), "client%u_first_unconfirmed", player);
        emit(key, client.first_unconfirmed_tick);
        (void)std::snprintf(key, sizeof(key), "client%u_refusals", player);
        emit(key, client.refusals);
        (void)std::snprintf(key, sizeof(key), "client%u_hash", player);
        emit(key, client.final_hash);
        (void)std::snprintf(key, sizeof(key), "client%u_converged", player);
        emit(key, client.converged ? 1U : 0U);
    }
    return write_trace(session, artefacts.trace_path);
}

// ================================================================================================
// ACT TWO: THE REPLAY
// ================================================================================================

namespace {

bool Playback::build() noexcept {
    if (!world.build()) {
        return false;
    }
    if (!ledger
             .declare(replay::EffectDeclaration{kExplosionKind, "explosion",
                                                replay::Speculation::Speculative,
                                                replay::Reconciliation::Cancel})
             .has_value()) {
        return false;
    }
    world.set_effect_sink(&Playback::on_effect, this);
    return true;
}

void Playback::on_effect(void* user, u64 kind, u64 instance) noexcept {
    auto* self = static_cast<Playback*>(user);
    if (self->engine.offer(kind, instance, /*predicted=*/false) == replay::EffectVerdict::Realise) {
        ++self->explosions;
        if (self->recorder != nullptr) {
            (void)self->recorder->record_effect(kind, instance);
        }
    }
}

[[nodiscard]] bool bind_all(replay::PlaybackDriver& driver, GameWorld& world) noexcept {
    for (u32 player = 0; player < kPlayers; ++player) {
        if (!driver.bind_participant(world.participant_bits(player), world.producer_of(player))
                 .has_value()) {
            return false;
        }
    }
    return true;
}

/// The whole session, replayed from tick zero. Returns false only when the replay could not be
/// RUN — a replay that ran and disagreed reports its disagreement through the emitted figures,
/// because a driver that cannot see the numbers cannot say which half failed.
[[nodiscard]] bool replay_whole(NetworkedSession& session, replay::RecordLog& recorded,
                                u64 last) noexcept {
    Playback playback(recorded);
    if (!playback.build()) {
        return false;
    }
    replay::RecordLog replayed(act_allocator(), recorded.manifest());
    replay::SessionRecorder recorder(replayed);
    replay::PlaybackDriver driver(act_allocator(), recorded);
    if (!bind_all(driver, playback.world)) {
        return false;
    }

    playback.recorder = &recorder;
    recorder.attach(playback.world.commands());
    u32 mismatched = 0;
    u32 produced_total = 0;
    for (u64 tick = 0; tick <= last; ++tick) {
        const determinism::SimulationPoint at{playback.epochs.current(), tick};
        recorder.set_point(at);
        playback.engine.set_point(at);
        auto produced = driver.produce(playback.world.commands(), tick);
        if (!produced || !playback.world.step(tick)) {
            return false;
        }
        produced_total += *produced;
        if ((tick % 10) == 0 && !recorder.record_checkpoint(tick).has_value()) {
            return false;
        }
        const u64 hash = playback.world.root_hash();
        // TICK BY TICK, not only at the end: a replay that diverged at tick 3 and converged again
        // by the last tick would pass a final-hash-only comparison.
        if (tick < session.trace().size() && session.trace()[tick].host_hash != hash) {
            ++mismatched;
        }
        if (!recorder.record_state_hash(hash).has_value()) {
            return false;
        }
    }
    replay::SessionRecorder::detach(playback.world.commands());

    emit("replay_commands_produced", driver.commands_produced());
    emit("replay_commands_expected", produced_total);
    emit("replay_unbound_participants", driver.unbound_participants());
    emit("replay_records_recorded", recorded.size());
    emit("replay_records_replayed", replayed.size());
    emit("replay_log_hash_recorded", recorded.hash());
    emit("replay_log_hash_replayed", replayed.hash());
    emit("replay_mismatched_ticks", mismatched);
    emit("replay_state_hash", playback.world.root_hash());
    emit("replay_explosions", playback.explosions);
    return true;
}

/// And the same claim from a checkpoint: restore, fast-forward, and compare at every tick between.
[[nodiscard]] bool replay_after_seek(NetworkedSession& session, replay::RecordLog& recorded,
                                     u64 last) noexcept {
    Playback sought(recorded);
    if (!sought.build()) {
        return false;
    }
    replay::PlaybackDriver seeker(act_allocator(), recorded);
    if (!bind_all(seeker, sought.world)) {
        return false;
    }
    const u64 target = (last / 2) - ((last / 2) % 10);
    const replay::SeekPlan plan = seeker.plan_seek(target, 0);
    emit("seek_valid", plan.valid ? 1U : 0U);
    emit("seek_has_checkpoint", plan.has_checkpoint ? 1U : 0U);
    emit("seek_checkpoint_tick", plan.checkpoint_tick);
    emit("seek_presentation_suppressed",
         plan.fast_forward_presentation == replay::Presentation::Suppressed ? 1U : 0U);
    if (!plan.valid || !plan.has_checkpoint) {
        return false;
    }

    replay::WindowRefusal refusal = replay::WindowRefusal::None;
    const replay::StateCapture* capture = session.checkpoints().find(
        determinism::SimulationPoint{determinism::Epoch{}, plan.checkpoint_tick}, refusal);
    if (capture == nullptr ||
        !capture->restore(sought.world.world(), sought.world.providers()).has_value()) {
        return false;
    }
    u32 mismatched = 0;
    for (u64 tick = plan.checkpoint_tick; tick <= last; ++tick) {
        sought.engine.set_point(determinism::SimulationPoint{sought.epochs.current(), tick});
        if (!seeker.produce(sought.world.commands(), tick) || !sought.world.step(tick)) {
            return false;
        }
        if (tick < session.trace().size() &&
            session.trace()[tick].host_hash != sought.world.root_hash()) {
            ++mismatched;
        }
    }
    emit("seek_mismatched_ticks", mismatched);
    emit("seek_state_hash", sought.world.root_hash());
    return true;
}

}  // namespace

bool act_replay(NetworkedSession& session) noexcept {
    replay::RecordLog& recorded = session.log();
    const u64 last = session.last_simulated_tick();
    return replay_whole(session, recorded, last) && replay_after_seek(session, recorded, last);
}

// ================================================================================================
// ACT THREE: THE INJECTED DIVERGENCE
// ================================================================================================

namespace {

[[nodiscard]] bool write_divergence(const char* path, const replay::DivergenceReport& report,
                                    const char* line) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return true;
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    (void)std::fprintf(file, "line %s\n", line);
    (void)std::fprintf(file, "tick %llu\n",
                       static_cast<unsigned long long>(report.window.first_diverging_tick));
    (void)std::fprintf(file, "agreeing %llu\n",
                       static_cast<unsigned long long>(report.window.last_agreeing_tick));
    (void)std::fprintf(file, "checkpoint %llu\n",
                       static_cast<unsigned long long>(report.window.checkpoint.tick));
    (void)std::fprintf(file, "entity %llu\n", static_cast<unsigned long long>(report.field.entity));
    (void)std::fprintf(file, "component %llu %s\n",
                       static_cast<unsigned long long>(report.field.component),
                       report.field.component_name);
    (void)std::fprintf(file, "field %llu %s\n", static_cast<unsigned long long>(report.field.field),
                       report.field.field_name);
    (void)std::fprintf(file, "values %llu %llu\n",
                       static_cast<unsigned long long>(report.field.left),
                       static_cast<unsigned long long>(report.field.right));
    (void)std::fprintf(file, "commands %u\n", report.window.command_count);
    (void)std::fprintf(file, "seed %llu\n",
                       static_cast<unsigned long long>(report.window.session_seed));
    (void)std::fclose(file);
    return true;
}

/// Every field of the narrowed report, as `key = value`. Split out of `act_divergence` because a
/// function that both drives two simulations and prints eighteen numbers is two functions.
void emit_narrowing(const replay::DivergenceReport& report, u64 expected_entity,
                    u64 window_records) noexcept {
    emit("divergence_narrowed", report.field.diverged ? 1U : 0U);
    emit("divergence_shape_mismatch", report.field.shape_mismatch ? 1U : 0U);
    emit("divergence_entity", report.field.entity);
    emit("divergence_expected_entity", expected_entity);
    emit("divergence_component", report.field.component);
    emit_text("divergence_component_name", report.field.component_name);
    emit("divergence_field", report.field.field);
    emit_text("divergence_field_name", report.field.field_name);
    emit("divergence_left", report.field.left);
    emit("divergence_right", report.field.right);
    emit("divergence_window_valid", report.window.valid ? 1U : 0U);
    emit("divergence_window_checkpoint", report.window.checkpoint.tick);
    emit("divergence_window_commands", report.window.command_count);
    emit("divergence_window_seed", report.window.session_seed);
    emit("divergence_window_records", window_records);
}

}  // namespace

bool act_divergence(NetworkedSession& session, const DivergenceOptions& options,
                    const Artefacts& artefacts, replay::DivergenceReport& out) noexcept {
    replay::RecordLog& recorded = session.log();
    const u64 last = session.last_simulated_tick();

    Playback left(recorded);
    Playback right(recorded);
    if (!left.build() || !right.build()) {
        return false;
    }
    replay::PlaybackDriver left_driver(act_allocator(), recorded);
    replay::PlaybackDriver right_driver(act_allocator(), recorded);
    if (!bind_all(left_driver, left.world) || !bind_all(right_driver, right.world)) {
        return false;
    }

    replay::DivergenceProbe probe(act_allocator(), recorded);
    determinism::StateHashTree left_tree(act_allocator());
    determinism::StateHashTree right_tree(act_allocator());

    for (u64 tick = 0; tick <= last; ++tick) {
        left.engine.set_point(determinism::SimulationPoint{left.epochs.current(), tick});
        right.engine.set_point(determinism::SimulationPoint{right.epochs.current(), tick});
        if (!left_driver.produce(left.world.commands(), tick) || !left.world.step(tick) ||
            !right_driver.produce(right.world.commands(), tick) || !right.world.step(tick)) {
            return false;
        }
        if (tick == options.inject_tick) {
            // THE INJECTION. One field, on one entity, in one of the two runs — the shape of a real
            // desync, where two machines agree about every command and disagree about one number.
            Health damaged = right.world.health_of(options.inject_player);
            damaged.shield += options.inject_shield;
            if (!right.world.set_health(options.inject_player, damaged)) {
                return false;
            }
        }
        if (!probe.observe(determinism::RunSide::Left, tick, left.world.root_hash()).has_value() ||
            !probe.observe(determinism::RunSide::Right, tick, right.world.root_hash())
                 .has_value()) {
            return false;
        }
        if (probe.diverged()) {
            // The cheap half named the tick. Pay for the expensive half HERE and nowhere else —
            // which is the whole reason the state hash is hierarchical.
            if (!left.world.hash(left_tree) || !right.world.hash(right_tree)) {
                return false;
            }
            break;
        }
    }

    emit("divergence_detected", probe.diverged() ? 1U : 0U);
    emit("divergence_ticks_compared", probe.ticks_compared());
    if (!probe.diverged()) {
        emit("divergence_narrowed", 0U);
        return true;
    }
    emit("divergence_first_tick", probe.first_diverging_tick());
    emit("divergence_last_agreeing_tick", probe.last_agreeing_tick());

    if (!probe.narrow(left_tree, right_tree, out).has_value()) {
        return false;
    }
    char line[replay::kDivergenceReportBuffer] = {};
    if (replay::format_divergence(line, sizeof(line), out) == 0) {
        return false;
    }
    emit_text("divergence_line", line);
    emit_narrowing(out, left.world.entity_of(options.inject_player).bits(),
                   static_cast<u64>(probe.window_records().size()));
    return write_divergence(artefacts.divergence_path, out, line);
}

// ================================================================================================
// ACT FOUR: THE CRASH ARTEFACT
// ================================================================================================

bool act_crash(NetworkedSession& session, const replay::DivergenceReport& divergence,
               const Artefacts& artefacts) noexcept {
    replay::CrashArtefact artefact(act_allocator());
    const u64 last = session.last_simulated_tick();
    if (!artefact
             .assemble(session.log().manifest(), session.crash_ring(),
                       replay::CrashTrigger::ReportedDefect,
                       determinism::SimulationPoint{determinism::Epoch{}, last})
             .has_value()) {
        return false;
    }
    if (divergence.valid && !artefact.attach_divergence(divergence).has_value()) {
        return false;
    }

    Array<u8> bytes(act_allocator());
    if (!artefact.write(bytes).has_value()) {
        return false;
    }
    if (artefacts.crash_path != nullptr && artefacts.crash_path[0] != '\0') {
        std::FILE* file = std::fopen(artefacts.crash_path, "wb");
        if (file == nullptr) {
            return false;
        }
        (void)std::fwrite(bytes.data(), 1, bytes.size(), file);
        (void)std::fclose(file);
    }
    emit("crash_bytes", static_cast<u64>(bytes.size()));

    replay::CrashArtefact loaded(act_allocator());
    replay::RejectReason why = replay::RejectReason::None;
    if (!loaded.read(bytes.span(), why).has_value()) {
        return false;
    }
    emit("crash_records", static_cast<u64>(loaded.records().size()));
    emit("crash_first_tick", loaded.first_tick());
    emit("crash_last_tick", loaded.last_tick());
    emit("crash_records_lost", loaded.records_lost());
    emit("crash_has_divergence", loaded.has_divergence() ? 1U : 0U);
    emit_text("crash_trigger", replay::crash_trigger_name(loaded.trigger()));

    replay::RecordLog recovered(act_allocator(), loaded.manifest());
    if (!loaded.to_log(recovered).has_value()) {
        return false;
    }
    replay::LogRecord checkpoint;
    if (!recovered.nearest_checkpoint(loaded.last_tick(), checkpoint)) {
        return false;
    }
    // The checkpoint must be INSIDE the window rather than at its ragged edge, or the first tick
    // would be re-simulated with only the commands the ring happened to keep.
    emit("crash_checkpoint_tick", checkpoint.tick);
    emit("crash_checkpoint_inside", checkpoint.tick > loaded.first_tick() ? 1U : 0U);

    Playback reproduction(recovered);
    if (!reproduction.build()) {
        return false;
    }
    replay::WindowRefusal refusal = replay::WindowRefusal::None;
    const replay::StateCapture* capture = session.checkpoints().find(
        determinism::SimulationPoint{determinism::Epoch{}, checkpoint.tick}, refusal);
    if (capture == nullptr ||
        !capture->restore(reproduction.world.world(), reproduction.world.providers()).has_value()) {
        return false;
    }
    replay::PlaybackDriver driver(act_allocator(), recovered);
    if (!bind_all(driver, reproduction.world)) {
        return false;
    }
    // THE DRIVER HAS TO BE POSITIONED, not merely asked for a tick: `plan_seek` is what moves its
    // cursor to the checkpoint, and a loop that skipped it produced nothing and reproduced a world
    // that had never run.
    const replay::SeekPlan plan = driver.plan_seek(checkpoint.tick, 0);
    emit("crash_seek_valid", plan.valid ? 1U : 0U);
    if (!plan.valid) {
        return false;
    }
    u32 produced_total = 0;
    for (u64 tick = checkpoint.tick; tick <= loaded.last_tick(); ++tick) {
        reproduction.engine.set_point(
            determinism::SimulationPoint{reproduction.epochs.current(), tick});
        auto produced = driver.produce(reproduction.world.commands(), tick);
        if (!produced || !reproduction.world.step(tick)) {
            return false;
        }
        produced_total += *produced;
    }
    emit("crash_commands_replayed", produced_total);
    // **THE CLAIM.** Not "the file parsed" — the final seconds of the session actually happened
    // again, and reached the hash the artefact itself carries.
    const u64 reproduced = reproduction.world.root_hash();
    emit("crash_reproduced_hash", reproduced);
    emit("crash_artefact_hash", loaded.recent_hash(loaded.recent_hash_count() - 1));
    emit("crash_reproduced",
         reproduced == loaded.recent_hash(loaded.recent_hash_count() - 1) ? 1U : 0U);
    emit("crash_unbound_participants", driver.unbound_participants());
    return true;
}

}  // namespace cy::mp
