#pragma once
// The sequence compiler: source to program, with the validations the specification requires and a
// report that names what it could not do. M8.c tasks 3.1, 3.2, 3.3 and 3.6.
//
// ================================================================================================
// THE COMPILER IS WHERE THE SPECIFICATION'S "SHALL FAIL" SCENARIOS LIVE
// ================================================================================================
//
// Five of `sequencing-and-cinematics`'s scenarios are compile-time refusals, and every one of them
// is a `Diagnostic` below with a test that breaks the rule and watches it go red:
//
//   "An impossible combination is rejected"  a presentation-domain sequence with an authoritative
//                                            gameplay track SHALL fail naming the track
//   "A cinematic cannot quietly change the world"  a presentation-only sequence attempting to
//                                            modify authoritative state SHALL fail
//   "Gameplay goes through gameplay"         authoritative change SHALL be a command or an event,
//                                            never a property track writing authoritative data
//   "Undeclared consequence is caught"       a sequence with authoritative tracks marked skippable
//                                            without declared outcomes SHALL fail
//   "An unsuitable track is rejected"        a non-deterministic adapter in a deterministic
//                                            sequence SHALL fail naming the track and the property
//
// A sixth is this milestone's own, and it is the one the roadmap's exit criterion names: A CAMERA
// BINDING DRIVEN BY A TRANSFORM TRACK IS REFUSED. `camera-system` requires that "Gameplay systems
// SHALL NOT set a camera's position or orientation directly" and `sequencing-and-cinematics`
// requires a sequence to "select rigs, lenses, and blends through the camera stack". Writing the
// transform would work, and it is the failure this milestone was told to prevent, so it is not
// left to review: the compiler refuses it and `tests/test_compile.cpp` proves the refusal fires.
//
// ================================================================================================
// COMPRESSION REPORTS ITS ACHIEVED ERROR
// ================================================================================================
//
// "Cooking SHALL be able to compress channel data — redundant key removal, quantisation within a
// declared tolerance, constant-section folding, and rotation-specific encoding — with the achieved
// error reported." So every option below is off by default (a runtime compile is not a cook), each
// one is measured against the ORIGINAL curve rather than against the previous stage, and
// `CompressionReport::max_error` is what was actually measured rather than the tolerance that was
// asked for.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/sequencing/adapters.h>
#include <cy/sequencing/program.h>
#include <cy/sequencing/source.h>

namespace cy::sequencing {

/// What a diagnostic is about. A code rather than a string comparison, because a test asserts the
/// refusal fired for the reason it was supposed to fire for.
enum class DiagnosticCode : u16 {
    None = 0,
    /// The clock domain does not permit this track's authority class.
    DomainForbidsAuthority,
    /// Authoritative change authored as a property track rather than as a command or an event.
    AuthoritativeStateWrittenDirectly,
    /// A camera binding driven by a transform track. See the header comment.
    CameraTransformWritten,
    /// Marked skippable, carries authoritative tracks, declares no required outcomes.
    SkippableWithoutOutcomes,
    /// A required outcome names a track or an event that does not exist.
    RequiredOutcomeUnresolved,
    /// A nested sequence that could not be resolved, or that nests itself.
    NestedUnresolved,
    NestedCycle,
    NestedTooDeep,
    /// No adapter is registered for the subsystem this track dispatches to.
    NoAdapter,
    /// The adapter has no such property, or has it at another type.
    UnknownProperty,
    PropertyTypeMismatch,
    /// The adapter's declaration disqualifies it for this sequence's policies.
    AdapterNotDeterministic,
    AdapterNotNetworkSafe,
    AdapterEditorOnly,
    AdapterCannotRestore,
    /// `ChannelType::Transform`: author it as three channels. See source.h.
    TransformChannelUnsupported,
    /// A boolean or enumeration channel declaring an interpolation other than `Constant`.
    DiscreteChannelInterpolated,
    /// A section whose end is not after its start, or that lies outside the declared duration.
    SectionRangeInvalid,
    SectionOutsideDuration,
    /// A track naming a binding that is not declared.
    BindingUnresolved,
    /// Two things at one stable identity.
    DuplicateStableId,
    /// A gameplay command track with no command stable id.
    CommandUndeclared,
    Count,
};

[[nodiscard]] const char* diagnostic_code_name(DiagnosticCode code) noexcept;

enum class Severity : u8 {
    Warning = 0,
    Error,
    Count,
};

/// One diagnostic, naming the authored identity it is about — which is the requirement in every
/// "SHALL fail with a diagnostic naming the track" scenario.
struct Diagnostic {
    DiagnosticCode code = DiagnosticCode::None;
    Severity severity = Severity::Error;
    Name track;
    Name detail;
    u32 track_stable_id = 0;
    u32 section_stable_id = 0;
    const char* message = "";
};

struct CompressionOptions {
    /// Remove a key whose value the neighbouring keys already reproduce within `tolerance`.
    bool remove_redundant_keys = false;
    /// Fold a channel whose keys are all one value into a constant. Evaluation then skips the
    /// search entirely, which is the "constant-section folding" the requirement names.
    bool fold_constants = false;
    /// Quantise each component to a multiple of `quantisation_step`.
    bool quantise = false;
    /// Normalise rotation keys and put them on one hemisphere before quantising, so that a
    /// quantised quaternion still interpolates the short way. The "rotation-specific encoding".
    bool rotation_encoding = false;
    f32 tolerance = 0.001F;
    f32 quantisation_step = 0.0009765625F;  // 2^-10, exactly representable
};

struct CompressionReport {
    u32 keys_in = 0;
    u32 keys_out = 0;
    u32 keys_removed = 0;
    u32 channels_folded = 0;
    u32 rotations_encoded = 0;
    /// MEASURED against the original curve at every original key time, not estimated.
    f32 max_error = 0.0F;
};

struct CompileOptions {
    CompressionOptions compression;
    /// Resolves a nested sequence's identity to its source. Null means nested sequences are an
    /// error rather than silently dropped.
    using NestedResolver = const SequenceSource* (*)(u64 sequence, void* user) noexcept;
    NestedResolver nested_resolver = nullptr;
    void* nested_user = nullptr;
    u32 max_nesting_depth = 8;
    /// A cooked compilation refuses editor-only adapters.
    bool cooking = false;
};

struct CompileReport {
    explicit CompileReport(Allocator& allocator) noexcept : diagnostics(allocator) {}

    Array<Diagnostic> diagnostics;
    CompressionReport compression;
    u32 segments = 0;
    u32 channels = 0;
    u32 events = 0;
    u32 markers = 0;
    u32 preload_entries = 0;
    u32 nested_flattened = 0;
    u32 errors = 0;
    u32 warnings = 0;

    [[nodiscard]] bool has(DiagnosticCode code) const noexcept;
    [[nodiscard]] const Diagnostic* first(DiagnosticCode code) const noexcept;
};

/// Compile one authored sequence into a program.
///
/// A class rather than a free function because compilation is several passes over shared scratch —
/// flatten, validate, build segments, compact channels, build the two indexes, derive the preload
/// plan — and a free function would either take eight out-parameters or hide the scratch in a
/// static. Constructed, used once, destroyed.
class SequenceCompiler {
public:
    SequenceCompiler(Allocator& allocator, const AdapterRegistry& registry) noexcept;

    /// Fills `out` and `report`. Fails when `report.errors` is non-zero — and the report still
    /// carries every diagnostic, because a compiler that stops at the first error makes an author
    /// fix a timeline one message at a time.
    [[nodiscard]] Status compile(const SequenceSource& source, const CompileOptions& options,
                                 Program& out, CompileReport& report) noexcept;

private:
    struct FlatSection {
        const Section* section = nullptr;
        const Track* track = nullptr;
        const SequenceSource* owner = nullptr;
        i64 offset = 0;
        u64 nested_sequence = 0;
    };

    [[nodiscard]] Status flatten(const SequenceSource& source, const CompileOptions& options,
                                 i64 offset, u32 depth, CompileReport& report) noexcept;
    [[nodiscard]] Status validate_track(const SequenceSource& source, const Track& track,
                                        const CompileOptions& options,
                                        CompileReport& report) noexcept;
    [[nodiscard]] Status build_segments(const CompileOptions& options, Program& out,
                                        CompileReport& report) noexcept;
    [[nodiscard]] Status build_index(Program& out) noexcept;
    [[nodiscard]] Status build_events(const SequenceSource& source, Program& out,
                                      CompileReport& report) noexcept;
    [[nodiscard]] static Status build_preload(Program& out, CompileReport& report) noexcept;
    [[nodiscard]] static Status compact_channel(const Channel& channel,
                                                const CompressionOptions& options, Program& out,
                                                CompiledChannel& compiled,
                                                CompressionReport& report) noexcept;
    static void diagnose(CompileReport& report, DiagnosticCode code, Severity severity,
                         const Track* track, const Section* section, const char* message,
                         Name detail = Name{}) noexcept;

    Allocator* allocator_;
    const AdapterRegistry* registry_;
    Array<FlatSection> flat_;
    Array<u64> nesting_;
    Array<CompiledEvent> pending_events_;
    Array<CompiledMarker> pending_markers_;
};

}  // namespace cy::sequencing
