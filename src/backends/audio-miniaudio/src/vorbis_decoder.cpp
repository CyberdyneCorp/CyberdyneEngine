// SPDX-License-Identifier: MIT
// Ogg Vorbis through stb_vorbis's push API, over any `AudioByteSource`. Issue #87.
//
// WHY THE PUSH API. stb_vorbis's pull API reads a `FILE*` or a whole buffer in memory; neither is
// a stream from a package with only a ring buffer resident. Its push API takes whatever bytes it is
// handed and says how many it used, so this decoder holds one bounded input window and refills it
// from the source as packets are consumed.
//
// HOW IT SEEKS, SAMPLE-ACCURATELY. The push API has no seek; it has `flush`, after which it
// resynchronises on the next Ogg page it is fed, and from the first page whose granule position it
// can trust it reports exact sample offsets (`stb_vorbis_get_sample_offset`). So a seek:
//
//   1. bisects the file's pages for the last one whose granule position is at least two long
//      blocks before the target (the packet after a resync only primes the overlap and produces
//      nothing, and a page whose last packet continues gives no position at all);
//   2. flushes, feeds from that page, and decodes, discarding every packet until positions are
//      known and then every frame before the target;
//   3. if the first known position is already past the target, backs off four times further and
//      tries again; a target in the first two blocks reopens the stream instead.
//
// The result is the same samples a decode from the start delivers at that frame — bit for bit,
// because past the priming packet every output is the same overlap-add of the same packets —
// and integration.audio_codecs checks exactly that at random positions.
//
// MEMORY. One arena for everything stb_vorbis allocates (its setup tables and its per-packet
// scratch), sized at open and grown only if stb says it is too small, plus the input window. Both
// come from the allocator the decoder was opened with; nothing else is allocated, ever.

#include "codec_internal.h"

#include <cy/core/math/scalar.h>
#include <cy/servers/audio/buffer.h>

// The declarations only: the definitions are the `stb_vorbis` dependency target's.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>  // NOLINT(bugprone-suspicious-include): the library's own header form

#include <cstring>

namespace cy::audio::detail {
namespace {

constexpr usize kInitialArena = usize{128} * 1024U;
constexpr usize kMaximumArena = usize{8} * 1024U * 1024U;
/// Above what stb_vorbis reports needing: its allocator rounds each block up to eight bytes.
constexpr usize kArenaSlack = 1024U;
/// The input window a decode starts with. stb_vorbis needs a whole packet, plus the next page's
/// header when the packet continues onto it, so the window grows when a stream's packets are
/// larger.
constexpr usize kInputWindow = usize{16} * 1024U;
/// An Ogg page is at most 27 + 255 + 255 * 255 bytes: the last one starts within this of the end,
/// and a bisection interval this narrow is walked rather than halved.
constexpr u64 kPageSpan = u64{66} * 1024U;
constexpr usize kMaximumInput = usize{1024} * 1024U;
constexpr u64 kNoGranule = ~0ULL;
/// What the page scan reads at a time, on the stack.
constexpr usize kScanChunk = 4096;

[[nodiscard]] u32 ogg_crc_entry(u32 index) noexcept {
    u32 value = index << 24U;
    for (u32 bit = 0; bit < 8; ++bit) {
        value = ((value & 0x80000000U) != 0) ? ((value << 1U) ^ 0x04C11DB7U) : (value << 1U);
    }
    return value;
}

/// The Ogg page checksum, continued over `size` more bytes that sit at `position` within the page:
/// CRC-32 with the 0x04C11DB7 polynomial, unreflected, the checksum field (bytes 22-25) read as
/// zero. A format checksum, not a security primitive.
[[nodiscard]] u32 ogg_crc_update(u32 crc, const u8* bytes, usize size, u64 position) noexcept {
    static const auto table = [] {
        struct Table {
            u32 entries[256];
        } built{};
        for (u32 index = 0; index < 256; ++index) {
            built.entries[index] = ogg_crc_entry(index);
        }
        return built;
    }();
    for (usize index = 0; index < size; ++index) {
        const u64 at = position + index;
        const u8 byte = (at >= 22 && at < 26) ? 0 : bytes[index];
        crc = (crc << 8U) ^ table.entries[((crc >> 24U) ^ byte) & 0xFFU];
    }
    return crc;
}

[[nodiscard]] u64 read_le64(const u8* bytes) noexcept {
    u64 value = 0;
    for (u32 byte = 0; byte < 8; ++byte) {
        value |= static_cast<u64>(bytes[byte]) << (8U * byte);
    }
    return value;
}

struct PageInfo {
    u64 offset = 0;
    u64 length = 0;
    /// The granule position, or `kNoGranule` when no packet ends on the page.
    u64 granule = kNoGranule;
};

class VorbisDecoder final : public AudioDecoder {
public:
    VorbisDecoder(AudioByteSource& source, Allocator& allocator) noexcept
        : allocator_(&allocator), source_(&source), arena_(allocator), input_(allocator) {}

    VorbisDecoder(const VorbisDecoder&) = delete;
    VorbisDecoder& operator=(const VorbisDecoder&) = delete;
    VorbisDecoder(VorbisDecoder&&) = delete;
    VorbisDecoder& operator=(VorbisDecoder&&) = delete;

    ~VorbisDecoder() override { close(); }

    [[nodiscard]] Status open() noexcept {
        if (Status sized = arena_.allocate(kInitialArena); !sized) {
            return sized;
        }
        if (Status sized = input_.allocate(kInputWindow); !sized) {
            return sized;
        }
        if (Status opened = open_stream(); !opened) {
            return opened;
        }
        if (Status fitted = fit_arena(); !fitted) {
            return fitted;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
        format_.sample_rate = info.sample_rate;
        format_.channels = static_cast<u32>(info.channels);
        if (format_.channels == 0 || format_.channels > 2) {
            return fail(ErrorCode::Unsupported,
                        "the engine plays mono and stereo; this Vorbis stream has more channels");
        }
        long_block_ = static_cast<u64>(info.max_frame_size) * 2U;
        read_loop_comments();
        format_.frame_count = last_granule();
        return ok();
    }

    [[nodiscard]] DecodedFormat format() const noexcept override { return format_; }
    [[nodiscard]] SourceLoop source_loop() const noexcept override { return loop_; }

    [[nodiscard]] Expected<u32, Error> decode(f32* out, u32 frames) noexcept override {
        u32 produced = 0;
        while (produced < frames) {
            if (out_offset_ < out_count_) {
                produced += take(out + (static_cast<usize>(produced) * format_.channels),
                                 frames - produced);
                continue;
            }
            const Expected<Packet, Error> packet = next_packet();
            if (!packet) {
                return make_unexpected(packet.error());
            }
            if (*packet == Packet::End) {
                break;
            }
        }
        return produced;
    }

    [[nodiscard]] Status seek(u64 frame) noexcept override {
        if (format_.frame_count != 0 && frame > format_.frame_count) {
            return fail(ErrorCode::OutOfRange, "a Vorbis seek past the end of the stream");
        }
        u64 margin = long_block_ * 2U;
        for (u32 attempt = 0; attempt < 4 && frame > margin; ++attempt, margin *= 4U) {
            const PageInfo page = page_at_or_before(frame - margin);
            if (page.granule == kNoGranule) {
                break;
            }
            const Expected<bool, Error> landed = seek_from_page(page, frame);
            if (!landed) {
                return make_unexpected(landed.error());
            }
            if (*landed) {
                return ok();
            }
        }
        return seek_from_start(frame);
    }

    [[nodiscard]] usize resident_bytes() const noexcept override {
        return sizeof(*this) + arena_.bytes() + input_.bytes();
    }

    void destroy() noexcept override {
        Allocator* allocator = allocator_;
        this->~VorbisDecoder();
        allocator->deallocate(this, sizeof(VorbisDecoder), alignof(VorbisDecoder));
    }

private:
    enum class Packet : u8 { Frames, End };

    void close() noexcept {
        if (vorbis_ != nullptr) {
            stb_vorbis_close(vorbis_);
            vorbis_ = nullptr;
        }
    }

    void reset_input(u64 offset) noexcept {
        begin_ = 0;
        end_ = 0;
        consumed_ = offset;
        eof_ = false;
        out_count_ = 0;
        out_offset_ = 0;
    }

    /// Open (or reopen) at the start of the stream, growing the arena or the window when stb_vorbis
    /// says it needs more.
    [[nodiscard]] Status open_stream() noexcept {
        close();
        reset_input(0);
        for (;;) {
            stb_vorbis_alloc alloc;
            alloc.alloc_buffer = reinterpret_cast<char*>(arena_.data());
            alloc.alloc_buffer_length_in_bytes = static_cast<int>(arena_.size());
            int used = 0;
            int error = 0;
            vorbis_ = stb_vorbis_open_pushdata(
                input_.data() + begin_, static_cast<int>(end_ - begin_), &used, &error, &alloc);
            if (vorbis_ != nullptr) {
                begin_ += static_cast<usize>(used);
                consumed_ += static_cast<u64>(used);
                audio_start_ = consumed_;
                position_ = 0;
                return ok();
            }
            if (Status grown = grow_for(error); !grown) {
                return grown;
            }
        }
    }

    /// The arena grew by doubling until the headers fitted; now that stb_vorbis has said what it
    /// needs — its setup tables, plus the larger of setup's scratch and a packet's — cut the arena
    /// to that and open again, so a stream holds what its codebooks need and not the next power of
    /// two above it.
    [[nodiscard]] Status fit_arena() noexcept {
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
        const auto setup = static_cast<usize>(info.setup_memory_required);
        const auto scratch = static_cast<usize>(
            math::max(info.setup_temp_memory_required, info.temp_memory_required));
        const usize needed = ((setup + scratch + kArenaSlack) + 15U) & ~usize{15};
        if (needed >= arena_.size()) {
            return ok();
        }
        close();
        if (Status sized = replace_arena(needed); !sized) {
            return sized;
        }
        return open_stream();
    }

    /// A new arena of `bytes`, the old one released FIRST: a failed open leaves nothing in the
    /// arena worth keeping, and releasing before allocating is what keeps an open's peak at one
    /// arena rather than two.
    [[nodiscard]] Status replace_arena(usize bytes) noexcept { return arena_.allocate(bytes); }

    [[nodiscard]] Status grow_for(int error) noexcept {
        if (error == VORBIS_outofmem) {
            if (arena_.size() * 2U > kMaximumArena) {
                return fail(ErrorCode::OutOfMemory,
                            "this Vorbis stream needs more decoder memory than the engine allows");
            }
            return replace_arena(arena_.size() * 2U);
        }
        if (error == VORBIS_need_more_data) {
            const Expected<usize, Error> read = refill();
            if (!read) {
                return make_unexpected(read.error());
            }
            if (*read == 0) {
                return fail(ErrorCode::InvalidArgument, "a Vorbis stream ends inside its headers");
            }
            return ok();
        }
        return fail(ErrorCode::InvalidArgument, "not a Vorbis stream stb_vorbis can decode");
    }

    /// Move the unread bytes to the front of the window and read more behind them. Grows the window
    /// only when a single packet does not fit in it.
    [[nodiscard]] Expected<usize, Error> refill() noexcept {
        if (begin_ > 0) {
            std::memmove(input_.data(), input_.data() + begin_, end_ - begin_);
            end_ -= begin_;
            begin_ = 0;
        }
        if (end_ == input_.size()) {
            if (input_.size() * 2U > kMaximumInput) {
                return fail(ErrorCode::InvalidArgument, "a Vorbis packet larger than a megabyte");
            }
            if (Status grown = input_.grow(input_.size() * 2U); !grown) {
                return make_unexpected(grown.error());
            }
        }
        const Expected<usize, Error> read =
            source_->read(consumed_ + end_, Span<u8>(input_.data() + end_, input_.size() - end_));
        if (!read) {
            return read;
        }
        end_ += *read;
        eof_ = (*read == 0);
        return read;
    }

    /// Copy decoded frames out of the last packet, interleaving them.
    [[nodiscard]] u32 take(f32* out, u32 frames) noexcept {
        const auto count = static_cast<u32>(math::min<i64>(frames, out_count_ - out_offset_));
        for (u32 frame = 0; frame < count; ++frame) {
            for (u32 channel = 0; channel < format_.channels; ++channel) {
                out[(static_cast<usize>(frame) * format_.channels) + channel] =
                    outputs_[channel][out_offset_ + static_cast<i32>(frame)];
            }
        }
        out_offset_ += static_cast<i32>(count);
        position_ += count;
        return count;
    }

    /// Decode the next packet that has frames to deliver. During a seek, packets before the target
    /// (or of unknown position) are discarded here.
    [[nodiscard]] Expected<Packet, Error> next_packet() noexcept {
        for (;;) {
            int channels = 0;
            float** outputs = nullptr;
            int samples = 0;
            const int used = stb_vorbis_decode_frame_pushdata(vorbis_, input_.data() + begin_,
                                                              static_cast<int>(end_ - begin_),
                                                              &channels, &outputs, &samples);
            begin_ += static_cast<usize>(used);
            consumed_ += static_cast<u64>(used);
            if (samples > 0) {
                if (accept(outputs, samples)) {
                    return Packet::Frames;
                }
                if (overshot_) {
                    return Packet::End;
                }
                continue;
            }
            if (used > 0) {
                continue;  // a page header, or a packet that only primed the overlap
            }
            if (eof_) {
                return Packet::End;
            }
            const Expected<usize, Error> read = refill();
            if (!read) {
                return make_unexpected(read.error());
            }
            if (*read == 0 && begin_ == end_) {
                return Packet::End;
            }
        }
    }

    /// Whether a decoded packet delivers frames, and from where. Outside a seek, all of it.
    [[nodiscard]] bool accept(float** outputs, int samples) noexcept {
        outputs_ = outputs;
        out_count_ = samples;
        out_offset_ = 0;
        if (!seeking_) {
            return true;
        }
        const int end = stb_vorbis_get_sample_offset(vorbis_);
        if (end < 0) {
            return false;  // no position yet: discard
        }
        const u64 last = static_cast<u64>(end);
        const u64 first = last - static_cast<u64>(samples);
        if (last <= target_) {
            return false;  // entirely before the target
        }
        if (first > target_) {
            overshot_ = true;  // the first known position is already past: back off further
            return false;
        }
        out_offset_ = static_cast<i32>(target_ - first);
        position_ = target_;
        seeking_ = false;
        return true;
    }

    /// Seek by resynchronising on `page`. False when the first known position overshot `frame`.
    [[nodiscard]] Expected<bool, Error> seek_from_page(const PageInfo& page, u64 frame) noexcept {
        stb_vorbis_flush_pushdata(vorbis_);
        reset_input(page.offset);
        seeking_ = true;
        overshot_ = false;
        target_ = frame;
        const Expected<Packet, Error> packet = next_packet();
        seeking_ = false;
        if (!packet) {
            return make_unexpected(packet.error());
        }
        if (overshot_) {
            return false;
        }
        if (*packet == Packet::End) {
            return fail(ErrorCode::OutOfRange, "a Vorbis seek ran off the end of the stream");
        }
        return true;
    }

    [[nodiscard]] Status seek_from_start(u64 frame) noexcept {
        if (Status reopened = open_stream(); !reopened) {
            return reopened;
        }
        f32 discard[1024 * 2];
        while (position_ < frame) {
            const auto want = static_cast<u32>(math::min<u64>(frame - position_, 1024));
            const Expected<u32, Error> got = decode(discard, want);
            if (!got) {
                return make_unexpected(got.error());
            }
            if (*got == 0) {
                return fail(ErrorCode::OutOfRange, "a Vorbis seek past the end of the stream");
            }
        }
        return ok();
    }

    // --- Ogg pages, for the bisection ------------------------------------------------------------

    /// The first page that starts at or after `offset`, checksum verified, or an empty one. Scans
    /// through a small stack buffer: the input window holds unread packets and must not be touched.
    [[nodiscard]] PageInfo page_after(u64 offset) const noexcept {
        const u64 size = source_->size();
        u8 chunk[kScanChunk];
        while (offset + 27 <= size) {
            const Expected<usize, Error> read = source_->read(offset, Span<u8>(chunk, kScanChunk));
            if (!read || *read < 27) {
                return {};
            }
            const usize window = *read;
            for (usize at = 0; at + 4 <= window; ++at) {
                if (std::memcmp(chunk + at, "OggS", 4) != 0) {
                    continue;
                }
                const PageInfo page = verify_page(offset + at);
                if (page.length != 0) {
                    return page;
                }
            }
            offset += window - 3;  // a capture pattern can straddle the chunk's end
        }
        return {};
    }

    /// The page at `offset`, if its header and checksum hold. The checksum is computed as the page
    /// streams through a small buffer, so a 64 KiB page costs no 64 KiB anywhere.
    [[nodiscard]] PageInfo verify_page(u64 offset) const noexcept {
        u8 header[27 + 255];
        const Expected<usize, Error> head = source_->read(offset, Span<u8>(header, 27));
        if (!head || *head != 27 || header[4] != 0) {
            return {};
        }
        const usize segments = header[26];
        const Expected<usize, Error> table =
            source_->read(offset + 27, Span<u8>(header + 27, segments));
        if (!table || *table != segments) {
            return {};
        }
        u64 body = 0;
        for (usize index = 0; index < segments; ++index) {
            body += header[27 + index];
        }
        u32 crc = ogg_crc_update(0, header, 27 + segments, 0);
        u8 chunk[kScanChunk];
        u64 done = 0;
        while (done < body) {
            const auto want = static_cast<usize>(math::min<u64>(body - done, kScanChunk));
            const Expected<usize, Error> read =
                source_->read(offset + 27 + segments + done, Span<u8>(chunk, want));
            if (!read || *read != want) {
                return {};
            }
            crc = ogg_crc_update(crc, chunk, want, 27 + segments + done);
            done += want;
        }
        const u32 stored = static_cast<u32>(header[22]) | (static_cast<u32>(header[23]) << 8U) |
                           (static_cast<u32>(header[24]) << 16U) |
                           (static_cast<u32>(header[25]) << 24U);
        if (crc != stored) {
            return {};
        }
        PageInfo info;
        info.offset = offset;
        info.length = 27 + segments + body;
        const u64 granule = read_le64(header + 6);
        info.granule = (granule == ~0ULL) ? kNoGranule : granule;
        return info;
    }

    /// The first page at or after `offset` that has a granule position.
    [[nodiscard]] PageInfo timed_page_after(u64 offset) const noexcept {
        PageInfo page = page_after(offset);
        while (page.length != 0 && page.granule == kNoGranule) {
            page = page_after(page.offset + page.length);
        }
        return page;
    }

    /// The last page whose granule position is at most `bound`, by bisection then a short walk.
    [[nodiscard]] PageInfo page_at_or_before(u64 bound) const noexcept {
        PageInfo best;
        u64 low = audio_start_;
        u64 high = source_->size();
        while (high - low > kPageSpan) {
            const u64 middle = low + ((high - low) / 2U);
            const PageInfo page = timed_page_after(middle);
            if (page.length == 0 || page.offset >= high || page.granule > bound) {
                high = middle;
                continue;
            }
            best = page;
            low = page.offset + page.length;
        }
        PageInfo walk = timed_page_after((best.length != 0) ? best.offset : audio_start_);
        while (walk.length != 0 && walk.granule <= bound) {
            best = walk;
            walk = timed_page_after(walk.offset + walk.length);
        }
        return best;
    }

    /// The last page's granule position: the stream's length in frames.
    [[nodiscard]] u64 last_granule() const noexcept {
        const u64 size = source_->size();
        const u64 from = (size > kPageSpan) ? size - kPageSpan : audio_start_;
        u64 last = 0;
        PageInfo page = page_after(from);
        while (page.length != 0) {
            if (page.granule != kNoGranule) {
                last = page.granule;
            }
            page = page_after(page.offset + page.length);
        }
        return last;
    }

    void read_loop_comments() noexcept {
        const stb_vorbis_comment comments = stb_vorbis_get_comment(vorbis_);
        LoopComments loop;
        for (int index = 0; index < comments.comment_list_length; ++index) {
            loop.observe(comments.comment_list[index]);
        }
        loop_ = loop.loop();
    }

    Allocator* allocator_;
    AudioByteSource* source_;
    AudioBuffer<u8> arena_;
    AudioBuffer<u8> input_;
    stb_vorbis* vorbis_ = nullptr;
    usize begin_ = 0;
    usize end_ = 0;
    /// The source offset of `input_[begin_]`.
    u64 consumed_ = 0;
    u64 audio_start_ = 0;
    bool eof_ = false;
    u64 long_block_ = 4096;
    DecodedFormat format_;
    SourceLoop loop_;
    float** outputs_ = nullptr;
    i32 out_count_ = 0;
    i32 out_offset_ = 0;
    u64 position_ = 0;
    bool seeking_ = false;
    bool overshot_ = false;
    u64 target_ = 0;
};

}  // namespace

Expected<DecoderPtr, Error> open_vorbis(AudioByteSource& source, Allocator& allocator) noexcept {
    void* storage = allocator.allocate(sizeof(VorbisDecoder), alignof(VorbisDecoder));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no memory for a Vorbis decoder");
    }
    auto* decoder = construct_at<VorbisDecoder>(storage, source, allocator);
    DecoderPtr owned(decoder);
    if (Status opened = decoder->open(); !opened) {
        return make_unexpected(opened.error());
    }
    return owned;
}

}  // namespace cy::audio::detail
