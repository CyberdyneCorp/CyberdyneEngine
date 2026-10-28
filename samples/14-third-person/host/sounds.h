// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_SOUNDS_H
#define CY_SAMPLE_THIRD_PERSON_SOUNDS_H
// sounds.h — the level's sounds, synthesised at start-up: two footsteps (flagstone and grass), a
// jump, a landing, the wind's loop and a bird. Nothing here is a file and nothing is third-party;
// each is a few lines of noise, filters and envelopes over a fixed linear congruential sequence, so
// the same bytes come out on every machine.
//
// The host makes one clip of each and registers it under the cue name the game plays it by
// (game/Contract.swift's `Content`).

#include <cy/core/base/types.h>

#include <vector>

namespace sample::tps {

/// One sound: its cue name and its mono samples.
struct Sound {
    const char* cue = "";
    std::vector<cy::f32> samples;
    /// Loops whole: the wind.
    bool looping = false;
};

/// Every sound the game plays, at `sample_rate`.
[[nodiscard]] std::vector<Sound> synthesise_sounds(cy::u32 sample_rate);

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_SOUNDS_H
