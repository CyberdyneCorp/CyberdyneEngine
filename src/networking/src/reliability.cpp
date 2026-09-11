#include <cy/networking/reliability.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace cy::net {
namespace {

template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object == nullptr) {
        return;
    }
    object->~T();
    allocator.deallocate(object, sizeof(T), alignof(T));
}

void write_u16(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value & 0xFFU);
    out[1] = static_cast<u8>((value >> 8) & 0xFFU);
}

void write_u32(u8* out, u32 value) noexcept {
    for (u32 byte = 0; byte < 4; ++byte) {
        out[byte] = static_cast<u8>((value >> (byte * 8)) & 0xFFU);
    }
}

[[nodiscard]] u16 read_u16(const u8* in) noexcept {
    return static_cast<u16>(static_cast<u16>(in[0]) | (static_cast<u16>(in[1]) << 8));
}

[[nodiscard]] u32 read_u32(const u8* in) noexcept {
    u32 value = 0;
    for (u32 byte = 0; byte < 4; ++byte) {
        value |= static_cast<u32>(in[byte]) << (byte * 8);
    }
    return value;
}

/// The replay window's width. Thirty-two is what one `ack_bits` covers, so the window a receiver
/// remembers and the window an acknowledgement describes are the same window by construction.
inline constexpr u16 kReplayWindow = 32;

/// Remove one element, keeping the order of the rest. `Array::remove_unordered` is O(1) and wrong
/// here: an unacknowledged queue in send order is what makes the arena compactable from the front.
template <class T>
void erase_ordered(Array<T>& array, usize index) noexcept {
    for (usize slot = index + 1; slot < array.size(); ++slot) {
        array[slot - 1] = array[slot];
    }
    array.pop_back();
}

}  // namespace

const char* delivery_mode_name(DeliveryMode mode) noexcept {
    switch (mode) {
        case DeliveryMode::Unreliable:
            return "Unreliable";
        case DeliveryMode::UnreliableSequenced:
            return "UnreliableSequenced";
        case DeliveryMode::ReliableUnordered:
            return "ReliableUnordered";
        case DeliveryMode::ReliableOrdered:
            return "ReliableOrdered";
    }
    return "unknown";
}

const char* connection_state_name(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected:
            return "Disconnected";
        case ConnectionState::Connecting:
            return "Connecting";
        case ConnectionState::Authenticating:
            return "Authenticating";
        case ConnectionState::Connected:
            return "Connected";
        case ConnectionState::Disconnecting:
            return "Disconnecting";
    }
    return "unknown";
}

const char* security_refusal_name(SecurityRefusal refusal) noexcept {
    switch (refusal) {
        case SecurityRefusal::None:
            return "None";
        case SecurityRefusal::NotEncrypted:
            return "NotEncrypted";
        case SecurityRefusal::NotAuthenticated:
            return "NotAuthenticated";
        case SecurityRefusal::NoReplayProtection:
            return "NoReplayProtection";
        case SecurityRefusal::NoSequenceValidation:
            return "NoSequenceValidation";
    }
    return "unknown";
}

SecurityRefusal admissible_for(const TransportSecurity& security, Deployment deployment) noexcept {
    // Correctness before confidentiality: a transport with no replay protection re-applies whatever
    // an attacker — or a broken router — sends twice, which is wrong in every deployment including
    // a test.
    if (!security.sequence_validated) {
        return SecurityRefusal::NoSequenceValidation;
    }
    if (!security.replay_protected) {
        return SecurityRefusal::NoReplayProtection;
    }
    if (deployment == Deployment::InProcess) {
        return SecurityRefusal::None;
    }
    // A transport that never leaves the process cannot be wiretapped on a network it never reaches.
    // Anything else must encrypt and authenticate before a shipping build may use it.
    if (security.in_process) {
        return SecurityRefusal::None;
    }
    if (deployment == Deployment::Shipping && !security.encrypted) {
        return SecurityRefusal::NotEncrypted;
    }
    if (deployment == Deployment::Shipping && !security.authenticated) {
        return SecurityRefusal::NotAuthenticated;
    }
    return SecurityRefusal::None;
}

const char* receive_verdict_name(ReceiveVerdict verdict) noexcept {
    switch (verdict) {
        case ReceiveVerdict::Delivered:
            return "Delivered";
        case ReceiveVerdict::Duplicate:
            return "Duplicate";
        case ReceiveVerdict::TooOld:
            return "TooOld";
        case ReceiveVerdict::Buffered:
            return "Buffered";
        case ReceiveVerdict::AckOnly:
            return "AckOnly";
    }
    return "unknown";
}

void encode_header(const DatagramHeader& header, u8* out) noexcept {
    write_u16(out, header.sequence);
    write_u16(out + 2, header.ack);
    write_u32(out + 4, header.ack_bits);
    out[8] = header.channel;
    out[9] = static_cast<u8>(header.delivery);
    write_u16(out + 10, header.payload_size);
}

bool decode_header(Span<const u8> bytes, DatagramHeader& out) noexcept {
    if (bytes.size() < kDatagramHeaderSize) {
        return false;
    }
    out.sequence = read_u16(bytes.data());
    out.ack = read_u16(bytes.data() + 2);
    out.ack_bits = read_u32(bytes.data() + 4);
    out.channel = bytes[8];
    const u8 delivery = bytes[9];
    if (delivery > static_cast<u8>(DeliveryMode::ReliableOrdered)) {
        return false;
    }
    out.delivery = static_cast<DeliveryMode>(delivery);
    out.payload_size = read_u16(bytes.data() + 10);
    if (out.payload_size > kMaxDatagramPayload) {
        return false;
    }
    return bytes.size() >= static_cast<usize>(kDatagramHeaderSize) + out.payload_size;
}

// --- ReliabilityChannel --------------------------------------------------------------------------

ReliabilityChannel::ReliabilityChannel(Allocator& allocator, ChannelId channel) noexcept
    : channel_(channel),
      send_arena_(allocator),
      unacked_(allocator),
      delivery_arena_(allocator),
      delivery_(allocator),
      reorder_arena_(allocator),
      reorder_(allocator) {}

DatagramHeader ReliabilityChannel::outgoing_header(DeliveryMode delivery, u16 sequence,
                                                   u16 payload_size) noexcept {
    DatagramHeader header;
    header.sequence = sequence;
    header.ack = newest_received_;
    header.ack_bits = received_bits_;
    header.channel = channel_;
    header.delivery = delivery;
    header.payload_size = payload_size;
    owes_ack_ = false;
    return header;
}

Status ReliabilityChannel::remember(u16 sequence, DeliveryMode delivery, Span<const u8> payload,
                                    u64 now_ms) noexcept {
    Pending pending;
    pending.sequence = sequence;
    pending.delivery = delivery;
    pending.offset = send_base_ + static_cast<u32>(send_arena_.size());
    pending.size = static_cast<u32>(payload.size());
    pending.attempts = 1;
    pending.next_attempt_ms = now_ms;
    if (Status appended = send_arena_.append(payload); !appended) {
        return appended;
    }
    return unacked_.push_back(pending);
}

Status ReliabilityChannel::frame(DeliveryMode delivery, Span<const u8> payload, u64 now_ms,
                                 Array<u8>& out) noexcept {
    if (payload.size() > kMaxDatagramPayload) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a datagram payload larger than the MTU allows; fragment above the "
                    "transport, where the sender knows what it is sending");
    }
    // Sequence zero is reserved for the bare acknowledgement, so the counter skips it on the wrap.
    if (next_sequence_ == 0) {
        next_sequence_ = 1;
    }
    const u16 sequence = next_sequence_++;
    const DatagramHeader header =
        outgoing_header(delivery, sequence, static_cast<u16>(payload.size()));

    const usize start = out.size();
    if (Status grown = out.resize(start + kDatagramHeaderSize + payload.size()); !grown) {
        return grown;
    }
    encode_header(header, out.data() + start);
    if (!payload.empty()) {
        std::memcpy(out.data() + start + kDatagramHeaderSize, payload.data(), payload.size());
    }
    if (is_reliable(delivery)) {
        // Remembered with the retransmission clock already running; `collect_retransmissions()`
        // decides when it is due against the policy's first timeout. The time is the caller's
        // because a queue timed from zero would retransmit everything on its first advance when the
        // transport's clock is a real one.
        return remember(sequence, delivery, payload, now_ms);
    }
    return ok();
}

Status ReliabilityChannel::frame_ack(Array<u8>& out) noexcept {
    const DatagramHeader header = outgoing_header(DeliveryMode::Unreliable, 0, 0);
    const usize start = out.size();
    if (Status grown = out.resize(start + kDatagramHeaderSize); !grown) {
        return grown;
    }
    encode_header(header, out.data() + start);
    return ok();
}

void ReliabilityChannel::drop_acknowledged(u16 sequence) noexcept {
    for (usize index = 0; index < unacked_.size(); ++index) {
        if (unacked_[index].sequence == sequence) {
            erase_ordered(unacked_, index);
            compact_send_arena();
            return;
        }
    }
}

void ReliabilityChannel::acknowledge(u16 ack, u32 ack_bits) noexcept {
    drop_acknowledged(ack);
    for (u16 offset = 0; offset < kReplayWindow; ++offset) {
        if ((ack_bits & (1U << offset)) != 0) {
            drop_acknowledged(static_cast<u16>(ack - (offset + 1)));
        }
    }
}

void ReliabilityChannel::compact_send_arena() noexcept {
    const u32 keep_from =
        unacked_.empty() ? send_base_ + static_cast<u32>(send_arena_.size()) : unacked_[0].offset;
    const u32 drop = keep_from - send_base_;
    if (drop == 0) {
        return;
    }
    const usize remaining = send_arena_.size() - drop;
    if (remaining != 0) {
        std::memmove(send_arena_.data(), send_arena_.data() + drop, remaining);
    }
    (void)send_arena_.resize(remaining);
    send_base_ = keep_from;
}

bool ReliabilityChannel::already_received(u16 sequence) const noexcept {
    if (!any_received_) {
        return false;
    }
    if (sequence == newest_received_) {
        return true;
    }
    if (sequence_newer(sequence, newest_received_)) {
        return false;
    }
    const u16 distance = static_cast<u16>(newest_received_ - sequence);
    if (distance > kReplayWindow) {
        // Outside the window. Treated as seen: accepting it would be accepting a datagram this side
        // can no longer prove it has not already applied, which is what replay protection is for.
        return true;
    }
    return (received_bits_ & (1U << (distance - 1))) != 0;
}

void ReliabilityChannel::note_received(u16 sequence) noexcept {
    if (!any_received_) {
        any_received_ = true;
        newest_received_ = sequence;
        received_bits_ = 0;
        return;
    }
    if (sequence_newer(sequence, newest_received_)) {
        const u16 shift = static_cast<u16>(sequence - newest_received_);
        received_bits_ = shift >= 32 ? 0U : ((received_bits_ << shift) | (1U << (shift - 1)));
        newest_received_ = sequence;
        return;
    }
    const u16 distance = static_cast<u16>(newest_received_ - sequence);
    if (distance <= kReplayWindow && distance > 0) {
        received_bits_ |= 1U << (distance - 1);
    }
}

Status ReliabilityChannel::queue_for_delivery(u16 sequence, DeliveryMode delivery,
                                              Span<const u8> payload) noexcept {
    Queued entry;
    entry.sequence = sequence;
    entry.delivery = delivery;
    entry.offset = delivery_base_ + static_cast<u32>(delivery_arena_.size());
    entry.size = static_cast<u32>(payload.size());
    if (Status appended = delivery_arena_.append(payload); !appended) {
        return appended;
    }
    if (Status pushed = delivery_.push_back(entry); !pushed) {
        return pushed;
    }
    ++delivered_;
    return ok();
}

Status ReliabilityChannel::buffer_for_ordering(u16 sequence, DeliveryMode delivery,
                                               Span<const u8> payload) noexcept {
    Queued entry;
    entry.sequence = sequence;
    entry.delivery = delivery;
    entry.offset = static_cast<u32>(reorder_arena_.size());
    entry.size = static_cast<u32>(payload.size());
    if (Status appended = reorder_arena_.append(payload); !appended) {
        return appended;
    }
    return reorder_.push_back(entry);
}

Status ReliabilityChannel::drain_ordered() noexcept {
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (usize index = 0; index < reorder_.size(); ++index) {
            if (reorder_[index].sequence != next_ordered_) {
                continue;
            }
            const Queued entry = reorder_[index];
            const Span<const u8> payload(reorder_arena_.data() + entry.offset, entry.size);
            if (Status queued = queue_for_delivery(entry.sequence, entry.delivery, payload);
                !queued) {
                return queued;
            }
            erase_ordered(reorder_, index);
            ++next_ordered_;
            progressed = true;
            break;
        }
    }
    if (reorder_.empty()) {
        reorder_arena_.clear();
    }
    return ok();
}

Status ReliabilityChannel::accept(const DatagramHeader& header, Span<const u8> payload,
                                  ReceiveVerdict& verdict) noexcept {
    acknowledge(header.ack, header.ack_bits);

    if (header.payload_size == 0 && header.sequence == 0) {
        verdict = ReceiveVerdict::AckOnly;
        return ok();
    }
    if (already_received(header.sequence)) {
        ++duplicates_;
        owes_ack_ = true;
        verdict = ReceiveVerdict::Duplicate;
        return ok();
    }
    if (header.delivery == DeliveryMode::UnreliableSequenced && any_received_ &&
        !sequence_newer(header.sequence, newest_received_)) {
        note_received(header.sequence);
        owes_ack_ = true;
        verdict = ReceiveVerdict::TooOld;
        return ok();
    }
    note_received(header.sequence);
    owes_ack_ = true;

    if (header.delivery != DeliveryMode::ReliableOrdered) {
        verdict = ReceiveVerdict::Delivered;
        return queue_for_delivery(header.sequence, header.delivery, payload);
    }
    if (header.sequence == next_ordered_) {
        if (Status queued = queue_for_delivery(header.sequence, header.delivery, payload);
            !queued) {
            return queued;
        }
        ++next_ordered_;
        verdict = ReceiveVerdict::Delivered;
        return drain_ordered();
    }
    if (!sequence_newer(header.sequence, next_ordered_)) {
        verdict = ReceiveVerdict::TooOld;
        return ok();
    }
    verdict = ReceiveVerdict::Buffered;
    return buffer_for_ordering(header.sequence, header.delivery, payload);
}

void ReliabilityChannel::compact_delivery_arena() noexcept {
    if (delivery_head_ < delivery_.size()) {
        return;
    }
    // Everything queued has been handed out. The arena is emptied and the logical base advanced by
    // what it held, so the offsets the next entries record stay monotonic and
    // `entry.offset - delivery_base_` stays the index into the arena it is.
    delivery_base_ += static_cast<u32>(delivery_arena_.size());
    delivery_.clear();
    delivery_arena_.clear();
    delivery_head_ = 0;
}

bool ReliabilityChannel::next_delivery(Span<const u8>& out, DeliveryMode& delivery) noexcept {
    if (delivery_head_ >= delivery_.size()) {
        compact_delivery_arena();
        return false;
    }
    const Queued entry = delivery_[delivery_head_++];
    out = Span<const u8>(delivery_arena_.data() + (entry.offset - delivery_base_), entry.size);
    delivery = entry.delivery;
    return true;
}

Status ReliabilityChannel::collect_retransmissions(u64 now_ms, const RetransmitPolicy& policy,
                                                   Array<u8>& out) noexcept {
    for (auto& pending : unacked_) {
        u32 timeout = policy.initial_timeout_ms;
        for (u32 attempt = 1; attempt < pending.attempts && timeout < policy.maximum_timeout_ms;
             ++attempt) {
            timeout *= 2;
        }
        timeout = std::min(timeout, policy.maximum_timeout_ms);
        if (now_ms < pending.next_attempt_ms + timeout) {
            continue;
        }
        if (pending.attempts >= policy.maximum_attempts) {
            abandoned_ = true;
            continue;
        }

        const DatagramHeader header =
            outgoing_header(pending.delivery, pending.sequence, static_cast<u16>(pending.size));
        const usize start = out.size();
        if (Status grown = out.resize(start + kDatagramHeaderSize + pending.size); !grown) {
            return grown;
        }
        encode_header(header, out.data() + start);
        std::memcpy(out.data() + start + kDatagramHeaderSize,
                    send_arena_.data() + (pending.offset - send_base_), pending.size);
        pending.attempts += 1;
        pending.next_attempt_ms = now_ms;
        ++retransmissions_;
    }
    return ok();
}

void ReliabilityChannel::clear() noexcept {
    next_sequence_ = 1;
    newest_received_ = 0;
    received_bits_ = 0;
    any_received_ = false;
    next_ordered_ = 1;
    owes_ack_ = false;
    abandoned_ = false;
    send_arena_.clear();
    send_base_ = 0;
    unacked_.clear();
    delivery_arena_.clear();
    delivery_base_ = 0;
    delivery_.clear();
    delivery_head_ = 0;
    reorder_arena_.clear();
    reorder_.clear();
}

// --- ReliableEndpoint ----------------------------------------------------------------------------

ReliableEndpoint::ReliableEndpoint(Allocator& allocator) noexcept : allocator_(&allocator) {}

ReliableEndpoint::~ReliableEndpoint() {
    for (auto& channel : channels_) {
        unmake(*allocator_, channel);
        channel = nullptr;
    }
}

Status ReliableEndpoint::prepare() noexcept {
    for (u32 index = 0; index < kMaxChannels; ++index) {
        if (channels_[index] != nullptr) {
            continue;
        }
        channels_[index] =
            make<ReliabilityChannel>(*allocator_, *allocator_, static_cast<ChannelId>(index));
        if (channels_[index] == nullptr) {
            return fail(ErrorCode::OutOfMemory, "networking: could not allocate a channel");
        }
    }
    return ok();
}

Status ReliableEndpoint::frame(ChannelId channel, DeliveryMode delivery, Span<const u8> payload,
                               u64 now_ms, Array<u8>& out) noexcept {
    if (channel >= kMaxChannels) {
        return fail(ErrorCode::OutOfRange, "networking: channel out of range");
    }
    if (Status ready = prepare(); !ready) {
        return ready;
    }
    return channels_[channel]->frame(delivery, payload, now_ms, out);
}

Status ReliableEndpoint::ingest(Span<const u8> wire, ChannelId& channel,
                                ReceiveVerdict& verdict) noexcept {
    DatagramHeader header;
    if (!decode_header(wire, header)) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a datagram whose header does not decode; refused rather than read "
                    "past");
    }
    if (header.channel >= kMaxChannels) {
        return fail(ErrorCode::OutOfRange, "networking: a datagram naming a channel out of range");
    }
    if (Status ready = prepare(); !ready) {
        return ready;
    }
    channel = header.channel;
    const Span<const u8> payload(wire.data() + kDatagramHeaderSize, header.payload_size);
    return channels_[header.channel]->accept(header, payload, verdict);
}

bool ReliableEndpoint::next_delivery(ChannelId channel, Span<const u8>& out,
                                     DeliveryMode& delivery) noexcept {
    if (channel >= kMaxChannels || channels_[channel] == nullptr) {
        return false;
    }
    return channels_[channel]->next_delivery(out, delivery);
}

Status ReliableEndpoint::collect_outgoing(u64 now_ms, Array<u8>& out) noexcept {
    if (Status ready = prepare(); !ready) {
        return ready;
    }
    for (auto& channel : channels_) {
        ReliabilityChannel& one = *channel;
        if (Status collected = one.collect_retransmissions(now_ms, policy_, out); !collected) {
            return collected;
        }
        if (one.owes_ack()) {
            if (Status acked = one.frame_ack(out); !acked) {
                return acked;
            }
        }
    }
    return ok();
}

bool ReliableEndpoint::abandoned() const noexcept {
    return std::ranges::any_of(channels_, [](const ReliabilityChannel* channel) noexcept {
        return channel != nullptr && channel->abandoned();
    });
}

u64 ReliableEndpoint::retransmissions() const noexcept {
    u64 total = 0;
    for (const auto* channel : channels_) {
        if (channel != nullptr) {
            total += channel->retransmissions();
        }
    }
    return total;
}

u64 ReliableEndpoint::duplicates_rejected() const noexcept {
    u64 total = 0;
    for (const auto* channel : channels_) {
        if (channel != nullptr) {
            total += channel->duplicates_rejected();
        }
    }
    return total;
}

}  // namespace cy::net
