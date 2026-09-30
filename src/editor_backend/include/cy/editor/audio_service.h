// SPDX-License-Identifier: MIT
// cy/editor/audio_service.h — the `audio.*` operations of the editor backend service. Issue #29.
//
// Every operation but `audio.capabilities.get` answers with the same state (schema 1, see
// `AudioAuthoring::encode_state`), read back from the server AFTER the request was applied: the
// gain a bus now has, whether the preview voice started, what Play is playing. The editor shows
// that rather than what it sent, so a refused edit cannot look applied.
//
//   audio.capabilities.get   ()                                        -> vocabulary
//   audio.mixer.apply        (cymixer 1 text)                          -> state
//   audio.cue.preview        (name, cycue 1 text, placement)           -> state
//   audio.preview.stop       ()                                        -> state
//   audio.state.get          ([f32 advance seconds, null backend only]) -> state
//
// Payloads are little-endian; a text is a u32 byte count and UTF-8. A placement is u8 spatial,
// position, listener position and listener forward (three f32 each), min and max distance (f32),
// and the attenuation model's name.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/editor/audio_authoring.h>

#include <string_view>

namespace cy::editor {

/// Why an `audio.*` request was refused, or an empty code when it was answered.
struct AudioRefusal {
    const char* code = nullptr;
    const char* detail = nullptr;

    [[nodiscard]] bool refused() const noexcept { return code != nullptr; }
};

/// Answer one `audio.*` request into `reply`. `audio` is null when the host has no audio server.
[[nodiscard]] AudioRefusal answer_audio(AudioAuthoring* audio, std::string_view operation,
                                        Span<const u8> payload, Array<u8>& reply) noexcept;

}  // namespace cy::editor
