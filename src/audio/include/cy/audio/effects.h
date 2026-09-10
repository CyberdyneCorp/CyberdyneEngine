#ifndef CY_AUDIO_EFFECTS_H
#define CY_AUDIO_EFFECTS_H
// Bus effects, their parameters, and their latency. M8.b task 10.2.
//
// `audio`: "The engine SHALL provide bus effects: gain, parametric EQ, low/high/band-pass and shelf
// filters, compressor, limiter, gate, reverb (algorithmic and convolution), delay, chorus, flanger,
// phaser, distortion, pitch shift, stereo widener, panner, and an analysis tap (spectrum and level
// metering). Effects SHALL be parameterisable at runtime with interpolated parameter changes, and
// SHALL report their latency so the engine can compensate. Custom effects SHALL be implementable in
// native code and registered like built-ins."
//
// --- WHAT IS IMPLEMENTED, AND WHAT IS DECLARED ---------------------------------------------------
//
// Stated here rather than discovered in a profile. IMPLEMENTED AND PROCESSING SAMPLES: gain, the
// biquad family (low-pass, high-pass, band-pass, low and high shelf, peaking EQ), delay,
// compressor, limiter, gate, distortion, panner, stereo widener, and the analysis tap. DECLARED
// WITH A LATENCY AND NO PROCESSING YET: algorithmic and convolution reverb, chorus, flanger, phaser
// and pitch shift — `EffectChain::process` passes their audio through unchanged and
// `EffectInstance:: implemented` is false, so a caller can tell rather than hearing nothing and
// wondering.
//
// That is the same rule this milestone's other modules follow: a capability that is not there is
// QUERYABLE, not silently absent.
//
// --- PARAMETERS INTERPOLATE, AND LATENCY IS REPORTED ---------------------------------------------
//
// A parameter set at runtime moves toward its new value over `interpolation_seconds` rather than
// jumping, because a jumped filter cutoff is a click. And every effect reports `latency_samples`,
// which the engine adds up per bus so the timing queries `audio`'s synchronisation requirement
// names are the time the LISTENER hears rather than the time the mixer wrote.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::audio {

/// The effect kinds. `audio` names sixteen and this is the sixteen.
enum class EffectKind : u8 {
    Gain = 0,
    ParametricEq,
    LowPass,
    HighPass,
    BandPass,
    LowShelf,
    HighShelf,
    Compressor,
    Limiter,
    Gate,
    ReverbAlgorithmic,
    ReverbConvolution,
    Delay,
    Chorus,
    Flanger,
    Phaser,
    Distortion,
    PitchShift,
    StereoWidener,
    Panner,
    AnalysisTap,
    /// A project's own, registered like a built-in.
    Custom,
    Count,
};

[[nodiscard]] const char* effect_kind_name(EffectKind kind) noexcept;

/// Whether this build actually processes that effect, or passes its audio through. See the header:
/// a capability that is not there is queryable rather than silent.
[[nodiscard]] bool effect_is_implemented(EffectKind kind) noexcept;

/// How many samples of delay an effect introduces, at a sample rate. Reported so the engine can
/// compensate — "WHEN an effect reports processing latency THEN the engine SHALL account for it in
/// timing queries used for synchronisation."
[[nodiscard]] u32 effect_latency_samples(EffectKind kind, u32 sample_rate) noexcept;

/// The parameters an effect reads. One struct for every kind, because an effect chain is an array
/// and an array of variants would be an allocation per parameter change.
struct EffectParameters {
    /// Linear gain. `Gain`, and the wet level of everything else.
    f32 gain = 1.0F;
    /// Hertz. The filters' corner, the EQ's centre.
    f32 frequency = 1000.0F;
    /// Q. The filters' resonance and the EQ's width.
    f32 q = 0.707F;
    /// Decibels. The shelf's and the peaking EQ's amount.
    f32 decibels = 0.0F;
    /// Compressor, limiter and gate: the level they act at, in linear amplitude.
    f32 threshold = 0.5F;
    /// Compressor: how much above the threshold is removed. Four is 4:1.
    f32 ratio = 4.0F;
    f32 attack_seconds = 0.005F;
    f32 release_seconds = 0.1F;
    /// Delay and the modulated effects: seconds.
    f32 time_seconds = 0.25F;
    f32 feedback = 0.3F;
    /// Wet/dry, in [0, 1].
    f32 mix = 0.5F;
    /// Panner: −1 is fully left, +1 fully right. Widener: 0 is mono, 1 is untouched, above widens.
    f32 pan = 0.0F;
    f32 width = 1.0F;
};

/// A custom effect's processing function. A function pointer rather than an interface: this is
/// called per block per instance, and a virtual call on that path is a dispatch the mixer pays for
/// in every callback.
using CustomEffectFn = void (*)(f32* samples, u32 frames, u32 channels,
                                const EffectParameters& parameters, void* user) noexcept;

/// One effect in a chain.
struct EffectInstance {
    EffectKind kind = EffectKind::Gain;
    Name name;
    /// The parameters in force. Moved toward `target` by `EffectChain::advance`.
    EffectParameters current;
    EffectParameters target;
    bool bypassed = false;
    /// False when this build declares the effect but does not process it. Queryable, not silent.
    bool implemented = true;
    u32 latency_samples = 0;
    /// `Custom` only.
    CustomEffectFn custom = nullptr;
    void* custom_user = nullptr;
};

/// What the analysis tap measured. "an analysis tap (spectrum and level metering)."
struct AnalysisResult {
    /// Linear peak and root-mean-square over the last block.
    f32 peak = 0.0F;
    f32 rms = 0.0F;
    /// Coarse band energies: low, low-mid, high-mid, high. Four bands rather than a full spectrum,
    /// because a meter is what a mixer window draws and an FFT per bus per block is not free —
    /// a caller wanting a real spectrum registers a custom effect with one.
    f32 bands[4] = {};
};

struct ChainReport {
    u32 effects = 0;
    u32 processed = 0;
    u32 bypassed = 0;
    /// Effects declared but not implemented in this build, which passed their audio through.
    u32 passthrough = 0;
    /// The sum of every effect's latency: what the engine compensates for.
    u32 latency_samples = 0;
};

/// A bus's effect chain.
class EffectChain {
public:
    EffectChain(Allocator& allocator, u32 sample_rate) noexcept;

    EffectChain(const EffectChain&) = delete;
    EffectChain& operator=(const EffectChain&) = delete;

    [[nodiscard]] Status add(const EffectInstance& effect) noexcept;
    [[nodiscard]] Status remove(Name name) noexcept;
    [[nodiscard]] EffectInstance* find(Name name) noexcept;
    [[nodiscard]] Span<const EffectInstance> effects() const noexcept { return effects_.span(); }

    /// Set a parameter target. The chain interpolates toward it — a jumped cutoff is a click.
    [[nodiscard]] Status set_parameters(Name name, const EffectParameters& parameters) noexcept;
    /// Advance the interpolation. Called once per block, before `process`.
    void advance(f32 dt) noexcept;
    f32 interpolation_seconds = 0.02F;

    /// Process one interleaved block in place.
    [[nodiscard]] Status process(f32* samples, u32 frames, u32 channels,
                                 ChainReport& report) noexcept;

    /// The last analysis tap's result.
    [[nodiscard]] const AnalysisResult& analysis() const noexcept { return analysis_; }

    /// The chain's total latency, which the engine adds to the audio clock.
    [[nodiscard]] u32 latency_samples() const noexcept;

private:
    static constexpr usize kNoDelayLine = ~static_cast<usize>(0);

    struct State {
        /// Biquad state, per channel.
        f32 x1[2] = {};
        f32 x2[2] = {};
        f32 y1[2] = {};
        f32 y2[2] = {};
        /// Dynamics envelope, per channel.
        f32 envelope[2] = {};
        /// Delay line write cursor.
        u32 cursor = 0;
        /// Where this effect's delay line starts, or `kNoDelayLine` when it needs none. ONLY THE
        /// EFFECTS THAT DELAY GET ONE: a line is a second of audio per channel, and giving one to
        /// every gain and filter in a chain would cost a megabyte and a millisecond of zeroing for
        /// nothing.
        usize delay_base = kNoDelayLine;
    };

    Array<EffectInstance> effects_;
    Array<State> states_;
    Array<f32> delay_lines_;
    AnalysisResult analysis_;
    u32 sample_rate_ = 48000;
};

}  // namespace cy::audio

#endif  // CY_AUDIO_EFFECTS_H
