#pragma once
// The server registry: backend selection, the null fallback, and bring-up order. Task 4.1.1.
//
// `engine-architecture` — "Server architecture": heavy subsystems are exposed as servers, singleton
// facades that own all of their state, address every object through opaque generational handles,
// and **have no knowledge of the ECS world, the scene graph, or scripting**. A backend is
// selectable by configuration, "falling back to a documented default and finally to a null
// implementation that keeps handle bookkeeping valid".
//
// --- WHAT IS HERE AND WHAT IS NOT ----------------------------------------------------------------
//
// `src/servers/` is layer 2 and does not exist yet: `RenderServer` arrives at M3, `PhysicsServer`
// and `AudioServer` at M4 and M7. What M2 owns is the *wiring* — which is src/runtime/'s whole
// remit — so this file is the registry, the selection rule and the null implementation, and it
// declares seven slots that are all empty. That is deliberate rather than premature: the selection
// rule and the fallback chain are where a subsystem quietly becomes non-optional, and writing them
// once, before there is a backend with opinions, is the only time it is cheap.
//
// --- WHY THE INTERFACE IS THIS NARROW ------------------------------------------------------------
//
// `Server` has four methods and none of them mentions an entity, a node, a world or a script. The
// requirement — "the server SHALL never dereference an ECS entity or a scene node" — is then a
// property of the header rather than a rule someone has to remember: a server implementation
// literally cannot be handed one. A component that drives a server holds a handle and pushes to it,
// which is the direction `engine-architecture`'s "Component drives a server through a handle"
// scenario fixes.
//
// Handles themselves are `cy::Handle<Tag>` from `<cy/core/values/handle.h>` and are each server's
// to declare — a `RenderInstanceHandle` means nothing to this file. What this file guarantees is
// that a *null* backend is still a real object that issues and validates handles, so a dedicated
// server build's `MeshRenderer` still gets a handle back and still has somewhere to push to.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

namespace cy::runtime {

/// The seven servers `engine-architecture` names, in bring-up order.
///
/// The order is the specification's startup sequence: the display server is brought up in its own
/// stage before the rest, and the remaining six come up together in the `Servers` stage. `Count` is
/// last and is not a server.
enum class ServerKind : u8 {
    Display = 0,
    Input,
    Render,
    Audio,
    Physics,
    Navigation,
    Text,
    Count,
};

inline constexpr u32 kServerCount = static_cast<u32>(ServerKind::Count);

const char* server_kind_name(ServerKind kind) noexcept;

/// The interface every server implements.
///
/// Note what is absent: no world, no node, no entity, no script. See the header comment — the
/// absence is the requirement.
class Server {
public:
    Server() = default;
    virtual ~Server() = default;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /// Which backend this is: "vulkan", "jolt", "null". Reported by the registry and named in the
    /// diagnostic when a requested backend was not the one that ran.
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;

    [[nodiscard]] virtual Status initialize() noexcept = 0;
    virtual void shutdown() noexcept = 0;

    /// True when this server does nothing but keep handle bookkeeping valid. Asked by the runtime's
    /// report, and by a test that a dedicated server build linked no renderer.
    [[nodiscard]] virtual bool is_null_backend() const noexcept { return false; }
};

/// Constructs one backend. Returns a server the factory owns for the process's lifetime — the
/// registry stores the pointer and never deletes it, because a backend registered by a module is
/// that module's to own and unload.
using ServerFactory = Expected<Server*, Error> (*)(Allocator& allocator, void* user) noexcept;

/// How a slot was filled, which is the part of backend selection worth reporting.
enum class BackendOutcome : u8 {
    /// No backend was requested and none is registered. The null implementation runs.
    NotRequested = 0,
    /// The requested backend was found.
    Requested,
    /// The requested backend was absent or failed, and the documented default ran instead.
    Default,
    /// Neither was available. The null implementation runs and handle bookkeeping stays valid.
    NullFallback,
};

const char* backend_outcome_name(BackendOutcome outcome) noexcept;

struct ServerSlot {
    ServerKind kind = ServerKind::Display;
    Server* server = nullptr;
    BackendOutcome outcome = BackendOutcome::NotRequested;
    /// What the configuration asked for, or empty. Kept so that the diagnostic can say "asked for
    /// vulkan, ran null" rather than only "ran null".
    const char* requested = "";
};

/// A server that does nothing and keeps handle bookkeeping valid.
///
/// Every slot has one, always, whether or not a backend was registered. That is what makes
/// `registry.get(ServerKind::Audio)` never null: a caller has no null branch to forget, and a
/// dedicated server build takes exactly the same code path a client build does.
class NullServer final : public Server {
public:
    explicit NullServer(ServerKind kind) noexcept : kind_(kind) {}

    [[nodiscard]] const char* backend_name() const noexcept override { return "null"; }
    [[nodiscard]] Status initialize() noexcept override { return ok(); }
    void shutdown() noexcept override {}
    [[nodiscard]] bool is_null_backend() const noexcept override { return true; }
    [[nodiscard]] ServerKind kind() const noexcept { return kind_; }

private:
    ServerKind kind_;
};

/// The seven slots, the factories registered for them, and the selection that fills them.
///
/// Registration happens before startup; selection happens in the `Servers` stage. Splitting the two
/// is what lets a module registered at level `Core` add a physics backend that the `Servers` stage
/// then chooses — `engine-architecture`'s "Module registers a backend" scenario, which requires the
/// factory to be registered "before the runtime constructs the physics server".
class ServerRegistry {
public:
    explicit ServerRegistry(Allocator& allocator) noexcept;

    ServerRegistry(const ServerRegistry&) = delete;
    ServerRegistry& operator=(const ServerRegistry&) = delete;

    /// Register a backend factory for a kind. Refuses a duplicate name within one kind.
    ///
    /// `is_default` marks the documented fallback for its kind. A second default for one kind is
    /// refused: "the documented default" is singular, and two of them would make which one ran a
    /// function of registration order.
    [[nodiscard]] Status register_backend(ServerKind kind, const char* name, ServerFactory factory,
                                          void* user = nullptr, bool is_default = false) noexcept;

    /// Fill every slot. `requested` names one backend per kind, indexed by `ServerKind`; a null or
    /// empty entry means "no preference", which takes the default.
    ///
    /// The chain is exactly `engine-architecture`'s: requested, then documented default, then null.
    /// A requested backend that is absent or fails to initialise is a *diagnostic*, not a startup
    /// failure — falling back is the documented behaviour, and a game that must have a particular
    /// backend checks `outcome()` and says so itself.
    [[nodiscard]] Status select_all(const char* const* requested, u32 count) noexcept;

    /// Never null: a slot with no backend holds its `NullServer`.
    [[nodiscard]] Server& get(ServerKind kind) noexcept;
    [[nodiscard]] const ServerSlot& slot(ServerKind kind) const noexcept;
    [[nodiscard]] BackendOutcome outcome(ServerKind kind) const noexcept {
        return slot(kind).outcome;
    }

    /// Shut down every selected server in the exact reverse of the order they were brought up, and
    /// return the slots to their null backends.
    void shutdown_all() noexcept;

    /// How many kinds are running something other than the null backend. What the startup log
    /// reports and what a dedicated server build's test asserts on.
    [[nodiscard]] u32 live_backends() const noexcept;

private:
    struct Registration {
        ServerKind kind = ServerKind::Display;
        const char* name = "";
        ServerFactory factory = nullptr;
        void* user = nullptr;
        bool is_default = false;
    };

    [[nodiscard]] const Registration* find(ServerKind kind, const char* name) const noexcept;
    [[nodiscard]] const Registration* find_default(ServerKind kind) const noexcept;
    /// Construct and initialise, or return null having emitted the diagnostic.
    [[nodiscard]] Server* bring_up(const Registration& registration) noexcept;
    /// The fallback chain for one kind: requested, then documented default, then null. Returns null
    /// for the last of those, with `outcome` saying which of the three happened.
    [[nodiscard]] Server* select_one(ServerKind kind, const char* wanted,
                                     BackendOutcome& outcome) noexcept;

    Allocator* allocator_;
    Array<Registration> registrations_;
    ServerSlot slots_[kServerCount];
    /// One per kind, so that `get()` never returns null and never allocates. Written out rather
    /// than looped over because `NullServer` is neither copyable nor movable — it is a `Server` —
    /// and the static_assert below fails the build if a kind is added without a null for it.
    NullServer nulls_[kServerCount] = {
        NullServer(ServerKind::Display), NullServer(ServerKind::Input),
        NullServer(ServerKind::Render),  NullServer(ServerKind::Audio),
        NullServer(ServerKind::Physics), NullServer(ServerKind::Navigation),
        NullServer(ServerKind::Text),
    };
    static_assert(kServerCount == 7, "add a NullServer for the new kind, above");
    /// Kinds in the order they were brought up, for the reverse-order shutdown.
    Array<ServerKind> order_;
};

}  // namespace cy::runtime
