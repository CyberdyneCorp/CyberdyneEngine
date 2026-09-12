#pragma once
// CyberField's vocabulary: what a field IS, who is allowed to write it, and who is allowed to read
// it. M10 tasks 1.1, 1.2 and 1.4.
//
// `environment-fields` — "Environment fields are a shared substrate": sparse, tiled, world-scale
// data addressed by world position, "a capability beneath terrain, foliage, and water — not owned
// by any of them". The failure that shape exists to prevent is named in the specification's own
// purpose: putting moisture inside terrain makes foliage depend on terrain to know whether the
// ground is wet, and the set of such dependencies is a knot.
//
// --- THIS FILE IS DESIGNED FOR PRODUCERS THAT DO NOT EXIST YET ----------------------------------
//
// Six of M10's rows write into this substrate and none of them has been written. A declaration that
// fits terrain and water and has to be EDITED for weather is the failure mode here, so every axis a
// producer might vary is a field of `FieldDeclaration` rather than a case in this module's code:
//
//   * the quantity's shape       — `FieldType`, one to four components, or an integer category;
//   * how it is stored           — `FieldEncoding`, f32 or quantised, because a world-scale
//                                  moisture field at f32 costs four times what it needs to;
//   * how thick the world is     — `vertical_cells`, 1 for a planar field and more for a
//                                  volumetric one. ONE sampling path with a degenerate case, not
//                                  two paths: see `store.h`;
//   * what resolutions exist     — `levels`, up to three, declared in metres per cell;
//   * how contributions combine  — `layer_rule`, so a baked base and a runtime delta compose by a
//                                  DECLARED rule rather than by write order;
//   * what it means for the sim  — `classification`, which is `simulation-and-determinism`'s and
//                                  not a second mechanism beside it;
//   * whether it recovers        — `potential`, so "a burned forest is still forest country" is a
//                                  property of a declaration rather than a convention.
//
// A project declaring a `radiation` field therefore writes a `FieldDeclaration` and changes no
// engine code, which is the specification's "Project field" scenario as a build property.
//
// --- ONE PRODUCER PER FIELD, AND WHY IT IS BOTH REFUSED AND UNSPELLABLE -------------------------
//
// "Two systems writing one field SHALL be a configuration error detected at startup or cook time,
// not a last-writer-wins race resolved at runtime."
//
//   REFUSED. `FieldRegistry::claim()` fails the second claim and NAMES BOTH PRODUCERS — the
//   incumbent and the challenger — in the error's own message, not only in a log line. That is
//   M10 tasks.md 1.2's exit criterion and `test_producers.cpp` is where it is held.
//
//   UNSPELLABLE. The only way to obtain a writable view of a field is `ProducerToken`, which
//   `claim()` issues once and which is move-only: there is no expression that gives a second system
//   a writer, so the race cannot be written even by a caller that ignores the error. This is the
//   shape `cy::determinism::Classified<>` uses for the firewall and it is picked for the same
//   reason: a diagnostic that fires at run time has already shipped the defect.
//
// --- DETERMINISM IS INHERITED ------------------------------------------------------------------
//
// design.md §3: "M10 adds no second mechanism." A field carries a `determinism::SimulationClass`,
// the firewall's direction is `determinism::may_read()`, and a gameplay-visible field's value is
// made streaming-independent by a REFUSAL at declaration rather than by a runtime check — see
// `validate_declaration()` and `store.h`'s `sample_deterministic()`.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>

namespace cy::environment {

// --- Identity -----------------------------------------------------------------------------------

/// A field's identity: a hash of its stable name, resolved at compile time where the name is a
/// literal.
///
/// An identity and not an index, for the reason `determinism::StreamId` is one: two systems that
/// independently name `moisture` get the same field without having been introduced, and a field's
/// identity does not depend on declaration order. Save data, cooked tiles and the persistence
/// overlay key on it.
struct FieldId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(FieldId, FieldId) noexcept = default;
};

/// A producer's identity, derived the same way, so a refusal can name it without holding a string.
struct ProducerId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(ProducerId, ProducerId) noexcept = default;
};

namespace detail {

/// FNV-1a over a name. Unseeded and `constexpr`, which is the whole point: `cy::hash_bytes()` is
/// randomised per process in development builds (core/memory/hash.h) and a field identity that
/// changed between two runs of the cooker would not be an identity. `random.h`'s `hash_name()` is
/// the same function for the same reason; it is spelled again here rather than reached into,
/// because it lives in that module's `detail` namespace and a substrate should not depend on
/// another module's private spelling.
[[nodiscard]] constexpr u64 hash_name(const char* text) noexcept {
    u64 value = 0xcbf2'9ce4'8422'2325ULL;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        value ^= static_cast<u64>(static_cast<u8>(*cursor));
        value *= 0x0000'0100'0000'01b3ULL;
    }
    return value;
}

}  // namespace detail

[[nodiscard]] constexpr FieldId field_id(const char* name) noexcept {
    return FieldId{detail::hash_name(name)};
}

[[nodiscard]] constexpr ProducerId producer_id(const char* name) noexcept {
    // Salted differently from `field_id`, so that a field and a producer of the same name are not
    // one number. Nothing compares the two, and that is exactly why they must not be equal by
    // accident in a diagnostic that prints both.
    return ProducerId{detail::hash_name(name) ^ 0x5052'4f44'5543'4552ULL};  // "PRODUCER"
}

/// The standard fields `environment-fields` requires the engine to define. Names, not declarations:
/// the meaning, unit, resolution and default of each belong to whichever row PRODUCES it, and a
/// substrate that declared them here would be a substrate with terrain's opinion baked into it.
///
/// They are listed so that two rows naming `wetness` name one field. `FieldRegistry::declare()`
/// accepts any name; these are the ones the specification says must exist.
namespace fields {
inline constexpr const char* kBiome = "biome";
inline constexpr const char* kMoisture = "moisture";
inline constexpr const char* kTemperature = "temperature";
inline constexpr const char* kSoil = "soil";
inline constexpr const char* kSnowDepth = "snow-depth";
inline constexpr const char* kWaterDistance = "water-distance";
inline constexpr const char* kWaterDepth = "water-depth";
inline constexpr const char* kWaterFlow = "water-flow";
inline constexpr const char* kWetness = "wetness";
inline constexpr const char* kWind = "wind";
inline constexpr const char* kBurnState = "burn-state";
inline constexpr const char* kHumanExclusion = "human-exclusion";
}  // namespace fields

// --- The shape of a value -----------------------------------------------------------------------

/// What one sample is. `Category` is the integer kind — biome, soil, a project's terrain-type index
/// — and it is separate from `Scalar` because a category that is interpolated is a category that
/// has been averaged into a value naming nothing.
enum class FieldType : u8 {
    Scalar = 0,
    Vec2,
    Vec3,
    Vec4,
    Category,
};

[[nodiscard]] const char* field_type_name(FieldType type) noexcept;

[[nodiscard]] constexpr u32 component_count(FieldType type) noexcept {
    switch (type) {
        case FieldType::Scalar:
        case FieldType::Category:
            return 1;
        case FieldType::Vec2:
            return 2;
        case FieldType::Vec3:
            return 3;
        case FieldType::Vec4:
            return 4;
    }
    return 1;
}

/// How a component is stored. A world-scale field is mostly storage, so the encoding is a
/// declaration rather than an assumption: moisture in [0,1] at `UNorm8` costs a quarter of `F32`
/// and is still finer than the phenomenon it describes.
///
/// `UNorm8` and `UNorm16` map the declared `[range_min, range_max]` onto the integer range, so the
/// declared range is part of the storage contract and not merely documentation.
enum class FieldEncoding : u8 {
    F32 = 0,
    UNorm8,
    UNorm16,
    /// Raw integers, for `Category`. No range mapping: the stored number IS the value.
    Uint8,
    Uint16,
};

[[nodiscard]] const char* field_encoding_name(FieldEncoding encoding) noexcept;

[[nodiscard]] constexpr u32 encoding_bytes(FieldEncoding encoding) noexcept {
    switch (encoding) {
        case FieldEncoding::F32:
            return 4;
        case FieldEncoding::UNorm16:
        case FieldEncoding::Uint16:
            return 2;
        case FieldEncoding::UNorm8:
        case FieldEncoding::Uint8:
            return 1;
    }
    return 4;
}

/// A sampled value. Four components because that is the widest `FieldType`, and a fixed array
/// because a sample is returned by value into a caller that is about to do arithmetic with it.
struct FieldValue {
    f32 components[4] = {0.0F, 0.0F, 0.0F, 0.0F};

    [[nodiscard]] static constexpr FieldValue scalar(f32 value) noexcept {
        return FieldValue{{value, 0.0F, 0.0F, 0.0F}};
    }
    [[nodiscard]] static constexpr FieldValue vec2(f32 x, f32 y) noexcept {
        return FieldValue{{x, y, 0.0F, 0.0F}};
    }
    [[nodiscard]] static constexpr FieldValue vec3(f32 x, f32 y, f32 z) noexcept {
        return FieldValue{{x, y, z, 0.0F}};
    }
    /// A category's value. Exact for every index below 2^24, which is every category any world will
    /// have; the float carrier keeps one value type across every field kind.
    [[nodiscard]] static constexpr FieldValue category(u32 index) noexcept {
        return FieldValue{{static_cast<f32>(index), 0.0F, 0.0F, 0.0F}};
    }

    [[nodiscard]] constexpr f32 x() const noexcept { return components[0]; }
    [[nodiscard]] constexpr f32 y() const noexcept { return components[1]; }
    [[nodiscard]] constexpr f32 z() const noexcept { return components[2]; }
    [[nodiscard]] constexpr u32 index() const noexcept { return static_cast<u32>(components[0]); }

    friend constexpr bool operator==(const FieldValue&, const FieldValue&) noexcept = default;
};

/// How a sample between lattice points is computed. `Nearest` is mandatory for `Category` — see
/// `validate_declaration()`, which refuses the other pairing rather than silently averaging two
/// biome indices into a third.
enum class FieldInterpolation : u8 { Nearest = 0, Linear };

/// How often a field's values change. It is what a consumer caches against and what the streamer
/// uses to decide whether a tile is worth keeping; it is declared rather than inferred, because a
/// field that is static in one project and per-frame in another is the same field with two
/// declarations, not two fields.
enum class FieldCadence : u8 { Static = 0, SlowlyVarying, PerFrame };

// --- Layers -------------------------------------------------------------------------------------

/// The two contributions a field may carry. `environment-fields`: "A field MAY have contributions
/// layered by declaration — a base baked layer plus a runtime delta layer — where the layering and
/// its combination rule are part of the field's declaration rather than an emergent property of
/// write order."
///
/// TWO AND NOT N. A third layer would need a declared order between the second and the third, which
/// is the write-order property the requirement exists to forbid, dressed as configuration. A
/// producer needing more composes them before it writes the delta.
enum class FieldLayer : u8 { Base = 0, Delta, kCount };

inline constexpr u32 kFieldLayerCount = static_cast<u32>(FieldLayer::kCount);

[[nodiscard]] const char* field_layer_name(FieldLayer layer) noexcept;

/// How the delta layer combines with the base. Declared per field; applied identically by every
/// reader, which is what the specification's "Declared layering" scenario asks for.
enum class FieldLayerRule : u8 {
    /// The delta replaces the base wherever the delta has data. Terraforming.
    Replace = 0,
    /// Summed. Snow depth accumulating over a baked base.
    Add,
    /// Scaled. A burn fraction attenuating baked vegetation density.
    Multiply,
    Max,
    Min,
};

[[nodiscard]] const char* field_layer_rule_name(FieldLayerRule rule) noexcept;

/// Apply the declared rule. One function, called by the CPU sampler and transliterated by the GPU
/// one, so that "SHALL be applied consistently by every reader" is one piece of code.
[[nodiscard]] FieldValue combine_layers(FieldLayerRule rule, const FieldValue& base,
                                        const FieldValue& delta, u32 components) noexcept;

// --- Residency ----------------------------------------------------------------------------------

/// `environment-fields` — "Field residency levels": macro, regional and local, "so that a
/// world-scale field can be present everywhere at low resolution while fine data exists only where
/// it is needed".
///
/// Ordered FINEST FIRST, so that a walk from `Local` upward is a walk from precise to coarse and
/// `finest_resident` is a `<` comparison. `kCount` is three because the specification names three;
/// a field that wants only one declares only one.
enum class FieldResidency : u8 { Local = 0, Regional, Macro, kCount };

inline constexpr u32 kFieldResidencyCount = static_cast<u32>(FieldResidency::kCount);

[[nodiscard]] const char* field_residency_name(FieldResidency level) noexcept;

/// One declared resolution. `cell_metres` of zero means the level is not declared for this field,
/// which is how "its permitted resolution range" is expressed without a second pair of fields.
struct FieldLevel {
    f32 cell_metres = 0.0F;
    /// Whether this level exists everywhere in the world, whether or not anything streamed it.
    /// "Macro-level data SHALL be resident for the whole world where a field declares it, since
    /// environmental and ecosystem state must exist for regions that are not loaded."
    bool resident_everywhere = false;

    [[nodiscard]] constexpr bool declared() const noexcept { return cell_metres > 0.0F; }
};

// --- The declaration
// ------------------------------------------------------------------------------

/// How a field's values are produced, which is what decides whether it may be deterministic.
///
/// "A deterministic field SHALL NOT be produced by a GPU pass whose result is not read back
/// deterministically; such a field SHALL be either CPU-produced or declared visual." That sentence
/// is enforced by `validate_declaration()` refusing the pairing, not by a comment.
enum class FieldProduction : u8 {
    /// Cooked, or written by CPU code from cooked data, the world seed and recorded changes.
    Cpu = 0,
    /// Written by a GPU pass. Legal, and legal ONLY for a field classified `Presentation`.
    Gpu,
};

[[nodiscard]] const char* field_production_name(FieldProduction production) noexcept;

/// Everything a field declares. `environment-fields` — "Field declaration": "a stable identifier,
/// its value type, its unit and semantic meaning, its spatial resolution and permitted resolution
/// range, its interpolation mode, its default value where no data exists, and whether it is static,
/// slowly varying, or per-frame."
///
/// `name` and `unit` and `semantics` are literals the caller owns for the lifetime of the registry.
/// They are `const char*` rather than owned strings because every declaration in the engine and in
/// a project is a literal in the producer's own translation unit, and a substrate that copied them
/// would allocate at startup for a diagnostic.
struct FieldDeclaration {
    const char* name = "";
    /// The unit, so that "its meaning, unit, and range SHALL be defined by the field declaration,
    /// not inferred from the producer's implementation" is a value a diagnostic can print.
    const char* unit = "";
    /// One line saying what the quantity IS. "A field's meaning is a contract between systems that
    /// never call each other", and this is where that contract is written down.
    const char* semantics = "";

    FieldType type = FieldType::Scalar;
    FieldEncoding encoding = FieldEncoding::F32;
    FieldInterpolation interpolation = FieldInterpolation::Linear;
    FieldCadence cadence = FieldCadence::Static;
    FieldProduction production = FieldProduction::Cpu;

    /// The declared range. For `UNorm8` and `UNorm16` it is also the storage mapping, so widening
    /// it changes what every stored byte means — which is why it is part of the declaration and why
    /// `declare()` refuses a re-declaration that differs.
    f32 range_min = 0.0F;
    f32 range_max = 1.0F;

    /// The value where no data exists. Returned by a sample outside resident data, which is what
    /// makes "SHALL NOT block or fault" a return value rather than a promise.
    FieldValue default_value;

    FieldLevel levels[kFieldResidencyCount];

    /// How thick the field is. 1 is planar — every standard field but wind — and more is
    /// volumetric. `vertical_metres` is the height of ONE vertical cell and
    /// `vertical_origin_metres` is where the column starts.
    u32 vertical_cells = 1;
    f32 vertical_metres = 1.0F;
    f32 vertical_origin_metres = 0.0F;

    FieldLayerRule layer_rule = FieldLayerRule::Replace;

    /// `simulation-and-determinism`'s classification, used here and not re-invented. A field
    /// carrying `Authoritative` or `Persistent` is gameplay-visible and everything in §1.4 of the
    /// tasks applies to it; `Presentation` is the visual half and is firewalled by
    /// `determinism::may_read()`, which is the same predicate the ECS write path uses.
    determinism::SimulationClass classification = determinism::SimulationClass::Presentation;

    /// The level a gameplay-visible sample is taken at. It must be a level declared
    /// `resident_everywhere`, so that the value gameplay sees cannot depend on what streamed —
    /// `store.h`'s `sample_deterministic()`, and M10 tasks.md 1.4.
    FieldResidency gameplay_level = FieldResidency::Macro;

    /// Whether runtime changes are recorded in the world persistence overlay. "so a burned forest
    /// or a dried riverbed survives a save."
    bool persistent = false;

    /// The field this one is the CURRENT STATE of, when it has one. `environment-fields` —
    /// "Potential and current state": current state "SHALL evolve toward potential over time at a
    /// declared rate, and SHALL be reduced by events, so that an environment recovers rather than
    /// being repainted". A zero identity means this field is not a recovering one.
    FieldId potential;
    /// Fraction of the remaining gap closed per second. Zero with a `potential` set means the
    /// recovery is driven by the producer rather than by the substrate's own step.
    f32 recovery_per_second = 0.0F;

    /// The agreement two readers of one field are held to — see `gpu.h`. Zero asks the substrate to
    /// derive it from the encoding's own quantum, which is the honest default: two readers of a
    /// `UNorm8` field cannot disagree by less than one step of it.
    f32 precision = 0.0F;

    [[nodiscard]] FieldId id() const noexcept { return field_id(name); }
    [[nodiscard]] u32 components() const noexcept { return component_count(type); }
    /// Bytes one lattice point occupies.
    [[nodiscard]] u32 value_bytes() const noexcept {
        return encoding_bytes(encoding) * components();
    }
    /// The smallest difference this field can represent, which is what two readers are held to.
    [[nodiscard]] f32 resolved_precision() const noexcept;
    [[nodiscard]] bool gameplay_visible() const noexcept {
        return determinism::is_authoritative(classification);
    }
};

/// Why a declaration was refused. A code rather than a string, so a test asserts on the reason
/// rather than on the wording, and so a diagnostic can print both.
enum class DeclarationProblem : u8 {
    None = 0,
    NoName,
    NoLevel,
    LevelsNotOrdered,
    CategoryInterpolated,
    EncodingMismatch,
    EmptyRange,
    NoVerticalCells,
    /// A gameplay-visible field produced on the GPU. The specification's own sentence, refused.
    GpuAuthoritative,
    /// A gameplay-visible field whose `gameplay_level` is not declared resident everywhere: its
    /// value would depend on what streamed, which is exactly what determinism forbids.
    GameplayLevelNotGuaranteed,
    /// A re-declaration that differs from the one already registered.
    Redeclared,
};

[[nodiscard]] const char* declaration_problem_name(DeclarationProblem problem) noexcept;

/// Check a declaration on its own. Separate from the registry so that a cooker and an editor can
/// validate a declaration before a registry exists, and so this file's refusals are testable
/// without one.
[[nodiscard]] DeclarationProblem validate_declaration(const FieldDeclaration& declaration) noexcept;

// --- Producers ----------------------------------------------------------------------------------

/// The capability to write one field. Issued once per field by `FieldRegistry::claim()`, move-only,
/// and the ONLY thing `FieldStore` accepts a write through.
///
/// It is move-only rather than copyable because that is what makes the second writer unspellable
/// rather than merely refused: a system handed a token can pass it on, and then it no longer has
/// one. The refusal in `claim()` is the diagnostic; this type is the guarantee.
class ProducerToken {
public:
    constexpr ProducerToken() = default;

    ProducerToken(const ProducerToken&) = delete;
    ProducerToken& operator=(const ProducerToken&) = delete;
    ProducerToken(ProducerToken&& other) noexcept
        : field_(other.field_), producer_(other.producer_), name_(other.name_) {
        other.field_ = FieldId{};
        other.producer_ = ProducerId{};
        other.name_ = "";
    }
    ProducerToken& operator=(ProducerToken&& other) noexcept {
        if (this != &other) {
            field_ = other.field_;
            producer_ = other.producer_;
            name_ = other.name_;
            other.field_ = FieldId{};
            other.producer_ = ProducerId{};
            other.name_ = "";
        }
        return *this;
    }
    ~ProducerToken() = default;

    [[nodiscard]] constexpr FieldId field() const noexcept { return field_; }
    [[nodiscard]] constexpr ProducerId producer() const noexcept { return producer_; }
    [[nodiscard]] constexpr const char* producer_name() const noexcept { return name_; }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return field_.is_valid() && producer_.is_valid();
    }

private:
    friend class FieldRegistry;
    ProducerToken(FieldId field, ProducerId producer, const char* name) noexcept
        : field_(field), producer_(producer), name_(name) {}

    FieldId field_;
    ProducerId producer_;
    const char* name_ = "";
};

/// What a refused claim was. Both producers, named — which is M10 tasks.md 1.2's exit criterion in
/// as many words: "a second producer for the same field fails, naming both producers".
struct ProducerConflict {
    FieldId field;
    const char* field_name = "";
    /// The producer that holds the field.
    const char* incumbent = "";
    /// The producer that was refused.
    const char* challenger = "";

    [[nodiscard]] bool occurred() const noexcept { return field.is_valid(); }
};

/// What a producer claims to be. `environment-fields`: a field's producer is "a baked source, a
/// system that writes it, or a project-supplied generator" — one of the three, never two of them.
enum class ProducerKind : u8 { Baked = 0, System, Generator };

[[nodiscard]] const char* producer_kind_name(ProducerKind kind) noexcept;

// --- The registry
// ---------------------------------------------------------------------------------

/// One field, as the registry holds it.
struct FieldRecord {
    FieldDeclaration declaration;
    FieldId id;
    /// Zero until something claims it. A field with no producer is legal — a cooked field nobody
    /// writes at run time is the ordinary case — and `unclaimed()` reports them for a diagnostic.
    ProducerId producer;
    const char* producer_name = "";
    ProducerKind producer_kind = ProducerKind::Baked;
    bool claimed = false;
};

/// A consumer's declared intent to read a field, and the class it reads as.
///
/// `environment-fields`: a visual field "SHALL be declared visual, and gameplay SHALL be prevented
/// from reading it by CONFIGURATION VALIDATION". That is what this exists for: the crossing is
/// caught when the configuration is validated, before a frame has run, rather than at the sample.
struct ConsumerDeclaration {
    const char* name = "";
    determinism::SimulationClass reader_class = determinism::SimulationClass::Presentation;
    FieldId field;
};

/// A firewall crossing found by `validate()`: who, reading what, as what.
struct FirewallViolation {
    const char* consumer = "";
    const char* field_name = "";
    FieldId field;
    determinism::SimulationClass reader_class = determinism::SimulationClass::Presentation;
    determinism::SimulationClass field_class = determinism::SimulationClass::Presentation;
};

/// The declarations, the producers, and the configuration-time refusals.
///
/// It holds no data. `FieldStore` holds tiles and takes a registry by reference — the split is the
/// same one `residency` draws between policy and storage, and for the same reason: a registry that
/// could hold a value would be a registry every producer had a reason to reach into.
class FieldRegistry {
public:
    explicit FieldRegistry(Allocator& allocator) noexcept;

    FieldRegistry(const FieldRegistry&) = delete;
    FieldRegistry& operator=(const FieldRegistry&) = delete;

    /// Declare a field. Re-declaring one identically is accepted — two modules that both need
    /// `wetness` to exist should not have to agree on which of them declares it — and re-declaring
    /// it DIFFERENTLY is refused, because the difference would silently change what every stored
    /// byte means.
    [[nodiscard]] Status declare(const FieldDeclaration& declaration) noexcept;

    /// Claim the right to write a field. The second claim fails and names both producers.
    ///
    /// `producer_name` must outlive the registry: it is a literal in the producer's own translation
    /// unit, and the conflict message points at it.
    [[nodiscard]] Expected<ProducerToken, Error> claim(FieldId field, const char* producer_name,
                                                       ProducerKind kind) noexcept;

    /// The last refused claim. Valid until the next one; `Error::message` from the refusal points
    /// into this object's own buffer and has the same lifetime.
    [[nodiscard]] const ProducerConflict& last_conflict() const noexcept { return conflict_; }
    [[nodiscard]] u32 conflict_count() const noexcept { return conflicts_; }

    /// Declare that `consumer`, whose own simulation class is `reader_class`, reads `field`.
    [[nodiscard]] Status declare_consumer(const char* consumer, determinism::SimulationClass klass,
                                          FieldId field) noexcept;

    /// Configuration validation. Reports every declared consumer whose class may not read the field
    /// it declared, by `determinism::may_read()` — the engine's own firewall predicate, not a
    /// second one. Empty output is a valid configuration.
    [[nodiscard]] Status validate(Array<FirewallViolation>& out) const noexcept;

    [[nodiscard]] const FieldRecord* find(FieldId field) const noexcept;
    [[nodiscard]] const FieldDeclaration* declaration(FieldId field) const noexcept;
    [[nodiscard]] usize size() const noexcept { return records_.size(); }
    [[nodiscard]] Span<const FieldRecord> records() const noexcept { return records_.span(); }

    /// Fields nothing has claimed. A diagnostic, not an error: see `FieldRecord::producer`.
    [[nodiscard]] Status unclaimed(Array<FieldId>& out) const noexcept;

private:
    [[nodiscard]] FieldRecord* find_mutable(FieldId field) noexcept;

    Allocator* allocator_;
    Array<FieldRecord> records_;
    HashMap<u64, usize> index_;
    Array<ConsumerDeclaration> consumers_;

    ProducerConflict conflict_;
    u32 conflicts_ = 0;
    /// The formatted refusal. `Error::message` is a `const char*` with no ownership, so the message
    /// that names both producers has to live somewhere with a longer life than the call — and this
    /// is that somewhere, documented rather than leaked.
    char conflict_message_[256] = {};
};

}  // namespace cy::environment

namespace cy {

template <>
struct Hash<environment::FieldId> {
    [[nodiscard]] u64 operator()(environment::FieldId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

}  // namespace cy
