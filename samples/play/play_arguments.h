// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PLAY_ARGUMENTS_H
#define CY_SAMPLE_PLAY_PLAY_ARGUMENTS_H
// play_arguments.h — the command line every played game takes, and the report it prints.
//
//   --play                       open a window and play (what `just play-fps` / `play-tps` run)
//   --window <w>x<h>             the window's size (1280x720)
//   --fullscreen                 borderless, at the desktop's resolution
//   --no-vsync                   present the newest frame without waiting for the display
//   --no-draw                    a window and no device: the play loop driven with no picture
//   --headless                   SDL's dummy video driver: no display either (what CI runs)
//   --frames <n>                 stop after n frames
//   --fixed-clock                one step a frame whatever the wall says: reproducible runs
//   --no-focus-pause             another window taking the focus does not pause (a film, a timing)
//   --input-script <file>        platform events at given frames (input_script.h)
//   --bindings <file>            a player's rebinding (bindings_file.h)
//   --capture <dir>              every --capture-every <n>-th presented frame as a PNG there
//   --validation                 the graphics API's validation layer (off when playing)
//
// The report is one `play` line a test reads: what the loop did, what paused it, and the frame
// times at the window's size.

#include <cy/core/base/types.h>

#include <string>

#include "play_host.h"

namespace cy::play {

struct PlayArguments {
    bool play = false;
    PlayOptions options;
    std::string capture_directory;
    std::string input_script;
    std::string bindings;
};

/// Consume the play flag at `argv[index]` (and its value). True when it was one.
[[nodiscard]] bool parse_play_flag(int argc, char** argv, int& index, PlayArguments& out) noexcept;

/// Whether `--play` is on the command line, before anything else is parsed.
[[nodiscard]] bool wants_play(int argc, char** argv) noexcept;

/// The `play` report line.
void print_play_report(const PlayReport& report) noexcept;

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PLAY_ARGUMENTS_H
