// samples/09-multiplayer — M9's closing artefact. Section 6.
//
// A FOUR-PLAYER SESSION WITH ROLLBACK UNDER PACKET LOSS; A REPLAY THAT REPRODUCES IT BIT-EXACTLY;
// AN INJECTED DIVERGENCE NARROWED TO A FIELD. Three claims, and each one is a measurement rather
// than an assertion — the loss is injected and counted, the replay is compared by digest and not by
// tolerance, and the divergence is put there on purpose so that what the engine says about it can
// be read.
//
// `samples/09-multiplayer/multiplayer.py` is the driver that judges this program's output, and
// `just run-multiplayer` runs the pair. This file prints `key = value` lines and nothing else that
// matters; a number a driver cannot parse is a number nobody checks.
//
// A SECOND SAMPLE RATHER THAN AN EXTENSION OF `samples/08-vertical-slice`, and the proposal says
// why: the subject is what happens BETWEEN machines, and a slice that runs in one cannot
// demonstrate it. The five "machines" here are five worlds on one seeded substrate, which is what
// `LocalNetwork` is for — not a stand-in for a network but an unreliable datagram substrate that
// the same `ReliableEndpoint` runs over as the UDP backend does.

#include <cy/core/base/types.h>

#include "acts.h"
#include "net.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace cy;
using namespace cy::mp;

struct Options {
    SessionOptions session;
    DivergenceOptions divergence;
    Artefacts artefacts;
    const char* act = "all";
};

constexpr const char* kActs[] = {"all", "session", "replay", "divergence", "crash"};

[[nodiscard]] bool wants(const Options& options, const char* act) noexcept {
    return std::strcmp(options.act, "all") == 0 || std::strcmp(options.act, act) == 0;
}

/// An act nobody declared runs NOTHING and would exit zero, which is a typo that reads as a pass.
[[nodiscard]] bool known_act(const char* act) noexcept {
    return std::ranges::any_of(
        kActs, [act](const char* candidate) { return std::strcmp(candidate, act) == 0; });
}

[[nodiscard]] u64 number(const char* text) noexcept {
    return static_cast<u64>(std::strtoull(text, nullptr, 10));
}

void usage() noexcept {
    std::printf(
        "usage: cy_sample_multiplayer [--ticks n] [--seed n] [--loss percent] [--latency ms]\n"
        "       [--input-delay ticks] [--inject-tick n] [--inject-player n]\n"
        "       [--only session|replay|divergence|crash] [--trace path] [--divergence path]\n"
        "       [--crash path]\n");
}

/// Parse. An unknown option is a failure rather than a shrug: a driver that misspelled a flag would
/// otherwise measure the default and report it as the number it asked for.
[[nodiscard]] bool parse(int argc, char** argv, Options& options) noexcept {
    for (int index = 1; index < argc; ++index) {
        const char* flag = argv[index];
        const bool has_value = index + 1 < argc;
        const char* value = has_value ? argv[index + 1] : "";
        if (std::strcmp(flag, "--help") == 0) {
            usage();
            return false;
        }
        if (!has_value) {
            std::printf("multiplayer: %s needs a value\n", flag);
            return false;
        }
        ++index;
        if (std::strcmp(flag, "--ticks") == 0) {
            options.session.ticks = number(value);
        } else if (std::strcmp(flag, "--seed") == 0) {
            options.session.seed = number(value);
        } else if (std::strcmp(flag, "--loss") == 0) {
            options.session.loss_percent = static_cast<u32>(number(value));
        } else if (std::strcmp(flag, "--latency") == 0) {
            options.session.latency_ms = static_cast<u32>(number(value));
        } else if (std::strcmp(flag, "--input-delay") == 0) {
            options.session.input_delay_ticks = static_cast<u32>(number(value));
        } else if (std::strcmp(flag, "--inject-tick") == 0) {
            options.divergence.inject_tick = number(value);
        } else if (std::strcmp(flag, "--inject-player") == 0) {
            options.divergence.inject_player = static_cast<u32>(number(value));
        } else if (std::strcmp(flag, "--only") == 0) {
            options.act = value;
        } else if (std::strcmp(flag, "--trace") == 0) {
            options.artefacts.trace_path = value;
        } else if (std::strcmp(flag, "--divergence") == 0) {
            options.artefacts.divergence_path = value;
        } else if (std::strcmp(flag, "--crash") == 0) {
            options.artefacts.crash_path = value;
        } else {
            std::printf("multiplayer: unknown option %s\n", flag);
            usage();
            return false;
        }
    }
    return true;
}

[[nodiscard]] replay::CompatibilityManifest manifest_for(const Options& options) noexcept {
    replay::CompatibilityManifest manifest;
    manifest.engine_build = 0x0009'2026ULL;
    manifest.project_build = 0x0910'0001ULL;
    manifest.plugin_lockfile_hash = 0x0910'ABCDULL;
    manifest.content_manifest_hash = 0x0910'FEDCULL;
    manifest.session_seed = options.session.seed;
    manifest.tick_rate_numerator = 60;
    manifest.tick_rate_denominator = 1;
    manifest.command_schema_version = 1;
    manifest.state_schema_version = 1;
    manifest.profile = determinism::DeterminismProfile::SamePlatform;
    return manifest;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    if (!known_act(options.act)) {
        std::printf("multiplayer: '%s' is not an act\n", options.act);
        usage();
        return 2;
    }
    if (options.divergence.inject_tick == 0) {
        // Two thirds of the way through, on a checkpoint-free tick, so the window the report
        // carries has a checkpoint BEHIND it and commands between the two.
        options.divergence.inject_tick = ((options.session.ticks * 2) / 3) + 3;
    }

    NetworkedSession session(options.session, manifest_for(options));
    if (!session.build() || !session.run()) {
        std::printf("multiplayer: the session did not run\n");
        return 1;
    }
    if (!report_session(session, options.artefacts)) {
        std::printf("multiplayer: the session could not be reported\n");
        return 1;
    }

    if (wants(options, "replay") && !act_replay(session)) {
        std::printf("multiplayer: the replay act did not run\n");
        return 1;
    }

    replay::DivergenceReport divergence;
    if (wants(options, "divergence") &&
        !act_divergence(session, options.divergence, options.artefacts, divergence)) {
        std::printf("multiplayer: the divergence act did not run\n");
        return 1;
    }
    if (wants(options, "crash") && !act_crash(session, divergence, options.artefacts)) {
        std::printf("multiplayer: the crash act did not run\n");
        return 1;
    }

    emit("acts_complete", 1U);
    return 0;
}
