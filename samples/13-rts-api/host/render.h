// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_RENDER_H
#define CY_SAMPLE_RTS_API_RENDER_H
// render.h — the RTS drawn, lit by the engine's device GI. Issue #35, the live half.
//
//   cy_sample_rts-api --render <dir>            the scripted session, drawn every frame
//   cy_sample_rts-api --render <dir> --gi off   the same frames without the GI: the control
//
// THE FIELD IS BOXES, as samples/13-rts-selection's is: the pipeline suites' one scene
// (`pipeline_test::FrameScene`), its floor shaped into the level's 32 m ground and its boxes into
// the barracks, the beacon, the crate and up to eight units where the game put them, seen from the
// game's own camera and lit by the level's sun. Every box is a box the GI holds too
// (illumination.h's `field_boxes`), in the same metres and the same colours.
//
// THE GI IS THE ENGINE'S FRAME FEATURE. `gi_gpu::GpuGiFrame` is attached over the level's one
// `IlluminationSystem` — the system the Swift game tunes through ABI 1.9 — so what the game does
// to the light is in the next frames: the GI volume it placed over its base, and the beacon it puts
// out at frame 151 and lights again at frame 251 (game/Lighting.swift). The frame composites the
// GI where the flat sky ambient sat; with `--gi off` the stage is not attached and the frame is the
// pipeline's as it always was.
//
// What it prints, one line the sample's render test reads: the drawn frame's wall time from its
// start to its readback (the scene and the GI's device passes), the game's frame before it (the
// Swift game, its fixed tick and the GI's host update, timed apart too), the GI's own device time,
// and the mean luminance of the frame round the beacon's foot at the three frames the game reads
// the light — lit, out, and lit again. It writes the frame at those three frames and the
// last (`rts-gi-<frame>.png`, or `rts-nogi-<frame>.png`).
//
// With `--fog` (ABI 1.10) the frame carries blue's fog of war: the visibility the host's grid keeps
// (host/fog.h), faded on the frame's clock and applied by `fog_of_war::FogOfWarPass` at the frame's
// `FogOfWar` stage, and the enemy camp's three boxes — the last three unit slots — drawn by blue's
// sight of them. The stills are then `rts-fog-<frame>.png`, and the report line says how many of
// the camp each still drew and how.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>

#include <memory>
#include <string>

namespace sample::rts {

class RtsHost;

struct RenderOptions {
    /// Where the stills are written.
    std::string directory;
    /// Attach the device GI. False is the control.
    bool gi = true;
    /// Draw blue's fog of war (ABI 1.10): the frame's `FogOfWar` stage over the grid the game's
    /// visions keep, and the enemy camp drawn as blue's sight allows — a raider only while blue
    /// sees it, the watchtower as its ghost once blue has left it. False is the frame it always
    /// was.
    bool fog = false;
};

class RtsRenderer {
public:
    explicit RtsRenderer(cy::Allocator& allocator) noexcept;
    ~RtsRenderer();

    RtsRenderer(const RtsRenderer&) = delete;
    RtsRenderer& operator=(const RtsRenderer&) = delete;

    /// A Vulkan device and the scene, and the GI over `host`'s system. Unavailable, naming it,
    /// where the machine has no device.
    [[nodiscard]] cy::Status start(RtsHost& host, const RenderOptions& options) noexcept;
    /// Draw the frame `host` just ran.
    [[nodiscard]] cy::Status frame(RtsHost& host) noexcept;
    /// The report line.
    void print_report() const noexcept;

private:
    struct State;
    cy::Allocator* allocator_;
    std::unique_ptr<State> state_;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_RENDER_H
