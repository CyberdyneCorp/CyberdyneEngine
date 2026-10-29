// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_HOSTED_GAME_H
#define CY_SAMPLE_PLAY_HOSTED_GAME_H
// hosted_game.h — a sample's game host and renderer as a `PlayGame`, so the play host can run it.
//
// A game host that runs a scripted session already has the parts of a frame; it only has to
// expose them separately, and every host here spells them the same way:
//
//   Host::Step                       the frame's time: `seconds`, `interpolation`, `paused`
//   host.input()                     its input server
//   host.pump()                      the scene tree's callbacks
//   host.tick(now)                   one fixed tick
//   host.drain(now)                  a paused frame's input, resolved again at the last tick
//   host.begin_frame()               the pointer's edges published
//   host.update(step)                the rest of the frame
//   host.pointer(), host.ui_event(e), host.set_pointer_over_ui(over)
//
//   renderer.frame(host)             draw what the host just ran
//   renderer.output(), width(), height()
//
// This adapter is the whole of what joins them, once, for every such game: the interface's frame
// sits between the pointer's publication and the behaviours' `onUpdate`, which is where the RTS
// host runs its own (samples/13-rts-api, `ui_frame`) and for the same reason — a click must reach
// its owner in the frame it happened, and the game must know whether the pointer is over the HUD
// before it reads the pointer.

#include "play_host.h"

namespace cy::play {

/// The renderer of a build that has none: a game played with `--no-draw`, or built without the
/// Vulkan renderer, is still driven by the play host — its window, its input, its loop and its
/// pause — with no picture.
struct NoRenderer {
    template <typename Host>
    [[nodiscard]] Status frame(Host& /*host*/) noexcept {
        return ok();
    }
    [[nodiscard]] static rhi::TextureHandle output() noexcept { return {}; }
    [[nodiscard]] static u32 width() noexcept { return 0; }
    [[nodiscard]] static u32 height() noexcept { return 0; }
};

template <typename Host, typename Renderer>
class HostedGame final : public PlayGame {
public:
    HostedGame(Host& host, PlayUi& ui, Renderer* renderer) noexcept
        : host_(&host), ui_(&ui), renderer_(renderer) {}

    [[nodiscard]] input::InputServer& input() noexcept override { return host_->input(); }

    [[nodiscard]] Status tick(Nanoseconds now) noexcept override {
        if (Status pumped = host_->pump(); !pumped) {
            return pumped;
        }
        return host_->tick(now);
    }

    [[nodiscard]] Status drain(Nanoseconds now) noexcept override { return host_->drain(now); }

    [[nodiscard]] Status frame(const FrameClock& clock) noexcept override {
        if (Status pumped = host_->pump(); !pumped) {
            return pumped;
        }
        host_->begin_frame();
        const CyInputPointer pointer = host_->pointer();
        UiPointer routed;
        routed.position = Vec2{pointer.position[0], pointer.position[1]};
        routed.pressed = pointer.buttons_pressed;
        routed.released = pointer.buttons_released;
        Host* host = host_;
        if (Status shown = ui_->frame(static_cast<f32>(clock.seconds), clock.viewport, routed,
                                      [host](const CyUiEvent& event) { host->ui_event(event); });
            !shown) {
            return shown;
        }
        host_->set_pointer_over_ui(ui_->pointer_over());
        typename Host::Step step;
        step.seconds = static_cast<f32>(clock.seconds);
        step.interpolation = clock.interpolation;
        step.paused = clock.paused;
        return host_->update(step);
    }

    [[nodiscard]] Expected<PresentSource, Error> draw() noexcept override {
        if (renderer_ == nullptr) {
            return fail(ErrorCode::Unavailable, "this game was started without a renderer");
        }
        if (Status drawn = renderer_->frame(*host_); !drawn) {
            return make_unexpected(drawn.error());
        }
        PresentSource source;
        source.texture = renderer_->output();
        source.format = rhi::Format::Rgba8Unorm;
        source.width = renderer_->width();
        source.height = renderer_->height();
        source.use = rhi::ImageUse::SampledRead;
        return source;
    }

private:
    Host* host_;
    PlayUi* ui_;
    Renderer* renderer_;
};

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_HOSTED_GAME_H
