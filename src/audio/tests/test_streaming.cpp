// SPDX-License-Identifier: MIT
// integration.audio_streaming: the stream player over the audio server and the null backend —
// starvation, underrun, seek, crossfade, loop, memory, and refills on the job system. Issue #87.
//
// Every track is cooked PCM GENERATED ON DEMAND by `ProceduralTrack`: a byte source that computes
// whichever bytes are asked for, so a ten-minute track has no ten-minute buffer anywhere and every
// frame the mixer produces says which source frame it was. Codecs are integration.audio_codecs'
// subject; this suite is the policy above them, and runs in a build with CY_AUDIO off.

#include <cy/audio/streaming.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>
#include <cy/servers/audio/server.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace cy;
using namespace cy::audio;

namespace {

[[nodiscard]] Allocator& heap() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

constexpr u32 kRate = 48000;
constexpr u32 kBuffer = 480;  // a 10 ms device buffer

/// `count` seconds of frames at the mix rate.
[[nodiscard]] constexpr u64 seconds(u64 count) noexcept {
    return count * kRate;
}

/// What a procedural track's frames are.
enum class Shape : u8 {
    /// Left is frame % 32768 - 16384, right its negation, as 16-bit samples: a frame read anywhere
    /// says where it came from.
    Ramp,
    /// Left at half scale, right silent.
    LeftOnly,
    /// Right at half scale, left silent.
    RightOnly,
};

[[nodiscard]] i16 ramp_sample(u64 frame) noexcept {
    return static_cast<i16>(static_cast<i32>(frame % 32768U) - 16384);
}

[[nodiscard]] f32 ramp_value(u64 frame) noexcept {
    return static_cast<f32>(ramp_sample(frame)) / 32768.0F;
}

/// A cooked PCM track whose bytes are computed when they are read.
class ProceduralTrack final : public AudioByteSource {
public:
    ProceduralTrack(Shape shape, u64 frames, AudioLoadMode mode = AudioLoadMode::Compressed,
                    bool looping = false, u64 loop_start = 0, u64 loop_end = 0)
        : shape_(shape) {
        header_.codec = AudioCodec::Pcm16;
        header_.load_mode = mode;
        header_.channels = 2;
        header_.sample_rate = kRate;
        header_.frame_count = frames;
        header_.looping = looping;
        header_.loop_start = loop_start;
        header_.loop_end = loop_end;
        header_.payload_size = frames * 4;
        CY_REQUIRE(
            write_cooked_audio_header(header_, Span<u8>(bytes_, sizeof(bytes_))).has_value());
    }

    [[nodiscard]] u64 size() const noexcept override {
        return kCookedAudioHeaderBytes + header_.payload_size;
    }

    [[nodiscard]] Expected<usize, Error> read(u64 offset, Span<u8> destination) noexcept override {
        usize written = 0;
        for (; written < destination.size() && offset + written < size(); ++written) {
            destination[written] = byte_at(offset + written);
        }
        return written;
    }

private:
    [[nodiscard]] u8 byte_at(u64 offset) const noexcept {
        if (offset < kCookedAudioHeaderBytes) {
            return bytes_[offset];
        }
        const u64 sample = (offset - kCookedAudioHeaderBytes) / 2;
        const u64 frame = sample / 2;
        const bool right = (sample % 2) == 1;
        i16 value = 0;
        switch (shape_) {
            case Shape::Ramp:
                value = right ? static_cast<i16>(-ramp_sample(frame)) : ramp_sample(frame);
                break;
            case Shape::LeftOnly:
                value = right ? i16{0} : i16{16384};
                break;
            case Shape::RightOnly:
                value = right ? i16{16384} : i16{0};
                break;
        }
        const auto bits = static_cast<u16>(value);
        return ((offset - kCookedAudioHeaderBytes) % 2 == 0) ? static_cast<u8>(bits & 0xFFU)
                                                             : static_cast<u8>(bits >> 8U);
    }

    Shape shape_;
    CookedAudioHeader header_;
    u8 bytes_[kCookedAudioHeaderBytes] = {};
};

/// Holds every refill until the case says to run them: a job system starved for as long as the
/// case wants.
class HeldExecutor final : public RefillExecutor {
public:
    [[nodiscard]] Status submit(Work work, void* user, bool reads_storage) noexcept override {
        (void)reads_storage;
        held_.push_back({work, user});
        return ok();
    }

    void run_all() {
        std::vector<Task> tasks;
        tasks.swap(held_);
        for (const Task& task : tasks) {
            task.work(task.user);
        }
    }

private:
    struct Task {
        Work work;
        void* user;
    };
    std::vector<Task> held_;
};

/// A server over the null backend and a player over it.
struct Rig {
    explicit Rig(RefillExecutor& executor, Allocator& player_allocator = heap(),
                 StreamPlayerConfig player_config = {}) {
        AudioBackendConfig backend_config;
        backend_config.requested.sample_rate = kRate;
        backend_config.requested.buffer_frames = kBuffer;
        CY_REQUIRE(backend.initialize(backend_config).has_value());
        AudioServerConfig config;
        config.requested = backend_config.requested;
        config.block_frames = 240;
        CY_REQUIRE(server.configure(config).has_value());
        CY_REQUIRE(server.initialize_with(backend).has_value());
        player = std::make_unique<StreamPlayer>(server, executor, player_allocator);
        CY_REQUIRE(player->configure(player_config).has_value());
        output.resize(static_cast<usize>(kBuffer) * 2);
    }

    void add(const char* name, ProceduralTrack& track) const {
        const Expected<TrackSource, Error> source = track_from_cooked(track);
        CY_REQUIRE(source.has_value());
        CY_REQUIRE(player->add_track(Name::intern(name), *source).has_value());
    }

    /// The last buffer's left and right samples at `frame`.
    [[nodiscard]] f32 left(u32 frame) const { return output[static_cast<usize>(frame) * 2]; }
    [[nodiscard]] f32 right(u32 frame) const { return output[(static_cast<usize>(frame) * 2) + 1]; }

    /// One frame of the game: pump, then the device takes one buffer.
    void frame() {
        player->pump();
        CY_REQUIRE_EQ(backend.advance(output.data(), kBuffer), kBuffer);
        server.update(0.01F);
    }

    NullAudioBackend backend;
    AudioServer server{heap()};
    std::unique_ptr<StreamPlayer> player;
    std::vector<f32> output;
};

}  // namespace

// --- The buffer guarantee ------------------------------------------------------------------------

CY_TEST_CASE("streaming: refills starved for just under the ring's length never underrun") {
    // The ring holds 0.5 s (24 000 frames). Refills run only every 45 buffers — 21 600 frames,
    // 0.45 s of starvation each time — for ten seconds. The guarantee: no underrun at all.
    HeldExecutor executor;
    Rig rig(executor);
    ProceduralTrack track(Shape::Ramp, seconds(20));
    rig.add("ramp", track);
    const Expected<StreamId, Error> stream = rig.player->play(Name::intern("ramp"), {});
    CY_REQUIRE(stream.has_value());
    executor.run_all();  // the first refill opens and primes

    u64 expected = 0;
    u64 wrong = 0;
    bool sounding = false;
    for (u32 buffer = 0; buffer < 1000; ++buffer) {
        rig.frame();
        if (buffer % 45 == 44) {
            executor.run_all();
        }
        // Every frame that sounds is the next one in the track.
        for (u32 frame = 0; frame < kBuffer; ++frame) {
            const f32 left = rig.left(frame);
            if (!sounding && left == 0.0F && expected == 0) {
                continue;  // still priming
            }
            sounding = true;
            if (left != ramp_value(expected) && wrong == 0) {
                wrong = expected + 1;
                CY_TEST_INFO("frame " << expected << " in buffer " << buffer);
            }
            ++expected;
        }
    }
    const StreamReport report = rig.player->report(*stream);
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK_EQ(report.underruns, 0U);
    CY_CHECK_GT(expected, static_cast<u64>(kRate) * 9U);
    CY_CHECK(rig.player->diagnostics().empty());
}

CY_TEST_CASE("streaming: starved past the ring, a stream plays silence, says so, and recovers") {
    HeldExecutor executor;
    Rig rig(executor);
    ProceduralTrack track(Shape::Ramp, seconds(20));
    rig.add("ramp", track);
    const Expected<StreamId, Error> stream = rig.player->play(Name::intern("ramp"), {});
    CY_REQUIRE(stream.has_value());
    executor.run_all();

    // Sixty buffers between refills: 28 800 frames against a 24 000-frame ring.
    u64 silent_frames = 0;
    for (u32 buffer = 0; buffer < 240; ++buffer) {
        rig.frame();
        if (buffer > 30) {
            for (u32 frame = 0; frame < kBuffer; ++frame) {
                silent_frames += (rig.left(frame) == 0.0F && rig.right(frame) == 0.0F) ? 1U : 0U;
            }
        }
        if (buffer % 60 == 59) {
            executor.run_all();
        }
    }
    const StreamReport starved = rig.player->report(*stream);
    CY_CHECK_GT(starved.underruns, 0U);
    CY_CHECK_GT(starved.underrun_frames, 0U);
    CY_CHECK_GT(silent_frames, 0U);
    // Checked before it is read: a REQUIRE does not stop a case in this build (no exceptions).
    const bool diagnosed = !rig.player->diagnostics().empty();
    CY_CHECK(diagnosed);
    if (diagnosed) {
        CY_CHECK(std::strcmp(rig.player->diagnostics()[0].code, "audio.stream.underrun") == 0);
        CY_CHECK(rig.player->diagnostics()[0].track == Name::intern("ramp"));
    }
    CY_CHECK_GT(rig.server.statistics().stream_underruns, 0U);

    // Fed every frame again, it plays on with no further underrun: silence, never a stall. (The
    // pump first reports what the last buffer counted, then the slate is wiped.)
    rig.player->pump();
    executor.run_all();
    const StreamReport reported = rig.player->report(*stream);
    rig.player->clear_diagnostics();
    for (u32 buffer = 0; buffer < 100; ++buffer) {
        rig.frame();
        executor.run_all();
    }
    CY_CHECK_EQ(rig.player->report(*stream).underruns, reported.underruns);
    CY_CHECK(rig.player->diagnostics().empty());
    CY_CHECK(rig.player->playing(*stream));
}

// --- Seek, loop, crossfade -----------------------------------------------------------------------

CY_TEST_CASE("streaming: a seek lands on the frame it names") {
    InlineRefillExecutor executor;
    Rig rig(executor);
    ProceduralTrack track(Shape::Ramp, seconds(10));
    rig.add("ramp", track);
    const Expected<StreamId, Error> stream = rig.player->play(Name::intern("ramp"), {});
    CY_REQUIRE(stream.has_value());
    for (u32 buffer = 0; buffer < 50; ++buffer) {
        rig.frame();
    }
    constexpr f64 kTarget = 2.5;
    constexpr u64 kTargetFrame = 120000;
    CY_REQUIRE(rig.player->seek(*stream, kTarget).has_value());

    // The flush fades the old position out over one mixer block and the ring re-primes. The first
    // block at the new position ramps in; every frame after it is exactly the track at the target.
    std::vector<f32> heard;
    for (u32 buffer = 0; buffer < 40; ++buffer) {
        rig.frame();
        for (u32 frame = 0; frame < kBuffer; ++frame) {
            heard.push_back(rig.left(frame));
        }
    }
    // Where the ramp-in block begins: the frame a block before the first exact target frame.
    usize ramp = heard.size();
    for (usize at = 0; at + 241 < heard.size(); ++at) {
        if (heard[at + 240] == ramp_value(kTargetFrame + 240) &&
            heard[at + 241] == ramp_value(kTargetFrame + 241)) {
            ramp = at;
            break;
        }
    }
    const bool resumed = ramp + 240 + 4000 < heard.size();
    usize wrong = 0;
    for (usize k = 0; resumed && k < 4000 && wrong == 0; ++k) {
        wrong = (heard[ramp + 240 + k] == ramp_value(kTargetFrame + 240 + k)) ? 0 : k + 1;
    }
    CY_CHECK_EQ(wrong, 0U);
    // The ramp-in block is the target's own frames, faded in over the block.
    if (resumed) {
        CY_CHECK_NEAR(heard[ramp + 120], ramp_value(kTargetFrame + 120) * (121.0F / 240.0F), 1e-6F);
    }
    CY_CHECK(resumed);
    const StreamReport report = rig.player->report(*stream);
    CY_CHECK_GE(report.position_seconds, kTarget);
    CY_CHECK_LT(report.position_seconds, kTarget + 0.5);
    CY_CHECK_EQ(report.underruns, 0U);
}

CY_TEST_CASE("streaming: a seek before the stream sounds starts it at the target") {
    // The refills are held, so the seek arrives while the first one — from frame zero — is still
    // waiting. The stream must start at the target anyway, with nothing of frame zero in the ring.
    HeldExecutor executor;
    Rig rig(executor);
    ProceduralTrack track(Shape::Ramp, seconds(10));
    rig.add("ramp", track);
    const Expected<StreamId, Error> stream = rig.player->play(Name::intern("ramp"), {});
    CY_REQUIRE(stream.has_value());
    CY_REQUIRE(rig.player->seek(*stream, 2.0).has_value());
    std::vector<f32> heard;
    for (u32 buffer = 0; buffer < 20; ++buffer) {
        executor.run_all();
        rig.frame();
        for (u32 frame = 0; frame < kBuffer; ++frame) {
            heard.push_back(rig.left(frame));
        }
    }
    usize first = 0;
    while (first < heard.size() && heard[first] == 0.0F) {
        ++first;
    }
    CY_REQUIRE(first + 1000 < heard.size());
    usize wrong = 0;
    for (usize k = 0; k < 1000 && wrong == 0; ++k) {
        wrong = (heard[first + k] == ramp_value(96000 + k)) ? 0 : k + 1;
    }
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK_EQ(rig.player->report(*stream).underruns, 0U);
}

CY_TEST_CASE("streaming: a loop is crossed again and again with no frame lost or repeated") {
    InlineRefillExecutor executor;
    Rig rig(executor);
    ProceduralTrack track(Shape::Ramp, 40000, AudioLoadMode::Compressed, true, 9000, 30000);
    rig.add("loop", track);
    const Expected<StreamId, Error> stream = rig.player->play(Name::intern("loop"), {});
    CY_REQUIRE(stream.has_value());
    u64 played = 0;
    u64 wrong = 0;
    bool started = false;
    for (u32 buffer = 0; buffer < 400; ++buffer) {
        rig.frame();
        for (u32 frame = 0; frame < kBuffer; ++frame) {
            if (!started && rig.left(frame) == 0.0F && played == 0) {
                continue;
            }
            started = true;
            const u64 source = (played < 30000) ? played : 9000 + ((played - 30000) % 21000);
            if (rig.left(frame) != ramp_value(source) && wrong == 0) {
                wrong = played + 1;
                CY_TEST_INFO("played frame " << played << " should be source frame " << source);
            }
            ++played;
        }
    }
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK_GT(played, 150000U);  // seven laps
    CY_CHECK_EQ(rig.player->report(*stream).underruns, 0U);
}

CY_TEST_CASE("streaming: a crossfade is equal-power and both ramps start in the same block") {
    InlineRefillExecutor executor;
    Rig rig(executor);
    ProceduralTrack left(Shape::LeftOnly, seconds(10));
    ProceduralTrack right(Shape::RightOnly, seconds(10));
    rig.add("left", left);
    rig.add("right", right);
    const Expected<StreamId, Error> first = rig.player->play(Name::intern("left"), {});
    CY_REQUIRE(first.has_value());
    for (u32 buffer = 0; buffer < 20; ++buffer) {
        rig.frame();
    }
    CY_REQUIRE_EQ(rig.output[0], 0.5F);

    constexpr f32 kSeconds = 0.5F;
    const Expected<StreamId, Error> second =
        rig.player->crossfade(*first, Name::intern("right"), kSeconds, {});
    CY_REQUIRE(second.has_value());
    // Collect the whole ramp: 24 000 frames, plus the buffers before it begins.
    std::vector<f32> left_gain;
    std::vector<f32> right_gain;
    for (u32 buffer = 0; buffer < 80; ++buffer) {
        rig.frame();
        for (u32 frame = 0; frame < kBuffer; ++frame) {
            left_gain.push_back(rig.left(frame) / 0.5F);
            right_gain.push_back(rig.right(frame) / 0.5F);
        }
    }
    // The ramp begins on the first frame the incoming track sounds, and on that same frame the
    // outgoing one starts falling: a frame whose powers do not sum to one would be a misalignment.
    usize begin = 0;
    bool held = true;
    while (begin < right_gain.size() && right_gain[begin] == 0.0F) {
        held = held && left_gain[begin] == 1.0F;
        ++begin;
    }
    CY_CHECK(held);  // the outgoing track at full gain until the incoming one sounds
    CY_REQUIRE(begin + 24000 < right_gain.size());
    usize wrong = 0;
    const usize ramp_end = std::min(begin + 24000, right_gain.size());
    for (usize frame = begin; frame < ramp_end && wrong == 0; ++frame) {
        const f32 t = static_cast<f32>(frame - begin + 1) / 24000.0F;
        const bool on_curve =
            std::fabs(left_gain[frame] - std::cos(t * 0.5F * math::kPi)) < 1e-4F &&
            std::fabs(right_gain[frame] - std::sin(t * 0.5F * math::kPi)) < 1e-4F &&
            std::fabs((left_gain[frame] * left_gain[frame]) +
                      (right_gain[frame] * right_gain[frame]) - 1.0F) < 1e-4F;
        if (!on_curve) {
            wrong = frame - begin + 1;
            CY_TEST_INFO("ramp frame " << (frame - begin) << ": outgoing " << left_gain[frame]
                                       << ", incoming " << right_gain[frame]);
        }
    }
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK_FALSE(rig.player->playing(*first));
    CY_CHECK(rig.player->playing(*second));
    CY_CHECK_FALSE(rig.player->report(*first).live);  // released once its fade ended
}

// --- Memory, jobs, refusals ----------------------------------------------------------------------

CY_TEST_CASE("streaming: a ten-minute track streams in the memory a one-minute track needs") {
    const auto peak_for = [](u64 minutes) {
        TrackingAllocator tracker(heap(), MemoryDomain::Audio, "stream");
        InlineRefillExecutor executor;
        StreamPlayerConfig config;
        config.capacity = 1;
        Rig rig(executor, tracker, config);
        ProceduralTrack track(Shape::Ramp, seconds(60 * minutes), AudioLoadMode::Streamed);
        rig.add("long", track);
        const Expected<StreamId, Error> stream = rig.player->play(Name::intern("long"), {});
        CY_REQUIRE(stream.has_value());
        // Play the opening twenty seconds, then seek near the end and play it out.
        for (u32 buffer = 0; buffer < 2000; ++buffer) {
            rig.frame();
        }
        CY_REQUIRE(rig.player->seek(*stream, static_cast<f64>(minutes * 60) - 10.0).has_value());
        for (u32 buffer = 0; buffer < 1100 && rig.player->playing(*stream); ++buffer) {
            rig.frame();
        }
        CY_CHECK_FALSE(rig.player->playing(*stream));  // it reached the end and released itself
        return tracker.peak_bytes();
    };
    const u64 one = peak_for(1);
    const u64 ten = peak_for(10);
    CY_TEST_MESSAGE("a streamed PCM track's peak: " << ten << " bytes for ten minutes, " << one
                                                    << " for one");
    CY_CHECK_EQ(ten, one);
    CY_CHECK_LT(ten, 512U * 1024U);
}

CY_TEST_CASE("streaming: refills run as jobs, storage reads on the async service, none blocking") {
    jobs::JobSystem jobs;
    jobs::JobSystemConfig jobs_config;
    jobs_config.worker_count = 2;
    CY_REQUIRE(jobs.start(jobs_config).has_value());
    jobs::AsyncService async;
    CY_REQUIRE(async.start(jobs).has_value());
    const u64 violations_before = jobs::blocking_violations();
    {
        JobRefillExecutor executor(jobs, async);
        Rig rig(executor);
        ProceduralTrack streamed(Shape::Ramp, seconds(10), AudioLoadMode::Streamed);
        ProceduralTrack in_memory(Shape::Ramp, seconds(10), AudioLoadMode::Compressed);
        rig.add("disk", streamed);
        rig.add("memory", in_memory);
        const Expected<StreamId, Error> a = rig.player->play(Name::intern("disk"), {});
        const Expected<StreamId, Error> b = rig.player->play(Name::intern("memory"), {});
        CY_REQUIRE(a.has_value());
        CY_REQUIRE(b.has_value());
        for (u32 buffer = 0; buffer < 200; ++buffer) {
            rig.player->pump();
            // A real frame does not wait for its refills; this one does so the case is
            // deterministic about what was mixed, not about which thread ran what.
            while (rig.player->refills_in_flight() != 0) {
                jobs.wait_for_idle();
            }
            CY_REQUIRE_EQ(rig.backend.advance(rig.output.data(), kBuffer), kBuffer);
            rig.server.update(0.01F);
        }
        CY_CHECK_EQ(rig.player->report(*a).underruns, 0U);
        CY_CHECK_EQ(rig.player->report(*b).underruns, 0U);
        CY_CHECK_GT(rig.player->report(*a).refills, 10U);
        CY_CHECK_GT(async.operations_completed(), 10U);  // the storage-reading stream's refills
        CY_CHECK_EQ(rig.player->refills_in_flight(), 0U);
    }
    CY_CHECK_EQ(jobs::blocking_violations(), violations_before);
    async.stop();
    jobs.shutdown();
}

CY_TEST_CASE("streaming: an unknown track, a full player and an undecodable codec are refused") {
    InlineRefillExecutor executor;
    StreamPlayerConfig config;
    config.capacity = 1;
    Rig rig(executor, heap(), config);
    ProceduralTrack track(Shape::Ramp, kRate);
    rig.add("ramp", track);
    CY_CHECK(rig.player->play(Name::intern("missing"), {}).error().code == ErrorCode::NotFound);
    CY_REQUIRE(rig.player->play(Name::intern("ramp"), {}).has_value());
    CY_CHECK(rig.player->play(Name::intern("ramp"), {}).error().code == ErrorCode::OutOfRange);

    // A Vorbis track in a player with only the built-in codecs: refused at play, with the reason.
    TrackSource vorbis;
    vorbis.header.codec = AudioCodec::Vorbis;
    vorbis.header.load_mode = AudioLoadMode::Streamed;
    vorbis.header.channels = 2;
    vorbis.header.sample_rate = 44100;
    vorbis.header.frame_count = 1000;
    vorbis.asset = &track;
    CY_REQUIRE(rig.player->add_track(Name::intern("vorbis"), vorbis).has_value());
    StreamPlayerConfig roomy;
    InlineRefillExecutor other;
    Rig wide(other, heap(), roomy);
    CY_REQUIRE(wide.player->add_track(Name::intern("vorbis"), vorbis).has_value());
    CY_CHECK(wide.player->play(Name::intern("vorbis"), {}).error().code == ErrorCode::Unavailable);
}
