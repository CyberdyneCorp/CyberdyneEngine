#pragma once
// The live edit compiler: an authoring change in, a validated runtime delta out. M11.b task 3.2.
//
// ================================================================================================
// THE REQUIREMENT THIS IS, AND THE ONE IT REPLACES
// ================================================================================================
//
// `live-editing` (Live editing is a compilation step): *"Changes made in the editor SHALL reach a
// running world through a live edit compiler that translates an authoring delta into a validated
// runtime delta. … Arbitrary mutation of a running world's objects from editor code SHALL NOT be a
// supported path."*
//
// What the tree had before M11.b was the forbidden shape and nothing else: the editor's only write
// path into a runtime was `Message::Apply` carrying an encoded **authoring** transaction, and it
// never reached a playing world at all — `cy-editor-services/src/mirror.rs` hard-coded
// `ApplyWhen::OnArrival` with its own comment saying *"a playing world would want
// `AtTickBoundary`, and the editor is what knows which"*. No edit had ever reached a playing world,
// so there was no live editing to give a policy to.
//
// This file is the translation step. It takes one authoring change, decides what it means for a
// running world through `LiveEditPolicyTable`, and then performs exactly that and nothing else.
//
// ================================================================================================
// THE THREE THINGS IT DOES THAT A "JUST WRITE THE FIELD" PATH WOULD NOT
// ================================================================================================
//
//   1. **It announces before it acts.** `announce()` answers with the policy, its provenance and
//      its reason without touching the running world, which is what the requirement's *"the editor
//      SHALL report the applicable policy before the user acts"* needs. `apply()` calls the same
//      function, so the announcement cannot disagree with the outcome.
//
//   2. **It preserves runtime state across a rebuild.** `reflect::PersistenceKind` has classified
//      every field since the foundations work and — the specification says so itself — *"until now
//      had no consumer"*. A `ReinitializeComponent` or `RecreateEntity` copies every bound field
//      classified `RuntimeState` or `PersistentState` out before the rebuild and back after, and
//      COUNTS both what it carried and what it could not. That is the "health stays at 53" scenario
//      made a measurement rather than a promise.
//
//   3. **It refuses rather than approximating.** A change to a field nothing has bound is refused
//      naming the field; a change whose policy is `Unsupported` is refused saying it applies on the
//      next run; a `ReloadAsset` with no rebinder installed is refused naming the missing rebinder
//      rather than quietly succeeding. `live-editing`'s "An unrepresentable change is refused"
//      scenario is a scenario about not partially applying.
//
// ================================================================================================
// HOW "WITHOUT A RESTART" IS MEASURED RATHER THAN CLAIMED
// ================================================================================================
//
// `LiveEditOutcome` carries the session's tick before and after. Every policy but `RestartWorld`
// leaves them equal — the simulation continued across the edit, which is the whole claim of live
// editing — and `RestartWorld` leaves `tick_after` behind `tick_before`, because the world it
// restarted begins again at zero. A test does not have to trust a boolean: it reads two tick
// numbers that the session, not this file, produced.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/component.h>
#include <cy/gameplay/live/policy.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>

#include <string_view>

namespace cy::gameplay::live {

/// What an authoring change is about.
enum class ChangeScope : u8 {
    /// One field of one component of one authored node.
    Node = 0,
    /// A property of the world itself — the simulation's gravity, its rates — which lives in the
    /// session's configuration rather than in any node. There is no running object to write it
    /// into, which is why its default policy is a restart rather than an immediate write.
    World,
};

/// One authoring change, in the terms the editor expresses it in.
///
/// The component type and the field are NAMES rather than identifiers, because that is what
/// survives the boundary: a `.cyworld` names a component by the type name in its own `type` section
/// and a field by the name on that section's `field` line, and physics' eight components have no
/// reflected `TypeId` at all.
struct AuthoringChange {
    ChangeScope scope = ChangeScope::Node;
    /// The authored node's identity — the editor's own, the one `PlaySession::entity_for` takes.
    /// Ignored for a `World`-scoped change.
    u64 node_identity = 0;
    std::string_view type;
    std::string_view field;
    /// The new value, in the world file's own value vocabulary, so that applying it to the document
    /// and applying it to the running world are the same value read twice.
    scene::serialization::WorldValue value;
};

/// What kind of thing a bound field holds, which is what says how many bytes to move and from where
/// in a `WorldValue`.
enum class LiveFieldKind : u8 {
    F32 = 0,
    Vec3,
    Quat,
    U32,
    Bool,
    /// A path naming an asset. Never written into the running component by this file: a rebind is
    /// the host's, through `LiveEditHost::rebind_asset`.
    AssetPath,
    /// A field whose authored value is not the running value's bytes — a collider's shape word,
    /// which the server cooks into a shape handle; a mesh name, which the bridge resolves. There is
    /// no in-place write for one, so a `Structural` field bound with an `Immediate` policy is
    /// **refused** rather than written wrongly. That refusal is the reason this kind exists.
    Structural,
};

/// One field of one component, as it exists in the RUNNING world.
///
/// This is the seam between an authored name and a live byte, and it is declared rather than
/// discovered for the reason above: not every component this engine simulates is reflected.
struct LiveFieldBinding {
    std::string_view type;
    std::string_view field;
    /// The ECS component the field lives in. `ecs::kInvalidComponent` for a `World`-scoped field,
    /// which has no component.
    ecs::ComponentTypeId component = ecs::kInvalidComponent;
    /// Byte offset of the field inside that component.
    u32 offset = 0;
    LiveFieldKind kind = LiveFieldKind::F32;
    /// The classification the default policy is derived from.
    FieldNature nature;
};

/// What the compiler could not do itself.
///
/// Two of the six policies reach outside a play session — an asset rebind is the asset system's and
/// a component this file did not create can only be rebuilt by whatever created it — so they are
/// an interface rather than a branch. A host that does not implement one refuses by name, which is
/// what stops an unimplemented policy from looking like an applied one.
class LiveEditHost {
public:
    LiveEditHost() = default;
    virtual ~LiveEditHost() = default;
    LiveEditHost(const LiveEditHost&) = delete;
    LiveEditHost& operator=(const LiveEditHost&) = delete;
    LiveEditHost(LiveEditHost&&) = delete;
    LiveEditHost& operator=(LiveEditHost&&) = delete;

    /// Rebuild one component of the entity simulating `identity` from the authored world.
    ///
    /// The default knows the components a `PlaySession` itself creates and refuses any other type
    /// by name — overriding it is how a project's own component joins the policy.
    [[nodiscard]] virtual Status reinitialize(PlaySession& session, u64 identity,
                                              std::string_view type) noexcept;

    /// Rebind the asset a field names. The default refuses, naming the absent rebinder: an engine
    /// with no asset system wired to a session cannot reload an asset, and saying so is better than
    /// returning success.
    [[nodiscard]] virtual Status rebind_asset(PlaySession& session, u64 identity,
                                              std::string_view type, std::string_view field,
                                              std::string_view path) noexcept;
};

/// What one compiled change did.
struct LiveEditOutcome {
    LiveEditPolicy policy = LiveEditPolicy::Unsupported;
    /// Whether the policy was declared for this field or derived from its classification.
    bool declared = false;
    /// Why that policy, in one line. Never null.
    const char* reason = "";
    /// Whether the change reached the running world. False for every refusal.
    bool applied = false;

    u32 fields_written = 0;
    u32 components_reinitialized = 0;
    u32 entities_recreated = 0;
    u32 assets_rebound = 0;
    u32 worlds_restarted = 0;

    /// Bound fields classified `RuntimeState` or `PersistentState` that were carried across a
    /// rebuild. `live-editing`'s "Health survives a prefab edit".
    u32 runtime_state_preserved = 0;
    /// And those that could not be, which the specification requires be **reported** rather than
    /// applied silently.
    u32 runtime_state_lost = 0;

    /// The session's tick either side of the change. Equal for every policy but `RestartWorld`.
    /// See the file header: this is how "without a restart" stops being a claim.
    u64 tick_before = 0;
    u64 tick_after = 0;
};

/// Translate authoring changes into runtime deltas, and apply them.
///
/// Holds no world: a compiler is a function of a policy table and a set of bindings, and the
/// session it acts on is an argument. That is deliberate — the same compiler answers `announce()`
/// for a session that is not running, which is what the inspector needs to show a policy beside a
/// field before anybody presses play.
class LiveEditCompiler {
public:
    LiveEditCompiler(Allocator& allocator, const LiveEditPolicyTable& policies) noexcept;

    /// Bind one field. Re-binding the same (type, field) replaces the binding.
    [[nodiscard]] Status bind(const LiveFieldBinding& binding) noexcept;

    /// Bind the fields a `PlaySession` itself creates — the transform, the collider and the three
    /// body kinds — using the component identifiers of `session`'s own world.
    ///
    /// Separate from the constructor because the identifiers only exist once a session has entered,
    /// and because a caller that binds nothing and asks for an immediate write should be refused
    /// rather than served.
    [[nodiscard]] Status bind_play_session(PlaySession& session) noexcept;

    /// The binding for one field, or null.
    [[nodiscard]] const LiveFieldBinding* binding_for(std::string_view type,
                                                      std::string_view field) const noexcept;

    /// What this change would do, without doing it. The announcement.
    ///
    /// Answers for an unbound field too — with `Unsupported` and a reason naming the field — so
    /// that an inspector showing a policy beside every field shows one beside every field.
    [[nodiscard]] LiveEditDecision announce(const AuthoringChange& change) const noexcept;

    /// Compile and apply one change against a running session.
    ///
    /// The authoring value is written into the authored document and into the session's restore
    /// target FIRST, so that a rebuild reads the new value and `stop()` restores the edited
    /// authoring state rather than the state before play. Then the policy is performed.
    ///
    /// **A `RestartWorld` drops every binding and takes the engine's again**, because a component
    /// identifier belongs to a world and the world is a new one. A caller that bound its own fields
    /// re-binds them after an outcome reporting `worlds_restarted > 0`; the alternative is a stale
    /// identifier writing into whichever column that number names in the new world.
    /// `authored` is the document the session is playing — the same `World` the session was
    /// constructed over. It is an argument rather than something read back out of the session
    /// because the session deliberately holds it by reference and exposes it to nobody: play writes
    /// exactly one thing into it, and widening that seam is how `restored_exactly` stops meaning
    /// anything.
    [[nodiscard]] Expected<LiveEditOutcome, Error> apply(PlaySession& session,
                                                         scene::serialization::World& authored,
                                                         LiveEditHost& host,
                                                         const AuthoringChange& change) noexcept;

    [[nodiscard]] usize bindings() const noexcept { return bindings_.size(); }

private:
    /// A binding with its names owned, for the reason `LiveEditPolicyTable` owns its own.
    struct Bound {
        u32 type_offset = 0;
        u32 type_length = 0;
        u32 field_offset = 0;
        u32 field_length = 0;
        ecs::ComponentTypeId component = ecs::kInvalidComponent;
        u32 offset = 0;
        LiveFieldKind kind = LiveFieldKind::F32;
        FieldNature nature;
    };

    [[nodiscard]] std::string_view text(u32 offset, u32 length) const noexcept;
    [[nodiscard]] const Bound* lookup(std::string_view type, std::string_view field) const noexcept;
    /// Write the authoring value into the document and into the session's restore target.
    [[nodiscard]] Status record_authoring(PlaySession& session,
                                          scene::serialization::World& authored,
                                          const AuthoringChange& change) noexcept;
    /// Write one value into a live component. `Immediate`'s whole body.
    ///
    /// A static member in all but name — it reads the binding it is handed and nothing of the
    /// compiler — and left a member because it is part of the compiler's story rather than a free
    /// function anyone may call with an arbitrary offset into a component.
    [[nodiscard]] static Status write_live(PlaySession& session, const Bound& bound,
                                           const AuthoringChange& change) noexcept;
    /// One policy each. Split out of `apply` rather than written inline: the switch over six
    /// policies, each with its own capture, refusal and count, put `apply` at a cognitive
    /// complexity of 31 — the band this project calls critical — and the six are genuinely
    /// independent operations that happen to share a decision.
    [[nodiscard]] Status apply_immediate(PlaySession& session, const AuthoringChange& change,
                                         LiveEditOutcome& outcome) noexcept;
    [[nodiscard]] Status apply_reinitialize(PlaySession& session, LiveEditHost& host,
                                            const AuthoringChange& change,
                                            LiveEditOutcome& outcome) noexcept;
    [[nodiscard]] Status apply_recreate(PlaySession& session, const AuthoringChange& change,
                                        LiveEditOutcome& outcome) noexcept;
    /// Static like `write_live`, and for the same reason: a rebind reads the change and the
    /// document it names, and nothing of the compiler.
    [[nodiscard]] static Status apply_reload_asset(PlaySession& session,
                                                   scene::serialization::World& authored,
                                                   LiveEditHost& host,
                                                   const AuthoringChange& change,
                                                   LiveEditOutcome& outcome) noexcept;
    [[nodiscard]] Status apply_restart(PlaySession& session, const AuthoringChange& change,
                                       LiveEditOutcome& outcome) noexcept;

    /// Copy every `RuntimeState`/`PersistentState` field of one entity out, and back.
    [[nodiscard]] Status capture_runtime_state(PlaySession& session, u64 identity) noexcept;
    [[nodiscard]] Status restore_runtime_state(PlaySession& session, u64 identity,
                                               LiveEditOutcome& outcome) noexcept;

    /// One captured runtime-state field: which binding, and the bytes.
    struct Captured {
        usize binding = 0;
        u8 bytes[16] = {};
        u32 length = 0;
    };

    const LiveEditPolicyTable* policies_;
    Array<Bound> bindings_;
    Array<char> names_;
    Array<Captured> captured_;
    /// The document, re-read so a change can be applied to the restore target without disturbing
    /// the live one. Kept as a member so a change does not allocate a world per call.
    Array<char> scratch_;
};

/// The field bindings a `PlaySession` creates, as a table, so that a reader can see the whole set
/// in one place and a test can assert over it.
///
/// Exposed rather than private to `bind_play_session` because `m11b`'s adversarial pass asks for a
/// declared policy to be broken and the break to be visible — a table that can only be reached
/// through the function that consumes it cannot be inspected.
[[nodiscard]] Span<const LiveFieldBinding> play_session_field_shapes() noexcept;

/// The policy declarations the engine makes for its own fields, applied to `table`.
///
/// Three of them are `RecreateEntity` or stronger and every one of them is a fact about what the
/// engine DOES with the value rather than about the value's classification — which is precisely why
/// they have to be declared. See the per-entry comments in the implementation.
[[nodiscard]] Status declare_engine_policies(LiveEditPolicyTable& table) noexcept;

}  // namespace cy::gameplay::live
