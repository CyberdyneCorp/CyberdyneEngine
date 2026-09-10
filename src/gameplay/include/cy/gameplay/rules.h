#pragma once
// Composable rules, the rules asset, and game phases. M8.b tasks 3.1 and 3.2.
//
// `gameplay-framework` — "Rules are composable and separate from state": rules are "**composable
// pieces** rather than a subclass chain: spawn selection, victory conditions, team formation,
// respawn, economy, and time rules SHALL be independently selectable", a **rules asset** declares a
// composition with its parameters "so that a game mode is authored as data", and "There SHALL NOT
// be an engine-provided game mode base class intended for subclassing."
//
// And "Game phases": the current phase is a **gameplay tag** so phases are hierarchical and
// extensible without changing an engine enumeration; rules validate transitions; transitions emit
// entering and leaving events "recorded with the authoritative simulation tick"; and systems
// declare the phases during which they are active.
//
// ================================================================================================
// WHY A RULE PIECE IS DATA AND A HOOK, NOT A CLASS
// ================================================================================================
//
// The subclass chain this requirement forbids appears the moment a rule is an object with a virtual
// method, because the second rule that needs one more field derives from the first. So a piece is a
// kind, a name, a parameter list and — for the logic data cannot express — the NAME of a native or
// scripted implementation the host resolves. A designer varying victory conditions and starting
// resources edits parameters; nobody derives from anything.
//
// ================================================================================================
// THE PHASE IS A TAG BECAUSE OVERTIME IS NOT AN ENGINE CONCERN
// ================================================================================================
//
// "WHEN a game adds an overtime phase THEN it SHALL add a tag and transition rule, without
// modifying an engine type." An enumeration cannot do that; a `TagId` can, and it brings the
// hierarchy with it — `Match.Play.Overtime` matches a system declared active in `Match.Play`
// without that system knowing overtime exists.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/validation.h>

namespace cy::gameplay {

/// The rule kinds the requirement names, plus the project's own. Independently selectable: a mode
/// composes one of each it cares about and leaves the rest out.
enum class RuleKind : u8 {
    SpawnSelection = 0,
    Victory,
    TeamFormation,
    Respawn,
    Economy,
    Time,
    /// `name` says which. The extension point that keeps this enumeration from growing per game.
    ProjectDefined,
    Count,
};

const char* rule_kind_name(RuleKind kind) noexcept;

/// One parameter of one piece. A number or a name — the two things authored data is.
struct RuleParameter {
    Name name;
    f64 number = 0.0;
    Name text;
};

/// One composable rule. **Not a base class**: a kind, parameters, and the name of the
/// implementation the host resolves for logic data cannot express.
struct RulePiece {
    RuleKind kind = RuleKind::ProjectDefined;
    Name name;
    /// A native or scripted implementation, resolved by the host. Empty when the parameters are
    /// the whole rule, which is the common case and the one a designer authors.
    Name implementation;
    u32 first_parameter = 0;
    u32 parameter_count = 0;
};

/// A game mode, authored as data: a composition of pieces with their parameters.
class RulesAsset {
public:
    RulesAsset(Allocator& allocator, Name name) noexcept;

    RulesAsset(const RulesAsset&) = delete;
    RulesAsset& operator=(const RulesAsset&) = delete;
    RulesAsset(RulesAsset&&) noexcept = default;
    RulesAsset& operator=(RulesAsset&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }

    [[nodiscard]] Expected<u32, Error> add_piece(RuleKind kind, Name name,
                                                 Name implementation = Name{}) noexcept;
    [[nodiscard]] Status set_parameter(u32 piece, Name parameter, f64 number) noexcept;
    [[nodiscard]] Status set_parameter(u32 piece, Name parameter, Name text) noexcept;

    [[nodiscard]] u32 piece_count() const noexcept { return static_cast<u32>(pieces_.size()); }
    [[nodiscard]] const RulePiece& piece_at(u32 index) const noexcept { return pieces_[index]; }
    /// The first piece of that kind, or `piece_count()`. A composition names each kind once; a
    /// mode that wants two victory conditions composes one victory rule with two parameters, which
    /// is what keeps "which one wins" out of the container's iteration order.
    [[nodiscard]] u32 find(RuleKind kind) const noexcept;
    [[nodiscard]] const RuleParameter* parameter(u32 piece, Name name) const noexcept;
    [[nodiscard]] f64 number(u32 piece, Name name, f64 fallback = 0.0) const noexcept;

    /// A content hash over the composition and its parameters. Two assets that compose the same
    /// mode hash the same; one that changed a starting resource does not.
    [[nodiscard]] u64 digest() const noexcept;

private:
    Name name_;
    Array<RulePiece> pieces_;
    Array<RuleParameter> parameters_;
};

/// What a phase transition did, recorded with the tick it happened at.
enum class PhaseEventKind : u8 { Leaving = 0, Entering, Count };

struct PhaseEvent {
    PhaseEventKind kind = PhaseEventKind::Entering;
    TagId phase = kInvalidTag;
    /// The authoritative simulation tick. "Transitions are recorded" is about this field: replay
    /// and network peers agree on WHEN a match began because the transition carries the tick.
    u64 tick = 0;
};

/// The session's phase, the transitions rules permit, and the events a transition emits.
class PhaseController {
public:
    explicit PhaseController(Allocator& allocator) noexcept;

    PhaseController(const PhaseController&) = delete;
    PhaseController& operator=(const PhaseController&) = delete;

    /// Permit `from` -> `to`. `kInvalidTag` as `from` is the session's INITIAL state — the phase
    /// a session is in before it has entered one — and not a wildcard: a rule that meant "from
    /// anywhere" would be indistinguishable from a rule that meant "at the start", and the two
    /// have opposite consequences for a phase nothing should return to.
    [[nodiscard]] Status allow(TagId from, TagId to) noexcept;

    /// Permit `to` from ANY phase. How an abort or a disconnect phase is declared without
    /// enumerating every source — said explicitly, because it is the dangerous one.
    [[nodiscard]] Status allow_from_any(TagId to) noexcept;

    /// Ask for a transition. Returns a `ValidationResult` rather than a bool for the reason
    /// `validation.h` gives at length: the interface, the AI, the authority and a test all read the
    /// same answer, and "not now" needs to say why.
    [[nodiscard]] ValidationResult check(TagId to) const noexcept;

    /// Perform it, recording the leaving and entering events at `tick`. Refuses exactly what
    /// `check()` refuses.
    [[nodiscard]] ValidationResult enter(TagId to, u64 tick) noexcept;

    [[nodiscard]] TagId current() const noexcept { return current_; }

    /// Is a system declared active in `declared` active while the session is in `current()`?
    /// Hierarchical: a system declared for `Match.Play` is active in `Match.Play.Overtime`.
    [[nodiscard]] bool active_in(const TagRegistry& registry, TagId declared) const noexcept;

    [[nodiscard]] u32 event_count() const noexcept { return static_cast<u32>(events_.size()); }
    [[nodiscard]] const PhaseEvent& event_at(u32 index) const noexcept { return events_[index]; }
    void clear_events() noexcept { events_.clear(); }

private:
    /// A `from` of this permits the transition from every phase. Distinct from `kInvalidTag`,
    /// which is the initial state.
    static constexpr TagId kAnyPhase = 0xFFFFFFFFU;

    struct Transition {
        TagId from = kInvalidTag;
        TagId to = kInvalidTag;
    };

    Array<Transition> transitions_;
    Array<PhaseEvent> events_;
    TagId current_ = kInvalidTag;
};

}  // namespace cy::gameplay
