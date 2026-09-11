// Playback: instances, the one mover every control funnels through, seeking, skipping, capture and
// restore, and the preload plan.

#include <cy/sequencing/player.h>

#include <algorithm>
#include <new>

namespace cy::sequencing {
namespace {

/// How much faster fast-forward runs. Distinct from skipping — the sequence still PLAYS, so its
/// events still cross and its adapters still evaluate; they may simply simplify while it is on.
inline constexpr i32 kFastForwardFactor = 4;

[[nodiscard]] bool contains(Span<const u32> values, u32 value) noexcept {
    return std::ranges::any_of(values,
                               [value](u32 candidate) noexcept { return candidate == value; });
}

/// Whether a crossed event fires, is suppressed, or is applied as state.
///
/// The four seek modes are not interchangeable, and this is the whole of the difference: an author
/// scrubbing must not fire a mission unlock, a replay must fire nothing because the log already
/// carries it, and a reconstructing seek applies an outcome without pretending the moment happened.
enum class EventDisposition : u8 { Fire, Apply, Suppress };

[[nodiscard]] EventDisposition disposition_of(SeekMode mode, SideEffectPolicy policy) noexcept {
    switch (mode) {
        case SeekMode::Runtime:
            return EventDisposition::Fire;
        case SeekMode::Preview:
            return (policy == SideEffectPolicy::ConfirmedOnly) ? EventDisposition::Suppress
                                                               : EventDisposition::Fire;
        case SeekMode::Replay:
            return EventDisposition::Suppress;
        case SeekMode::Reconstruct:
            return (policy == SideEffectPolicy::Idempotent ||
                    policy == SideEffectPolicy::ConfirmedOnly)
                       ? EventDisposition::Apply
                       : EventDisposition::Suppress;
        case SeekMode::Count:
            break;
    }
    return EventDisposition::Suppress;
}

[[nodiscard]] CameraBlend blend_from(const Segment& segment, bool out) noexcept {
    CameraBlend blend;
    blend.duration_seconds = out ? segment.blend_out_seconds : segment.blend_in_seconds;
    blend.curve = segment.blend_curve;
    blend.position = segment.blend_position;
    blend.rotation = segment.blend_rotation;
    blend.lens = segment.blend_lens;
    return blend;
}

}  // namespace

const char* playback_scope_name(PlaybackScope scope) noexcept {
    switch (scope) {
        case PlaybackScope::World:
            return "World";
        case PlaybackScope::Session:
            return "Session";
        case PlaybackScope::LocalPlayer:
            return "LocalPlayer";
        case PlaybackScope::EditorPreview:
            return "EditorPreview";
        case PlaybackScope::Count:
            break;
    }
    return "Unknown";
}

const char* instance_state_name(InstanceState state) noexcept {
    switch (state) {
        case InstanceState::Created:
            return "Created";
        case InstanceState::Preparing:
            return "Preparing";
        case InstanceState::Ready:
            return "Ready";
        case InstanceState::Playing:
            return "Playing";
        case InstanceState::Paused:
            return "Paused";
        case InstanceState::Seeking:
            return "Seeking";
        case InstanceState::Completing:
            return "Completing";
        case InstanceState::Completed:
            return "Completed";
        case InstanceState::Stopped:
            return "Stopped";
        case InstanceState::Failed:
            return "Failed";
        case InstanceState::Count:
            break;
    }
    return "Unknown";
}

const char* failure_reason_name(FailureReason reason) noexcept {
    switch (reason) {
        case FailureReason::None:
            return "None";
        case FailureReason::UnresolvedBinding:
            return "UnresolvedBinding";
        case FailureReason::UnreadyAsset:
            return "UnreadyAsset";
        case FailureReason::IncompatibleClock:
            return "IncompatibleClock";
        case FailureReason::UnsupportedTrack:
            return "UnsupportedTrack";
        case FailureReason::Count:
            break;
    }
    return "Unknown";
}

const char* playback_mode_name(PlaybackMode mode) noexcept {
    switch (mode) {
        case PlaybackMode::Once:
            return "Once";
        case PlaybackMode::Loop:
            return "Loop";
        case PlaybackMode::PingPong:
            return "PingPong";
        case PlaybackMode::Hold:
            return "Hold";
        case PlaybackMode::Manual:
            return "Manual";
        case PlaybackMode::Count:
            break;
    }
    return "Unknown";
}

const char* seek_mode_name(SeekMode mode) noexcept {
    switch (mode) {
        case SeekMode::Preview:
            return "Preview";
        case SeekMode::Runtime:
            return "Runtime";
        case SeekMode::Replay:
            return "Replay";
        case SeekMode::Reconstruct:
            return "Reconstruct";
        case SeekMode::Count:
            break;
    }
    return "Unknown";
}

SequencePlayer::SequencePlayer(Allocator& allocator, const AdapterRegistry& registry,
                               PlaybackScope scope, u32 scope_id) noexcept
    : allocator_(&allocator),
      registry_(&registry),
      scope_(scope),
      scope_id_(scope_id),
      instances_(allocator),
      generations_(allocator),
      misses_(allocator),
      scratch_(allocator) {}

SequencePlayer::~SequencePlayer() {
    for (Instance* instance : instances_.span()) {
        if (instance != nullptr) {
            instance->~Instance();
            allocator_->deallocate(instance, sizeof(Instance), alignof(Instance));
        }
    }
}

SequencePlayer::Instance* SequencePlayer::find(InstanceId instance) noexcept {
    if (instance.is_null() || instance.index() >= instances_.size()) {
        return nullptr;
    }
    if (generations_[instance.index()] != instance.generation()) {
        return nullptr;
    }
    return instances_[instance.index()];
}

const SequencePlayer::Instance* SequencePlayer::find(InstanceId instance) const noexcept {
    if (instance.is_null() || instance.index() >= instances_.size()) {
        return nullptr;
    }
    if (generations_[instance.index()] != instance.generation()) {
        return nullptr;
    }
    return instances_[instance.index()];
}

u64 SequencePlayer::target_for(const Instance& instance, u32 binding) noexcept {
    for (const BindingResolution& resolution : instance.bindings.span()) {
        if (resolution.stable_id == binding) {
            return resolution.resolved ? resolution.target : 0;
        }
    }
    return 0;
}

bool SequencePlayer::binding_resolved(const Instance& instance, u32 binding) noexcept {
    if (binding == 0) {
        return true;
    }
    for (const BindingResolution& resolution : instance.bindings.span()) {
        if (resolution.stable_id == binding) {
            return resolution.resolved;
        }
    }
    return false;
}

Expected<InstanceId, Error> SequencePlayer::create(const Program& program,
                                                   const PlayRequest& request) noexcept {
    void* storage = allocator_->allocate(sizeof(Instance), alignof(Instance));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no room for a sequence instance");
    }
    auto* instance = new (storage) Instance(*allocator_);
    instance->program = &program;
    instance->generation = program.generation();
    instance->mode = request.mode;
    instance->rate = request.rate;
    instance->prepare_policy = request.prepare;
    instance->exclusive_group = request.exclusive_group;
    instance->priority = request.priority;
    instance->ticks = request.start.ticks();
    instance->id = next_id_++;

    for (const BindingResolution& resolution : request.bindings) {
        if (Status pushed = instance->bindings.push_back(resolution); !pushed) {
            instance->~Instance();
            allocator_->deallocate(storage, sizeof(Instance), alignof(Instance));
            return make_unexpected(pushed.error());
        }
    }
    for (const ParameterOverride& parameter : request.parameters) {
        if (Status pushed = instance->parameters.push_back(parameter); !pushed) {
            instance->~Instance();
            allocator_->deallocate(storage, sizeof(Instance), alignof(Instance));
            return make_unexpected(pushed.error());
        }
    }

    // An editor preview may not drive the simulation clock: previewing is not simulating, and a
    // scrubber that advanced authoritative time would make an author's inspection a game event.
    if (scope_ == PlaybackScope::EditorPreview && program.domain() == ClockDomain::Simulation) {
        instance->state = InstanceState::Failed;
        instance->failure = FailureReason::IncompatibleClock;
    }

    // Reuse a freed slot before growing, so a long session of short cinematics does not grow this
    // array without bound. The generation is what makes a stale handle stale.
    u32 slot = static_cast<u32>(instances_.size());
    for (usize index = 0; index < instances_.size(); ++index) {
        if (instances_[index] == nullptr) {
            slot = static_cast<u32>(index);
            break;
        }
    }
    if (slot == instances_.size()) {
        if (Status pushed = instances_.push_back(instance); !pushed) {
            instance->~Instance();
            allocator_->deallocate(storage, sizeof(Instance), alignof(Instance));
            return make_unexpected(pushed.error());
        }
        if (Status pushed = generations_.push_back(1); !pushed) {
            return make_unexpected(pushed.error());
        }
    } else {
        instances_[slot] = instance;
        ++generations_[slot];
    }
    instance->slot = slot;
    return InstanceId::from_slot(slot, generations_[slot]);
}

Status SequencePlayer::prepare(InstanceId id) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    if (instance->state == InstanceState::Failed) {
        return fail(ErrorCode::PermissionDenied, failure_reason_name(instance->failure));
    }
    instance->pending_assets.clear();
    for (const PreloadEntry& entry : instance->program->preload_plan()) {
        bool known = false;
        for (const u64 asset : instance->pending_assets.span()) {
            known = known || asset == entry.asset;
        }
        if (!known) {
            if (Status pushed = instance->pending_assets.push_back(entry.asset); !pushed) {
                return pushed;
            }
        }
    }
    instance->state =
        instance->pending_assets.empty() ? InstanceState::Ready : InstanceState::Preparing;
    return ok();
}

Status SequencePlayer::asset_ready(InstanceId id, u64 asset) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    for (usize index = 0; index < instance->pending_assets.size(); ++index) {
        if (instance->pending_assets[index] == asset) {
            instance->pending_assets.remove_unordered(index);
            break;
        }
    }
    if (instance->pending_assets.empty() && instance->state == InstanceState::Preparing) {
        instance->state = InstanceState::Ready;
    }
    return ok();
}

Status SequencePlayer::report_preload_miss(InstanceId id, u64 asset, u64 substituted) noexcept {
    const Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    PreloadMiss miss;
    miss.asset = asset;
    miss.substituted = substituted;
    miss.instance = id;
    for (const PreloadEntry& entry : instance->program->preload_plan()) {
        if (entry.asset == asset) {
            miss.deadline_ticks = entry.required_at;
            break;
        }
    }
    return misses_.push_back(miss);
}

Status SequencePlayer::play(InstanceId id) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    if (instance->state == InstanceState::Failed) {
        return fail(ErrorCode::PermissionDenied, failure_reason_name(instance->failure));
    }

    // "A sequence with unresolved required bindings SHALL fail to start with a structured error
    // naming them, rather than playing partially." The name travels in `failure_detail`.
    for (const CompiledBinding& binding : instance->program->bindings()) {
        if (binding.requirement != BindingRequirement::Required) {
            continue;
        }
        if (!binding_resolved(*instance, binding.stable_id)) {
            instance->state = InstanceState::Failed;
            instance->failure = FailureReason::UnresolvedBinding;
            instance->failure_detail = binding.stable_id;
            return fail(ErrorCode::InvalidArgument, "a required binding is unresolved");
        }
    }

    if (!instance->pending_assets.empty() &&
        instance->prepare_policy == PreparePolicy::FailIfNotReady) {
        instance->state = InstanceState::Failed;
        instance->failure = FailureReason::UnreadyAsset;
        instance->failure_detail = static_cast<u32>(instance->pending_assets[0]);
        return fail(ErrorCode::Unavailable, "required content is not resident");
    }
    if (!instance->pending_assets.empty() &&
        instance->prepare_policy == PreparePolicy::WaitForContent) {
        instance->state = InstanceState::Preparing;
        return ok();
    }

    instance->state = InstanceState::Playing;
    instance->accumulator.reset();
    suspend_group(instance->exclusive_group, instance->id);
    return ok();
}

void SequencePlayer::suspend_group(Name group, u32 except) noexcept {
    if (group.is_empty()) {
        return;
    }
    // "Exclusive groups SHALL be supported, so that starting one sequence in a group predictably
    // stops or suspends another." Suspended rather than stopped: a suspended instance keeps its
    // time and its captures, so the sequence it interrupted can resume where it was.
    for (Instance* instance : instances_.span()) {
        if (instance == nullptr || instance->id == except) {
            continue;
        }
        if (instance->exclusive_group == group) {
            instance->suspended = true;
        }
    }
}

Status SequencePlayer::pause(InstanceId id) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    if (instance->state == InstanceState::Playing) {
        instance->state = InstanceState::Paused;
    }
    return ok();
}

Status SequencePlayer::resume(InstanceId id) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    if (instance->state == InstanceState::Paused) {
        instance->state = InstanceState::Playing;
        instance->accumulator.reset();
    }
    return ok();
}

Status SequencePlayer::set_rate(InstanceId id, PlayRate rate) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    if (!rate.valid()) {
        return fail(ErrorCode::InvalidArgument, "play rate out of range");
    }
    instance->rate = rate;
    instance->accumulator.reset();
    return ok();
}

Status SequencePlayer::set_parameter(InstanceId id, u32 stable_id,
                                     const ChannelValue& value) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    for (ParameterOverride& parameter : instance->parameters.span()) {
        if (parameter.stable_id == stable_id) {
            parameter.value = value;
            return ok();
        }
    }
    return instance->parameters.push_back(ParameterOverride{stable_id, value});
}

Status SequencePlayer::set_fast_forward(InstanceId id, bool active) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    instance->fast_forward = active;
    return ok();
}

bool SequencePlayer::fast_forwarding(InstanceId id) const noexcept {
    const Instance* instance = find(id);
    return instance != nullptr && instance->fast_forward;
}

Expected<InstanceStatus, Error> SequencePlayer::status(InstanceId id) const noexcept {
    const Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    InstanceStatus status;
    status.state = instance->state;
    status.failure = instance->failure;
    status.failure_detail = instance->failure_detail;
    status.time = SequenceTime::from_ticks(instance->ticks);
    status.mode = instance->mode;
    status.rate = instance->rate;
    status.active_sections = static_cast<u32>(instance->active.size());
    status.program_generation = instance->generation;
    status.suspended = instance->suspended;
    return status;
}

u32 SequencePlayer::live_instances() const noexcept {
    u32 count = 0;
    for (const Instance* instance : instances_.span()) {
        count += (instance != nullptr) ? 1U : 0U;
    }
    return count;
}

// --- The one mover ------------------------------------------------------------------------------
//
// Playing, seeking, stepping and skipping all reach `move_to()`, which is what makes "the same time
// reached by playing, seeking, stepping, or replaying SHALL be the same instant" a property of the
// code. Four functions do the work, in this order, and each is small enough to read in one screen:
// events crossed, the active set, sections left, sections entered, values emitted.

Status SequencePlayer::emit_events(Instance& instance, i64 from, i64 to, SeekMode mode,
                                   DispatchBatches& out) noexcept {
    const Program& program = *instance.program;
    const Span<const CompiledEvent> crossed = program.events_between(
        SequenceTime::from_ticks(std::min(from, to)), SequenceTime::from_ticks(std::max(from, to)));
    for (const CompiledEvent& event : crossed) {
        const EventDisposition disposition = disposition_of(mode, event.policy);
        if (disposition == EventDisposition::Suppress) {
            continue;
        }
        EventRequest request;
        request.type = event.type;
        request.policy = event.policy;
        request.binding = event.binding;
        request.target = target_for(instance, event.binding);
        request.ticks = event.ticks;
        request.applied_by_skip = disposition == EventDisposition::Apply;
        request.payload_size = event.payload_size;
        for (u16 index = 0; index < event.payload_size && index < kMaxEventPayload; ++index) {
            request.payload[index] = event.payload[index];
        }
        request.provenance.instance = instance.id;
        request.provenance.track_stable_id = event.track_stable_id;
        request.provenance.section_stable_id = event.stable_id;
        if (Status pushed = out.events.push_back(request); !pushed) {
            return pushed;
        }
    }

    // Markers are a distinct concept and travel with a flag; a marker is never a gameplay event.
    for (const CompiledMarker& marker : program.markers()) {
        const bool inside = marker.ticks > std::min(from, to) && marker.ticks <= std::max(from, to);
        if (!inside || mode == SeekMode::Replay) {
            continue;
        }
        EventRequest request;
        request.type = marker.name;
        request.policy = SideEffectPolicy::Reversible;
        request.marker = true;
        request.ticks = marker.ticks;
        request.provenance.instance = instance.id;
        if (Status pushed = out.events.push_back(request); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status SequencePlayer::leave_sections(Instance& instance, DispatchBatches& out) noexcept {
    const Program& program = *instance.program;
    for (const u32 segment_index : instance.previous.span()) {
        if (contains(instance.active.span(), segment_index)) {
            continue;
        }
        const Segment& segment = program.segments()[segment_index];
        Provenance provenance;
        provenance.instance = instance.id;
        provenance.track_stable_id = program.debug()[segment.debug].track_stable_id;
        provenance.section_stable_id = program.debug()[segment.debug].section_stable_id;
        provenance.order = segment_index;

        if (segment.kind == TrackKind::Camera) {
            CameraRequest request;
            request.binding = segment.binding;
            request.rig = target_for(instance, segment.binding);
            request.priority = segment.priority + instance.priority;
            request.release = true;
            request.blend_out = blend_from(segment, true);
            request.provenance = provenance;
            if (Status pushed = out.cameras.push_back(request); !pushed) {
                return pushed;
            }
        }
        // Section-lifetime content goes when its section does; anything longer-lived is released
        // (or handed over) at completion. Only what was actually spawned is released, which is why
        // `spawned` exists rather than a walk over the program's segments.
        if (segment.spawn_template != 0 && segment.spawn_lifetime == 0) {
            for (usize index = instance.spawned.size(); index > 0; --index) {
                if (instance.spawned[index - 1] != segment_index) {
                    continue;
                }
                ReleaseRequest request;
                request.spawned = segment.spawn_template;
                request.lifetime = SpawnLifetime::Section;
                request.provenance = provenance;
                if (Status pushed = out.releases.push_back(request); !pushed) {
                    return pushed;
                }
                instance.spawned.remove_unordered(index - 1);
            }
        }
        if (Status restored = restore_section(instance, segment_index, out); !restored) {
            return restored;
        }
    }
    return ok();
}

Status SequencePlayer::restore_section(Instance& instance, u32 segment_index,
                                       DispatchBatches& out) noexcept {
    const Segment& segment = instance.program->segments()[segment_index];
    if (segment.completion != CompletionPolicy::Restore) {
        // HoldFinal, KeepPermanently and Custom each leave the value where it is; only the captures
        // this section took are dropped.
        for (usize index = instance.captures.size(); index > 0; --index) {
            if (instance.captures[index - 1].segment == segment_index) {
                instance.captures.remove_unordered(index - 1);
            }
        }
        return ok();
    }
    // ONLY WHAT THIS SECTION TOUCHED. "An arbitrary object snapshot SHALL NOT be taken" — so the
    // restore walks the captures this section recorded and nothing else.
    for (usize index = instance.captures.size(); index > 0; --index) {
        const CaptureEntry& entry = instance.captures[index - 1];
        if (entry.segment != segment_index) {
            continue;
        }
        PropertyHost* host = registry_->host(entry.property.adapter);
        if (host != nullptr) {
            (void)host->restore(entry.target, entry.property, entry.value);
        } else {
            // No host: the value goes back through the batch instead, so that a subsystem that
            // consumes batches rather than implementing `PropertyHost` still sees the restoration.
            ValueRequest request;
            request.subsystem = segment.subsystem;
            request.target = entry.target;
            request.binding = segment.binding;
            request.property = entry.property;
            request.value = entry.value;
            request.priority = segment.priority + instance.priority;
            request.weight = 1.0F;
            request.provenance.instance = instance.id;
            request.provenance.order = segment_index;
            if (Status pushed = out.values.push_back(request); !pushed) {
                return pushed;
            }
        }
        instance.captures.remove_unordered(index - 1);
    }
    return ok();
}

Status SequencePlayer::enter_sections(Instance& instance, i64 from, DispatchBatches& out) noexcept {
    const Program& program = *instance.program;
    for (const u32 segment_index : instance.active.span()) {
        const Segment& segment = program.segments()[segment_index];
        // Two distinct questions, and conflating them is a defect this milestone's capture found:
        //   * PREPARED — the active range was entered, and the section has not started yet.
        //   * STARTED  — the section's own range was entered, from either direction.
        const bool was_active = contains(instance.previous.span(), segment_index);
        const bool inside_now = instance.ticks >= segment.start && instance.ticks <= segment.end;
        const bool inside_before = from >= segment.start && from <= segment.end;
        if (was_active && inside_before) {
            continue;  // already started, and still running
        }
        if (was_active && !inside_now) {
            continue;  // still in the pre-roll or the post-roll: announced once, not once a frame
        }
        Provenance provenance;
        provenance.instance = instance.id;
        provenance.track_stable_id = program.debug()[segment.debug].track_stable_id;
        provenance.section_stable_id = program.debug()[segment.debug].section_stable_id;
        provenance.order = segment_index;

        // The pre-roll: entered before the section's own range, which is the whole point of it.
        if (!inside_now) {
            PrepareRequest request;
            request.subsystem = segment.subsystem;
            request.binding = segment.binding;
            request.target = target_for(instance, segment.binding);
            request.asset = segment.asset;
            request.lead_ticks = segment.start - instance.ticks;
            request.provenance = provenance;
            if (Status pushed = out.prepares.push_back(request); !pushed) {
                return pushed;
            }
            if (segment.kind == TrackKind::CameraCut) {
                // A CUT IS NOT A SURPRISE. Announced over its pre-roll, with the lead time the
                // author declared, so temporal history, shadow caches and residency prepare.
                CameraRequest cut;
                cut.binding = segment.binding;
                cut.rig = target_for(instance, segment.binding);
                cut.cut = true;
                cut.anticipated = true;
                cut.cut_lead_seconds = static_cast<f32>(seconds_from_time(
                    program.rate(), SequenceTime::from_ticks(segment.start - instance.ticks)));
                cut.provenance = provenance;
                if (Status pushed = out.cameras.push_back(cut); !pushed) {
                    return pushed;
                }
            }
            continue;
        }

        if (segment.completion == CompletionPolicy::Restore) {
            if (Status captured = capture_section(instance, segment_index); !captured) {
                return captured;
            }
        }
        if (segment.spawn_template != 0) {
            SpawnRequest request;
            request.template_id = segment.spawn_template;
            request.target = target_for(instance, segment.binding);
            request.lifetime = static_cast<SpawnLifetime>(
                std::min<u8>(segment.spawn_lifetime, static_cast<u8>(SpawnLifetime::Manual)));
            request.provenance = provenance;
            if (Status pushed = out.spawns.push_back(request); !pushed) {
                return pushed;
            }
            if (Status kept = instance.spawned.push_back(segment_index); !kept) {
                return kept;
            }
        }
        if (segment.kind == TrackKind::GameplayCommand) {
            // A COMMAND IS INTENT AND FIRES ONCE, when the section is entered. It carries the
            // sequence instance and the track for diagnostics and nothing that validation reads —
            // `gameplay-framework`: "Provenance SHALL NOT affect validation, ordering, or
            // execution."
            CommandRequest request;
            request.command_stable_id = segment.command_stable_id;
            request.binding = segment.binding;
            request.target = target_for(instance, segment.binding);
            request.payload_size = segment.command_payload_size;
            for (u16 index = 0; index < segment.command_payload_size && index < kMaxEventPayload;
                 ++index) {
                request.payload[index] = segment.command_payload[index];
            }
            request.provenance = provenance;
            if (Status pushed = out.commands.push_back(request); !pushed) {
                return pushed;
            }
        }
        if (segment.kind == TrackKind::CameraCut) {
            CameraRequest cut;
            cut.binding = segment.binding;
            cut.rig = target_for(instance, segment.binding);
            cut.cut = true;
            cut.provenance = provenance;
            if (Status pushed = out.cameras.push_back(cut); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status SequencePlayer::capture_section(Instance& instance, u32 segment_index) noexcept {
    const Program& program = *instance.program;
    const Segment& segment = program.segments()[segment_index];
    const u64 target = target_for(instance, segment.binding);
    for (u32 offset = 0; offset < segment.channel_count; ++offset) {
        const CompiledChannel& channel = program.channels()[segment.channel_begin + offset];
        PropertyHost* host = registry_->host(channel.target.adapter);
        if (host == nullptr) {
            continue;
        }
        CaptureEntry entry;
        entry.target = target;
        entry.property = channel.target;
        entry.segment = segment_index;
        if (Status captured = host->capture(target, channel.target, entry.value); !captured) {
            continue;  // an adapter that cannot capture this property leaves it uncaptured
        }
        if (Status pushed = instance.captures.push_back(entry); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status SequencePlayer::emit_active(Instance& instance, DispatchBatches& out) noexcept {
    const Program& program = *instance.program;
    for (const u32 segment_index : instance.active.span()) {
        const Segment& segment = program.segments()[segment_index];
        if (instance.ticks < segment.start || instance.ticks > segment.end) {
            continue;  // inside the pre-roll or the post-roll: prepared, not evaluated
        }
        Provenance provenance;
        provenance.instance = instance.id;
        provenance.track_stable_id = program.debug()[segment.debug].track_stable_id;
        provenance.section_stable_id = program.debug()[segment.debug].section_stable_id;
        provenance.order = segment_index;

        // The instant this section is evaluated at, with its own loop behaviour applied. Local
        // rather than absolute: a looping section repeats its own range without the sequence
        // looping.
        i64 local = instance.ticks;
        const i64 span = segment.end - segment.start;
        if (segment.loop != LoopBehaviour::None && span > 0) {
            const i64 elapsed = instance.ticks - segment.start;
            i64 cycle = elapsed % span;
            if (segment.loop == LoopBehaviour::PingPong && ((elapsed / span) % 2) == 1) {
                cycle = span - cycle;
            }
            local = segment.start + cycle;
        }

        if (segment.kind == TrackKind::Camera) {
            CameraRequest request;
            request.binding = segment.binding;
            request.rig = target_for(instance, segment.binding);
            request.priority = segment.priority + instance.priority;
            request.weight = segment.weight;
            request.blend_in = blend_from(segment, false);
            request.blend_out = blend_from(segment, true);
            request.exclusive_group = segment.exclusive_group;
            request.has_framing_target = segment.framing_binding != 0;
            request.framing_target = target_for(instance, segment.framing_binding);
            request.provenance = provenance;
            if (Status pushed = out.cameras.push_back(request); !pushed) {
                return pushed;
            }
        }

        for (u32 offset = 0; offset < segment.channel_count; ++offset) {
            const u32 channel_index = segment.channel_begin + offset;
            const CompiledChannel& channel = program.channels()[channel_index];
            ChannelValue value;
            if (!program.sample(channel_index, SequenceTime::from_ticks(local), value)) {
                continue;
            }
            if (segment.kind == TrackKind::TimeScale) {
                TimeScaleRequest request;
                request.domain = segment.time_scale_domain;
                request.scale = value.components[0];
                request.provenance = provenance;
                if (Status pushed = out.time_scales.push_back(request); !pushed) {
                    return pushed;
                }
                continue;
            }
            if (!channel.target.valid()) {
                continue;
            }
            ValueRequest request;
            request.subsystem = segment.subsystem;
            request.target = target_for(instance, segment.binding);
            request.binding = segment.binding;
            request.property = channel.target;
            request.value = value;
            request.priority = segment.priority + instance.priority;
            request.weight = segment.weight;
            request.blend = segment.blend;
            request.blend_group = segment.blend_group;
            request.provenance = provenance;
            if (Status pushed = out.values.push_back(request); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status SequencePlayer::move_to(Instance& instance, i64 target_ticks, SeekMode mode, bool crossing,
                               DispatchBatches& out) noexcept {
    const i64 from = instance.ticks;
    if (crossing) {
        if (Status emitted = emit_events(instance, instance.ticks, target_ticks, mode, out);
            !emitted) {
            return emitted;
        }
    }
    instance.ticks = target_ticks;

    // THE INDEX, and nothing else. This is the whole of "seeking does not scan": one bucket lookup
    // for the destination, a difference against what was active, and a span of crossed events.
    instance.previous.clear();
    for (const u32 segment : instance.active.span()) {
        if (Status pushed = instance.previous.push_back(segment); !pushed) {
            return pushed;
        }
    }
    if (Status found =
            instance.program->active_at(SequenceTime::from_ticks(target_ticks), instance.active);
        !found) {
        return found;
    }
    if (Status left = leave_sections(instance, out); !left) {
        return left;
    }
    if (Status entered = enter_sections(instance, from, out); !entered) {
        return entered;
    }
    return emit_active(instance, out);
}

Status SequencePlayer::complete(Instance& instance, DispatchBatches& out) noexcept {
    instance.state = InstanceState::Completing;
    instance.previous.clear();
    for (const u32 segment : instance.active.span()) {
        if (Status pushed = instance.previous.push_back(segment); !pushed) {
            return pushed;
        }
    }
    instance.active.clear();
    if (Status left = leave_sections(instance, out); !left) {
        return left;
    }
    // Anything the sequence spawned and still owns goes now — and a PERSISTENT spawn is a
    // HANDOVER rather than a release, so ownership is never left ambiguous when a sequence ends.
    for (const u32 segment_index : instance.spawned.span()) {
        const Segment& segment = instance.program->segments()[segment_index];
        ReleaseRequest request;
        request.spawned = segment.spawn_template;
        request.lifetime = static_cast<SpawnLifetime>(
            std::min<u8>(segment.spawn_lifetime, static_cast<u8>(SpawnLifetime::Manual)));
        request.handover = request.lifetime == SpawnLifetime::Persistent;
        request.provenance.instance = instance.id;
        request.provenance.order = segment_index;
        if (Status pushed = out.releases.push_back(request); !pushed) {
            return pushed;
        }
    }
    instance.spawned.clear();
    instance.previous.clear();
    instance.state = InstanceState::Completed;
    return ok();
}

Status SequencePlayer::stop(InstanceId id, DispatchBatches& out) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    // "Interruption is not a special case": completion policies apply exactly as they would at a
    // natural end, which is why this calls the same function the end does.
    if (Status completed = complete(*instance, out); !completed) {
        return completed;
    }
    instance->state = InstanceState::Stopped;
    return ok();
}

Status SequencePlayer::seek(InstanceId id, SequenceTime time, SeekMode mode,
                            DispatchBatches& out) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    const InstanceState previous_state = instance->state;
    instance->state = InstanceState::Seeking;
    instance->accumulator.reset();
    const Status moved = move_to(*instance, time.ticks(), mode, true, out);
    instance->state =
        (previous_state == InstanceState::Seeking) ? InstanceState::Paused : previous_state;
    return moved;
}

Status SequencePlayer::jump_to_marker(InstanceId id, Name marker, SeekMode mode,
                                      DispatchBatches& out) noexcept {
    const Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    const CompiledMarker* found = instance->program->find_marker(marker);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "no such marker");
    }
    return seek(id, SequenceTime::from_ticks(found->ticks), mode, out);
}

Status SequencePlayer::step_to(InstanceId id, SequenceTime time, DispatchBatches& out) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    return move_to(*instance, time.ticks(), SeekMode::Runtime, true, out);
}

Status SequencePlayer::skip(InstanceId id, DispatchBatches& out) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    const Program& program = *instance->program;
    if (program.skip_policy() == SkipPolicy::NotSkippable) {
        return fail(ErrorCode::PermissionDenied, "the sequence is not skippable");
    }

    // APPLY WHAT IS SKIPPED. The declared outcomes first, then the presentation jumps to the end.
    // Stopping is not the implementation of skipping, and the compiler has already refused a
    // sequence that would have needed this to guess.
    if (program.skip_policy() == SkipPolicy::ApplyRequiredOutcomes) {
        for (const RequiredOutcome& outcome : program.required_outcomes()) {
            if (Status applied = apply_outcome(*instance, outcome, out); !applied) {
                return applied;
            }
        }
    }
    if (Status moved = move_to(*instance, program.duration().ticks(), SeekMode::Replay, false, out);
        !moved) {
        return moved;
    }
    return complete(*instance, out);
}

Status SequencePlayer::apply_outcome(Instance& instance, const RequiredOutcome& outcome,
                                     DispatchBatches& out) noexcept {
    const Program& program = *instance.program;
    if (outcome.event != 0) {
        for (const CompiledEvent& event : program.events()) {
            if (event.stable_id != outcome.event) {
                continue;
            }
            EventRequest request;
            request.type = event.type;
            request.policy = event.policy;
            request.binding = event.binding;
            request.target = target_for(instance, event.binding);
            request.ticks = event.ticks;
            request.applied_by_skip = true;
            request.payload_size = event.payload_size;
            for (u16 index = 0; index < event.payload_size && index < kMaxEventPayload; ++index) {
                request.payload[index] = event.payload[index];
            }
            request.provenance.instance = instance.id;
            request.provenance.track_stable_id = event.track_stable_id;
            request.provenance.section_stable_id = event.stable_id;
            return out.events.push_back(request);
        }
        return ok();
    }

    // No event named: the outcome is the FINAL STATE of the named track — its last section
    // evaluated at its own end, plus the command that section carries.
    const Segment* last = nullptr;
    u32 last_index = 0;
    for (usize index = 0; index < program.segments().size(); ++index) {
        const Segment& segment = program.segments()[index];
        if (program.debug()[segment.debug].track_stable_id != outcome.track) {
            continue;
        }
        if (last == nullptr || segment.end > last->end) {
            last = &segment;
            last_index = static_cast<u32>(index);
        }
    }
    if (last == nullptr) {
        return ok();
    }
    Provenance provenance;
    provenance.instance = instance.id;
    provenance.track_stable_id = outcome.track;
    provenance.section_stable_id = program.debug()[last->debug].section_stable_id;
    provenance.order = last_index;
    if (last->kind == TrackKind::GameplayCommand) {
        CommandRequest request;
        request.command_stable_id = last->command_stable_id;
        request.binding = last->binding;
        request.target = target_for(instance, last->binding);
        request.payload_size = last->command_payload_size;
        for (u16 index = 0; index < last->command_payload_size && index < kMaxEventPayload;
             ++index) {
            request.payload[index] = last->command_payload[index];
        }
        request.provenance = provenance;
        if (Status pushed = out.commands.push_back(request); !pushed) {
            return pushed;
        }
    }
    for (u32 offset = 0; offset < last->channel_count; ++offset) {
        const u32 channel_index = last->channel_begin + offset;
        ChannelValue value;
        if (!program.sample(channel_index, SequenceTime::from_ticks(last->end), value)) {
            continue;
        }
        const CompiledChannel& channel = program.channels()[channel_index];
        if (!channel.target.valid()) {
            continue;
        }
        ValueRequest request;
        request.subsystem = last->subsystem;
        request.target = target_for(instance, last->binding);
        request.binding = last->binding;
        request.property = channel.target;
        request.value = value;
        request.priority = last->priority + instance.priority;
        request.weight = last->weight;
        request.provenance = provenance;
        if (Status pushed = out.values.push_back(request); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status SequencePlayer::advance(i64 delta_nanoseconds, DispatchBatches& out) noexcept {
    for (Instance* record : instances_.span()) {
        if (record == nullptr || record->state != InstanceState::Playing || record->suspended ||
            record->mode == PlaybackMode::Manual) {
            continue;
        }
        PlayRate rate = record->rate;
        if (record->fast_forward) {
            rate.numerator *= kFastForwardFactor;
        }
        const Expected<SequenceTime, Error> delta =
            record->accumulator.advance(record->program->rate(), rate, delta_nanoseconds);
        if (!delta) {
            return Status{make_unexpected(delta.error())};
        }
        if (Status advanced = advance_one(*record, delta.value().ticks(), out); !advanced) {
            return advanced;
        }
    }
    return ok();
}

Status SequencePlayer::advance_one(Instance& instance, i64 delta_ticks,
                                   DispatchBatches& out) noexcept {
    const i64 end = instance.program->duration().ticks();
    i64 target = instance.ticks + delta_ticks;
    if (end <= 0) {
        return move_to(instance, target, SeekMode::Runtime, true, out);
    }

    switch (instance.mode) {
        case PlaybackMode::Loop:
        case PlaybackMode::PingPong:
            while (target > end) {
                // The tail of this cycle first, so an event at the very end is crossed before the
                // wrap rather than skipped by it. Exact arithmetic: `target - end` is a tick count,
                // so a loop that runs for hours lands on the same instants it did on the first
                // cycle. That is the requirement's "repeated loops SHALL NOT drift".
                if (Status moved = move_to(instance, end, SeekMode::Runtime, true, out); !moved) {
                    return moved;
                }
                target -= end;
                instance.ticks = 0;
                if (instance.mode == PlaybackMode::PingPong) {
                    instance.reverse_leg = !instance.reverse_leg;
                }
            }
            return move_to(instance, instance.reverse_leg ? (end - target) : target,
                           SeekMode::Runtime, true, out);
        case PlaybackMode::Hold:
            return move_to(instance, std::min(target, end), SeekMode::Runtime, true, out);
        case PlaybackMode::Once:
        case PlaybackMode::Manual:
        case PlaybackMode::Count:
            break;
    }
    if (target >= end) {
        if (Status moved = move_to(instance, end, SeekMode::Runtime, true, out); !moved) {
            return moved;
        }
        return complete(instance, out);
    }
    return move_to(instance, target, SeekMode::Runtime, true, out);
}

Status SequencePlayer::publish_preload(SequenceTime horizon,
                                       Array<PreloadRequest>& out) const noexcept {
    out.clear();
    for (const Instance* instance : instances_.span()) {
        if (instance == nullptr) {
            continue;
        }
        const bool live = instance->state == InstanceState::Playing ||
                          instance->state == InstanceState::Preparing ||
                          instance->state == InstanceState::Ready ||
                          instance->state == InstanceState::Paused;
        if (!live) {
            continue;
        }
        for (const PreloadEntry& entry : instance->program->preload_plan()) {
            const i64 lead = entry.required_at - instance->ticks;
            if (lead > horizon.ticks() || entry.releasable_at < instance->ticks) {
                continue;
            }
            PreloadRequest request;
            request.asset = entry.asset;
            request.lead_ticks = lead;
            request.priority = entry.priority;
            request.instance = InstanceId::from_slot(instance->slot, generations_[instance->slot]);
            if (Status pushed = out.push_back(request); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status SequencePlayer::publish_future_shots(SequenceTime horizon,
                                            Array<FutureShot>& out) const noexcept {
    out.clear();
    for (const Instance* instance : instances_.span()) {
        if (instance == nullptr || instance->state != InstanceState::Playing) {
            continue;
        }
        for (const Segment& segment : instance->program->segments()) {
            if (segment.kind != TrackKind::Camera && segment.kind != TrackKind::CameraCut) {
                continue;
            }
            const i64 lead = segment.start - instance->ticks;
            if (lead < 0 || lead > horizon.ticks()) {
                continue;
            }
            FutureShot shot;
            shot.instance = InstanceId::from_slot(instance->slot, generations_[instance->slot]);
            shot.rig = target_for(*instance, segment.binding);
            shot.framing_target = target_for(*instance, segment.framing_binding);
            shot.lead_ticks = lead;
            shot.cut = segment.kind == TrackKind::CameraCut;
            shot.importance = segment.weight;
            if (Status pushed = out.push_back(shot); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status SequencePlayer::reload(InstanceId id, const Program& program, ReloadPolicy policy,
                              DispatchBatches& out) noexcept {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such sequence instance");
    }
    if (policy == ReloadPolicy::KeepPrevious) {
        return ok();
    }
    if (policy == ReloadPolicy::Stop) {
        return stop(id, out);
    }
    const i64 equivalent = instance->ticks;
    // Binding and parameter state are preserved: they are the instance's, not the program's.
    instance->program = &program;
    instance->generation = program.generation();
    instance->active.clear();
    instance->previous.clear();
    instance->captures.clear();
    if (policy == ReloadPolicy::Restart) {
        return move_to(*instance, 0, SeekMode::Replay, false, out);
    }
    if (equivalent > program.duration().ticks()) {
        // "Where a structural change makes continuation impossible, the editor SHALL explain why
        // rather than silently restarting." The instance is left where it was and the caller is
        // told which way the change went.
        return fail(ErrorCode::OutOfRange,
                    "the recompiled sequence is shorter than the instance's current time");
    }
    return move_to(*instance, equivalent, SeekMode::Replay, false, out);
}

}  // namespace cy::sequencing
