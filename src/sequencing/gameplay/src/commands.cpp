// The gameplay bridge: one producer buffer, ordinary commands, and the notes beside them.

#include <cy/sequencing/gameplay/commands.h>

namespace cy::sequencing {

CommandBridge::CommandBridge(Allocator& allocator, gameplay::CommandStream& stream) noexcept
    : stream_(&stream), notes_(allocator) {}

Status CommandBridge::initialize(Name debug_name, gameplay::ControlSourceId source,
                                 gameplay::ParticipantId participant) noexcept {
    const Expected<u32, Error> opened = stream_->open_producer(debug_name);
    if (!opened) {
        return Status{make_unexpected(opened.error())};
    }
    producer_ = opened.value();
    source_ = source;
    participant_ = participant;
    opened_ = true;
    return ok();
}

Status CommandBridge::submit(Span<const CommandRequest> requests, u64 tick,
                             CommandBridgeReport& report) noexcept {
    if (!opened_) {
        return fail(ErrorCode::Unavailable, "the command bridge has no producer");
    }
    gameplay::CommandBuffer& buffer = stream_->producer(producer_);
    for (const CommandRequest& request : requests) {
        const gameplay::CommandTypeId type = stream_->find(request.command_stable_id);
        if (type == gameplay::kInvalidCommandType) {
            ++report.unknown_types;
            continue;
        }
        gameplay::Command command;
        command.type = type;
        command.tick = tick;
        command.participant = participant_;
        command.source = source_;
        command.target = ecs::Entity::from_bits(request.target);
        // `ControlSourceKind::Script` is what a sequence is: an authored producer of intent. It is
        // recorded for a diagnostic and read by nothing that decides anything.
        command.provenance.kind = gameplay::ControlSourceKind::Script;
        command.provenance.source = source_.index();
        command.payload_size = request.payload_size;
        for (u16 index = 0; index < request.payload_size && index < gameplay::kMaxCommandPayload;
             ++index) {
            command.payload[index] = request.payload[index];
        }
        if (Status recorded = buffer.record(command); !recorded) {
            ++report.refused;
            continue;
        }
        CommandNote note;
        note.command_sequence = buffer.at(buffer.size() - 1).sequence;
        note.instance = request.provenance.instance;
        note.track_stable_id = request.provenance.track_stable_id;
        note.section_stable_id = request.provenance.section_stable_id;
        if (Status kept = notes_.push_back(note); !kept) {
            return kept;
        }
        ++report.submitted;
    }
    return ok();
}

}  // namespace cy::sequencing
