#include <cy/import/live_import.h>

#include <chrono>
#include <cstring>

namespace cy::import {
namespace {

[[nodiscard]] u64 now_micros() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void copy_into(char* out, usize capacity, std::string_view text) noexcept {
    const usize length = text.size() < capacity - 1 ? text.size() : capacity - 1;
    if (length != 0) {
        std::memcpy(out, text.data(), length);
    }
    out[length] = '\0';
}

}  // namespace

const char* live_edit_outcome_name(LiveEditOutcome outcome) noexcept {
    switch (outcome) {
        case LiveEditOutcome::Applied:
            return "applied";
        case LiveEditOutcome::NotResident:
            return "not-resident";
        case LiveEditOutcome::Unchanged:
            return "unchanged";
        case LiveEditOutcome::Refused:
            return "refused";
        case LiveEditOutcome::ReloadFailed:
            return "reload-failed";
    }
    return "unchanged";
}

LiveImportSession::LiveImportSession(ImportPipeline& pipeline,
                                     assets::FileWatcher& watcher) noexcept
    : pipeline_(&pipeline), watcher_(&watcher) {}

Status LiveImportSession::register_source(const assets::VirtualPath& source) noexcept {
    for (const assets::VirtualPath& existing : sources_) {
        if (existing == source) {
            return ok();
        }
    }
    return sources_.push_back(source);
}

Status LiveImportSession::watch(const assets::VirtualPath& path) noexcept {
    return watcher_->watch(path);
}

Status LiveImportSession::prime(i64 now_ns) noexcept {
    return watcher_->prime(now_ns);
}

void LiveImportSession::on_change(void* user, const assets::WatchEvent& event) noexcept {
    (void)event;
    auto* self = static_cast<LiveImportSession*>(user);
    ++self->changes_this_poll_;
}

Expected<usize, Error> LiveImportSession::poll(i64 now_ns,
                                               const ImportSettings& settings) noexcept {
    ++stats_.polls;
    changes_this_poll_ = 0;
    last_edits_.clear();

    Expected<u32, Error> events = watcher_->poll(now_ns, &LiveImportSession::on_change, this);
    if (!events) {
        return make_unexpected(events.error());
    }
    stats_.changes_seen += changes_this_poll_;
    if (changes_this_poll_ == 0) {
        // The overwhelming majority of polls. Nothing is opened, nothing is imported, and the
        // session costs the watcher's own sweep and no more.
        return usize{0};
    }

    const u64 started = now_micros();
    usize recooked = 0;
    for (const assets::VirtualPath& source : sources_) {
        Expected<AssetImportOutcome, Error> outcome = pipeline_->import_file(source, settings);
        if (!outcome) {
            return make_unexpected(outcome.error());
        }

        LiveEditRecord record;
        copy_into(record.source, LiveEditRecord::kNameCapacity, source.view());
        record.id = outcome.value().id;

        if (outcome.value().cache == assets::CacheOutcome::Hit) {
            // The cache answered, so this source is unaffected by whatever changed. Recorded as
            // nothing rather than reported: a session that logged every unchanged asset on every
            // keystroke would bury the one line that matters.
            continue;
        }
        ++recooked;
        ++stats_.recooked;

        if (!outcome.value().succeeded()) {
            record.outcome = LiveEditOutcome::Refused;
            copy_into(record.reason, LiveEditRecord::kNameCapacity,
                      outcome.value().cache_reason[0] != '\0'
                          ? std::string_view(outcome.value().cache_reason)
                          : std::string_view("the importer reported an error; see the report"));
            ++stats_.refused;
            if (Status pushed = last_edits_.push_back(record); !pushed) {
                return make_unexpected(pushed.error());
            }
            continue;
        }

        // Reload every asset this source produced that something is holding. The database is the
        // id-to-source index the pipeline filled in as it published, so this asks it rather than
        // keeping a second list that could disagree.
        record.outcome = LiveEditOutcome::NotResident;
        if (assets_ != nullptr) {
            struct Visit {
                LiveImportSession* session;
                const assets::VirtualPath* source;
                LiveEditRecord* record;
            };
            Visit visit{this, &source, &record};
            pipeline_->database().for_each(
                [](void* user, const assets::AssetMeta& meta) noexcept {
                    auto* state = static_cast<Visit*>(user);
                    if (meta.source != *state->source) {
                        return;
                    }
                    const Status reloaded = state->session->assets_->reload(meta.id);
                    if (reloaded) {
                        ++state->record->reloaded;
                        state->record->outcome = LiveEditOutcome::Applied;
                        return;
                    }
                    // NotFound means nothing holds it, which is not a failure: the next load takes
                    // the new bytes. Anything else IS a failure, and the previous bytes stay in use
                    // — which is `live-editing`'s requirement rather than a consolation.
                    if (reloaded.error().code != ErrorCode::NotFound) {
                        state->record->outcome = LiveEditOutcome::ReloadFailed;
                        copy_into(state->record->reason, LiveEditRecord::kNameCapacity,
                                  reloaded.error().message);
                    }
                },
                &visit);
        }

        if (record.outcome == LiveEditOutcome::Applied) {
            ++stats_.applied;
        } else if (record.outcome == LiveEditOutcome::ReloadFailed) {
            ++stats_.reload_failures;
        }
        record.duration_micros = outcome.value().duration_micros;
        if (Status pushed = last_edits_.push_back(record); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    stats_.last_latency_micros = now_micros() - started;
    return recooked;
}

Span<const LiveEditRecord> LiveImportSession::last_edits() const noexcept {
    return {last_edits_.data(), last_edits_.size()};
}

}  // namespace cy::import
