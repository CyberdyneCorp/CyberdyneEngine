// The reproduction artefact, written as text.
//
// Text on purpose: it is read by a human triaging a bug report, by a tool that plays it back, and
// by whatever ticketing system the report was pasted into. It names files rather than embedding
// them, so it is a few hundred bytes whatever the size of the slice it points at.
//
// The writer REFUSES two artefacts rather than writing them. One with no replay slice points at
// nothing and would be a reproduction in name only. One that claims less than exact fidelity
// without saying why sends a reader looking for a cause the artefact already knows. Both are
// failures the specification names, and the only place they can be caught is here.

#include <cy/core/diagnostics/reproduction.h>

#include "internal.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace cy::diag {
namespace {

constexpr u32 kLinkPathCapacity = 512;

struct Link {
    char path[kLinkPathCapacity] = {};
    std::atomic<u8> fidelity{static_cast<u8>(Fidelity::Unstated)};
    std::atomic<u64> first_tick{0};
    std::atomic<u64> last_tick{0};
    std::atomic<bool> present{false};
};

Link& link() noexcept {
    static Link instance;
    return instance;
}

bool empty(const char* text) noexcept {
    return text == nullptr || text[0] == '\0';
}

}  // namespace

const char* fidelity_name(Fidelity fidelity) noexcept {
    switch (fidelity) {
        case Fidelity::Unstated:
            return "unstated";
        case Fidelity::Exact:
            return "exact";
        case Fidelity::Approximate:
            return "approximate";
        case Fidelity::NotReproducible:
            return "not-reproducible";
    }
    return "unknown";
}

Expected<u64, cy::Error> write_reproduction(const char* path, const Reproduction& record) noexcept {
    if (empty(path)) {
        return fail(ErrorCode::InvalidArgument, "a reproduction artefact needs a path");
    }
    if (empty(record.replay_log_path)) {
        return fail(ErrorCode::InvalidArgument,
                    "a reproduction artefact with no replay slice reproduces nothing");
    }
    if (record.fidelity != Fidelity::Exact && empty(record.fidelity_reason)) {
        return fail(
            ErrorCode::InvalidArgument,
            "fidelity short of exact requires a reason; an artefact that implies a fidelity "
            "it does not have is worse than no artefact");
    }

    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return fail(ErrorCode::Io, "the reproduction artefact could not be created");
    }
    int written = std::fprintf(
        file,
        "cyberdyne-reproduction 1\n"
        "build_identity: %s\n"
        "determinism_profile: %s\n"
        "\n[fidelity]\n"
        "fidelity: %s\n"
        "reason: %s\n"
        "\n[window]\n"
        "first_tick: %llu\n"
        "last_tick: %llu\n"
        "checkpoint_tick: %llu\n"
        "session_seed: %llu\n"
        "log_hash: 0x%016llx\n"
        "final_state_hash: 0x%016llx\n"
        "external_results: %u\n"
        "\n[artefacts]\n"
        "replay_log: %s\n"
        "capture: %s\n"
        "crash_report: %s\n"
        "\n[end]\n",
        record.build_identity, record.profile, fidelity_name(record.fidelity),
        record.fidelity_reason, static_cast<unsigned long long>(record.first_tick),
        static_cast<unsigned long long>(record.last_tick),
        static_cast<unsigned long long>(record.checkpoint_tick),
        static_cast<unsigned long long>(record.session_seed),
        static_cast<unsigned long long>(record.log_hash),
        static_cast<unsigned long long>(record.final_state_hash), record.external_result_count,
        record.replay_log_path, record.capture_path, record.crash_report_path);
    std::fclose(file);
    if (written < 0) {
        return fail(ErrorCode::Io, "the reproduction artefact could not be written");
    }
    return static_cast<u64>(written);
}

void set_reproduction_link(const char* path, Fidelity fidelity, u64 first_tick,
                           u64 last_tick) noexcept {
    Link& state = link();
    if (empty(path)) {
        clear_reproduction_link();
        return;
    }
    std::memset(state.path, 0, sizeof(state.path));
    std::strncpy(state.path, path, kLinkPathCapacity - 1);
    state.fidelity.store(static_cast<u8>(fidelity), std::memory_order_relaxed);
    state.first_tick.store(first_tick, std::memory_order_relaxed);
    state.last_tick.store(last_tick, std::memory_order_relaxed);
    state.present.store(true, std::memory_order_release);
}

ReproductionLink reproduction_link() noexcept {
    Link& state = link();
    ReproductionLink out;
    out.present = state.present.load(std::memory_order_acquire);
    out.path = state.path;
    out.fidelity = static_cast<Fidelity>(state.fidelity.load(std::memory_order_relaxed));
    out.first_tick = state.first_tick.load(std::memory_order_relaxed);
    out.last_tick = state.last_tick.load(std::memory_order_relaxed);
    return out;
}

void clear_reproduction_link() noexcept {
    Link& state = link();
    state.present.store(false, std::memory_order_release);
    state.path[0] = '\0';
    state.fidelity.store(static_cast<u8>(Fidelity::Unstated), std::memory_order_relaxed);
    state.first_tick.store(0, std::memory_order_relaxed);
    state.last_tick.store(0, std::memory_order_relaxed);
}

}  // namespace cy::diag
