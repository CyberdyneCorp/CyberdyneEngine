#pragma once
// A sequence is a command producer. M8.c task 3.4.
//
// ================================================================================================
// THE SIMULATION CANNOT TELL, AND THAT IS THE REQUIREMENT
// ================================================================================================
//
// `sequencing-and-cinematics`: "Commands emitted by a sequence SHALL be indistinguishable to the
// simulation from commands emitted by any other producer, and SHALL be validated identically."
// `gameplay-framework` says the same thing from the other side: "Provenance SHALL NOT affect
// validation, ordering, or execution."
//
// So this bridge opens a `CommandBuffer` through `CommandStream::open_producer()` — the one door,
// the same one human input and an AI use — fills an ordinary `cy::gameplay::Command`, and records
// it. There is no sequence-shaped field on the command, no branch in validation, and nothing here
// that the other five producers could not also do.
//
// ================================================================================================
// WHERE THE SEQUENCE AND TRACK IDENTITY GO INSTEAD
// ================================================================================================
//
// The specification also requires that "Sequence-emitted commands SHALL carry the sequence instance
// and track identity for diagnostics". `Provenance` is two fields — a kind and a source index — and
// widening it would put sequence-shaped data on the command itself, which is the thing the
// paragraph above forbids.
//
// So the identity travels BESIDE the command: `notes()` records, per submitted command, the
// producer sequence number the buffer assigned and the instance, track and section that produced
// it. A diagnostic joins the two on the sequence number; validation never sees the note; and
// `tests/test_commands.cpp` asserts that two commands differing only in their note validate
// identically and commit in the same order.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/gameplay/command.h>
#include <cy/sequencing/dispatch.h>

namespace cy::sequencing {

/// The diagnostic half of a sequence-emitted command. Never read by validation — see the header.
struct CommandNote {
    /// The sequence number the producer's buffer assigned. The join key.
    u32 command_sequence = 0;
    u32 instance = 0;
    u32 track_stable_id = 0;
    u32 section_stable_id = 0;
};

struct CommandBridgeReport {
    u32 submitted = 0;
    /// A command whose stable id no declaration matches. Counted rather than dropped silently: it
    /// is what a sequence cooked against an older command schema looks like.
    u32 unknown_types = 0;
    u32 refused = 0;
};

/// Turns a sequence's command batch into gameplay commands.
class CommandBridge {
public:
    CommandBridge(Allocator& allocator, gameplay::CommandStream& stream) noexcept;

    /// Open the producer. `source` and `participant` are whose intent the sequence carries — a
    /// sequence acts for a participant exactly as an AI does, and validation checks the same
    /// control binding it would for any other source.
    [[nodiscard]] Status initialize(Name debug_name, gameplay::ControlSourceId source,
                                    gameplay::ParticipantId participant) noexcept;

    [[nodiscard]] Status submit(Span<const CommandRequest> requests, u64 tick,
                                CommandBridgeReport& report) noexcept;

    [[nodiscard]] Span<const CommandNote> notes() const noexcept { return notes_.span(); }
    void clear_notes() noexcept { notes_.clear(); }
    [[nodiscard]] u32 producer() const noexcept { return producer_; }

private:
    gameplay::CommandStream* stream_;
    gameplay::ControlSourceId source_;
    gameplay::ParticipantId participant_;
    u32 producer_ = 0;
    bool opened_ = false;
    Array<CommandNote> notes_;
};

}  // namespace cy::sequencing
