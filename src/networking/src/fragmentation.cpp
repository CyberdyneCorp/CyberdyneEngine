// SPDX-License-Identifier: MIT
#include <cy/networking/fragmentation.h>

#include <cstring>
#include <utility>

namespace cy::net {

Status write_whole(Span<const u8> message, Array<u8>& out) noexcept {
    out.clear();
    if (message.size() > kMaxWholeMessage) {
        return fail(ErrorCode::OutOfRange, "networking: a whole message larger than a datagram");
    }
    if (Status pushed = out.push_back(static_cast<u8>(MessageKind::Whole)); !pushed) {
        return pushed;
    }
    return out.append(message);
}

Status write_fragment(u16 message, u32 index, Span<const u8> whole, Array<u8>& out) noexcept {
    out.clear();
    const u32 count = fragments_for(whole.size());
    if (count == 0 || count > kMaxFragments || index >= count) {
        return fail(ErrorCode::OutOfRange, "networking: no such fragment of this message");
    }
    const usize begin = static_cast<usize>(index) * kFragmentChunk;
    const usize remaining = whole.size() - begin;
    const usize size = remaining < kFragmentChunk ? remaining : kFragmentChunk;
    const u8 header[kFragmentHeaderBytes] = {
        static_cast<u8>(MessageKind::Fragment),
        static_cast<u8>(message & 0xFFU),
        static_cast<u8>(message >> 8),
        static_cast<u8>(index),
        static_cast<u8>(count),
    };
    if (Status appended = out.append(Span<const u8>(header, kFragmentHeaderBytes)); !appended) {
        return appended;
    }
    return out.append(whole.subspan(begin, size));
}

Reassembler::Reassembler(Allocator& allocator, u32 max_message_bytes) noexcept
    : allocator_(&allocator),
      max_message_bytes_(max_message_bytes),
      partials_(allocator),
      completed_(allocator) {}

Reassembler::~Reassembler() {
    while (!partials_.empty()) {
        drop(static_cast<u32>(partials_.size() - 1));
    }
}

Reassembler::Partial* Reassembler::find(ChannelId channel, u16 message) noexcept {
    for (Partial* partial : partials_) {
        if (partial->channel == channel && partial->message == message) {
            return partial;
        }
    }
    return nullptr;
}

void Reassembler::drop(u32 index) noexcept {
    Partial* partial = partials_[index];
    partial->~Partial();
    allocator_->deallocate(partial, sizeof(Partial), alignof(Partial));
    partials_.erase(index);
}

Reassembler::Partial* Reassembler::start(ChannelId channel, u16 message, u32 count,
                                         u64 now_us) noexcept {
    if (partials_.size() >= kMaxPartial) {
        // The oldest is the one least likely to complete. Dropping it is what bounds a peer that
        // sends first fragments and nothing else.
        drop(0);
        ++discarded_;
    }
    void* storage = allocator_->allocate(sizeof(Partial), alignof(Partial));
    if (storage == nullptr) {
        return nullptr;
    }
    auto* partial = construct_at<Partial>(storage, *allocator_);
    partial->channel = channel;
    partial->message = message;
    partial->count = count;
    partial->first_seen_us = now_us;
    if (!partial->bytes.resize(static_cast<usize>(count) * kFragmentChunk) ||
        !partials_.push_back(partial)) {
        partial->~Partial();
        allocator_->deallocate(partial, sizeof(Partial), alignof(Partial));
        return nullptr;
    }
    return partial;
}

ReassemblyVerdict Reassembler::accept(ChannelId channel, Span<const u8> payload, u64 now_us,
                                      Span<const u8>& out) noexcept {
    if (payload.empty()) {
        ++rejected_;
        return ReassemblyVerdict::Rejected;
    }
    if (payload[0] == static_cast<u8>(MessageKind::Whole)) {
        out = payload.subspan(kWholeHeaderBytes);
        return ReassemblyVerdict::Complete;
    }
    if (payload[0] != static_cast<u8>(MessageKind::Fragment) ||
        payload.size() <= kFragmentHeaderBytes) {
        ++rejected_;
        return ReassemblyVerdict::Rejected;
    }
    return accept_fragment(channel, payload, now_us, out);
}

ReassemblyVerdict Reassembler::accept_fragment(ChannelId channel, Span<const u8> payload,
                                               u64 now_us, Span<const u8>& out) noexcept {
    const auto message = static_cast<u16>(payload[1] | (payload[2] << 8));
    const u32 index = payload[3];
    const u32 count = payload[4];
    const Span<const u8> chunk = payload.subspan(kFragmentHeaderBytes);
    const bool last = index + 1 == count;
    const bool malformed = count < 2 || index >= count || chunk.size() > kFragmentChunk ||
                           (!last && chunk.size() != kFragmentChunk) ||
                           static_cast<u64>(count) * kFragmentChunk >
                               static_cast<u64>(max_message_bytes_) + kFragmentChunk;
    if (malformed) {
        ++rejected_;
        return ReassemblyVerdict::Rejected;
    }

    Partial* partial = find(channel, message);
    if (partial != nullptr && partial->count != count) {
        ++rejected_;
        return ReassemblyVerdict::Rejected;
    }
    if (partial == nullptr) {
        partial = start(channel, message, count, now_us);
        if (partial == nullptr) {
            ++rejected_;
            return ReassemblyVerdict::Rejected;
        }
    }
    const u64 bit = 1ULL << (index % 64);
    if ((partial->bits[index / 64] & bit) != 0) {
        return ReassemblyVerdict::Pending;  // A duplicate fragment changes nothing.
    }
    partial->bits[index / 64] |= bit;
    ++partial->arrived;
    std::memcpy(partial->bytes.data() + (static_cast<usize>(index) * kFragmentChunk), chunk.data(),
                chunk.size());
    if (last) {
        partial->last_size = static_cast<u32>(chunk.size());
    }
    if (partial->arrived < partial->count) {
        return ReassemblyVerdict::Pending;
    }
    for (u32 slot = 0; slot < partials_.size(); ++slot) {
        if (partials_[slot] == partial) {
            return finish(slot, out);
        }
    }
    return ReassemblyVerdict::Rejected;
}

ReassemblyVerdict Reassembler::finish(u32 index, Span<const u8>& out) noexcept {
    Partial* partial = partials_[index];
    const usize size =
        (static_cast<usize>(partial->count - 1) * kFragmentChunk) + partial->last_size;
    if (size > max_message_bytes_) {
        ++rejected_;
        drop(index);
        return ReassemblyVerdict::Rejected;
    }
    completed_.clear();
    if (!completed_.append(Span<const u8>(partial->bytes.data(), size))) {
        drop(index);
        return ReassemblyVerdict::Rejected;
    }
    drop(index);
    out = completed_.span();
    return ReassemblyVerdict::Complete;
}

void Reassembler::expire(u64 now_us, u64 max_age_us) noexcept {
    u32 index = 0;
    while (index < partials_.size()) {
        if (now_us - partials_[index]->first_seen_us > max_age_us) {
            drop(index);
            ++discarded_;
            continue;
        }
        ++index;
    }
}

}  // namespace cy::net
