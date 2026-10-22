// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_MAP_RENDER_H
#define CY_SAMPLE_RTS_API_MAP_RENDER_H
// map_render.h — the RTS session drawn on the RTS test map (content/rts/frontier.cymap).
//
//   cy_sample_rts-api --render <dir> --map <frontier.cypak>
//
// The same scripted session render.h draws over boxes, drawn instead on a map that looks like a
// game: the cooked package (map/map_load.h) loaded through the asset system — the terrain with its
// painted layers, the lake, the forest the engine's foliage placement grew, the buildings and the
// resources — and the game's units as the skinned worker, animated. Everything is the engine's
// own frame: `FrameAssembly` and `FrameRecorder` with the standard material and its texture
// table, the sun's shadow map with the percentage-closer soft filter, ambient occlusion
// (`occlusion::AmbientOcclusionPass`), the device GI over the level's `IlluminationSystem` the
// Swift game tunes (`gi_gpu::GpuGiFrame`), blue's fog of war (`fog_of_war::FogOfWarPass`), the
// skinning pass (`skinning::SkinnedScene`) and the HUD the game built (`ui::render::UiRenderer`).
//
// WHAT MOVES, AND FROM WHERE:
//
//   the workers        the host's units, posed by the engine's animation system the game drives
//                      (ABI 1.7): the matrices it published, uploaded as they are
//   the company        the lockstep session's sixteen units (ABI 1.8), on their field north of the
//                      level, walking or standing as the session says, the rig's clips sampled
//                      here; the first group is blue's, the second another side's, drawn only where
//                      blue's fog of war shows it
//   the camp           two raiders and the watchtower, drawn by blue's sight (ABI 1.10)
//   the crate          where its body is
//
// THE MAP DOES NOT TOUCH THE GAME'S RULES. The level is the map's flat square at the origin and the
// game's rules run there as before; the map adds the GI's boxes for its buildings and the ground
// the fog of war's grid covers (`map::map_bounds`, fog_grid.h), so every surface drawn has a cell
// and ground blue never saw is black. `smoke.rts_api_map` holds the gameplay half of the report to
// the headless run on the same map, and every fog decision to the run without `--map`.
//
// What it prints: one line with the frame's wall time over the timed frames (from the start of the
// frame's recording to its readback, the device's work included), the worst frame, the GI's and
// the fog's device time, what the frame drew, and the map's load. It writes stills at the frames
// render.h does (`rts-map-<frame>.png`), and every frame to `--video <dir>` when asked.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>

#include <memory>
#include <string>
#include <vector>

#include "illumination.h"

namespace sample::rts {

class RtsHost;

namespace map {
struct MapContent;
}

/// Where the frame looks from.
enum class MapView : cy::u8 {
    /// The game's own camera: what the player sees.
    Game = 0,
    /// A slow orbit over the map that keeps the level, the company and the village in frame: the
    /// video's camera. The game's camera still runs the game.
    Overview,
};

struct MapRenderOptions {
    /// Where the stills are written.
    std::string directory;
    /// Every `video_every`th frame, as `frame-<n>.png`, for a video. Empty writes none.
    std::string video_directory;
    cy::u32 video_every = 1;
    cy::u32 width = 1920;
    cy::u32 height = 1080;
    MapView view = MapView::Game;
    /// The frame's features. Each off is that feature's control.
    bool gi = true;
    bool fog = true;
    bool hud = true;
    bool ambient_occlusion = true;
    bool shadows = true;
    /// The graphics API's validation layer, which every other run of this sample keeps on. Off is
    /// for measuring what the frame costs, which the layer's own checks would be part of.
    bool validation = true;
};

/// The frame's numbers, over the timed frames.
struct MapRenderReport {
    cy::u64 frames = 0;
    cy::u64 timed = 0;
    cy::f64 frame_ms = 0.0;
    cy::f64 worst_ms = 0.0;
    cy::f64 game_ms = 0.0;
    /// The frame's two halves: recording and submitting its command buffers on the processor, and
    /// waiting for the device to finish them — the device's own time, as near as a wall clock
    /// that starts at the submission can say.
    cy::f64 record_ms = 0.0;
    cy::f64 device_wait_ms = 0.0;
    cy::f64 gi_device_ms = 0.0;
    cy::f64 fog_device_ms = 0.0;
    cy::u32 draws = 0;
    cy::u32 skinned_draws = 0;
    cy::u32 instances = 0;
    cy::u32 triangles = 0;
    cy::u32 plants = 0;
    cy::u32 stills = 0;
    cy::u32 validation_errors = 0;
    /// On the last frame: figures of another side drawn because blue sees them, and withheld
    /// because it does not (the camp's raiders and the company's second group).
    cy::u32 enemies_shown = 0;
    cy::u32 enemies_hidden = 0;
};

/// The map's buildings and resources as the level's GI holds them (`HostOptions::extra_gi_boxes`):
/// a box round each, coloured by its main part's material. The barracks and the beacon are the
/// level's own boxes already, and are left out.
[[nodiscard]] std::vector<FieldBox> map_gi_boxes(const map::MapContent& content);

class MapRenderer {
public:
    explicit MapRenderer(cy::Allocator& allocator) noexcept;
    ~MapRenderer();

    MapRenderer(const MapRenderer&) = delete;
    MapRenderer& operator=(const MapRenderer&) = delete;

    /// A Vulkan device, the map's geometry, materials and textures made resident, and the frame's
    /// stages over `host`. Unavailable, naming it, where the machine has no device. `content` is
    /// the map as the loader read it; it is copied to the device and not kept.
    [[nodiscard]] cy::Status start(RtsHost& host, const map::MapContent& content,
                                   const MapRenderOptions& options) noexcept;
    /// Draw the frame `host` just ran.
    [[nodiscard]] cy::Status frame(RtsHost& host) noexcept;
    [[nodiscard]] const MapRenderReport& report() const noexcept;
    /// The report line.
    void print_report() const noexcept;

private:
    struct State;
    cy::Allocator* allocator_;
    std::unique_ptr<State> state_;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_MAP_RENDER_H
