#pragma once
// Gameplay lifetimes, scoped services and the gameplay context. Task 4.4.1.
//
// `gameplay-framework` — "Gameplay lifetime model" names four nested lifetimes, "Scoped services"
// says services are reached through an explicit context rather than global lookup, and "Gameplay
// context" says systems receive the world, the world session, the game session, the tick, the
// services in scope, and the command and event buffers.
//
// ================================================================================================
// WHAT IS *NOT* ON THE CONTEXT, AND WHY THAT IS THE INTERESTING PART
// ================================================================================================
//
// There is no input on this context. No device, no action, no key, no `InputServer`. design.md §3:
// "Input reaches simulation **only** as commands. There is no 'read the input state in a system'
// path." The context is the one thing every gameplay system is handed, so the absence here is the
// absence everywhere — a system cannot reach what it was never given.
//
// This module also declares **no dependency on `cy::servers-input`**, so the headers are not even
// on the include path of a gameplay translation unit. `src/gameplay/tests/test_bypass.cpp` asserts
// exactly that with `__has_include`, and it fails the moment somebody adds the dependency. See
// README.md for why that matters more than it looks: replay, rollback and lockstep are one command
// log read three ways, and that is only true if the log is complete.
//
// ================================================================================================
// WHY THE SESSION OUTLIVES THE WORLD
// ================================================================================================
//
// "A game session SHALL be able to span several worlds in sequence or simultaneously — lobby, play,
// results — without participants, teams, or session state being destroyed and recreated. Changing
// world SHALL NOT end the session."
//
// So `GameSession` owns the participants and `WorldSession` owns nothing but a world and a role.
// Moving from a lobby to a play world adds and removes `WorldSession`s; the participant list does
// not move, is not copied, and is not rebuilt. A design that hung participants off the world would
// make "the same player" a thing that has to be re-established at every transition, and every
// system that held a participant identity across one would be holding a stale one.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>

namespace cy::ecs {
class World;
}

namespace cy::gameplay {

class CommandStream;

/// The lifetimes, outermost first. A service declares one of these and gets its lifetime from it.
enum class Scope : u8 {
    /// The process.
    Application = 0,
    /// Launch to shutdown, across sessions: local users, profiles, saves, progression.
    GameInstance,
    /// One logical play session — a match, a campaign run, a replay, an editor play.
    Session,
    /// One world's participation in a session.
    World,
    /// One participant's own services.
    Player,
    Count,
};

const char* scope_name(Scope scope) noexcept;

/// What a service does to the data it holds, declared so that a scheduler can order the systems
/// that use it.
///
/// `gameplay-framework`: "A service SHALL declare its data access so that the scheduler can order
/// systems that use it, exactly as for systems. Ad-hoc locking inside gameplay services SHALL NOT
/// be the coordination mechanism." The declaration is here; the scheduler that reads it is the
/// ECS's, and M4 records the declaration rather than pretending to schedule on it.
enum class ServiceAccess : u8 {
    Read = 0,
    Write,
    /// The service is its own synchronisation. Declared so that "this one really is shared" is a
    /// statement someone made rather than an omission.
    Shared,
};

CY_HANDLE_TAG(Participant);
/// A participant's identity. **Stable across control changes, world changes and reconnection** —
/// `gameplay-framework` requires it, and it is what makes "identity survives the avatar" true.
using ParticipantId = Handle<ParticipantTag>;

CY_HANDLE_TAG(WorldSessionRef);
using WorldSessionId = Handle<WorldSessionRefTag>;

/// What a participant is. Note that a bot and a remote human are both participants: "Remote
/// participants SHALL NOT require any local resource — no viewport, no input device, no listener",
/// and the way to make that structural is for none of those to appear in this record.
enum class ParticipantKind : u8 {
    LocalHuman = 0,
    RemoteHuman,
    Bot,
    RemoteBot,
    Spectator,
    ServerAgent,
    Count,
};

const char* participant_kind_name(ParticipantKind kind) noexcept;

struct Participant {
    ParticipantId id;
    ParticipantKind kind = ParticipantKind::Bot;
    Name name;
    /// The team's stable identifier, or zero. Team *relationships* are a separate requirement and
    /// are not part of M4's six tasks — see README.md for what is thinner than the tasks claim.
    u32 team = 0;
    /// Only a local human has one, and only a local human needs one. A remote participant leaves it
    /// at `kNoLocalPlayer`, which is the structural half of "no local resource".
    u32 local_player = 0xFFFFFFFFU;
};

inline constexpr u32 kNoLocalPlayer = 0xFFFFFFFFU;

/// Services with declared scopes, reached from a context.
///
/// No global lookup and no singleton: `gameplay-framework` — "There SHALL NOT be a set of global
/// gameplay manager singletons" and "it SHALL obtain it from its context, and no global accessor
/// SHALL be required". There is no static instance of this class anywhere in the engine, which is
/// what makes the requirement checkable rather than aspirational.
///
/// A service is stored as a pointer plus an opaque **type tag**, not as a base class: the engine
/// builds with `-fno-rtti`, so `dynamic_cast` is unavailable and a common base would be the wrong
/// answer anyway — it would make every gameplay service inherit from an engine type, which is the
/// object hierarchy this capability exists to avoid.
class ServiceRegistry {
public:
    explicit ServiceRegistry(Allocator& allocator) noexcept : services_(allocator) {}

    /// The per-type tag. A function-local static's address, unique per instantiation, stable for
    /// the process, and free — the `-fno-rtti` replacement for `typeid`.
    template <class T>
    [[nodiscard]] static const void* type_tag() noexcept {
        static const char tag = 0;
        return &tag;
    }

    template <class T>
    [[nodiscard]] Status add(Scope scope, Name name, T* service,
                             ServiceAccess access = ServiceAccess::Write) noexcept {
        return add_erased(scope, name, service, type_tag<T>(), access);
    }

    /// The service, or null when nothing is registered under that name **at that scope with that
    /// type**. All three have to match: a name collision across scopes is legitimate (a "clock" per
    /// session and per world), and a type mismatch is a bug that a `void*` cast would have hidden.
    template <class T>
    [[nodiscard]] T* find(Scope scope, Name name) noexcept {
        return static_cast<T*>(find_erased(scope, name, type_tag<T>()));
    }

    /// Destroy every service at a scope. The pointers are the caller's — the registry never owns
    /// storage — so this drops the registrations and nothing else, which is what "scope determines
    /// lifetime" means for an engine with no garbage collector.
    void clear_scope(Scope scope) noexcept;

    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(services_.size()); }
    [[nodiscard]] u32 count_at(Scope scope) const noexcept;

    struct Record {
        Scope scope = Scope::Application;
        Name name;
        void* service = nullptr;
        const void* type_tag = nullptr;
        ServiceAccess access = ServiceAccess::Write;
    };

    [[nodiscard]] const Record& at(u32 index) const noexcept { return services_[index]; }

private:
    [[nodiscard]] Status add_erased(Scope scope, Name name, void* service, const void* tag,
                                    ServiceAccess access) noexcept;
    [[nodiscard]] void* find_erased(Scope scope, Name name, const void* tag) noexcept;

    Array<Record> services_;
};

/// One world's participation in a session.
struct WorldSession {
    WorldSessionId id;
    ecs::World* world = nullptr;
    /// "primary", "preview", "lobby". A `Name`, so a project adds a role without changing an
    /// engine enumeration.
    Name role;
};

/// One logical play session: a match, a campaign run, a replay, an editor play.
///
/// Owns the participants and the seed. Does **not** own a world — see the header comment.
class GameSession {
public:
    static constexpr u32 kMaxWorlds = 8;

    GameSession(Allocator& allocator, u64 seed) noexcept;

    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    [[nodiscard]] u64 seed() const noexcept { return seed_; }

    [[nodiscard]] Expected<ParticipantId, Error> add_participant(
        ParticipantKind kind, Name name, u32 team = 0, u32 local_player = kNoLocalPlayer) noexcept;
    void remove_participant(ParticipantId participant) noexcept;
    [[nodiscard]] const Participant* participant(ParticipantId id) const noexcept;
    [[nodiscard]] u32 participant_count() const noexcept {
        return static_cast<u32>(participants_.size());
    }
    [[nodiscard]] const Participant& participant_at(u32 index) const noexcept {
        return participants_[index];
    }

    /// Add a world to the session. Returns its world-session identity.
    [[nodiscard]] Expected<WorldSessionId, Error> add_world(ecs::World* world, Name role) noexcept;
    /// Remove a world. **The session continues**, and so does every participant — which is the
    /// requirement's "Players survive a level change".
    void remove_world(WorldSessionId id) noexcept;
    [[nodiscard]] const WorldSession* world_session(WorldSessionId id) const noexcept;
    [[nodiscard]] u32 world_count() const noexcept { return world_count_; }
    [[nodiscard]] const WorldSession& world_at(u32 index) const noexcept { return worlds_[index]; }

    /// The session's current phase, as a `Name` standing in for the gameplay tag the full
    /// requirement asks for. Hierarchical tags are `gameplay-framework`'s "Gameplay tags"
    /// requirement and are not one of M4's six tasks; the shape here is the one a tag registry
    /// slots into without a change at the call sites.
    [[nodiscard]] Name phase() const noexcept { return phase_; }
    void set_phase(Name phase) noexcept { phase_ = phase; }

    [[nodiscard]] ServiceRegistry& services() noexcept { return services_; }

private:
    u64 seed_;
    Array<Participant> participants_;
    u32 next_participant_ = 1;
    WorldSession worlds_[kMaxWorlds];
    u32 world_count_ = 0;
    u32 next_world_ = 1;
    Name phase_;
    ServiceRegistry services_;
};

/// What a gameplay system is handed. Cheap — references and handles, no allocation — and passed
/// explicitly rather than obtained from a global.
///
/// Read the list twice: what is here, and what is not. `gameplay-framework` requires "the world,
/// the world session, the game session, the current simulation tick, the services in scope, and the
/// command and event buffers". Every one of those is a pointer or a value. Input is not on the list
/// and cannot be reached from anything that is.
struct GameplayContext {
    ecs::World* world = nullptr;
    const WorldSession* world_session = nullptr;
    GameSession* session = nullptr;
    ServiceRegistry* services = nullptr;
    /// The only door intent comes through. See command.h.
    CommandStream* commands = nullptr;
    /// A moment is `(epoch, tick)`, never a tick alone: rollback moves the tick backwards and two
    /// occurrences of tick 8842 are distinguishable only by their epoch.
    determinism::SimulationPoint at;

    [[nodiscard]] bool valid() const noexcept {
        return session != nullptr && services != nullptr && commands != nullptr;
    }
};

}  // namespace cy::gameplay
