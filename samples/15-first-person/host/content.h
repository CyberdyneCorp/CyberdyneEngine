// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_CONTENT_H
#define CY_SAMPLE_FIRST_PERSON_CONTENT_H
// content.h — the content this host makes in code and registers under the names arena.h and
// game/Contract.swift share: the actions and their bindings, and four sounds.
//
//   player.move      a two-axis action on WASD, and the pad's left stick
//   player.look      the pad's right stick (the mouse's look is its pointer's counts)
//   player.sprint    left Shift, or the left stick pressed
//   player.jump      Space, or the pad's south button
//   player.crouch    C or left Control, or the east button
//   weapon.fire      the left mouse button, or the right trigger
//   weapon.aim       the right mouse button, or the left trigger: the sights
//   weapon.reload    R, or the west button
//   weapon.shot      a crack and its tail: noise under a fast decay, a low thump under it
//   weapon.impact    a short tick
//   target.hit       a dull knock: a decaying low tone
//   target.down      a falling tone
//
// The sounds are generated, not loaded — a fixed linear congruential sequence and fixed tones — so
// they are the same samples on every machine, the way samples/13-rts-api makes its click.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/servers/input/server.h>

namespace sample::fps {

/// Declare the actions, register the "fps" mapping context and push it for user 0. Calls
/// `finalize_declarations`.
[[nodiscard]] cy::Status declare_input(cy::input::InputServer& input,
                                       cy::Allocator& allocator) noexcept;

/// The four sounds, mono at `sample_rate`.
enum class Sound : cy::u8 { Shot = 0, Impact, Hit, Down, Count };
[[nodiscard]] const char* sound_cue(Sound sound) noexcept;
[[nodiscard]] cy::Status make_sound(Sound sound, cy::u32 sample_rate,
                                    cy::Array<cy::f32>& out) noexcept;

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_CONTENT_H
