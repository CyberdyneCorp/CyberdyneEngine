// The audio server: lifecycle, playback, the game-thread update and the realtime mix.
// See cy/servers/audio/server.h.

#include <cy/servers/audio/server.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <cmath>
#include <cstring>

namespace cy::audio {
namespace {

constexpr u32 kInvalidSlot = 0xFFFFFFFFU;
/// Two, because `ChannelLayout` reaches stereo and no further at Seed. Named so the places that
/// assume it are findable when a surround layout lands.
constexpr u32 kMaxChannels = 2;

[[nodiscard]] u32 tier_index(SimulationTier tier) noexcept {
    return static_cast<u32>(tier);
}

/// One-pole low pass, applied in place. `coefficient` of zero is a pass-through.
[[nodiscard]] f32 filter_sample(f32 input, f32& state, f32 coefficient) noexcept {
    state += (input - state) * (1.0F - coefficient);
    return state;
}

/// Read one frame from a clip with linear interpolation between the two samples either side of
/// `position`. Linear rather than nearest: nearest-neighbour resampling of a pitched voice is
/// audibly grainy, and linear is what a mixer at this tier is expected to do.
[[nodiscard]] f32 sample_clip(const ClipDescription& clip, f64 position, u32 channel) noexcept {
    if (clip.samples == nullptr || clip.frame_count == 0) {
        return 0.0F;
    }
    const auto index = static_cast<u32>(position);
    if (index >= clip.frame_count) {
        return 0.0F;
    }
    const u32 next = (index + 1U < clip.frame_count) ? (index + 1U) : index;
    const auto fraction = static_cast<f32>(position - static_cast<f64>(index));
    const u32 source_channel = (channel < clip.channels) ? channel : (clip.channels - 1U);
    const f32 a = clip.samples[(static_cast<usize>(index) * clip.channels) + source_channel];
    const f32 b = clip.samples[(static_cast<usize>(next) * clip.channels) + source_channel];
    return math::lerp(a, b, fraction);
}

void apply_effect(const BusEffect& effect, f32* buffer, u32 frames, u32 channels,
                  f32* state) noexcept {
    if (effect.bypass) {
        return;
    }
    switch (effect.kind) {
        case EffectKind::Gain:
            for (usize i = 0; i < static_cast<usize>(frames) * channels; ++i) {
                buffer[i] *= effect.parameter_a;
            }
            break;
        case EffectKind::LowPass:
        case EffectKind::HighPass: {
            // One pole, per channel, with the state carried across blocks by the caller. The
            // coefficient is the usual exp(-2*pi*f/fs) form; a filter whose state reset each block
            // would click at every block boundary.
            const f32 coefficient = math::clamp(effect.parameter_b, 0.0F, 0.9999F);
            for (u32 frame = 0; frame < frames; ++frame) {
                for (u32 channel = 0; channel < channels; ++channel) {
                    const usize i = (static_cast<usize>(frame) * channels) + channel;
                    const f32 low = filter_sample(buffer[i], state[channel], coefficient);
                    buffer[i] = (effect.kind == EffectKind::LowPass) ? low : (buffer[i] - low);
                }
            }
            break;
        }
        case EffectKind::Limiter: {
            const f32 ceiling = math::max(effect.parameter_a, 0.0001F);
            for (usize i = 0; i < static_cast<usize>(frames) * channels; ++i) {
                buffer[i] = math::clamp(buffer[i], -ceiling, ceiling);
            }
            break;
        }
        case EffectKind::Count:
            break;
    }
}

}  // namespace

u32 TierBudgets::for_tier(SimulationTier tier) const noexcept {
    switch (tier) {
        case SimulationTier::FullAcoustic:
            return full_acoustic;
        case SimulationTier::Spatialised:
            return spatialised;
        case SimulationTier::Simple:
            return simple;
        case SimulationTier::Virtual:
        case SimulationTier::Count:
            break;
    }
    // `Virtual` has no budget: it is where everything the other budgets could not hold ends up, and
    // a budget on it would mean dropping voices, which the specification forbids.
    return 0xFFFFFFFFU;
}

// --- Lifecycle
// ------------------------------------------------------------------------------------

AudioServer::AudioServer(Allocator& allocator) noexcept
    : allocator_(&allocator),
      buses_(allocator),
      voices_(allocator),
      clips_(allocator),
      listeners_(allocator),
      bus_buffers_(allocator),
      levels_(allocator),
      voice_scratch_(allocator),
      ranking_(allocator) {}

AudioServer::~AudioServer() {
    shutdown();
}

const char* AudioServer::backend_name() const noexcept {
    return (backend_ == nullptr) ? "none" : backend_->name();
}

bool AudioServer::is_null_backend() const noexcept {
    return (backend_ == nullptr) || backend_->is_null_backend();
}

Status AudioServer::configure(const AudioServerConfig& config) noexcept {
    if (initialized_) {
        return fail(ErrorCode::AlreadyExists,
                    "the audio server is already initialized; configure it before initialize()");
    }
    if (config.block_frames == 0 || config.voice_capacity == 0 || config.bus_capacity == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "an audio server needs a non-zero block size, voice capacity and bus capacity");
    }
    config_ = config;
    return ok();
}

Status AudioServer::initialize() noexcept {
    AudioBackendConfig backend_config;
    backend_config.requested = config_.requested;
    if (Status started = null_backend_.initialize(backend_config); !started) {
        return started;
    }
    return initialize_with(null_backend_);
}

Status AudioServer::initialize_with(AudioBackend& backend) noexcept {
    if (initialized_) {
        return fail(ErrorCode::AlreadyExists, "the audio server is already initialized");
    }
    backend_ = &backend;
    format_ = backend.format();
    if (format_.channels() == 0 || format_.channels() > kMaxChannels) {
        backend_ = nullptr;
        return fail(ErrorCode::Unsupported,
                    "the audio server mixes mono and stereo; a wider layout arrives with the "
                    "acoustics backend");
    }
    if (Status allocated = allocate_buffers(); !allocated) {
        backend_ = nullptr;
        return allocated;
    }
    if (Status compiled = buses_.compile(); !compiled) {
        backend_ = nullptr;
        return compiled;
    }
    initialized_ = true;
    if (Status started = backend.start(&AudioServer::render_callback, this); !started) {
        initialized_ = false;
        backend_ = nullptr;
        return started;
    }
    return ok();
}

void AudioServer::shutdown() noexcept {
    if (backend_ != nullptr) {
        backend_->stop();
        backend_ = nullptr;
    }
    initialized_ = false;
    voices_.clear();
    clips_.clear();
    listeners_.clear();
    bus_buffers_.clear();
    levels_.clear();
    voice_scratch_.clear();
    ranking_.clear();
    frames_played_.store(0, std::memory_order_relaxed);
    statistics_ = AudioStatistics{};
}

Status AudioServer::allocate_buffers() noexcept {
    const u32 channels = format_.channels();
    const usize block = config_.block_frames;

    if (Status sized = voices_.resize(config_.voice_capacity); !sized) {
        return sized;
    }
    if (Status sized = clips_.resize(config_.clip_capacity); !sized) {
        return sized;
    }
    if (Status sized = listeners_.resize(config_.listener_capacity); !sized) {
        return sized;
    }
    // EVERY BUFFER THE MIXER TOUCHES IS ALLOCATED HERE, because the thread that would otherwise
    // discover it needed one is the one that must not allocate.
    if (Status sized =
            bus_buffers_.resize(static_cast<usize>(config_.bus_capacity) * channels * block);
        !sized) {
        return sized;
    }
    if (Status sized = levels_.resize(config_.bus_capacity); !sized) {
        return sized;
    }
    if (Status sized = voice_scratch_.resize(block * kMaxChannels); !sized) {
        return sized;
    }
    return ranking_.reserve(config_.voice_capacity);
}

Status AudioServer::device_changed() noexcept {
    if (backend_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the audio server has no backend");
    }
    // Reinitialise the buffers at the new rate and layout WITHOUT touching the voices: `audio`
    // requires that "playback SHALL continue" across a device change, and a voice's position is in
    // clip frames rather than device frames, so it survives a rate change unchanged.
    backend_->stop();
    format_ = backend_->format();
    if (format_.channels() == 0 || format_.channels() > kMaxChannels) {
        return fail(ErrorCode::Unsupported, "the new device's channel layout is not supported");
    }
    const u32 channels = format_.channels();
    if (Status sized = bus_buffers_.resize(static_cast<usize>(config_.bus_capacity) * channels *
                                           config_.block_frames);
        !sized) {
        return sized;
    }
    return backend_->start(&AudioServer::render_callback, this);
}

// --- Clips
// ------------------------------------------------------------------------------------------

Expected<ClipHandle, Error> AudioServer::create_clip(const ClipDescription& clip) noexcept {
    if (clip.samples == nullptr || clip.frame_count == 0) {
        return fail(ErrorCode::InvalidArgument, "a clip needs sample data and a frame count");
    }
    if (clip.channels == 0 || clip.channels > kMaxChannels || clip.sample_rate == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a clip is mono or stereo and has a non-zero sample rate");
    }
    for (usize i = 0; i < clips_.size(); ++i) {
        ClipSlot& slot = clips_[i];
        if (slot.live || slot.pending_release) {
            continue;
        }
        ++slot.generation;
        slot.live = true;
        slot.desc = clip;
        return ClipHandle::from_slot(static_cast<u32>(i), slot.generation);
    }
    return fail(ErrorCode::OutOfRange, "the audio server has no free clip slots");
}

const ClipDescription* AudioServer::clip(ClipHandle handle) const noexcept {
    const u32 index = handle.index();
    if (index >= clips_.size()) {
        return nullptr;
    }
    const ClipSlot& slot = clips_[index];
    if (!slot.live || slot.generation != handle.generation()) {
        return nullptr;
    }
    return &slot.desc;
}

void AudioServer::destroy_clip(ClipHandle handle) noexcept {
    const u32 index = handle.index();
    if (index >= clips_.size()) {
        return;
    }
    ClipSlot& slot = clips_[index];
    if (!slot.live || slot.generation != handle.generation()) {
        return;
    }
    // STOP FIRST, RELEASE LATER. The samples are borrowed and the audio thread may be part way
    // through a block that reads them, so the slot is only reused once `update()` has seen every
    // voice on it finish. The stop is a hard one rather than a fade: a click is preferable to
    // reading memory the caller is about to free.
    for (usize i = 0; i < voices_.size(); ++i) {
        VoiceSlot& voice = voices_[i];
        if (!voice.control.live || voice.playback.clip != handle) {
            continue;
        }
        AudioCommand command;
        command.kind = CommandKind::Stop;
        command.voice = VoiceHandle::from_slot(static_cast<u32>(i), voice.generation);
        command.scalar = 0.0F;  // no fade
        (void)enqueue(command);
    }
    slot.live = false;
    slot.pending_release = true;
}

// --- Listeners
// ----------------------------------------------------------------------------------------

Expected<ListenerHandle, Error> AudioServer::create_listener(const Listener& listener) noexcept {
    for (usize i = 0; i < listeners_.size(); ++i) {
        ListenerSlot& slot = listeners_[i];
        if (slot.live) {
            continue;
        }
        ++slot.generation;
        slot.live = true;
        slot.listener = listener;
        return ListenerHandle::from_slot(static_cast<u32>(i), slot.generation);
    }
    return fail(ErrorCode::OutOfRange, "the audio server has no free listener slots");
}

Status AudioServer::set_listener(ListenerHandle handle, const Listener& listener) noexcept {
    const u32 index = handle.index();
    if (index >= listeners_.size()) {
        return fail(ErrorCode::NotFound, "no such listener");
    }
    ListenerSlot& slot = listeners_[index];
    if (!slot.live || slot.generation != handle.generation()) {
        return fail(ErrorCode::NotFound, "no such listener");
    }
    slot.listener = listener;
    return ok();
}

void AudioServer::destroy_listener(ListenerHandle handle) noexcept {
    const u32 index = handle.index();
    if (index >= listeners_.size()) {
        return;
    }
    ListenerSlot& slot = listeners_[index];
    if (slot.live && slot.generation == handle.generation()) {
        slot.live = false;
    }
}

const Listener* AudioServer::primary_listener() const noexcept {
    const Listener* best = nullptr;
    for (const ListenerSlot& slot : listeners_) {
        if (!slot.live || !slot.listener.active) {
            continue;
        }
        if (best == nullptr || slot.listener.priority > best->priority) {
            best = &slot.listener;
        }
    }
    return best;
}

// --- Playback
// -------------------------------------------------------------------------------------------

f32 AudioServer::next_variation() noexcept {
    // xorshift64*, advanced once per call. A declared stream rather than a global generator, so a
    // fixed sequence of `play()` calls produces a fixed sequence of variations and a replay of one
    // frame sounds the same twice.
    variation_state_ ^= variation_state_ >> 12U;
    variation_state_ ^= variation_state_ << 25U;
    variation_state_ ^= variation_state_ >> 27U;
    const u64 value = variation_state_ * 0x2545F4914F6CDD1DULL;
    // [-1, 1]
    return (static_cast<f32>((value >> 40U) & 0xFFFFFFU) / 8388608.0F) - 1.0F;
}

u32 AudioServer::slot_of(VoiceHandle handle) const noexcept {
    const u32 index = handle.index();
    if (index >= voices_.size()) {
        return kInvalidSlot;
    }
    const VoiceSlot& slot = voices_[index];
    if (!slot.control.live || slot.generation != handle.generation()) {
        return kInvalidSlot;
    }
    return index;
}

bool AudioServer::enqueue(const AudioCommand& command) noexcept {
    if (commands_.push(command)) {
        return true;
    }
    // Counted, never silent. A dropped `Stop` leaves a sound playing forever, and a number in the
    // report is what turns that into a bug somebody can find.
    ++statistics_.dropped_commands;
    return false;
}

Expected<u32, Error> AudioServer::allocate_voice(const VoiceDescription& description) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the audio server is not initialized");
    }
    const ClipDescription* source = clip(description.clip);
    if (source == nullptr) {
        return fail(ErrorCode::NotFound, "no such audio clip");
    }
    const BusHandle bus = description.bus.is_null() ? buses_.master() : description.bus;
    const u32 bus_index = buses_.index_of(bus);
    if (bus_index == BusGraph::kInvalidIndex) {
        return fail(ErrorCode::NotFound, "no such audio bus");
    }
    if (bus_index >= config_.bus_capacity) {
        return fail(ErrorCode::OutOfRange,
                    "this bus is beyond the mixer's bus capacity, which was fixed at initialize()");
    }

    for (usize i = 0; i < voices_.size(); ++i) {
        VoiceSlot& slot = voices_[i];
        if (slot.control.live) {
            continue;
        }
        ++slot.generation;
        slot.control.live = true;
        slot.control.desc = description;
        slot.control.desc.bus = bus;
        slot.control.tier = SimulationTier::Simple;
        slot.control.importance = 0.0F;
        slot.control.volume_scale =
            math::max(1.0F + (next_variation() * description.volume_variation), 0.0F);
        slot.control.pitch_scale =
            math::max(1.0F + (next_variation() * description.pitch_variation), 0.01F);

        // Written BEFORE the Play command is pushed and never touched again: the queue's release
        // store is what publishes them to the audio thread. See server.h's class comment.
        slot.playback = VoicePlayback{};
        slot.playback.clip = description.clip;
        slot.playback.bus_index = bus_index;
        slot.playback.looping = description.looping;
        slot.playback.spatialised = description.spatialised;
        slot.playback.one_shot = description.one_shot;
        slot.playback.start_frame = description.start_frame;
        slot.mix[0] = VoiceMixState{};
        slot.mix[1] = VoiceMixState{};
        slot.finished.store(0, std::memory_order_relaxed);
        return static_cast<u32>(i);
    }
    return fail(ErrorCode::OutOfRange, "the audio server has no free voice slots");
}

Expected<VoiceHandle, Error> AudioServer::play(const VoiceDescription& description) noexcept {
    const Expected<u32, Error> slot = allocate_voice(description);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    const VoiceHandle handle = VoiceHandle::from_slot(*slot, voices_[*slot].generation);

    AudioCommand command;
    command.kind = CommandKind::Play;
    command.voice = handle;
    if (!enqueue(command)) {
        voices_[*slot].control.live = false;
        return fail(ErrorCode::Unavailable, "the audio command queue is full");
    }
    return handle;
}

Status AudioServer::play_one_shot(const VoiceDescription& description) noexcept {
    VoiceDescription one_shot = description;
    one_shot.one_shot = true;
    one_shot.looping = false;  // a looping one-shot never releases itself, which is a leak
    const Expected<VoiceHandle, Error> handle = play(one_shot);
    if (!handle) {
        return make_unexpected(handle.error());
    }
    return ok();
}

Status AudioServer::stop(VoiceHandle handle) noexcept {
    if (slot_of(handle) == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    AudioCommand command;
    command.kind = CommandKind::Stop;
    command.voice = handle;
    command.scalar = config_.stop_fade_seconds;
    return enqueue(command) ? ok()
                            : fail(ErrorCode::Unavailable, "the audio command queue is full");
}

Status AudioServer::pause(VoiceHandle handle) noexcept {
    if (slot_of(handle) == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    AudioCommand command;
    command.kind = CommandKind::Pause;
    command.voice = handle;
    return enqueue(command) ? ok()
                            : fail(ErrorCode::Unavailable, "the audio command queue is full");
}

Status AudioServer::resume(VoiceHandle handle) noexcept {
    if (slot_of(handle) == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    AudioCommand command;
    command.kind = CommandKind::Resume;
    command.voice = handle;
    return enqueue(command) ? ok()
                            : fail(ErrorCode::Unavailable, "the audio command queue is full");
}

Status AudioServer::seek(VoiceHandle handle, u32 frame) noexcept {
    if (slot_of(handle) == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    AudioCommand command;
    command.kind = CommandKind::Seek;
    command.voice = handle;
    command.scalar = static_cast<f32>(frame);
    return enqueue(command) ? ok()
                            : fail(ErrorCode::Unavailable, "the audio command queue is full");
}

Status AudioServer::set_volume(VoiceHandle handle, f32 volume) noexcept {
    const u32 slot = slot_of(handle);
    if (slot == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    // The gameplay-side value changes here; the gain the mixer applies is recomputed by the next
    // `update()` and published with it. There is no path by which the game thread writes a gain the
    // audio thread is reading.
    voices_[slot].control.desc.volume = math::max(volume, 0.0F);
    return ok();
}

Status AudioServer::set_pitch(VoiceHandle handle, f32 pitch) noexcept {
    const u32 slot = slot_of(handle);
    if (slot == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    voices_[slot].control.desc.pitch = math::clamp(pitch, 0.01F, 8.0F);
    return ok();
}

Status AudioServer::set_position(VoiceHandle handle, Vec3 position, Vec3 velocity) noexcept {
    const u32 slot = slot_of(handle);
    if (slot == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    voices_[slot].control.desc.position = position;
    voices_[slot].control.desc.velocity = velocity;
    return ok();
}

Status AudioServer::set_occlusion(VoiceHandle handle, f32 occlusion) noexcept {
    const u32 slot = slot_of(handle);
    if (slot == kInvalidSlot) {
        return fail(ErrorCode::NotFound, "no such voice");
    }
    voices_[slot].control.desc.occlusion = math::clamp(occlusion, 0.0F, 1.0F);
    return ok();
}

bool AudioServer::playing(VoiceHandle handle) const noexcept {
    const u32 index = handle.index();
    if (index >= voices_.size()) {
        return false;
    }
    const VoiceSlot& slot = voices_[index];
    return slot.control.live && slot.generation == handle.generation();
}

SimulationTier AudioServer::tier_of(VoiceHandle handle) const noexcept {
    const u32 slot = slot_of(handle);
    return (slot == kInvalidSlot) ? SimulationTier::Virtual : voices_[slot].control.tier;
}

f64 AudioServer::position_of(VoiceHandle handle) const noexcept {
    const u32 slot = slot_of(handle);
    return (slot == kInvalidSlot) ? 0.0 : voices_[slot].playback.position_frames;
}

// --- The game-thread update
// -------------------------------------------------------------------------------

void AudioServer::update(f32 delta_seconds) noexcept {
    (void)delta_seconds;
    if (!initialized_) {
        return;
    }

    // Reap what the mixer finished. The audio thread sets `finished` only after it has stopped
    // touching the slot, so freeing it here needs no synchronisation beyond the acquire.
    for (VoiceSlot& slot : voices_) {
        if (slot.finished.load(std::memory_order_acquire) != 0) {
            slot.control.live = false;
            slot.finished.store(0, std::memory_order_relaxed);
        }
    }

    // A clip whose last voice has gone can be reused. See `destroy_clip()`.
    for (usize i = 0; i < clips_.size(); ++i) {
        ClipSlot& clip_slot = clips_[i];
        if (!clip_slot.pending_release) {
            continue;
        }
        bool referenced = false;
        for (const VoiceSlot& voice : voices_) {
            if (voice.control.live && voice.playback.clip.index() == static_cast<u32>(i)) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            clip_slot.pending_release = false;
            clip_slot.desc = ClipDescription{};
        }
    }

    Listener fallback;
    const Listener* listener = primary_listener();
    const Listener& active = (listener == nullptr) ? fallback : *listener;

    score_voices(active);
    assign_tiers();
    publish_mix_state(active);
}

void AudioServer::score_voices(const Listener& listener) noexcept {
    ranking_.clear();
    for (usize i = 0; i < voices_.size(); ++i) {
        VoiceSlot& slot = voices_[i];
        if (!slot.control.live) {
            continue;
        }
        slot.control.importance = voice_importance(slot.control, listener);
        (void)ranking_.push_back(static_cast<u32>(i));
    }

    // Insertion sort, descending by importance, breaking ties by slot index. Insertion because the
    // list is nearly sorted from the previous update — importance changes slowly — and because the
    // tie-break by slot is what makes the order a function of the state rather than of the sort.
    for (usize i = 1; i < ranking_.size(); ++i) {
        const u32 value = ranking_[i];
        const f32 key = voices_[value].control.importance;
        usize j = i;
        while (j > 0) {
            const u32 previous = ranking_[j - 1];
            const f32 other = voices_[previous].control.importance;
            if (other > key || (other == key && previous < value)) {
                break;
            }
            ranking_[j] = previous;
            --j;
        }
        ranking_[j] = value;
    }
}

void AudioServer::assign_tiers() noexcept {
    u32 assigned[static_cast<usize>(SimulationTier::Count)] = {0, 0, 0, 0};
    statistics_.demoted_by_budget = 0;
    statistics_.acoustic_fallbacks = 0;

    for (const u32 index : ranking_) {
        VoiceSlot& slot = voices_[index];
        const VoiceControl& control = slot.control;

        // The tier a voice would like: inaudible sources go straight to Virtual, non-spatialised
        // ones need no panning, and everything else asks for Spatialised.
        SimulationTier wanted = SimulationTier::Spatialised;
        if (control.importance <= config_.tier_hysteresis * 0.01F) {
            wanted = SimulationTier::Virtual;
        } else if (!control.desc.spatialised) {
            wanted = SimulationTier::Simple;
        }
        // Gameplay's floor. "**WHEN** a dialogue line is pinned to at least `Spatialised` **THEN**
        // it SHALL never be demoted below that tier regardless of distance or budget pressure."
        if (static_cast<u8>(wanted) > static_cast<u8>(control.desc.minimum_tier)) {
            wanted = control.desc.minimum_tier;
        }
        if (wanted == SimulationTier::FullAcoustic) {
            // There is no acoustics backend at Seed. The specification's own fallback is to render
            // as `Spatialised`; counted rather than silent, so a project that thinks it is getting
            // propagation can see that it is not.
            wanted = SimulationTier::Spatialised;
            ++statistics_.acoustic_fallbacks;
        }

        // BUDGETS DEMOTE, THEY DO NOT DROP. A voice past every budget becomes Virtual, which
        // advances its position and mixes nothing — never stopped, because a looping ambience that
        // was stopped would restart rather than resume.
        SimulationTier tier = wanted;
        while (tier != SimulationTier::Virtual &&
               assigned[tier_index(tier)] >= config_.budgets.for_tier(tier)) {
            if (tier == control.desc.minimum_tier) {
                break;  // pinned: the floor wins over the budget
            }
            tier = static_cast<SimulationTier>(static_cast<u8>(tier) + 1U);
            ++statistics_.demoted_by_budget;
        }
        ++assigned[tier_index(tier)];
        slot.control.tier = tier;
    }

    statistics_.active_voices = 0;
    statistics_.virtual_voices = 0;
    for (usize i = 0; i < static_cast<usize>(SimulationTier::Count); ++i) {
        statistics_.voices_in_tier[i] = assigned[i];
    }
    statistics_.virtual_voices = assigned[tier_index(SimulationTier::Virtual)];
    statistics_.active_voices = static_cast<u32>(ranking_.size()) - statistics_.virtual_voices;
}

void AudioServer::publish_mix_state(const Listener& listener) noexcept {
    const u32 current = published_mix_.load(std::memory_order_relaxed);
    const u32 back = 1U - current;
    const f32 sample_rate = static_cast<f32>(format_.sample_rate);

    for (VoiceSlot& slot : voices_) {
        VoiceMixState& mix = slot.mix[back];
        mix = VoiceMixState{};
        if (!slot.control.live) {
            continue;
        }
        const VoiceControl& control = slot.control;
        mix.tier = control.tier;
        mix.audible = control.tier != SimulationTier::Virtual &&
                      buses_.audible(buses_.handle_at(slot.playback.bus_index));
        mix.pitch = math::clamp(control.desc.pitch * control.pitch_scale, 0.01F, 8.0F);

        f32 gain = control.desc.volume * control.volume_scale;
        if (control.desc.spatialised) {
            const Vec3 to_source = control.desc.position - listener.transform.translation;
            gain *= attenuation_gain(control.desc.attenuation, length(to_source));
            gain *= cone_gain(control.desc.cone, control.desc.forward, -to_source);

            f32 occlusion_gain = 1.0F;
            const f32 cutoff =
                occlusion_filter(control.desc.occlusion, sample_rate, occlusion_gain);
            gain *= occlusion_gain;
            // exp(-2*pi*f/fs), the one-pole coefficient. A cutoff at or above Nyquist gives a
            // coefficient of about zero, which is the pass-through this expression should produce.
            mix.filter_coefficient = math::clamp(
                std::exp(-2.0F * math::kPi * cutoff / math::max(sample_rate, 1.0F)), 0.0F, 0.9999F);

            mix.pitch *= doppler_pitch(listener.transform.translation, listener.velocity,
                                       control.desc.position, control.desc.velocity,
                                       config_.speed_of_sound, config_.doppler_factor);

            // A `Simple` voice is attenuated but not panned: that is exactly what the tier means.
            if (control.tier == SimulationTier::Spatialised) {
                const Vec3 local = inverse(listener.transform.rotation) * to_source;
                const PanGains pan = pan_stereo(local);
                mix.gains = PanGains{pan.left * gain, pan.right * gain};
            } else {
                constexpr f32 centre = 0.70710678F;
                mix.gains = PanGains{gain * centre, gain * centre};
            }
        } else {
            mix.gains = PanGains{gain, gain};
        }
        mix.pitch = math::clamp(mix.pitch, 0.01F, 8.0F);
    }

    // ONE RELEASE STORE PUBLISHES THE WHOLE FRAME'S MIX STATE. Everything written above happens
    // before it; the acquire load in `render()` is its pair. That is the double buffering `audio`
    // requires, and it is why the mixer never sees half of one update and half of the next.
    published_mix_.store(back, std::memory_order_release);
}

// --- The realtime mix
// -------------------------------------------------------------------------------------

void AudioServer::render_callback(f32* frames, u32 frame_count, u32 channels, void* user) noexcept {
    auto* server = static_cast<AudioServer*>(user);
    if (server != nullptr) {
        server->render(frames, frame_count, channels);
    }
}

void AudioServer::drain_commands() noexcept {
    AudioCommand command;
    while (commands_.pop(command)) {
        const u32 index = command.voice.index();
        if (command.kind == CommandKind::SetBusVolume || index >= voices_.size()) {
            continue;
        }
        VoiceSlot& slot = voices_[index];
        if (slot.generation != command.voice.generation()) {
            continue;  // the slot was reused; the command is for a voice that no longer exists
        }
        VoicePlayback& playback = slot.playback;

        switch (command.kind) {
            case CommandKind::Play:
                playback.state = VoiceState::Playing;
                playback.position_frames = 0.0;
                playback.fade = 1.0F;
                playback.previous_gains = PanGains{0.0F, 0.0F};
                break;
            case CommandKind::Stop:
                // A zero fade is a hard stop, which is what `destroy_clip()` sends. Anything else
                // fades: "Stopping or destroying a voice SHALL apply a short **fade-out** rather
                // than cutting."
                if (command.scalar <= 0.0F) {
                    playback.state = VoiceState::Stopped;
                    slot.finished.store(1, std::memory_order_release);
                } else {
                    playback.state = VoiceState::Stopping;
                }
                break;
            case CommandKind::Pause:
                if (playback.state == VoiceState::Playing) {
                    playback.state = VoiceState::Paused;
                }
                break;
            case CommandKind::Resume:
                if (playback.state == VoiceState::Paused) {
                    playback.state = VoiceState::Playing;
                }
                break;
            case CommandKind::Seek:
                playback.position_frames = static_cast<f64>(math::max(command.scalar, 0.0F));
                break;
            case CommandKind::None:
            case CommandKind::SetVolume:
            case CommandKind::SetPitch:
            case CommandKind::SetPosition:
            case CommandKind::SetOcclusion:
            case CommandKind::SetBusVolume:
            case CommandKind::Count:
                // The parameter changes travel in the published mix state rather than as commands:
                // they are recomputed every update anyway, and a queue entry per parameter per
                // voice per frame would be the queue's dominant traffic for no benefit. The
                // enumerators stay because a scheduled parameter change — a fade over a stated time
                // — is what will use them, and appending to this enum later is cheaper than
                // renumbering it.
                break;
        }
    }
}

f32* AudioServer::bus_buffer(u32 bus_index, u32 channels) noexcept {
    const usize stride = static_cast<usize>(config_.block_frames) * channels;
    return bus_buffers_.data() + (static_cast<usize>(bus_index) * stride);
}

namespace {

/// Write one frame of a voice into its bus buffer, ramping the gain from `previous` to `target`.
///
/// Extracted from the block loop so that the loop is a loop and the frame is a frame: this is the
/// only place the ramp, the resample and the occlusion filter meet, and it is small enough to read
/// as one idea.
void write_frame(f32* destination, u32 frame, u32 channels, const ClipDescription& source,
                 VoicePlayback& playback, const VoiceMixState& mix, f32 ramp, f32 fade) noexcept {
    for (u32 channel = 0; channel < channels; ++channel) {
        const f32 gain =
            math::lerp(playback.previous_gains.channel(channel), mix.gains.channel(channel), ramp) *
            fade;
        f32 sample = sample_clip(source, playback.position_frames, channel);
        if (mix.filter_coefficient > 0.0F) {
            sample = filter_sample(sample, playback.filter_state[channel], mix.filter_coefficient);
        }
        destination[(static_cast<usize>(frame) * channels) + channel] += sample * gain;
    }
}

/// Advance one frame's worth of playback position, wrapping a loop or ending the voice.
///
/// SAMPLE ACCURATE: the overshoot past the loop end is carried into the new position rather than
/// discarded, so a loop does not gain or lose a fraction of a frame every cycle and a rhythmic loop
/// does not drift.
void advance_position(VoicePlayback& playback, const ClipDescription& source, f64 advance,
                      u32 loop_end) noexcept {
    playback.position_frames += advance;
    if (playback.position_frames < static_cast<f64>(loop_end)) {
        return;
    }
    if (!playback.looping) {
        playback.state = VoiceState::Stopped;
        return;
    }
    const f64 span = static_cast<f64>(loop_end) - static_cast<f64>(source.loop_start);
    if (span > 0.0) {
        playback.position_frames -= span;
    } else {
        playback.position_frames = static_cast<f64>(source.loop_start);
    }
}

}  // namespace

void AudioServer::mix_voice(VoiceSlot& slot, const VoiceMixState& mix, u32 block_frames,
                            u32 channels) noexcept {
    VoicePlayback& playback = slot.playback;
    const ClipDescription& source = clips_[playback.clip.index()].desc;

    // How fast the clip advances per output frame: the pitch, times the ratio of the clip's rate to
    // the device's. A 44.1 kHz clip on a 48 kHz device plays at 0.919 frames per output frame,
    // which is the resampling the backend is otherwise asked to do.
    const f64 rate_ratio = (format_.sample_rate == 0) ? 1.0
                                                      : static_cast<f64>(source.sample_rate) /
                                                            static_cast<f64>(format_.sample_rate);
    const f64 advance_per_frame = static_cast<f64>(mix.pitch) * rate_ratio;
    const u32 loop_end = source.effective_loop_end();

    if (playback.state == VoiceState::Paused) {
        return;  // a paused voice advances nothing, which is what distinguishes it from virtual
    }
    const bool mixing = mix.audible;
    f32* destination = mixing ? bus_buffer(playback.bus_index, channels) : nullptr;

    const f32 fade_step =
        (playback.state == VoiceState::Stopping && config_.stop_fade_seconds > 0.0F)
            ? (1.0F / (config_.stop_fade_seconds * static_cast<f32>(format_.sample_rate)))
            : 0.0F;

    for (u32 frame = 0; frame < block_frames && playback.state != VoiceState::Stopped; ++frame) {
        // THE GAIN RAMP. Interpolated across the block from the previous block's gains to this
        // one's, so a parameter change is a slope rather than a step. `audio`: "**WHEN** a voice's
        // volume changes between callbacks **THEN** the gain SHALL ramp across the buffer rather
        // than stepping."
        const f32 ramp = static_cast<f32>(frame) / static_cast<f32>(block_frames);
        if (fade_step > 0.0F) {
            playback.fade = math::max(playback.fade - fade_step, 0.0F);
        }
        if (destination != nullptr) {
            write_frame(destination, frame, channels, source, playback, mix, ramp, playback.fade);
        }
        advance_position(playback, source, advance_per_frame, loop_end);
        if (fade_step > 0.0F && playback.fade <= 0.0F) {
            playback.state = VoiceState::Stopped;
        }
    }

    playback.previous_gains = mix.gains;
    playback.previous_pitch = mix.pitch;
    if (playback.state == VoiceState::Stopped) {
        slot.finished.store(1, std::memory_order_release);
    }
}

namespace {

/// Apply a bus's gain and measure what came out. One pass, because a second one over the same
/// samples would double the memory traffic of the mixer's hottest loop for a number a meter reads.
[[nodiscard]] BusLevel apply_gain_and_measure(f32* buffer, usize count, f32 volume) noexcept {
    BusLevel level;
    f32 sum_squares = 0.0F;
    for (usize i = 0; i < count; ++i) {
        buffer[i] *= volume;
        level.peak = math::max(level.peak, std::fabs(buffer[i]));
        sum_squares += buffer[i] * buffer[i];
    }
    level.rms = std::sqrt(sum_squares / static_cast<f32>(math::max<usize>(count, 1)));
    return level;
}

/// Add `source` into `target`, scaled. The one operation every routing edge is made of.
void accumulate_into(f32* target, const f32* source, usize count, f32 level) noexcept {
    for (usize i = 0; i < count; ++i) {
        target[i] += source[i] * level;
    }
}

}  // namespace

void AudioServer::route_bus(BusHandle handle, const f32* buffer, usize count,
                            u32 channels) noexcept {
    // Sum into the output and every send. Done in the compiled order, so a bus's own sources have
    // already been summed into it by the time it is processed — which is what makes a reverb send
    // arrive in the same block rather than a block late.
    const BusDescription* description = buses_.description(handle);
    if (description == nullptr) {
        return;
    }
    if (!description->output.is_null()) {
        const u32 output_index = buses_.index_of(description->output);
        if (output_index != BusGraph::kInvalidIndex && output_index < config_.bus_capacity) {
            accumulate_into(bus_buffer(output_index, channels), buffer, count, 1.0F);
        }
    }
    for (const BusSend& send : buses_.sends(handle)) {
        const u32 send_index = buses_.index_of(send.target);
        if (send_index != BusGraph::kInvalidIndex && send_index < config_.bus_capacity) {
            accumulate_into(bus_buffer(send_index, channels), buffer, count, send.level);
        }
    }
}

void AudioServer::process_buses(u32 block_frames, u32 channels) noexcept {
    const usize count = static_cast<usize>(block_frames) * channels;
    for (const BusHandle handle : buses_.order()) {
        const u32 index = buses_.index_of(handle);
        const BusDescription* description = buses_.description(handle);
        if (index == BusGraph::kInvalidIndex || index >= config_.bus_capacity ||
            description == nullptr) {
            continue;
        }
        f32* buffer = bus_buffer(index, channels);

        if (!description->bypass) {
            // The filter state is local to the block. A bus effect's state should carry across
            // blocks the way a voice's does; it does not yet, and that is a known gap rather than
            // an oversight — a per-bus state array is what M8's effect set needs and what it will
            // size.
            f32 state[kMaxChannels] = {0.0F, 0.0F};
            for (const BusEffect& effect : buses_.effects(handle)) {
                apply_effect(effect, buffer, block_frames, channels, state);
            }
        }

        const f32 volume = buses_.audible(handle) ? description->volume : 0.0F;
        levels_[index] = apply_gain_and_measure(buffer, count, volume);
        route_bus(handle, buffer, count, channels);
    }
}

void AudioServer::advance_virtual(VoiceSlot& slot, const VoiceMixState& mix,
                                  u32 block_frames) noexcept {
    // VIRTUALISED, NOT STOPPED: the position advances and nothing is mixed, so a looping ambience
    // resumes where it would have been rather than restarting. One multiply-add and a wrap, which
    // is what makes thousands of these affordable.
    VoicePlayback& playback = slot.playback;
    const ClipDescription& source = clips_[playback.clip.index()].desc;
    const f64 rate_ratio = (format_.sample_rate == 0) ? 1.0
                                                      : static_cast<f64>(source.sample_rate) /
                                                            static_cast<f64>(format_.sample_rate);
    playback.position_frames +=
        static_cast<f64>(mix.pitch) * rate_ratio * static_cast<f64>(block_frames);

    const u32 loop_end = source.effective_loop_end();
    if (playback.position_frames < static_cast<f64>(loop_end)) {
        return;
    }
    if (!playback.looping) {
        playback.state = VoiceState::Stopped;
        slot.finished.store(1, std::memory_order_release);
        return;
    }
    // A whole block may span several laps of a short loop, so the wrap subtracts as many spans as
    // it needs rather than one.
    const f64 span = static_cast<f64>(loop_end) - static_cast<f64>(source.loop_start);
    if (span > 0.0) {
        const f64 laps =
            std::floor((playback.position_frames - static_cast<f64>(source.loop_start)) / span);
        playback.position_frames -= span * laps;
    }
}

void AudioServer::mix_block(u32 block_frames, u32 channels, u32 published) noexcept {
    std::memset(
        static_cast<void*>(bus_buffers_.data()), 0,
        static_cast<usize>(config_.bus_capacity) * channels * config_.block_frames * sizeof(f32));

    const u64 clock = frames_played_.load(std::memory_order_relaxed);
    for (VoiceSlot& slot : voices_) {
        VoicePlayback& playback = slot.playback;
        if (playback.state == VoiceState::Stopped) {
            continue;
        }
        // A SCHEDULED VOICE IS LIVE AND SILENT UNTIL ITS FRAME. `audio`: "**WHEN** a sound is
        // scheduled for a specific audio time **THEN** it SHALL begin at that sample, not at the
        // start of the next frame." The block granularity here is the honest limit at Seed, and it
        // is stated rather than papered over.
        if (playback.start_frame > clock + block_frames) {
            continue;
        }
        const VoiceMixState& mix = slot.mix[published];

        // A VOICE THAT HAS JUST BEEN VIRTUALISED IS MIXED ONCE MORE, RAMPING TO SILENCE. `audio`
        // requires tier transitions to be "hysteretic and cross-faded, so a source oscillating near
        // a threshold does not produce audible artefacts" — and dropping a voice out of the mix the
        // instant it crosses a budget is a step of its whole amplitude, which is a click. One block
        // of ramp costs one block; after it the voice is free, because its previous gains are zero
        // and this branch is not taken again. Promotion needs no special case: a voice returning to
        // the mix already has zero previous gains and ramps up.
        const bool leaving_the_mix =
            mix.tier == SimulationTier::Virtual &&
            (playback.previous_gains.left != 0.0F || playback.previous_gains.right != 0.0F);
        if (leaving_the_mix) {
            VoiceMixState silent = mix;
            silent.tier = SimulationTier::Simple;
            silent.audible = true;
            silent.gains = PanGains{0.0F, 0.0F};
            mix_voice(slot, silent, block_frames, channels);
        } else if (mix.tier == SimulationTier::Virtual) {
            advance_virtual(slot, mix, block_frames);
        } else {
            mix_voice(slot, mix, block_frames, channels);
        }
    }

    process_buses(block_frames, channels);
}

void AudioServer::render(f32* frames, u32 frame_count, u32 channels) noexcept {
    if (frames == nullptr || frame_count == 0 || channels == 0) {
        return;
    }
    if (!initialized_ || channels != format_.channels()) {
        std::memset(static_cast<void*>(frames), 0,
                    static_cast<usize>(frame_count) * channels * sizeof(f32));
        return;
    }

    drain_commands();
    const u32 published = published_mix_.load(std::memory_order_acquire);

    u32 produced = 0;
    while (produced < frame_count) {
        const u32 block = math::min(config_.block_frames, frame_count - produced);
        mix_block(block, channels, published);

        const f32* master = bus_buffer(buses_.index_of(buses_.master()), channels);
        f32* destination = frames + (static_cast<usize>(produced) * channels);
        for (usize i = 0; i < static_cast<usize>(block) * channels; ++i) {
            destination[i] = master[i];
        }

        produced += block;
        frames_played_.fetch_add(block, std::memory_order_relaxed);
        ++statistics_.blocks_mixed;
    }
}

AudioClock AudioServer::clock() const noexcept {
    AudioClock result;
    result.frames_played = frames_played_.load(std::memory_order_relaxed);
    result.sample_rate = format_.sample_rate;
    result.output_latency_frames = (backend_ == nullptr) ? 0 : backend_->output_latency_frames();
    return result;
}

}  // namespace cy::audio
