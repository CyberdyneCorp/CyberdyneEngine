// SPDX-License-Identifier: MIT
#include "scene_audio.h"

#include <cstdio>
#include <utility>

#if defined(CY_EDITOR_WINDOW_HAS_DEVICE_AUDIO)
#    include <cy/backends/audio/miniaudio_backend.h>
#endif

namespace cy::sample::editor_window {

SceneAudio::SceneAudio(Allocator& allocator, std::string project) noexcept
    : allocator_(&allocator), project_(std::move(project)) {}

SceneAudio::~SceneAudio() {
    // The server stops the device's callback before the device goes.
    authoring_.reset();
    device_.reset();
}

Status SceneAudio::initialize() noexcept {
#if defined(CY_EDITOR_WINDOW_HAS_DEVICE_AUDIO)
    device_ = std::make_unique<audio::MiniaudioBackend>(*allocator_);
    authoring_ = std::make_unique<editor::AudioAuthoring>(*allocator_, device_.get());
    if (Status opened = authoring_->initialize(); opened) {
        authoring_->set_project(project_);
        return authoring_->apply_project_mixer();
    } else {
        std::fprintf(stderr,
                     "cy_editor_window_runtime: audio: the output device did not open (%s); "
                     "mixing on the null backend, silently\n",
                     opened.error().message);
    }
    authoring_.reset();
    device_.reset();
#endif
    authoring_ = std::make_unique<editor::AudioAuthoring>(*allocator_);
    if (Status opened = authoring_->initialize(); !opened) {
        authoring_.reset();
        return opened;
    }
    authoring_->set_project(project_);
    return authoring_->apply_project_mixer();
}

editor::AudioAuthoring* SceneAudio::authoring() noexcept {
    return authoring_.get();
}

const char* SceneAudio::backend() const noexcept {
    return authoring_ ? authoring_->server().backend_name() : "none";
}

void SceneAudio::frame(f32 delta_seconds, const first_light::Camera& camera) noexcept {
    if (!authoring_) {
        return;
    }
    const Vec3 position{static_cast<f32>(camera.position[0]), static_cast<f32>(camera.position[1]),
                        static_cast<f32>(camera.position[2])};
    // A preview places its own listener at the editor camera; Play follows the frame's camera.
    if (authoring_->playing()) {
        authoring_->set_listener(position, camera.forward);
    }
    authoring_->pump(delta_seconds);
}

Status SceneAudio::start_play(const scene::serialization::World& world) noexcept {
    if (!authoring_) {
        return fail(ErrorCode::Unavailable, "audio: this host has no audio server");
    }
    return authoring_->start_play(world);
}

void SceneAudio::pause_play(bool paused) noexcept {
    if (authoring_) {
        authoring_->pause_play(paused);
    }
}

void SceneAudio::stop_play() noexcept {
    if (authoring_) {
        authoring_->stop_play();
    }
}

u32 SceneAudio::play_voices() const noexcept {
    return authoring_ ? static_cast<u32>(authoring_->play_voices()) : 0U;
}

const std::vector<editor::EmitterMarker>& SceneAudio::markers(
    const scene::serialization::World& world) noexcept {
    Expected<std::vector<editor::EmitterMarker>, Error> read = editor::read_emitters(world);
    if (read) {
        markers_ = std::move(*read);
        warned_ = false;
    } else {
        markers_.clear();
        if (!warned_) {
            std::fprintf(stderr, "cy_editor_window_runtime: audio source: %s\n",
                         read.error().message);
            warned_ = true;
        }
    }
    return markers_;
}

}  // namespace cy::sample::editor_window
