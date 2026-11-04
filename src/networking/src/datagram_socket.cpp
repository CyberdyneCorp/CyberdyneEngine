// SPDX-License-Identifier: MIT
#include <cy/networking/datagram_socket.h>
#include <cy/networking/udp_transport.h>

#if defined(__linux__) || defined(__APPLE__)
#    define CY_NET_SOCKET_POSIX 1
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#elif defined(_WIN32)
#    define CY_NET_SOCKET_WINSOCK 1
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "Ws2_32.lib")
#endif

namespace cy::net {

// --- UdpSocket -----------------------------------------------------------------------------------

namespace {

#if defined(CY_NET_SOCKET_WINSOCK)
using NativeSocket = SOCKET;
using AddressLength = int;
using BufferLength = int;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;

[[nodiscard]] bool start_sockets() noexcept {
    // WSAStartup is reference counted; once per process is enough and teardown is the process's.
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
void close_native(NativeSocket handle) noexcept {
    (void)::closesocket(handle);
}
[[nodiscard]] bool make_nonblocking(NativeSocket handle) noexcept {
    u_long nonblocking = 1;
    if (::ioctlsocket(handle, FIONBIO, &nonblocking) != 0) {
        return false;
    }
    // Winsock reports an ICMP port-unreachable for an EARLIER send as a failure of the NEXT
    // `recvfrom` on a UDP socket (WSAECONNRESET), so one client that went away would make the
    // server's socket read as empty once per datagram it had sent there. Turn that off: a
    // connectionless socket has no connection to reset.
#    ifndef SIO_UDP_CONNRESET
#        define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#    endif
    BOOL report = FALSE;
    DWORD returned = 0;
    (void)::WSAIoctl(handle, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned,
                     nullptr, nullptr);
    return true;
}
#elif defined(CY_NET_SOCKET_POSIX)
using NativeSocket = int;
using AddressLength = socklen_t;
using BufferLength = size_t;
constexpr NativeSocket kInvalidNative = -1;

[[nodiscard]] bool start_sockets() noexcept {
    return true;
}
void close_native(NativeSocket handle) noexcept {
    (void)::close(handle);
}
[[nodiscard]] bool make_nonblocking(NativeSocket handle) noexcept {
    const int flags = ::fcntl(handle, F_GETFL, 0);
    return flags >= 0 && ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) >= 0;
}
#endif

#if defined(CY_NET_SOCKET_WINSOCK) || defined(CY_NET_SOCKET_POSIX)
[[nodiscard]] NativeSocket native(i64 handle) noexcept {
    return static_cast<NativeSocket>(handle);
}

[[nodiscard]] sockaddr_in to_native(const SocketAddress& address) noexcept {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = htonl(static_cast<u32>(address.host));
    out.sin_port = htons(address.port);
    return out;
}
#endif

}  // namespace

UdpSocket::UdpSocket(Allocator& allocator) noexcept : inbound_(allocator) {}

UdpSocket::~UdpSocket() {
    close();
}

Expected<SocketAddress, Error> UdpSocket::resolve(const char* text) const noexcept {
    const UdpAddress parsed = parse_udp_address(text);
    if (!parsed.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a UDP address is \"a.b.c.d:port\" with a non-zero port");
    }
    return SocketAddress{parsed.ipv4, parsed.port};
}

#if defined(CY_NET_SOCKET_WINSOCK) || defined(CY_NET_SOCKET_POSIX)

Status UdpSocket::open(u16 port) noexcept {
    if (is_open()) {
        return fail(ErrorCode::AlreadyExists, "networking: this socket is already bound");
    }
    if (!start_sockets()) {
        return fail(ErrorCode::Unavailable, "networking: the socket library did not start");
    }
    if (Status sized = inbound_.resize(kMaxSocketDatagram); !sized) {
        return sized;
    }
    const NativeSocket handle = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (handle == kInvalidNative) {
        return fail(ErrorCode::Unavailable, "networking: could not create a UDP socket");
    }
    if (!make_nonblocking(handle)) {
        close_native(handle);
        return fail(ErrorCode::Unavailable, "networking: could not make the socket non-blocking");
    }
    // INADDR_ANY rather than the loopback: a dedicated server bound to the loopback accepts nobody,
    // and a test connecting to 127.0.0.1 would never notice.
    const sockaddr_in local = to_native(SocketAddress{INADDR_ANY, port});
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        close_native(handle);
        return fail(ErrorCode::Unavailable, "networking: could not bind the UDP socket");
    }
    sockaddr_in bound{};
    AddressLength bound_size = sizeof(bound);
    bound_port_ = ::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_size) == 0
                      ? ntohs(bound.sin_port)
                      : port;
    handle_ = static_cast<i64>(handle);
    return ok();
}

void UdpSocket::close() noexcept {
    if (is_open()) {
        close_native(native(handle_));
        handle_ = kClosed;
    }
    bound_port_ = 0;
}

Status UdpSocket::send_to(const SocketAddress& to, Span<const u8> bytes) noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "networking: the socket is not bound");
    }
    const sockaddr_in remote = to_native(to);
    const auto written = ::sendto(native(handle_), reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<BufferLength>(bytes.size()), 0,
                                  reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    if (written < 0) {
        return fail(ErrorCode::Io, "networking: the datagram could not be sent");
    }
    return ok();
}

bool UdpSocket::receive_from(SocketAddress& from, Span<const u8>& bytes) noexcept {
    if (!is_open()) {
        return false;
    }
    sockaddr_in remote{};
    AddressLength remote_size = sizeof(remote);
    const auto read = ::recvfrom(native(handle_), reinterpret_cast<char*>(inbound_.data()),
                                 static_cast<BufferLength>(inbound_.size()), 0,
                                 reinterpret_cast<sockaddr*>(&remote), &remote_size);
    if (read <= 0) {
        return false;
    }
    from.host = ntohl(remote.sin_addr.s_addr);
    from.port = ntohs(remote.sin_port);
    bytes = Span<const u8>(inbound_.data(), static_cast<usize>(read));
    return true;
}

#else

Status UdpSocket::open(u16 /*port*/) noexcept {
    return fail(ErrorCode::Unsupported, "networking: this platform has no socket implementation");
}
void UdpSocket::close() noexcept {}
Status UdpSocket::send_to(const SocketAddress& /*to*/, Span<const u8> /*bytes*/) noexcept {
    return fail(ErrorCode::Unsupported, "networking: this platform has no socket implementation");
}
bool UdpSocket::receive_from(SocketAddress& /*from*/, Span<const u8>& /*bytes*/) noexcept {
    return false;
}

#endif

// --- SimulatedSocket -----------------------------------------------------------------------------

Expected<SocketAddress, Error> SimulatedSocket::resolve(const char* text) const noexcept {
    const PeerId host = network_->find_host(text);
    if (!host.valid()) {
        return fail(ErrorCode::NotFound, "networking: no simulated host has that name");
    }
    return SocketAddress{host.value(), 1};
}

Status SimulatedSocket::send_to(const SocketAddress& to, Span<const u8> bytes) noexcept {
    const PeerId target =
        PeerId::make(static_cast<u32>(to.host & 0xFFFF'FFFFULL), static_cast<u32>(to.host >> 32));
    return network_->offer(self_, target, now_ms_, bytes);
}

bool SimulatedSocket::receive_from(SocketAddress& from, Span<const u8>& bytes) noexcept {
    PeerId sender;
    if (!network_->take(self_, sender, bytes)) {
        return false;
    }
    from = SocketAddress{sender.value(), 1};
    return true;
}

void SimulatedSocket::advance(u64 now_ms) noexcept {
    now_ms_ = now_ms;
    network_->advance(now_ms);
}

}  // namespace cy::net
