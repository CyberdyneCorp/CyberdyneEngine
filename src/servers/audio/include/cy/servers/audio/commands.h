#pragma once
// The lock-free hand-off between the game thread and the audio thread. Task 4.3.5.
//
// `audio` — "Thread-safe state handoff": "Gameplay and audio threads SHALL exchange state without
// locks in the mixer: commands SHALL be enqueued to a lock-free queue consumed at the start of each
// callback, and voice state SHALL be double buffered."
//
// ================================================================================================
// WHY A QUEUE AND NOT A MUTEX
// ================================================================================================
//
// A mutex held by the game thread while the audio callback wants it does not make the audio late,
// it makes it ABSENT: the device's deadline passes, the driver plays whatever was in the buffer,
// and the result is a click on every contended frame. There is no acceptable "occasionally" for a
// realtime thread, so the mixer takes no lock at all — the requirement says so in as many words and
// this file is how it is kept.
//
// SINGLE PRODUCER, SINGLE CONSUMER, and both halves say so. The game thread produces; the audio
// thread consumes. A second producer would need a compare-and-swap on the write index, which is a
// different queue with a different cost, and pretending one queue serves both cases is how a
// multi-threaded producer is added later without anybody noticing the assumption it broke. Gameplay
// that wants to play a sound from a worker posts through the game thread, which is where every
// other server's mutation already goes.
//
// ================================================================================================
// A COMMAND IS A POD AND STAYS ONE
// ================================================================================================
//
// No pointer that could dangle, no allocation, no destructor. A command sits in a ring buffer for
// as long as the audio thread takes to notice it, and anything in it that needed freeing would need
// freeing on whichever thread got there second.
//
// A FULL QUEUE IS REPORTED, NOT DROPPED SILENTLY. `push()` returns false, the server counts it, and
// the diagnostics show it — a queue that silently drops a `Stop` leaves a sound playing forever and
// gives nobody a reason.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/servers/audio/handles.h>

#include <atomic>

namespace cy::audio {

/// What the game thread asks the mixer to do.
enum class CommandKind : u8 {
    None = 0,
    Play,
    Stop,
    Pause,
    Resume,
    Seek,
    SetVolume,
    SetPitch,
    SetPosition,
    SetOcclusion,
    SetBusVolume,
    Count,
};

[[nodiscard]] const char* command_kind_name(CommandKind kind) noexcept;

/// One command. Trivially copyable, 48 bytes, no pointer.
struct AudioCommand {
    CommandKind kind = CommandKind::None;
    VoiceHandle voice;
    BusHandle bus;
    /// Interpretation depends on `kind`: the volume, the pitch, the occlusion, the seek position in
    /// frames. One field rather than a union, because a union of four floats is four floats and the
    /// name would have to be read from the kind either way.
    f32 scalar = 0.0F;
    Vec3 vector{0.0F, 0.0F, 0.0F};
    Vec3 secondary{0.0F, 0.0F, 0.0F};
};

static_assert(std::is_trivially_copyable_v<AudioCommand>,
              "a command crosses a thread boundary in a ring buffer and must be a POD");

/// A single-producer, single-consumer ring of commands.
///
/// `Capacity` is a power of two so the index wrap is a mask. Sized at construction rather than
/// grown: growing would allocate, and the thread that discovered it was full is the one that must
/// not.
template <u32 Capacity>
class CommandQueue {
public:
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "CommandQueue's capacity is a power of two so the wrap is a mask");

    /// Game thread only. False when the queue is full — reported, never dropped silently.
    [[nodiscard]] bool push(const AudioCommand& command) noexcept {
        const u32 write = write_.load(std::memory_order_relaxed);
        const u32 next = (write + 1U) & kMask;
        // acquire on the read index: everything the consumer released before advancing it is
        // visible here, which is what makes the slot safe to overwrite.
        if (next == read_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[write] = command;
        // release: the slot's contents are published before the index that exposes them. Without
        // this the consumer can legally see the new index and the old slot.
        write_.store(next, std::memory_order_release);
        return true;
    }

    /// Audio thread only. False when empty.
    [[nodiscard]] bool pop(AudioCommand& out) noexcept {
        const u32 read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[read];
        read_.store((read + 1U) & kMask, std::memory_order_release);
        return true;
    }

    /// Approximate, and only ever used by a diagnostic. Two relaxed loads of indices that another
    /// thread is moving cannot be a consistent pair, and a count that is one out in a report is
    /// worth less than a fence on the realtime path.
    [[nodiscard]] u32 approximate_size() const noexcept {
        const u32 write = write_.load(std::memory_order_relaxed);
        const u32 read = read_.load(std::memory_order_relaxed);
        return (write - read) & kMask;
    }

    [[nodiscard]] static constexpr u32 capacity() noexcept { return Capacity - 1U; }

private:
    static constexpr u32 kMask = Capacity - 1U;

    AudioCommand slots_[Capacity];
    std::atomic<u32> write_{0};
    std::atomic<u32> read_{0};
};

}  // namespace cy::audio
