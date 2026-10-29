// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_PLAY_H
#define CY_SAMPLE_RTS_API_PLAY_H
// play.h — `--play`: the battle as a game a person plays, in a window, in real time.
//
//   cy_sample_rts-api --play --map frontier.cypak          a window, your mouse and keyboard
//   just play-rts                                          the same, built and found for you
//       --width <n> --height <n>   the window's size, and the frame's (1920 by 1080)
//       --no-vsync                 present as fast as the device draws
//       --record <file>            write the session's command log when it ends
//       --replay <file>            watch a recorded session: its orders, not yours or the AI's
//       --input <file>             a written hand instead of yours (input_script.h)
//       --frames <n>               end after this many frames
//       --headless                 no window and no device: SDL's events alone (the tests)
//
// WHAT THIS FILE IS, AND WHAT IT IS NOT. It is the loop a desktop game has: the platform's window
// and its events (`Sdl3DisplayServer`), the platform's keyboard and mouse feeding the engine's
// input server (`Sdl3InputSource`), the host's frames at sixty a second whatever the display's
// rate, the frame drawn (map_render.h) and presented to the window's swapchain, letterboxed
// (present_pass.h), and the session's command log written at the end (replay.h). It decides
// nothing about the game. Every click's meaning, every order, the opponent, the pause and the end
// are the Swift game's (game/Skirmish.swift); the host is told to hold its clock or to stop through
// `SkirmishControl`, and does.
//
// THE POINTER. The frame is drawn at a fixed extent and the game lays out, picks and hit-tests in
// a 1280 by 720 view; the window is any size. The letterbox that scales the frame into the window
// is inverted once, where the pointer is read (`Sdl3InputSource::set_pointer_mapping`), so the game
// never sees a window coordinate.
//
// A WRITTEN HAND. `--input` replays a script of keys, pointer moves, clicks and the wheel by
// pushing real SDL events (`Sdl3EventInjector`), one frame at a time, so a test or a capture drives
// the game through the same door a person does; a script's frames are the host's, so a scripted
// session is unpaced and the same every run.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string>

#include "replay.h"
#if defined(CY_RTS_API_MAP)
#    include "map_render.h"
#endif

namespace sample::rts {

class RtsHost;

namespace map {
struct MapContent;
}

struct PlayOptions {
    /// A window and a device; false is `--headless`: SDL's events alone, nothing drawn.
    bool window = true;
    bool vsync = true;
    /// `--input`: the written hand, or empty for a person's.
    std::string input;
    /// `--record`: where the command log is written when the session ends; empty writes none.
    std::string record;
    /// `--replay`: the recorded log the host is giving the session's orders from, or null.
    const ReplayLog* replay = nullptr;
    /// End after this many frames; zero runs until the game quits, the window closes, the script
    /// says `quit`, or a replay has run all its ticks.
    cy::u64 frames = 0;
#if defined(CY_RTS_API_MAP)
    /// The frame: its extent is the window's first size. The map it is drawn on.
    MapRenderOptions render;
    const map::MapContent* map = nullptr;
#endif
};

/// What a session did, for the report.
struct PlayReport {
    cy::u64 frames = 0;
    cy::u64 ticks = 0;
    bool window = false;
    cy::u32 width = 0;
    cy::u32 height = 0;
    cy::u32 resizes = 0;
    cy::u64 presented = 0;
    cy::u64 injected = 0;
    cy::u32 missed_aims = 0;
    /// Why it ended: the game asked (`SkirmishControl.quit`), the window was closed, the script
    /// said `quit`, the frames ran out, or a replay ran its ticks.
    const char* ended = "";
    /// The frame's cost, from the renderer, over its timed frames.
    cy::f64 frame_ms = 0.0;
    cy::f64 worst_ms = 0.0;
    /// Presenting it: waiting for the window's image, drawing onto it, handing it back.
    cy::f64 present_ms = 0.0;
    cy::u32 outlined = 0;
    cy::u32 validation_errors = 0;
};

/// Run `host` — started with `HostOptions::skirmish` and `platform_input` — as a person's session.
/// Unavailable, naming it, where the machine has no display or no device for a window.
[[nodiscard]] cy::Status run_play(RtsHost& host, const PlayOptions& options,
                                  PlayReport& report) noexcept;

void print_play_report(const PlayReport& report) noexcept;

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_PLAY_H
