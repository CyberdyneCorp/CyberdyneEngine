#include <cy/networking/udp_transport.h>

#include <cstring>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#    define CY_NET_HAS_POSIX_SOCKETS 1
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#else
#    define CY_NET_HAS_POSIX_SOCKETS 0
#endif

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

}  // namespace

bool udp_available() noexcept {
    return CY_NET_HAS_POSIX_SOCKETS != 0;
}

namespace {

/// Parse a dotted quad into host byte order. Separated from the address parser because the two
/// halves have nothing to do with each other and together they scored 30 on the cognitive
/// complexity metric — over the band this project accepts for anything that is not a compiler.
[[nodiscard]] bool parse_ipv4(const char* text, u32& out) noexcept {
    u32 octets[4] = {};
    u32 filled = 0;
    u32 value = 0;
    bool any_digit = false;
    for (const char* cursor = text;; ++cursor) {
        if (*cursor >= '0' && *cursor <= '9') {
            value = (value * 10) + static_cast<u32>(*cursor - '0');
            any_digit = true;
            if (value > 255) {
                return false;
            }
            continue;
        }
        if (*cursor != '.' && *cursor != '\0') {
            return false;
        }
        if (!any_digit || filled >= 4) {
            return false;
        }
        octets[filled++] = value;
        value = 0;
        any_digit = false;
        if (*cursor == '\0') {
            break;
        }
    }
    if (filled != 4) {
        return false;
    }
    // Host byte order. The conversion to network order happens at the socket boundary and nowhere
    // else, so nothing above that boundary has to know which it is holding.
    out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

/// Parse a decimal port. Zero is not a port here — it is the "any port" sentinel `open()` takes —
/// so an address that names it is refused rather than half-accepted.
[[nodiscard]] bool parse_port(const char* text, u16& out) noexcept {
    u32 port = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        port = (port * 10) + static_cast<u32>(*cursor - '0');
        if (port > 65535) {
            return false;
        }
    }
    if (port == 0) {
        return false;
    }
    out = static_cast<u16>(port);
    return true;
}

}  // namespace

UdpAddress parse_udp_address(const char* text) noexcept {
    UdpAddress address;
    if (text == nullptr) {
        return address;
    }
    const char* colon = std::strrchr(text, ':');
    if (colon == nullptr || colon == text) {
        return address;
    }
    char host[64] = {};
    const auto length = static_cast<usize>(colon - text);
    if (length == 0 || length >= sizeof(host)) {
        return address;
    }
    std::memcpy(static_cast<void*>(host), static_cast<const void*>(text), length);

    u32 ipv4 = 0;
    u16 port = 0;
    if (!parse_ipv4(host, ipv4) || !parse_port(colon + 1, port)) {
        // Either half failing leaves the address invalid rather than half-built, which is what a
        // hand-rolled parser gets wrong first.
        return address;
    }
    address.ipv4 = ipv4;
    address.port = port;
    return address;
}

UdpTransport::UdpTransport(Allocator& allocator) noexcept
    : allocator_(&allocator), links_(allocator), outgoing_(allocator), inbound_(allocator) {}

UdpTransport::~UdpTransport() {
    close();
    for (auto& link : links_) {
        unmake(*allocator_, link);
    }
}

#if CY_NET_HAS_POSIX_SOCKETS

Status UdpTransport::open(u16 port) noexcept {
    if (socket_ >= 0) {
        return fail(ErrorCode::AlreadyExists, "networking: this transport is already bound");
    }
    const int handle = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (handle < 0) {
        return fail(ErrorCode::Unavailable, "networking: could not create a UDP socket");
    }
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(handle);
        return fail(ErrorCode::Unavailable, "networking: could not make the socket non-blocking");
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    // INADDR_ANY, not the loopback. A dedicated server that bound the loopback would accept nobody,
    // and the tests here would still pass — which is the shape of defect that ships. The tests
    // connect to 127.0.0.1 either way.
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) {
        ::close(handle);
        return fail(ErrorCode::Unavailable, "networking: could not bind the UDP socket");
    }
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_size) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    } else {
        bound_port_ = port;
    }
    socket_ = handle;
    if (Status sized = inbound_.resize(kMaxWireDatagram); !sized) {
        close();
        return sized;
    }
    return ok();
}

void UdpTransport::close() noexcept {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    bound_port_ = 0;
}

Status UdpTransport::transmit(const UdpAddress& address, Span<const u8> wire) noexcept {
    if (socket_ < 0) {
        return fail(ErrorCode::Unavailable, "networking: the transport is not bound");
    }
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = htonl(address.ipv4);
    remote.sin_port = htons(address.port);
    const ssize_t written = ::sendto(socket_, wire.data(), wire.size(), 0,
                                     reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    if (written < 0) {
        return fail(ErrorCode::Io, "networking: the datagram could not be sent");
    }
    ++sent_;
    return ok();
}

bool UdpTransport::pump() noexcept {
    if (socket_ < 0) {
        return false;
    }
    sockaddr_in from{};
    socklen_t from_size = sizeof(from);
    const ssize_t read = ::recvfrom(socket_, inbound_.data(), inbound_.size(), 0,
                                    reinterpret_cast<sockaddr*>(&from), &from_size);
    if (read <= 0) {
        return false;
    }
    ++received_;
    UdpAddress address;
    address.ipv4 = ntohl(from.sin_addr.s_addr);
    address.port = ntohs(from.sin_port);
    Link* link = find(address);
    if (link == nullptr) {
        // Unsolicited. Counted and dropped — see the header's note on why answering one is the
        // cheapest amplification there is.
        ++strangers_;
        return true;
    }
    link->stats.datagrams_in += 1;
    link->stats.bytes_in += static_cast<u64>(read);
    ChannelId channel = 0;
    ReceiveVerdict verdict = ReceiveVerdict::Delivered;
    if (!link->endpoint.ingest(Span<const u8>(inbound_.data(), static_cast<usize>(read)), channel,
                               verdict)) {
        return true;
    }
    if (verdict == ReceiveVerdict::Duplicate) {
        link->stats.datagrams_duplicate += 1;
    }
    return true;
}

#else

Status UdpTransport::open(u16) noexcept {
    return fail(ErrorCode::Unsupported,
                "networking: this build has no socket implementation; Windows is unverified and "
                "reported so rather than stubbed");
}

void UdpTransport::close() noexcept {
    socket_ = -1;
}

Status UdpTransport::transmit(const UdpAddress&, Span<const u8>) noexcept {
    return fail(ErrorCode::Unsupported, "networking: this build has no socket implementation");
}

bool UdpTransport::pump() noexcept {
    return false;
}

#endif

UdpTransport::Link* UdpTransport::find(PeerId peer) noexcept {
    for (auto& link : links_) {
        if (link->remote == peer) {
            return link;
        }
    }
    return nullptr;
}

const UdpTransport::Link* UdpTransport::find(PeerId peer) const noexcept {
    for (auto* link : links_) {
        if (link->remote == peer) {
            return link;
        }
    }
    return nullptr;
}

UdpTransport::Link* UdpTransport::find(const UdpAddress& address) noexcept {
    for (auto& link : links_) {
        if (link->address.ipv4 == address.ipv4 && link->address.port == address.port) {
            return link;
        }
    }
    return nullptr;
}

Expected<UdpTransport::Link*, Error> UdpTransport::open_link(const UdpAddress& address) noexcept {
    if (Link* existing = find(address); existing != nullptr) {
        return existing;
    }
    const PeerId peer = PeerId::make(next_slot_++, 1);
    Link* link = make<Link>(*allocator_, *allocator_, peer, address);
    if (link == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a link");
    }
    if (Status ready = link->endpoint.prepare(); !ready) {
        unmake(*allocator_, link);
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate the link's channels");
    }
    if (!links_.push_back(link)) {
        unmake(*allocator_, link);
        return fail(ErrorCode::OutOfMemory, "networking: could not register a link");
    }
    return link;
}

Expected<PeerId, Error> UdpTransport::connect(const char* address) noexcept {
    const UdpAddress parsed = parse_udp_address(address);
    if (!parsed.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a UDP address is \"a.b.c.d:port\" with a non-zero port");
    }
    return accept(parsed);
}

Expected<PeerId, Error> UdpTransport::accept(const UdpAddress& address) noexcept {
    Expected<Link*, Error> link = open_link(address);
    if (!link) {
        return make_unexpected(link.error());
    }
    return link.value()->remote;
}

void UdpTransport::disconnect(PeerId peer) noexcept {
    if (Link* link = find(peer); link != nullptr) {
        link->state = ConnectionState::Disconnected;
    }
}

Status UdpTransport::send(PeerId peer, ChannelId channel, DeliveryMode delivery,
                          Span<const u8> bytes) noexcept {
    Link* link = find(peer);
    if (link == nullptr) {
        return fail(ErrorCode::NotFound, "networking: no link to that peer");
    }
    if (link->state != ConnectionState::Connected) {
        return fail(ErrorCode::Unavailable, "networking: the peer is not connected");
    }
    outgoing_.clear();
    if (Status framed = link->endpoint.frame(channel, delivery, bytes, now_ms_, outgoing_);
        !framed) {
        return framed;
    }
    link->stats.datagrams_out += 1;
    link->stats.bytes_out += outgoing_.size();
    return transmit(link->address, outgoing_.span());
}

void UdpTransport::flush(u64 now_ms) noexcept {
    for (auto* held : links_) {
        Link& link = *held;
        if (link.state != ConnectionState::Connected) {
            continue;
        }
        outgoing_.clear();
        if (!link.endpoint.collect_outgoing(now_ms, outgoing_)) {
            continue;
        }
        usize cursor = 0;
        while (cursor + kDatagramHeaderSize <= outgoing_.size()) {
            DatagramHeader header;
            const Span<const u8> rest(outgoing_.data() + cursor, outgoing_.size() - cursor);
            if (!decode_header(rest, header)) {
                break;
            }
            const usize length = kDatagramHeaderSize + header.payload_size;
            (void)transmit(link.address, Span<const u8>(outgoing_.data() + cursor, length));
            link.stats.datagrams_out += 1;
            link.stats.bytes_out += length;
            cursor += length;
        }
        link.stats.retransmissions = link.endpoint.retransmissions();
    }
}

void UdpTransport::advance(u64 now_ms) noexcept {
    now_ms_ = now_ms;
    flush(now_ms);
    // Drain the socket until it is empty. A bounded loop would leave datagrams for the next tick,
    // which under load is a queue that never empties.
    while (pump()) {
    }
}

bool UdpTransport::drain(Datagram& out) noexcept {
    const usize slots = links_.size() * kMaxChannels;
    for (usize attempt = 0; attempt < slots; ++attempt) {
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

bool UdpTransport::receive(Datagram& out) noexcept {
    if (drain(out)) {
        return true;
    }
    while (pump()) {
        if (drain(out)) {
            return true;
        }
    }
    return false;
}

ConnectionState UdpTransport::state(PeerId peer) const noexcept {
    const Link* link = find(peer);
    return link == nullptr ? ConnectionState::Disconnected : link->state;
}

ConnectionStats UdpTransport::stats(PeerId peer) const noexcept {
    const Link* link = find(peer);
    return link == nullptr ? ConnectionStats{} : link->stats;
}

TransportSecurity UdpTransport::security() const noexcept {
    TransportSecurity security;
    // Said plainly. `admissible_for(security, Deployment::Shipping)` therefore returns
    // `NotEncrypted`, which is the specification's requirement as a refusal.
    security.encrypted = false;
    security.authenticated = false;
    security.replay_protected = true;
    security.sequence_validated = true;
    security.in_process = false;
    security.mechanism = "sequencing and the replay window from cy::net::reliability; no DTLS yet";
    return security;
}

}  // namespace cy::net
