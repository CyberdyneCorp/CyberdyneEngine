#pragma once
// Session state fragments. M8.b task 3.1.
//
// `gameplay-framework` — "Session state fragments": session state is "a set of **reflected
// fragments** registered with the session, not one monolithic state object". Each declares its
// authority, its visibility and its persistence class, and "Those declarations SHALL drive
// replication, save, replay, and interface observation, **so that one declaration serves all four**
// rather than each being configured separately." Player state is fragments too.
//
// ================================================================================================
// ONE DECLARATION, FOUR DERIVED ANSWERS — AND THAT IS THE WHOLE POINT
// ================================================================================================
//
// The failure this requirement prevents is four configuration files. A match clock is declared
// server-authoritative and visible to everyone; if replication, save, replay and the interface each
// read their own setting, the day somebody changes one of them is the day a spectator sees a clock
// the server is not sending. So the four answers here are **functions of the declaration** —
// `replicated()`, `saved()`, `replay_relevant()` and `observable_by()` — and there is no setter for
// any of them. Changing what a fragment does means changing what it IS.
//
// ================================================================================================
// A FRAGMENT IS BYTES, NOT AN OBJECT
// ================================================================================================
//
// `gameplay-framework`'s forbidden-patterns list names "The game instance used as a store for
// arbitrary mutable global state", and the shape that becomes is a `SessionState` class everybody
// adds a field to. A fragment is a declared size and a slice of one blob, addressed by a stable
// identifier: a feature registers one without modifying an engine type, which is the requirement's
// "Fragments are added independently".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/gameplay/context.h>

#include <cstring>

namespace cy::gameplay {

/// Who may change a fragment's authoritative value.
enum class FragmentAuthority : u8 {
    Server = 0,
    Client,
    Local,
    Deterministic,
    Count,
};

/// Who may see it.
enum class FragmentVisibility : u8 {
    Everyone = 0,
    Owner,
    Team,
    AuthorityOnly,
    LocalOnly,
    Count,
};

/// What a save captures, and what a replay carries.
enum class PersistenceClass : u8 {
    SessionTransient = 0,
    WorldPersistent,
    ProfilePersistent,
    SaveGame,
    /// Computed from other state. Never saved, never replicated — recomputed.
    Derived,
    Count,
};

const char* fragment_authority_name(FragmentAuthority authority) noexcept;
const char* fragment_visibility_name(FragmentVisibility visibility) noexcept;
const char* persistence_class_name(PersistenceClass persistence) noexcept;

/// Whose state a fragment is.
enum class FragmentScope : u8 {
    /// The session's own: the match clock, the score line, the objective set.
    Session = 0,
    /// One participant's: identity, score, progression, economy, statistics.
    Player,
    Count,
};

/// How an observer stands to a fragment, for the visibility answer.
enum class ObserverRelation : u8 {
    Owner = 0,
    Teammate,
    Other,
    Authority,
    Local,
    Count,
};

using FragmentId = u32;
inline constexpr FragmentId kInvalidFragment = 0xFFFFFFFFU;

/// What a fragment is, as declared. **This declaration is the only configuration there is.**
struct FragmentDeclaration {
    Name name;
    /// Persistent identity, assigned by the project. Never derived from the name.
    u32 stable_id = 0;
    u16 schema_version = 1;
    FragmentScope scope = FragmentScope::Session;
    FragmentAuthority authority = FragmentAuthority::Server;
    FragmentVisibility visibility = FragmentVisibility::Everyone;
    PersistenceClass persistence = PersistenceClass::SessionTransient;
    /// The fragment's size in bytes. Data, not an object.
    u16 size = 0;
};

/// The fragments registered with a session, and their bytes.
class FragmentStore {
public:
    explicit FragmentStore(Allocator& allocator) noexcept;

    FragmentStore(const FragmentStore&) = delete;
    FragmentStore& operator=(const FragmentStore&) = delete;

    /// Register a fragment. A feature calls this; no engine type changes.
    [[nodiscard]] Expected<FragmentId, Error> register_fragment(
        const FragmentDeclaration& declaration) noexcept;
    /// One participant's copy of a `FragmentScope::Player` fragment.
    [[nodiscard]] Status add_player(ParticipantId participant) noexcept;
    void remove_player(ParticipantId participant) noexcept;

    [[nodiscard]] FragmentId find(u32 stable_id) const noexcept;
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(declarations_.size()); }
    [[nodiscard]] const FragmentDeclaration& declaration(FragmentId id) const noexcept {
        return declarations_[id];
    }

    /// Read and write the bytes. `participant` is ignored for a session fragment and required for
    /// a player one.
    [[nodiscard]] Status write(FragmentId id, const void* bytes, u16 size,
                               ParticipantId participant = ParticipantId{}) noexcept;
    [[nodiscard]] Status read(FragmentId id, void* bytes, u16 size,
                              ParticipantId participant = ParticipantId{}) const noexcept;

    template <class T>
    [[nodiscard]] Status write_value(FragmentId id, const T& value,
                                     ParticipantId participant = ParticipantId{}) noexcept {
        static_assert(__is_trivially_copyable(T), "a fragment is bytes, not an object");
        return write(id, &value, static_cast<u16>(sizeof(T)), participant);
    }
    template <class T>
    [[nodiscard]] Status read_value(FragmentId id, T& value,
                                    ParticipantId participant = ParticipantId{}) const noexcept {
        static_assert(__is_trivially_copyable(T), "a fragment is bytes, not an object");
        return read(id, &value, static_cast<u16>(sizeof(T)), participant);
    }

    // --- The four derived answers. No setters, by design — see the header comment. ---------------

    /// Does this fragment go on the wire? Derived from authority and visibility together: a
    /// local-only fragment does not replicate however it is authored, and neither does one only
    /// this peer may change.
    [[nodiscard]] bool replicated(FragmentId id) const noexcept;
    /// Does a save capture it?
    [[nodiscard]] bool saved(FragmentId id) const noexcept;
    /// Does a replay carry it, or is it recomputed from the command stream?
    [[nodiscard]] bool replay_relevant(FragmentId id) const noexcept;
    /// May an observer standing in that relation see it?
    [[nodiscard]] bool observable_by(FragmentId id, ObserverRelation relation) const noexcept;

private:
    /// One participant's player-scoped bytes. A row per participant rather than one blob with a
    /// stride, because registering a player fragment after a participant joined must grow what
    /// that participant already holds — a feature activated mid-session is the ordinary case.
    struct PlayerRow {
        ParticipantId participant;
        Array<u8> bytes;
    };

    [[nodiscard]] const PlayerRow* player_row(ParticipantId participant) const noexcept;
    [[nodiscard]] PlayerRow* player_row(ParticipantId participant) noexcept;
    /// The bytes of `id` for `participant`, or null when the request does not name a valid slice.
    [[nodiscard]] u8* slice(FragmentId id, ParticipantId participant, u16 size) noexcept;
    [[nodiscard]] const u8* slice(FragmentId id, ParticipantId participant,
                                  u16 size) const noexcept;

    Allocator* allocator_;
    Array<FragmentDeclaration> declarations_;
    /// Byte offset of each fragment into its scope's storage.
    Array<u32> offsets_;
    Array<u8> session_bytes_;
    Array<PlayerRow> players_;
    u32 player_stride_ = 0;
};

}  // namespace cy::gameplay
