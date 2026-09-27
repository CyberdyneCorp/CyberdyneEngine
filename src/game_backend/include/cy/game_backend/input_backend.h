// SPDX-License-Identifier: MIT
// cy/game_backend/input_backend.h — the `input` adapter behind ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer A. The contract is cy/abi/game/input.h and design.md.
//
// `InputAdapter` answers the `input_*` entries from a `cy::input::InputServer`:
//
//   * ACTION STATE is the record the server resolved for its last tick (`InputUser::action_state`
//     after `resolve_tick`). Nothing here reads a device for it, which is what makes the answer the
//     same on a live run and on a replay that feeds the same events.
//   * CONTEXTS are the server's registered `MappingContext`s, found by name and pushed onto or
//     popped from one user's stack. The user's resolved table is rebuilt at the next
//     `resolve_tick`, so a push is effective from the next tick and never mid-tick.
//   * THE POINTER AND THE MODIFIER KEYS are device state, read from the mouse and keyboard the
//     server routes to that user. The pointer's EDGES — buttons pressed and released, motion and
//     wheel — are "since the previous frame update", which the server does not keep: it applies
//     events only inside `resolve_tick` and zeroes the mouse's deltas after each. So the adapter
//     keeps them, and needs two calls from the host's loop:
//
//        adapter.observe_pending();          // before EVERY InputServer::resolve_tick
//        server.resolve_tick(...);
//        ...
//        adapter.begin_frame();              // once per frame, before frame-update dispatch
//
//     `observe_pending` walks the events about to be resolved, in resolution order, and folds the
//     mouse's into per-user accumulators; `begin_frame` publishes them as the frame's pointer and
//     starts the next window. A host that skips both still gets position, buttons held and the
//     flags — those are read live — with every edge zero.
//
// The adapter owns nothing but those accumulators and borrows the server, which must outlive it.
// Not thread-safe, like the server: the game thread only.

#pragma once

#include <cy/abi/game/input.h>
#include <cy/abi/host.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/server.h>

namespace cy::game_backend {

/// Implements `cy::abi::game::InputBackend` over a `cy::input::InputServer`.
class InputAdapter final : public cy::abi::game::InputBackend {
public:
    /// How many input users have pointer edges tracked. `input-and-actions` asks for at least 8
    /// local users; a user past this still reports its pointer, with every edge zero.
    static constexpr u32 kMaxPointerUsers = 16;

    explicit InputAdapter(cy::input::InputServer& server) noexcept : server_(&server) {}

    /// Fold the mouse events waiting in the server's window into the frame's pointer edges. Call
    /// immediately before each `InputServer::resolve_tick`.
    void observe_pending() noexcept;

    /// Publish the edges accumulated since the previous call as this frame's, and start the next
    /// window. Call once per frame, before frame-update dispatch.
    void begin_frame() noexcept;

    /// What the host knows and the input server does not: whether the pointer is inside the
    /// window's client area, and whether an interface layer holds pointer focus. By default a
    /// user with a pointer is in the window and not over an interface.
    void set_pointer_focus(u32 user, bool in_window, bool over_ui) noexcept;

    [[nodiscard]] CyResult find_action(const char* name,
                                       CyInputAction& out_action) noexcept override;
    [[nodiscard]] CyResult action_state(u32 user, CyInputAction action,
                                        CyInputActionState& out_state) noexcept override;
    [[nodiscard]] CyResult pointer(u32 user, CyInputPointer& out_pointer) noexcept override;
    [[nodiscard]] CyResult modifiers(u32 user, u32& out_modifiers) noexcept override;
    [[nodiscard]] CyResult find_context(const char* name,
                                        CyInputContext& out_context) noexcept override;
    [[nodiscard]] CyResult push_context(u32 user, CyInputContext context,
                                        i32 priority) noexcept override;
    [[nodiscard]] CyResult pop_context(u32 user, CyInputContext context) noexcept override;

private:
    /// One user's pointer edges over one window.
    struct PointerEdges {
        u32 pressed = 0;
        u32 released = 0;
        f32 delta[2] = {0.0F, 0.0F};
        f32 wheel[2] = {0.0F, 0.0F};
    };
    /// Per user: the edges being accumulated, the edges the current frame publishes, the button
    /// level the accumulation compares against, and the host's focus flags.
    struct PointerTrack {
        PointerEdges accumulating;
        PointerEdges published;
        u32 level = 0;
        bool level_known = false;
        bool in_window = true;
        bool over_ui = false;
    };

    [[nodiscard]] CyResult check_user(u32 user) const noexcept;
    /// The context `context` names, or null when it names none this server registered.
    [[nodiscard]] const cy::input::MappingContext* resolve_context(
        CyInputContext context) const noexcept;
    void observe(const cy::input::DeviceEvent& event, u32 user) noexcept;

    cy::input::InputServer* server_;
    PointerTrack tracks_[kMaxPointerUsers];
};

/// Bind `adapter` as `host`'s input backend (`host.game.input`), or unbind with null. The one place
/// an embedder wires the input service; the adapter must outlive the binding.
void bind(cy::abi::Host& host, InputAdapter* adapter) noexcept;

}  // namespace cy::game_backend
