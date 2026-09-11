#pragma once
// The dedicated server: headless, and excluded at build and cook time rather than at runtime.
// M9 task 4.6.
//
// ================================================================================================
// THE EXCLUSION IS A LINK-GRAPH FACT, NOT A RUNTIME BRANCH
// ================================================================================================
//
// `networking-and-replication` — "Dedicated server": "The engine SHALL support a **dedicated
// server** configuration that excludes client-only subsystems and content **at build and cook time,
// rather than disabling them at runtime**. A dedicated server build SHALL exclude: the renderer,
// VFX, UI, client audio, and the editor."
//
// A runtime flag would satisfy nobody: the renderer would still be linked, the shaders still
// packaged, and the first `if (!headless)` anyone forgot would create a window on a machine with no
// display. So the build half is `src/networking/CMakeLists.txt`'s dependency list — this module
// names `cy::core-*`, `cy::ecs`, `cy::gameplay` and `cy::replay` and nothing else, so no graphics,
// audio or interface symbol is reachable from a server built on it — and
// `tests/test_server.cpp` asserts the *source* half with `__has_include`, the way
// `src/gameplay/tests/test_bypass.cpp` asserts that gameplay cannot reach an input device.
//
// The cook half is `CookExclusions` below: what a dedicated server cook drops, what it keeps, and
// the report the specification asks for — "WHEN a server cook completes THEN it SHALL report what
// was excluded and the resulting size, so accidental inclusions are visible."
//
// ================================================================================================
// WHAT THIS CLASS IS AND IS NOT
// ================================================================================================
//
// `DedicatedServer` is the **session loop's networking half**: admit peers, verify their
// compatibility scope, hold their budgets, and step the transport. It owns no world and no tick —
// `gameplay-framework` owns those, and a server that drove the simulation from here would be a
// second scheduler. `step()` takes the tick it is told about.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/networking/authority.h>
#include <cy/networking/mode.h>
#include <cy/networking/scheduler.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// What a dedicated server cook drops and keeps. `networking-and-replication`: "a dedicated server
/// cook SHALL exclude client-only assets — textures, high-resolution meshes, audio, shaders, VFX
/// assets — while retaining collision, navigation, and gameplay data".
struct CookExclusions {
    bool textures = true;
    bool shaders = true;
    bool audio = true;
    bool vfx_assets = true;
    bool high_resolution_meshes = true;
    /// The subset case: "WHEN a mesh contributes collision geometry THEN the server cook SHALL
    /// retain the collision representation without the render mesh." Retaining the whole mesh would
    /// be the easy wrong answer, so it is a separate flag rather than a consequence.
    bool keep_collision_from_meshes = true;
    bool keep_navigation = true;
    bool keep_gameplay_data = true;
};

/// One cook's result, as the specification requires it to be reported.
struct CookReport {
    u32 assets_examined = 0;
    u32 excluded = 0;
    u32 retained = 0;
    /// Meshes whose render data was dropped and whose collision data was kept. The subset case, as
    /// a number, so that "we kept the whole mesh" and "we kept its collision" do not read alike.
    u32 collision_subsets = 0;
    u64 bytes_excluded = 0;
    u64 bytes_retained = 0;
};

/// An asset as the cook sees it. A flat value, because this module has no asset database and a cook
/// profile that needed one could not be tested without a content tree.
struct CookAsset {
    enum class Kind : u8 {
        Texture = 0,
        Shader,
        Audio,
        VfxAsset,
        Mesh,
        Collision,
        Navigation,
        GameplayData,
    };

    Kind kind = Kind::GameplayData;
    u64 bytes = 0;
    /// Meshes only: the bytes of the collision representation inside them.
    u64 collision_bytes = 0;
};

const char* cook_asset_kind_name(CookAsset::Kind kind) noexcept;

/// Apply `exclusions` to `assets`. Pure: a cook profile that is a function is a cook profile a test
/// can exhaust.
[[nodiscard]] CookReport apply_cook_profile(const CookExclusions& exclusions,
                                            Span<const CookAsset> assets) noexcept;

/// A connected peer, from the server's side.
struct ServerPeer {
    PeerId peer;
    ConnectionState state = ConnectionState::Disconnected;
    CompatibilityScope scope{};
    BandwidthBudget budget{};
    /// The last snapshot this peer acknowledged.
    u32 acknowledged = 0;
    u64 joined_tick = 0;
    /// True once the application's authentication step has admitted it.
    bool admitted = false;
};

/// Why a join was refused. `JoinRefusal` covers the compatibility half; these are the server's own.
enum class AdmissionRefusal : u8 {
    None = 0,
    SessionFull,
    /// The compatibility scope differs. `reason` carries which.
    Incompatible,
    AlreadyJoined,
};

const char* admission_refusal_name(AdmissionRefusal refusal) noexcept;

struct AdmissionRejection {
    AdmissionRefusal refusal = AdmissionRefusal::None;
    JoinRefusal compatibility = JoinRefusal::None;
};

/// The dedicated server's networking half. Headless by construction — see the header comment.
class DedicatedServer {
public:
    DedicatedServer(Allocator& allocator, Transport& transport, NetworkMode mode,
                    const CompatibilityScope& scope, u32 maximum_peers) noexcept;

    DedicatedServer(const DedicatedServer&) = delete;
    DedicatedServer& operator=(const DedicatedServer&) = delete;

    /// Admit a peer whose scope has been read from its handshake. The compatibility check is
    /// `mode.h`'s, so a lockstep server and a lockstep client refuse each other by the same rule.
    [[nodiscard]] Expected<u32, AdmissionRejection> admit(PeerId peer,
                                                          const CompatibilityScope& scope,
                                                          u64 tick) noexcept;

    void drop(PeerId peer) noexcept;

    [[nodiscard]] ServerPeer* find(PeerId peer) noexcept;
    [[nodiscard]] const ServerPeer* find(PeerId peer) const noexcept;

    /// Step the transport. The server's only clock input, and it is given rather than read: a
    /// headless server driven by `std::chrono` would be a headless server whose replay does not
    /// reproduce.
    void step(u64 tick, u64 now_ms) noexcept;

    [[nodiscard]] u32 peer_count() const noexcept { return static_cast<u32>(peers_.size()); }
    [[nodiscard]] const ServerPeer& peer_at(u32 index) const noexcept { return peers_[index]; }
    [[nodiscard]] NetworkMode mode() const noexcept { return mode_; }
    [[nodiscard]] u32 maximum_peers() const noexcept { return maximum_; }
    [[nodiscard]] u64 admissions() const noexcept { return admissions_; }
    [[nodiscard]] u64 refusals() const noexcept { return refusals_; }
    [[nodiscard]] u64 steps() const noexcept { return steps_; }

    /// `networking-and-replication` — "Scene and level synchronisation": the server starts play
    /// only when every admitted peer has reported readiness.
    [[nodiscard]] Status report_ready(PeerId peer) noexcept;
    [[nodiscard]] bool everyone_ready() const noexcept;

private:
    Allocator* allocator_;
    Transport* transport_;
    NetworkMode mode_;
    CompatibilityScope scope_;
    u32 maximum_;
    Array<ServerPeer> peers_;
    Array<bool> ready_;
    u64 admissions_ = 0;
    u64 refusals_ = 0;
    u64 steps_ = 0;
};

}  // namespace cy::net
