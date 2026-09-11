// Playback, seeking, and presentation tracks. M9 task 3.1.

#include <cy/replay/playback.h>

#include <cy/core/memory/allocator.h>

#include <cstring>

namespace cy::replay {

const char* presentation_name(Presentation value) noexcept {
    switch (value) {
        case Presentation::Present:
            return "Present";
        case Presentation::Suppressed:
            return "Suppressed";
    }
    return "Unknown";
}

const char* presentation_track_kind_name(PresentationTrackKind kind) noexcept {
    switch (kind) {
        case PresentationTrackKind::CameraDirection:
            return "CameraDirection";
        case PresentationTrackKind::Annotation:
            return "Annotation";
        case PresentationTrackKind::DirectorCut:
            return "DirectorCut";
        case PresentationTrackKind::Marker:
            return "Marker";
    }
    return "Unknown";
}

// --- PlaybackClock -------------------------------------------------------------------------------

PlaybackClock::PlaybackClock(u32 tick_rate_numerator, u32 tick_rate_denominator) noexcept
    : rate_numerator_(tick_rate_numerator), rate_denominator_(tick_rate_denominator) {}

Status PlaybackClock::set_speed(PlaybackSpeed speed) noexcept {
    if (speed.numerator == 0 || speed.denominator == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: a playback speed of zero is a pause, not a speed");
    }
    speed_ = speed;
    // The accumulator is carried across the change rather than cleared: it holds a fraction of a
    // tick of real time that has already elapsed, and discarding it would make a speed change lose
    // time — a replay stepped through at four speeds would end at a different tick each time.
    return ok();
}

u64 PlaybackClock::tick_micros() const noexcept {
    if (!valid()) {
        return 0;
    }
    // Tick rate is ticks per second as a rational: 60/1 is sixty ticks a second, so one tick is
    // 1e6 * denominator / numerator microseconds. `speed_` is deliberately not in this expression.
    return (1'000'000ULL * static_cast<u64>(rate_denominator_)) / static_cast<u64>(rate_numerator_);
}

u32 PlaybackClock::advance(u64 micros) noexcept {
    if (!valid() || paused_ || micros == 0) {
        return 0;
    }
    // Accumulate in "rate units" rather than in microseconds so nothing is rounded before the
    // division: `micros * rate_num * speed_num` against `1e6 * rate_den * speed_den`.
    accumulator_ += micros * static_cast<u64>(rate_numerator_) * static_cast<u64>(speed_.numerator);
    const u64 per_tick =
        1'000'000ULL * static_cast<u64>(rate_denominator_) * static_cast<u64>(speed_.denominator);
    if (per_tick == 0) {
        return 0;
    }
    const u64 ticks = accumulator_ / per_tick;
    accumulator_ -= ticks * per_tick;
    issued_ += ticks;
    // A caller that stalled for a minute gets a minute of ticks and not four billion of them; the
    // cap is the honest shape of "how many whole ticks fit", and a caller that wants to drop them
    // reads the number and decides.
    return ticks > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<u32>(ticks);
}

void PlaybackClock::reset() noexcept {
    accumulator_ = 0;
    issued_ = 0;
    paused_ = false;
}

// --- PlaybackDriver ------------------------------------------------------------------------------

Status PlaybackDriver::bind_participant(u64 participant, u32 producer_order) noexcept {
    for (const Binding& bound : bindings_) {
        if (bound.participant == participant) {
            return fail(ErrorCode::AlreadyExists,
                        "replay: that participant is already bound to a producer");
        }
    }
    return bindings_.push_back(Binding{participant, producer_order});
}

u32 PlaybackDriver::producer_for(u64 participant) noexcept {
    for (const Binding& bound : bindings_) {
        if (bound.participant == participant) {
            return bound.producer_order;
        }
    }
    ++unbound_;
    return default_producer_;
}

SeekPlan PlaybackDriver::plan_seek(u64 target_tick, u64 current_tick) noexcept {
    SeekPlan plan;
    plan.target_tick = target_tick;
    plan.backward = target_tick < current_tick;

    if (log_->size() == 0) {
        return plan;
    }
    // A target past everything recorded is not a seek, it is a request for a tick the session never
    // reached. Refused rather than clamped: a viewer who asked for tick 100 000 of a 4 000-tick
    // replay should be told, not silently shown the end.
    const LogRecord& last = log_->at(log_->size() - 1);
    if (target_tick > last.tick) {
        return plan;
    }

    plan.valid = true;
    LogRecord checkpoint;
    if (log_->nearest_checkpoint(target_tick, checkpoint)) {
        plan.has_checkpoint = true;
        plan.checkpoint_tick = checkpoint.tick;
        plan.checkpoint_address = checkpoint.value;
        plan.first_tick = checkpoint.tick;
    } else {
        // No checkpoint at or before the target. The honest plan is "start at the beginning and
        // simulate forward", and `has_checkpoint == false` is what lets a caller report the cost.
        plan.first_tick = log_->at(0).tick;
    }
    plan.fast_forward_ticks = target_tick - plan.first_tick;

    // The cursor is positioned at the first record of the window so `produce()` walks forward from
    // the restore point rather than from wherever the last call left it.
    cursor_.rewind();
    LogRecord ignored;
    (void)cursor_.seek(plan.first_tick, ignored);
    return plan;
}

Expected<u32, Error> PlaybackDriver::produce(gameplay::CommandStream& stream, u64 tick) noexcept {
    u32 produced = 0;
    LogRecord record;
    while (cursor_.next_command_at(tick, record)) {
        const u32 order = producer_for(record.command.participant.bits());
        if (order >= stream.producer_count()) {
            return fail(ErrorCode::OutOfRange,
                        "replay: playback bound to a producer that is not open");
        }
        gameplay::Command command = record.command;
        // PROVENANCE IS REWRITTEN AND NOTHING DOWNSTREAM MAY CARE. `gameplay-framework`:
        // "Provenance SHALL NOT affect validation, ordering, or execution." A replay that pretended
        // its commands were still `Human` would be the first step towards something that did care.
        command.provenance.kind = gameplay::ControlSourceKind::Replay;
        if (Status recorded = stream.producer(order).record(command); !recorded) {
            return make_unexpected(recorded.error());
        }
        ++produced;
        ++produced_;
    }
    return produced;
}

void PlaybackDriver::rewind() noexcept {
    cursor_.rewind();
    produced_ = 0;
    unbound_ = 0;
}

// --- Presentation tracks -------------------------------------------------------------------------

Status PresentationTrack::append(const PresentationSample& sample) noexcept {
    if (sample.payload_size > kMaxTrackPayload) {
        return fail(ErrorCode::InvalidArgument, "replay: track sample payload too large");
    }
    if (!samples_.empty() && sample.tick < samples_[samples_.size() - 1].tick) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: a presentation track is tick-monotonic; that sample goes backwards");
    }
    return samples_.push_back(sample);
}

bool PresentationTrack::sample_at(u64 tick, PresentationSample& out) const noexcept {
    // Backwards from the end: the sample in force is the last one at or before `tick`, and a viewer
    // scrubbing forward asks about ticks near the end far more often than near the start.
    for (usize index = samples_.size(); index > 0; --index) {
        if (samples_[index - 1].tick <= tick) {
            out = samples_[index - 1];
            return true;
        }
    }
    return false;
}

void PresentationTrack::clear() noexcept {
    samples_.clear();
}

PresentationTrackSet::~PresentationTrackSet() {
    clear();
}

Expected<PresentationTrack*, Error> PresentationTrackSet::add(PresentationTrackKind kind,
                                                              const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "replay: a track needs a name");
    }
    if (find(name) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "replay: that track name is taken");
    }
    void* memory = allocator_->allocate(sizeof(PresentationTrack), alignof(PresentationTrack));
    if (memory == nullptr) {
        return fail(ErrorCode::OutOfMemory, "replay: no memory for a track");
    }
    auto* track = construct_at<PresentationTrack>(memory, *allocator_, kind, name);
    if (Status pushed = tracks_.push_back(track); !pushed) {
        track->~PresentationTrack();
        allocator_->deallocate(memory, sizeof(PresentationTrack), alignof(PresentationTrack));
        return make_unexpected(pushed.error());
    }
    return track;
}

PresentationTrack* PresentationTrackSet::find(const char* name) noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (PresentationTrack* track : tracks_) {
        if (std::strcmp(track->name(), name) == 0) {
            return track;
        }
    }
    return nullptr;
}

const PresentationTrack* PresentationTrackSet::find(const char* name) const noexcept {
    return const_cast<PresentationTrackSet*>(this)->find(name);
}

bool PresentationTrackSet::camera_override(u64 tick, PresentationSample& out) const noexcept {
    for (const PresentationTrack* track : tracks_) {
        if (track->kind() != PresentationTrackKind::CameraDirection) {
            continue;
        }
        if (track->sample_at(tick, out)) {
            return true;
        }
    }
    return false;
}

void PresentationTrackSet::clear() noexcept {
    for (PresentationTrack* track : tracks_) {
        track->~PresentationTrack();
        allocator_->deallocate(track, sizeof(PresentationTrack), alignof(PresentationTrack));
    }
    tracks_.clear();
}

}  // namespace cy::replay
