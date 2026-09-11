#include <cy/networking/local_transport.h>

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

[[nodiscard]] bool same_text(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

/// The stream the condition simulator draws from. Presentation, deliberately: a dropped datagram is
/// not part of any peer's authoritative state, and `simulation-and-determinism` requires a stream
/// that is not to be declared as such. It is nevertheless fully reproducible — the classification
/// says what the draw *means*, not whether it repeats.
[[nodiscard]] determinism::RandomStream condition_stream(u64 seed) noexcept {
    return determinism::RandomSource(seed).stream("net.conditions",
                                                  determinism::StreamPurpose::Presentation);
}

}  // namespace

struct LocalNetwork::Host {
    const char* name = "";
    u32 generation = 1;
    Array<u8> inbox_arena;
    struct Entry {
        u32 from_slot = 0;
        u32 offset = 0;
        u32 size = 0;
    };
    Array<Entry> inbox;
    u32 inbox_head = 0;
    u32 inbox_base = 0;

    explicit Host(Allocator& allocator) noexcept : inbox_arena(allocator), inbox(allocator) {}
};

LocalNetwork::LocalNetwork(Allocator& allocator, u64 seed) noexcept
    : allocator_(&allocator),
      stream_(condition_stream(seed)),
      hosts_(allocator),
      arena_(allocator),
      in_flight_(allocator) {}

LocalNetwork::~LocalNetwork() {
    for (auto& host : hosts_) {
        unmake(*allocator_, host);
    }
}

Expected<PeerId, Error> LocalNetwork::add_host(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a local host needs a name to be found "
                    "by");
    }
    if (find_host(name).valid()) {
        return fail(ErrorCode::AlreadyExists, "networking: a local host with that name exists");
    }
    Host* host = make<Host>(*allocator_, *allocator_);
    if (host == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a local host");
    }
    host->name = name;
    const u32 slot = static_cast<u32>(hosts_.size());
    if (Status pushed = hosts_.push_back(host); !pushed) {
        unmake(*allocator_, host);
        return fail(ErrorCode::OutOfMemory, "networking: could not register a local host");
    }
    return PeerId::make(slot, host->generation);
}

PeerId LocalNetwork::find_host(const char* name) const noexcept {
    for (usize index = 0; index < hosts_.size(); ++index) {
        if (same_text(hosts_[index]->name, name)) {
            return PeerId::make(static_cast<u32>(index), hosts_[index]->generation);
        }
    }
    return {};
}

u32 LocalNetwork::draw(u64 ordinal, u64 index) noexcept {
    const determinism::SimulationPoint at{determinism::Epoch{0}, ordinal};
    return stream_.draw_u32(at, 0, index) % 100U;
}

Status LocalNetwork::schedule(u32 from_slot, u32 to_slot, u64 deliver_at_ms,
                              Span<const u8> wire) noexcept {
    InFlight entry;
    entry.deliver_at_ms = deliver_at_ms;
    entry.from_slot = from_slot;
    entry.to_slot = to_slot;
    entry.offset = arena_base_ + static_cast<u32>(arena_.size());
    entry.size = static_cast<u32>(wire.size());
    entry.ordinal = next_ordinal_++;
    if (Status appended = arena_.append(wire); !appended) {
        return appended;
    }
    return in_flight_.push_back(entry);
}

Status LocalNetwork::offer(PeerId from, PeerId to, u64 now_ms, Span<const u8> wire) noexcept {
    if (!from.valid() || !to.valid() || to.slot() >= hosts_.size()) {
        return fail(ErrorCode::NotFound, "networking: no such local host");
    }
    ++offered_;
    const u64 ordinal = offered_;

    if (conditions_.loss_percent != 0 && draw(ordinal, 0) < conditions_.loss_percent) {
        ++dropped_;
        return ok();
    }

    u64 delay = conditions_.latency_ms;
    if (conditions_.jitter_ms != 0) {
        delay += draw(ordinal, 1) % (conditions_.jitter_ms + 1);
    }
    if (conditions_.reorder_percent != 0 && draw(ordinal, 2) < conditions_.reorder_percent) {
        delay += conditions_.latency_ms + conditions_.jitter_ms + 1;
    }
    if (Status scheduled = schedule(from.slot(), to.slot(), now_ms + delay, wire); !scheduled) {
        return scheduled;
    }
    if (conditions_.duplication_percent != 0 &&
        draw(ordinal, 3) < conditions_.duplication_percent) {
        ++duplicated_;
        return schedule(from.slot(), to.slot(), now_ms + delay, wire);
    }
    return ok();
}

void LocalNetwork::advance(u64 now_ms) noexcept {
    // Two passes: deliver everything due, oldest ordinal first, then drop what was delivered. The
    // ordinal sort is what makes "sent first arrives first when both are due" a property rather
    // than a coincidence of the array's compaction.
    for (;;) {
        usize best = in_flight_.size();
        for (usize index = 0; index < in_flight_.size(); ++index) {
            const InFlight& one = in_flight_[index];
            if (one.deliver_at_ms > now_ms) {
                continue;
            }
            if (best == in_flight_.size() || one.ordinal < in_flight_[best].ordinal) {
                best = index;
            }
        }
        if (best == in_flight_.size()) {
            break;
        }
        const InFlight entry = in_flight_[best];
        Host& host = *hosts_[entry.to_slot];
        Host::Entry queued;
        queued.from_slot = entry.from_slot;
        queued.offset = host.inbox_base + static_cast<u32>(host.inbox_arena.size());
        queued.size = entry.size;
        const Span<const u8> bytes(arena_.data() + (entry.offset - arena_base_), entry.size);
        if (host.inbox_arena.append(bytes) && host.inbox.push_back(queued)) {
            ++delivered_;
        } else {
            // Out of memory in a test substrate is a dropped datagram, which is a thing the network
            // is allowed to do. Counted, so it is visible rather than silent.
            ++dropped_;
        }
        // Order-preserving removal, so the remaining ordinals stay ascending.
        for (usize slot = best + 1; slot < in_flight_.size(); ++slot) {
            in_flight_[slot - 1] = in_flight_[slot];
        }
        in_flight_.pop_back();
    }
    if (in_flight_.empty()) {
        arena_base_ += static_cast<u32>(arena_.size());
        arena_.clear();
    }
}

bool LocalNetwork::take(PeerId at, PeerId& from, Span<const u8>& wire) noexcept {
    if (!at.valid() || at.slot() >= hosts_.size()) {
        return false;
    }
    Host& host = *hosts_[at.slot()];
    if (host.inbox_head >= host.inbox.size()) {
        host.inbox_base += static_cast<u32>(host.inbox_arena.size());
        host.inbox.clear();
        host.inbox_arena.clear();
        host.inbox_head = 0;
        return false;
    }
    const Host::Entry entry = host.inbox[host.inbox_head++];
    from = PeerId::make(entry.from_slot, hosts_[entry.from_slot]->generation);
    wire = Span<const u8>(host.inbox_arena.data() + (entry.offset - host.inbox_base), entry.size);
    return true;
}

// --- LocalTransport ------------------------------------------------------------------------------

LocalTransport::LocalTransport(Allocator& allocator, LocalNetwork& network, PeerId self) noexcept
    : allocator_(&allocator),
      network_(&network),
      self_(self),
      links_(allocator),
      outgoing_(allocator) {}

LocalTransport::~LocalTransport() {
    for (auto& link : links_) {
        unmake(*allocator_, link);
    }
}

LocalTransport::Link* LocalTransport::find(PeerId peer) noexcept {
    for (auto& link : links_) {
        if (link->remote == peer) {
            return link;
        }
    }
    return nullptr;
}

const LocalTransport::Link* LocalTransport::find(PeerId peer) const noexcept {
    for (auto* link : links_) {
        if (link->remote == peer) {
            return link;
        }
    }
    return nullptr;
}

Expected<LocalTransport::Link*, Error> LocalTransport::open(PeerId peer,
                                                            ConnectionState initial) noexcept {
    if (Link* existing = find(peer); existing != nullptr) {
        return existing;
    }
    Link* link = make<Link>(*allocator_, *allocator_, peer, initial);
    if (link == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a link");
    }
    if (Status ready = link->endpoint.prepare(); !ready) {
        unmake(*allocator_, link);
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate the link's channels");
    }
    if (Status pushed = links_.push_back(link); !pushed) {
        unmake(*allocator_, link);
        return fail(ErrorCode::OutOfMemory, "networking: could not register a link");
    }
    return link;
}

Expected<PeerId, Error> LocalTransport::connect(const char* address) noexcept {
    const PeerId remote = network_->find_host(address);
    if (!remote.valid()) {
        return fail(ErrorCode::NotFound, "networking: no local host under that name");
    }
    const ConnectionState initial =
        authentication_required_ ? ConnectionState::Authenticating : ConnectionState::Connected;
    Expected<Link*, Error> link = open(remote, initial);
    if (!link) {
        return make_unexpected(link.error());
    }
    return remote;
}

Expected<PeerId, Error> LocalTransport::accept(PeerId peer) noexcept {
    const ConnectionState initial =
        authentication_required_ ? ConnectionState::Authenticating : ConnectionState::Connected;
    Expected<Link*, Error> link = open(peer, initial);
    if (!link) {
        return make_unexpected(link.error());
    }
    return peer;
}

Status LocalTransport::admit(PeerId peer) noexcept {
    Link* link = find(peer);
    if (link == nullptr) {
        return fail(ErrorCode::NotFound, "networking: no link to that peer");
    }
    link->state = ConnectionState::Connected;
    return ok();
}

void LocalTransport::disconnect(PeerId peer) noexcept {
    Link* link = find(peer);
    if (link != nullptr) {
        link->state = ConnectionState::Disconnected;
    }
}

Status LocalTransport::send(PeerId peer, ChannelId channel, DeliveryMode delivery,
                            Span<const u8> bytes) noexcept {
    Link* link = find(peer);
    if (link == nullptr) {
        return fail(ErrorCode::NotFound, "networking: no link to that peer");
    }
    if (link->state != ConnectionState::Connected) {
        // A peer that has not been admitted receives nothing. `networking-and-replication`: it
        // "SHALL remain in a pending state until authentication succeeds or times out, without
        // receiving game state".
        return fail(ErrorCode::Unavailable,
                    "networking: the peer is not admitted; an unauthenticated link carries no "
                    "application payload");
    }
    outgoing_.clear();
    if (Status framed = link->endpoint.frame(channel, delivery, bytes, now_ms_, outgoing_);
        !framed) {
        return framed;
    }
    link->stats.datagrams_out += 1;
    link->stats.bytes_out += outgoing_.size();
    return network_->offer(self_, peer, now_ms_, outgoing_.span());
}

void LocalTransport::flush(u64 now_ms) noexcept {
    for (auto* held : links_) {
        Link& link = *held;
        if (link.state == ConnectionState::Disconnected) {
            continue;
        }
        outgoing_.clear();
        if (!link.endpoint.collect_outgoing(now_ms, outgoing_)) {
            continue;
        }
        if (outgoing_.empty()) {
            continue;
        }
        // Every collected datagram is offered as one wire datagram, because the substrate's loss
        // and reordering must apply per datagram rather than per batch.
        usize cursor = 0;
        while (cursor + kDatagramHeaderSize <= outgoing_.size()) {
            DatagramHeader header;
            const Span<const u8> rest(outgoing_.data() + cursor, outgoing_.size() - cursor);
            if (!decode_header(rest, header)) {
                break;
            }
            const usize length = kDatagramHeaderSize + header.payload_size;
            (void)network_->offer(self_, link.remote, now_ms,
                                  Span<const u8>(outgoing_.data() + cursor, length));
            link.stats.datagrams_out += 1;
            link.stats.bytes_out += length;
            cursor += length;
        }
        link.stats.retransmissions = link.endpoint.retransmissions();
    }
}

void LocalTransport::advance(u64 now_ms) noexcept {
    now_ms_ = now_ms;
    flush(now_ms);
    network_->advance(now_ms);
}

bool LocalTransport::drain(Datagram& out) noexcept {
    for (u32 attempt = 0; attempt < links_.size() * kMaxChannels; ++attempt) {
        if (drain_link_ >= links_.size()) {
            drain_link_ = 0;
            drain_channel_ = 0;
        }
        Link& link = *links_[drain_link_];
        Span<const u8> payload;
        DeliveryMode delivery = DeliveryMode::Unreliable;
        if (link.state == ConnectionState::Connected &&
            link.endpoint.next_delivery(static_cast<ChannelId>(drain_channel_), payload,
                                        delivery)) {
            out.peer = link.remote;
            out.channel = static_cast<ChannelId>(drain_channel_);
            out.delivery = delivery;
            out.bytes = payload;
            return true;
        }
        ++drain_channel_;
        if (drain_channel_ >= kMaxChannels) {
            drain_channel_ = 0;
            ++drain_link_;
        }
    }
    return false;
}

bool LocalTransport::receive(Datagram& out) noexcept {
    for (;;) {
        if (drain(out)) {
            return true;
        }
        PeerId from;
        Span<const u8> wire;
        if (!network_->take(self_, from, wire)) {
            return false;
        }
        Link* link = find(from);
        if (link == nullptr || link->state == ConnectionState::Disconnected) {
            continue;
        }
        link->stats.datagrams_in += 1;
        link->stats.bytes_in += wire.size();
        ChannelId channel = 0;
        ReceiveVerdict verdict = ReceiveVerdict::Delivered;
        if (!link->endpoint.ingest(wire, channel, verdict)) {
            continue;
        }
        if (verdict == ReceiveVerdict::Duplicate) {
            link->stats.datagrams_duplicate += 1;
        }
    }
}

ConnectionState LocalTransport::state(PeerId peer) const noexcept {
    const Link* link = find(peer);
    return link == nullptr ? ConnectionState::Disconnected : link->state;
}

ConnectionStats LocalTransport::stats(PeerId peer) const noexcept {
    const Link* link = find(peer);
    if (link == nullptr) {
        return {};
    }
    ConnectionStats copy = link->stats;
    copy.round_trip_us = network_->conditions().latency_ms * 2000U;
    copy.jitter_us = network_->conditions().jitter_ms * 1000U;
    copy.loss_per_mille = network_->conditions().loss_percent * 10U;
    return copy;
}

TransportSecurity LocalTransport::security() const noexcept {
    TransportSecurity security;
    security.in_process = true;
    security.replay_protected = true;
    security.sequence_validated = true;
    security.mechanism = "in-process; sequencing and the replay window from cy::net::reliability";
    return security;
}

}  // namespace cy::net
