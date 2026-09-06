// Reloading while the runtime runs. M5 task 1.1. See cy/abi/live_reload.h for the model.
//
// Everything here is arithmetic over one directory listing and one string. The reload itself is
// `BehaviourRuntime::reload()`, which M4 wrote and measured; this file's whole job is to decide
// WHEN it is called and WITH WHAT, and to be cheap enough that the deciding can happen every frame.

#include <cy/abi/live_reload.h>

#include <cy/core/assets/file.h>
#include <cy/ecs/world.h>

#include <cstring>

namespace cy::abi {
namespace {

/// Append `text` to `out` at `offset` and keep the buffer NUL-terminated. Returns the new offset.
///
/// Character by character rather than with `memcpy`, which is the convention
/// src/backends/shader/src/cache.cpp states and the reason is the same one: a `memcpy` that leaves
/// a buffer unterminated is what `bugprone-not-null-terminated-result` reports, correctly, because
/// the next reader has to work out whether the buffer is a C string or a length-tracked fragment.
/// This one is a C string — it is handed to `dlopen` — so it is terminated at every step.
usize append(char* out, usize offset, const char* text) noexcept {
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        out[offset++] = *cursor;
    }
    out[offset] = '\0';
    return offset;
}

/// Join `directory`, a separator and `name` into `out`. False when it would not fit, which the
/// caller reports rather than truncating: a truncated path is a path that names a different file.
bool join_path(char* out, usize capacity, const char* directory, const char* name) noexcept {
    const usize directory_length = std::strlen(directory);
    const usize name_length = std::strlen(name);
    const bool needs_separator = directory_length > 0 && directory[directory_length - 1] != '/';
    const usize total = directory_length + (needs_separator ? 1U : 0U) + name_length + 1U;
    if (total > capacity) {
        return false;
    }
    usize offset = append(out, 0, directory);
    if (needs_separator) {
        out[offset++] = '/';
        out[offset] = '\0';
    }
    (void)append(out, offset, name);
    return true;
}

/// `<stem>_g<generation><extension>` into `out`. False when it would not fit.
///
/// Written by hand rather than with `snprintf` for the reason the rest of src/abi/ avoids it: this
/// translation unit is on the path a module loads through, and the formatting it needs is one
/// unsigned decimal.
bool format_generation_name(char* out, usize capacity, const char* stem, u32 generation,
                            const char* extension) noexcept {
    char digits[10] = {};
    usize digit_count = 0;
    u32 value = generation;
    do {
        digits[digit_count++] = static_cast<char>('0' + static_cast<char>(value % 10U));
        value /= 10U;
    } while (value != 0);

    const usize stem_length = std::strlen(stem);
    const usize extension_length = std::strlen(extension);
    if (stem_length + 2U + digit_count + extension_length + 1U > capacity) {
        return false;
    }
    usize offset = append(out, 0, stem);
    out[offset++] = '_';
    out[offset++] = 'g';
    while (digit_count > 0) {
        out[offset++] = digits[--digit_count];
    }
    out[offset] = '\0';
    (void)append(out, offset, extension);
    return true;
}

/// The generation number in `<stem>_g<N><extension>`, or false.
///
/// Deliberately strict: `_g` immediately after the stem, at least one digit, the extension
/// immediately after the digits, and nothing else. `libCyGame_g12.so` matches with 12;
/// `libCyGame_g.so`, `libCyGame_gx.so` and `libCyGame_g1.so.bak` do not. A looser rule would pick
/// up a debug-information file or an editor's backup and hand it to `dlopen`.
bool generation_of(const char* name, const char* stem, const char* extension, u32& out) noexcept {
    const usize stem_length = std::strlen(stem);
    if (std::strncmp(name, stem, stem_length) != 0) {
        return false;
    }
    const char* cursor = name + stem_length;
    if (cursor[0] != '_' || cursor[1] != 'g') {
        return false;
    }
    cursor += 2;
    if (*cursor < '0' || *cursor > '9') {
        return false;
    }
    u64 value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        value = (value * 10U) + static_cast<u64>(*cursor - '0');
        if (value > 0xFFFF'FFFFULL) {
            return false;  // a number no generation counter will reach: not ours.
        }
        ++cursor;
    }
    if (std::strcmp(cursor, extension) != 0) {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

/// What `fs::enumerate` carries through its `void*`.
struct Scan {
    const char* stem = "";
    const char* extension = "";
    u32 generation = 0;
    u64 size = 0;
    bool found = false;
};

bool visit(void* user, const assets::DirectoryEntry& entry) noexcept {
    Scan& scan = *static_cast<Scan*>(user);
    if (entry.is_directory) {
        return true;
    }
    u32 generation = 0;
    if (!generation_of(entry.name, scan.stem, scan.extension, generation)) {
        return true;
    }
    // The HIGHEST generation present, not the next one: a session paused while three builds
    // happened should arrive at the newest, not walk through two images it will never run.
    if (!scan.found || generation > scan.generation) {
        scan.found = true;
        scan.generation = generation;
        scan.size = entry.size;
    }
    return true;
}

}  // namespace

const char* live_reload_outcome_name(LiveReloadOutcome outcome) noexcept {
    switch (outcome) {
        case LiveReloadOutcome::Idle:
            return "idle";
        case LiveReloadOutcome::Waiting:
            return "waiting";
        case LiveReloadOutcome::Applied:
            return "applied";
        case LiveReloadOutcome::Refused:
            return "refused";
        case LiveReloadOutcome::Deferred:
            return "deferred";
    }
    return "unknown";
}

LiveReload::LiveReload(BehaviourRuntime& runtime) noexcept : runtime_(runtime) {
    status_.generation = runtime_.generation();
    status_.candidate = status_.generation;
    judged_generation_ = status_.generation;
}

Status LiveReload::watch(const LiveReloadSource& source) noexcept {
    const bool complete = source.directory != nullptr && source.directory[0] != '\0' &&
                          source.stem != nullptr && source.stem[0] != '\0' &&
                          source.extension != nullptr && source.extension[0] != '\0';
    if (!complete) {
        return fail(ErrorCode::InvalidArgument,
                    "a live-reload source needs a directory, a stem and an extension");
    }
    if (!assets::fs::is_directory(source.directory)) {
        return fail(ErrorCode::NotFound,
                    "the live-reload staging directory does not exist; a watcher pointed at one "
                    "that is never created reports 'nothing changed' for the whole session");
    }
    source_ = source;
    watching_ = true;
    // The image already loaded is not a change. Seeding the judged state with the live generation
    // is what stops the first poll of a freshly loaded runtime from reloading what it just ran.
    judged_generation_ = runtime_.generation();
    seen_generation_ = 0;
    seen_size_ = 0;
    return ok();
}

Status LiveReload::request(const char* library_path) noexcept {
    if (library_path == nullptr || library_path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a live-reload request needs a library path");
    }
    return set_pending(library_path, runtime_.generation() + 1U);
}

Status LiveReload::set_pending(const char* path, u32 generation) noexcept {
    const usize length = std::strlen(path);
    if (length + 1U > kMaxLiveReloadPath) {
        return fail(ErrorCode::BufferTooSmall,
                    "the module path is longer than this watcher carries");
    }
    std::memcpy(pending_path_, path, length + 1U);
    pending_ = true;
    pending_generation_ = generation;
    status_.candidate = generation;
    return ok();
}

LiveReload::Candidate LiveReload::newest_generation() const noexcept {
    Scan scan{source_.stem, source_.extension, 0, 0, false};
    // A failed enumeration is not an error here: a staging directory removed mid-session means
    // there is nothing to reload, which is what an empty scan already says.
    (void)assets::fs::enumerate(source_.directory, false, &visit, &scan);
    return Candidate{scan.generation, scan.size, scan.found};
}

LiveReloadOutcome LiveReload::poll() noexcept {
    ++status_.polls;
    if (!watching_) {
        return pending_ ? LiveReloadOutcome::Waiting : LiveReloadOutcome::Idle;
    }

    const Candidate candidate = newest_generation();
    // `judged_generation_` is every generation this watcher has already loaded, already refused, or
    // already decided it cannot name. Without it a build that fails to load is re-attempted at
    // every poll for the rest of the session — one bad image becoming a reload per frame.
    if (!candidate.found || candidate.generation <= judged_generation_) {
        return pending_ ? LiveReloadOutcome::Waiting : LiveReloadOutcome::Idle;
    }

    // THE SETTLE RULE. The same generation at the same size twice in a row is a file the build has
    // finished writing. One poll of latency, and it removes the common half-written case; the other
    // half is still handled, because a truncated image fails to open and the reload is refused.
    const bool settled = candidate.generation == seen_generation_ && candidate.size == seen_size_;
    seen_generation_ = candidate.generation;
    seen_size_ = candidate.size;
    status_.candidate = candidate.generation;
    if (!settled) {
        return LiveReloadOutcome::Waiting;
    }

    char name[kMaxLiveReloadPath] = {};
    char path[kMaxLiveReloadPath] = {};
    const bool named = format_generation_name(name, sizeof(name), source_.stem,
                                              candidate.generation, source_.extension) &&
                       join_path(path, sizeof(path), source_.directory, name) &&
                       set_pending(path, candidate.generation).has_value();
    if (!named) {
        // The path cannot be spelled in the space this watcher carries, and it never will be. Judge
        // it so the scan stops rediscovering it, and say nothing is pending — which is true.
        judged_generation_ = candidate.generation;
        return LiveReloadOutcome::Idle;
    }
    return LiveReloadOutcome::Waiting;
}

Expected<LiveReloadStatus, Error> LiveReload::apply_at_frame_boundary(
    const ecs::World& world) noexcept {
    if (!pending_) {
        status_.outcome = LiveReloadOutcome::Idle;
        status_.generation = runtime_.generation();
        return status_;
    }

    // NOT A BOUNDARY. A reload destroys and recreates every behaviour instance and re-registers
    // every type; doing that under an iterating query would invalidate the iteration in a way the
    // ECS cannot detect, because the ECS is not what moved. Deferring costs one frame of latency
    // and the request stays pending.
    if (world.iterating()) {
        ++status_.deferred;
        status_.outcome = LiveReloadOutcome::Deferred;
        status_.generation = runtime_.generation();
        return status_;
    }

    const u32 attempted = pending_generation_;
    Expected<ReloadReport, Error> reloaded = runtime_.reload(pending_path_);
    pending_ = false;
    pending_generation_ = 0;
    judged_generation_ = attempted > judged_generation_ ? attempted : judged_generation_;

    if (!reloaded) {
        // The loader could not begin — a path that is not a file, a manifest that forbids reload.
        // The previous generation is untouched; report it as a refusal carrying the loader's own
        // message rather than inventing one.
        ++status_.refused;
        status_.outcome = LiveReloadOutcome::Refused;
        status_.generation = runtime_.generation();
        status_.candidate = status_.generation;
        status_.report = ReloadReport{ReloadFailure::ImageDidNotOpen, status_.generation, 0, 0,
                                      reloaded.error().message};
        return status_;
    }

    status_.report = reloaded.value();
    status_.generation = runtime_.generation();
    status_.candidate = status_.generation;
    judged_generation_ =
        status_.generation > judged_generation_ ? status_.generation : judged_generation_;
    if (status_.report.failure == ReloadFailure::None) {
        ++status_.applied;
        status_.outcome = LiveReloadOutcome::Applied;
    } else {
        ++status_.refused;
        status_.outcome = LiveReloadOutcome::Refused;
    }
    return status_;
}

}  // namespace cy::abi
