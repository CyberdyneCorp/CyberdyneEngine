#pragma once
// The artefact's four acts. M9 tasks 6.1 to 6.4.
//
// Each act is a CLAIM AND A MEASUREMENT, and each one reads the SAME session — the one the host
// played in act one and recorded as it went. That is the milestone's subtitle made into a program:
// one command log, read five ways. A sample that played four separate sessions, one per act, would
// be demonstrating four things rather than one thing four times.
//
//   session      four players over a lossy network: prediction, reconciliation and rollback under
//                packet loss, with the loss injected and counted.
//   replay       the recorded session replayed into a fresh world — the same log digest and the
//                same state hash at every tick, and again after seeking to a checkpoint.
//   divergence   one field of one component on one entity perturbed deliberately, and the report
//                the engine produces about it.
//   crash        the bounded replay buffer flushed into a crash artefact, written, read back, and
//                re-simulated to the hash the artefact itself carries.

#include <cy/core/base/types.h>

#include "net.h"

namespace cy::mp {

/// Where an act writes what a driver reads. An act prints `key = value` lines for the numbers and
/// writes these two files for the picture.
struct Artefacts {
    const char* trace_path = "";
    const char* divergence_path = "";
    const char* crash_path = "";
};

struct DivergenceOptions {
    u64 inject_tick = 0;
    u32 inject_player = 2;
    u32 inject_shield = 7;
};

[[nodiscard]] bool act_replay(NetworkedSession& session) noexcept;
[[nodiscard]] bool act_divergence(NetworkedSession& session, const DivergenceOptions& options,
                                  const Artefacts& artefacts,
                                  replay::DivergenceReport& out) noexcept;
[[nodiscard]] bool act_crash(NetworkedSession& session, const replay::DivergenceReport& divergence,
                             const Artefacts& artefacts) noexcept;

/// Print what the session did, and write the trace the picture is drawn from.
[[nodiscard]] bool report_session(NetworkedSession& session, const Artefacts& artefacts) noexcept;

/// `key = value`, which is the whole protocol between this program and its driver.
void emit(const char* key, u64 value) noexcept;
void emit_text(const char* key, const char* value) noexcept;

}  // namespace cy::mp
