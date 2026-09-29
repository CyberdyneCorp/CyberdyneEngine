// SPDX-License-Identifier: MIT
#pragma once
// The hosted runtime's audio: one engine `AudioServer` for the mixer editor, cue preview and Play.
// Issue #29, and the open task #14 left in `editor-scene-lights-and-game-view` ("audio remains an
// open task").
//
// A DEVICE WHEN THERE IS ONE, AND IT SAYS WHICH. With `CY_AUDIO` the runtime opens the miniaudio
// output device, so a previewed cue is heard. Where no device opens — a CI runner, a machine with
// no sound server — it mixes on the null backend and prints that it did; the editor's audio panel
// shows the backend by name from every state reply, so the difference is never silent.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/editor/audio_authoring.h>

#include <memory>
#include <string>
#include <vector>

#include "scene.h"

namespace cy::sample::editor_window {

class SceneAudio {
public:
    SceneAudio(Allocator& allocator, std::string project) noexcept;
    ~SceneAudio();
    SceneAudio(const SceneAudio&) = delete;
    SceneAudio& operator=(const SceneAudio&) = delete;
    SceneAudio(SceneAudio&&) = delete;
    SceneAudio& operator=(SceneAudio&&) = delete;

    /// Open a device where this build has one, else the null backend. Fails only when neither
    /// initialises, and then `authoring()` is null.
    [[nodiscard]] Status initialize() noexcept;

    /// The server the backend service answers `audio.*` from, or null when there is none.
    [[nodiscard]] editor::AudioAuthoring* authoring() noexcept;
    [[nodiscard]] const char* backend() const noexcept;

    /// Once per published frame: the listener follows the camera the frame was rendered from.
    void frame(f32 delta_seconds, const cy::sample::first_light::Camera& camera) noexcept;

    /// Play: start the world's autoplay sources, hold them for Pause, stop them for Stop.
    [[nodiscard]] Status start_play(const scene::serialization::World& world) noexcept;
    void pause_play(bool paused) noexcept;
    void stop_play() noexcept;
    [[nodiscard]] u32 play_voices() const noexcept;

    /// The world's audio sources, for the emitter gizmos and for picking. Empty for a world whose
    /// sources do not read; the reason is printed once.
    [[nodiscard]] const std::vector<editor::EmitterMarker>& markers(
        const scene::serialization::World& world) noexcept;

private:
    Allocator* allocator_;
    std::string project_;
    std::unique_ptr<audio::AudioBackend> device_;
    std::unique_ptr<editor::AudioAuthoring> authoring_;
    std::vector<editor::EmitterMarker> markers_;
    bool warned_ = false;
};

}  // namespace cy::sample::editor_window
