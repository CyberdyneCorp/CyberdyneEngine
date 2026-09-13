// Deciding between a local and a remote copy of one save. See conflict.h for the rule and for why
// the type this works over has no timestamp in it.

#include <cy/save/conflict.h>

namespace cy::save {
namespace {

/// -1, 0 or +1: where `a` stands to `b` on one ordered marker.
template <class T>
[[nodiscard]] int compare(T a, T b) noexcept {
    if (a < b) {
        return -1;
    }
    return a > b ? 1 : 0;
}

/// Which marker a one-sided lead is reported as. Strongest evidence first: how far the player got,
/// then how long they played, then how many times the game wrote — a save ahead only on the last of
/// those is ahead only in bookkeeping, and the reason line should say so rather than sound like
/// progress.
[[nodiscard]] const char* lead_reason(int progress, int point) noexcept {
    if (progress != 0) {
        return "further progress, and behind on nothing";
    }
    if (point != 0) {
        return "a later simulation point, and behind on nothing";
    }
    return "more generations, and behind on nothing";
}

[[nodiscard]] bool same_save(const SaveSummary& local, const SaveSummary& remote) noexcept {
    return local.project == remote.project && local.save == remote.save &&
           local.campaign == remote.campaign;
}

}  // namespace

SaveSummary summarise(const Manifest& manifest) noexcept {
    SaveSummary summary;
    summary.project = manifest.project;
    summary.save = manifest.save;
    summary.campaign = manifest.campaign;
    summary.generation = manifest.generation;
    summary.simulation_point = manifest.simulation_point;
    summary.progress = manifest.progress;
    summary.content_version = manifest.content_version;
    return summary;
}

const char* conflict_outcome_name(ConflictOutcome outcome) noexcept {
    switch (outcome) {
        case ConflictOutcome::Identical:
            return "identical";
        case ConflictOutcome::KeepLocal:
            return "keep-local";
        case ConflictOutcome::KeepRemote:
            return "keep-remote";
        case ConflictOutcome::Divergent:
            return "divergent";
        case ConflictOutcome::Unrelated:
            return "unrelated";
    }
    return "unknown";
}

ConflictResolution resolve_conflict(const SaveSummary& local, const SaveSummary& remote) noexcept {
    ConflictResolution result;
    result.content_version_differs = !(local.content_version == remote.content_version);

    if (!same_save(local, remote)) {
        result.outcome = ConflictOutcome::Unrelated;
        result.reason = "a different project, save slot or campaign";
        return result;
    }

    const int generation = compare(local.generation, remote.generation);
    const int point = compare(local.simulation_point, remote.simulation_point);
    const int progress = compare(local.progress, remote.progress);

    if (generation == 0 && point == 0 && progress == 0) {
        result.outcome = ConflictOutcome::Identical;
        result.reason = "the same generation, simulation point and progress";
        return result;
    }

    // "At or ahead on every ordered marker, and ahead on one" — the `!= 0` above has already
    // established that at least one differs, so a side that is behind on nothing is ahead on
    // something.
    if (generation >= 0 && point >= 0 && progress >= 0) {
        result.outcome = ConflictOutcome::KeepLocal;
        result.reason = lead_reason(progress, point);
        return result;
    }
    if (generation <= 0 && point <= 0 && progress <= 0) {
        result.outcome = ConflictOutcome::KeepRemote;
        result.reason = lead_reason(progress, point);
        return result;
    }

    result.outcome = ConflictOutcome::Divergent;
    result.reason =
        "one campaign played twice: neither copy is at or ahead of the other on all of generation, "
        "simulation point and progress";
    return result;
}

ConflictResolution resolve_conflict(const Manifest& local, const Manifest& remote) noexcept {
    return resolve_conflict(summarise(local), summarise(remote));
}

}  // namespace cy::save
