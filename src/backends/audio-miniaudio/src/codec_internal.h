// SPDX-License-Identifier: MIT
#pragma once
// Shared between the backend's two decoder translation units. Not installed; names no library type.

#include <cy/servers/audio/decoder.h>

#include <string_view>

namespace cy::audio::detail {

/// A loop from `LOOPSTART` plus `LOOPLENGTH` or `LOOPEND` comments — the convention game audio
/// tools write into Vorbis comments (in Ogg and in FLAC). Call once per comment; the result is
/// `present` once a start and a length or end have both been seen. `LOOPEND` is exclusive here,
/// as `SourceLoop::end` is.
class LoopComments {
public:
    void observe(std::string_view comment) noexcept;
    [[nodiscard]] SourceLoop loop() const noexcept;

private:
    bool has_start_ = false;
    bool has_length_ = false;
    bool has_end_ = false;
    u64 start_ = 0;
    u64 length_ = 0;
    u64 end_ = 0;
};

/// Open an Ogg Vorbis stream. src/vorbis_decoder.cpp.
[[nodiscard]] Expected<DecoderPtr, Error> open_vorbis(AudioByteSource& source,
                                                      Allocator& allocator) noexcept;

}  // namespace cy::audio::detail
