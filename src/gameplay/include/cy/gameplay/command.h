#pragma once
// One validated command stream — the simulation's only input. Task 4.4.3, and design.md §3.
//
// ================================================================================================
// THE INVARIANT, AND WHY IT IS WORTH A HEADER COMMENT THIS LONG
// ================================================================================================
//
// design.md §3: "Input reaches simulation **only** as commands. There is no 'read the input state
// in a system' path, and no tool that pokes simulation state directly.
//
//   Replay, rollback and lockstep are not three mechanisms — they are one command log read three
//   ways. That is only true if the log is complete. A single system that reads a device directly
//   does not merely bypass the stream; it makes the M9 guarantees **unachievable** until someone
//   finds and removes it, and nothing will point at it, because everything will appear to work
//   until a desync months later."
//
// Three things follow, and all three are structural here rather than advisory:
//
//   1. **`src/gameplay/` declares no dependency on `cy::servers-input`.** A gameplay translation
//      unit cannot include an input header, because the header is not on its include path.
//      `tests/test_bypass.cpp` asserts that with `__has_include` and fails if the dependency is
//      ever added.
//   2. **`GameplayContext` carries no input.** The context is the only thing a system is handed, so
//      a system cannot reach what it was never given. See context.h.
//   3. **The log is the whole of intent.** `CommandLog` records every committed command, and
//      replaying it into a fresh session reproduces the run. `test_bypass.cpp` demonstrates the
//      converse by building a system that reads a side channel and showing that its replay
//      *diverges* — which is what "unachievable" looks like when you can see it.
//
// ================================================================================================
// WHY SUBMISSION IS PER PRODUCER AND THE ORDER KEY IS NOT A THREAD
// ================================================================================================
//
// "Command submission SHALL scale: submission SHALL NOT serialise through a single lock, and
// commands SHALL be accumulated per worker and committed deterministically."
//
// So each producer records into its own `CommandBuffer` with no lock, and `commit()` merges by
// `(producer order, sequence)`. `order` is the producer's registration index — **never a thread
// identity**, which `simulation-and-determinism` forbids in an ordering key, because the thread
// that happened to run a system is not reproducible and a replay would then depend on the
// scheduler.
//
// Provenance travels on the command and is read by nothing but diagnostics: `gameplay-framework` —
// "Provenance SHALL NOT affect validation, ordering, or execution." The merge key excludes it, and
// `tests/test_commands.cpp` asserts that two commands differing only in provenance validate
// identically and commit in the same order.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/gameplay/validation.h>

#include <cstring>

namespace cy::gameplay {

/// The dense runtime index of a command type. Not persistent — the persistent identity is
/// `CommandDeclaration::stable_id`, exactly as for an input action, and for the same reason.
using CommandTypeId = u32;
inline constexpr CommandTypeId kInvalidCommandType = 0xFFFFFFFFU;

/// How much the transport must promise. Declared per command type, because "fire" and "set the
/// build queue" have different answers and a transport that guessed would be wrong for one of them.
enum class Reliability : u8 {
    /// Superseded by the next one of its kind. A movement frame.
    Unreliable = 0,
    /// Must arrive. A build order.
    Reliable,
};

/// What a command type is, as declared.
struct CommandDeclaration {
    Name name;
    /// Persistent identity. Never derived from the name; zero is null.
    u32 stable_id = 0;
    /// `gameplay-framework` — "Command and gameplay state schemas SHALL be versioned, so a replay
    /// or save from an older build is either migrated where supported or rejected clearly."
    u16 schema_version = 1;
    /// May a client run this before the authority confirms it?
    bool predictable = false;
    /// Local-only commands are never transmitted — a camera shake, a UI ping.
    bool local_only = false;
    /// Does the authority decide, or is this the client's to apply?
    bool authoritative = true;
    Reliability reliability = Reliability::Reliable;
    /// The channel the producer must control the target on, for the structural check. Empty means
    /// the command is not addressed to an entity at all — a session-level command.
    Name channel;
    /// The capability bit the target must accept. Zero means none required.
    u32 required_capability = 0;
};

/// Who produced a command. **Diagnostics only.**
struct Provenance {
    ControlSourceKind kind = ControlSourceKind::Human;
    /// The control source's index, or zero.
    u32 source = 0;
};

/// The largest payload a command carries inline.
///
/// Fixed and small on purpose: a command is a value that a replay writes to a file and a network
/// peer puts in a packet, and a pointer here would make both of those a marshalling step. A command
/// needing more than this is a command carrying content rather than intent, and the content belongs
/// in an asset the command names.
inline constexpr u16 kMaxCommandPayload = 48;

/// One gameplay command.
struct Command {
    CommandTypeId type = kInvalidCommandType;
    /// The tick it was submitted for. Part of the log's identity and of replay's addressing.
    u64 tick = 0;
    /// Who is asking. What validation checks — not the provenance.
    ParticipantId participant;
    /// Which source is asking, for the control check.
    ControlSourceId source;
    /// The entity addressed, or a null entity for a group or session-level command.
    ecs::Entity target;
    /// The group addressed, or null.
    GroupId group;
    Provenance provenance;
    /// Assigned by the buffer. The second half of the merge key.
    u32 sequence = 0;
    u16 payload_size = 0;
    u8 payload[kMaxCommandPayload] = {};

    /// Copy a POD payload in. Refuses anything that does not fit rather than truncating, because a
    /// truncated command is a command that executes with the wrong arguments.
    template <class T>
    [[nodiscard]] bool set_payload(const T& value) noexcept {
        static_assert(sizeof(T) <= kMaxCommandPayload, "a command payload is intent, not content");
        static_assert(__is_trivially_copyable(T), "a command payload must be a POD");
        // `memcpy` rather than a byte loop, for two reasons that both matter. It is the only
        // spelling that is defined for reading an object's representation, and it is the one the
        // static analyser models — a hand-written loop over `reinterpret_cast<const u8*>` is
        // reported as reading uninitialised memory on every call, because the analyser cannot see
        // that the bytes of an `i32` are initialised when the `i32` is.
        payload_size = static_cast<u16>(sizeof(T));
        std::memcpy(static_cast<void*>(payload), static_cast<const void*>(&value), sizeof(T));
        return true;
    }

    template <class T>
    [[nodiscard]] bool read_payload(T& out) const noexcept {
        static_assert(__is_trivially_copyable(T), "a command payload must be a POD");
        if (payload_size != sizeof(T)) {
            return false;
        }
        std::memcpy(static_cast<void*>(&out), static_cast<const void*>(payload), sizeof(T));
        return true;
    }
};

/// One producer's uncommitted commands. Recorded into with no lock, merged at the commit.
class CommandBuffer {
public:
    CommandBuffer(Allocator& allocator, u32 order) noexcept : commands_(allocator), order_(order) {}

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) noexcept = default;

    [[nodiscard]] Status record(const Command& command) noexcept;

    /// The producer's registration index. **Never a thread identity** — see the header comment.
    [[nodiscard]] u32 order() const noexcept { return order_; }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(commands_.size()); }
    [[nodiscard]] const Command& at(u32 index) const noexcept { return commands_[index]; }
    void clear() noexcept { commands_.clear(); }

private:
    Array<Command> commands_;
    u32 order_;
    u32 next_sequence_ = 0;
};

/// Every command that was committed, in order. The thing a replay reads.
class CommandLog {
public:
    explicit CommandLog(Allocator& allocator) noexcept : entries_(allocator) {}

    [[nodiscard]] Status append(const Command& command) noexcept {
        return entries_.push_back(command);
    }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entries_.size()); }
    [[nodiscard]] const Command& at(u32 index) const noexcept { return entries_[index]; }
    void clear() noexcept { entries_.clear(); }

    /// A value identity over the whole log. Two runs that produced the same intent produce the same
    /// number; a run that produced different intent does not. What a desync report compares first.
    [[nodiscard]] u64 hash() const noexcept;

private:
    Array<Command> entries_;
};

/// The one door into the simulation.
class CommandStream {
public:
    CommandStream(Allocator& allocator, ControlRegistry& control) noexcept;
    ~CommandStream();

    CommandStream(const CommandStream&) = delete;
    CommandStream& operator=(const CommandStream&) = delete;

    [[nodiscard]] Expected<CommandTypeId, Error> declare(
        const CommandDeclaration& declaration) noexcept;
    [[nodiscard]] CommandTypeId find(u32 stable_id) const noexcept;
    [[nodiscard]] const CommandDeclaration& declaration(CommandTypeId type) const noexcept {
        return declarations_[type];
    }
    [[nodiscard]] u32 type_count() const noexcept { return static_cast<u32>(declarations_.size()); }

    /// Open a producer's buffer. Every producer — human input, an AI, a network peer, a replay, a
    /// test — gets one of these and there is no other way in. `gameplay-framework`: "Commands SHALL
    /// be produced by all producers through one path."
    [[nodiscard]] Expected<u32, Error> open_producer(Name debug_name) noexcept;
    [[nodiscard]] CommandBuffer& producer(u32 order) noexcept { return *producers_[order]; }
    [[nodiscard]] u32 producer_count() const noexcept {
        return static_cast<u32>(producers_.size());
    }

    /// The capability mask an entity accepts. A derived index rather than an ECS component, because
    /// this module is testable with no world — see README.md. Rebuilding it from components is a
    /// later change with no call-site consequences.
    [[nodiscard]] Status set_capabilities(ecs::Entity entity, u32 mask) noexcept;
    [[nodiscard]] u32 capabilities(ecs::Entity entity) const noexcept;

    /// Register a game-specific rule for a command type. Runs **after** the structural checks —
    /// `gameplay-framework`: "The engine SHALL validate structurally before game-specific logic
    /// runs."
    using RuleFn = void (*)(const GameplayContext& context, const Command& command,
                            ValidationResult& result, void* user) noexcept;
    [[nodiscard]] Status add_rule(CommandTypeId type, RuleFn rule, void* user) noexcept;

    /// Validate **without executing**. The interface greys out an action and says why by calling
    /// exactly this, and the authority rejects an illegal command by calling exactly this — one
    /// implementation, so the two cannot disagree.
    [[nodiscard]] ValidationResult validate(const GameplayContext& context,
                                            const Command& command) const noexcept;

    /// Merge every producer's buffer for `tick`, validate, and publish.
    ///
    /// Rejected commands do not reach the committed list; they are counted and their reasons are
    /// available through `last_rejections()`, which is what `gameplay-framework`'s rule debugger
    /// reads. A rejected command is **not** logged: the log is the record of what the simulation
    /// consumed, and a replay that re-ran rejected commands would depend on the rejection being
    /// reproduced identically, which is a second determinism obligation for no benefit.
    void commit(const GameplayContext& context, u64 tick) noexcept;

    [[nodiscard]] u32 committed_count() const noexcept {
        return static_cast<u32>(committed_.size());
    }
    [[nodiscard]] const Command& committed(u32 index) const noexcept { return committed_[index]; }

    [[nodiscard]] CommandLog& log() noexcept { return log_; }
    [[nodiscard]] const CommandLog& log() const noexcept { return log_; }

    struct Rejection {
        Command command;
        ValidationResult result;
    };
    [[nodiscard]] u32 rejection_count() const noexcept {
        return static_cast<u32>(rejections_.size());
    }
    [[nodiscard]] const Rejection& rejection(u32 index) const noexcept {
        return rejections_[index];
    }

    /// Route a command addressed to a group: one command per member that accepts the capability.
    /// The members that do not are **reported**, not silently dropped —
    /// `gameplay-framework`'s "A mixed selection".
    [[nodiscard]] Status route_to_group(const Command& prototype, u32 producer,
                                        Array<ecs::Entity>& excluded) noexcept;

private:
    struct Rule {
        CommandTypeId type = kInvalidCommandType;
        RuleFn rule = nullptr;
        void* user = nullptr;
    };
    struct CapabilityEntry {
        u64 entity = 0;
        u32 mask = 0;
    };

    Allocator* allocator_;
    ControlRegistry* control_;
    Array<CommandDeclaration> declarations_;
    Array<CommandBuffer*> producers_;
    Array<Name> producer_names_;
    Array<Command> committed_;
    Array<Rejection> rejections_;
    Array<Rule> rules_;
    Array<CapabilityEntry> capabilities_;
    CommandLog log_;
};

}  // namespace cy::gameplay
