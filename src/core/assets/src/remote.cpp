// SPDX-License-Identifier: MIT
// The remote mount's transport. See cy/core/assets/remote.h for the protocol and for why the
// socket half is POSIX only.

#include <cy/core/assets/remote.h>

#include <bit>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
#    define CY_ASSETS_POSIX_SOCKETS 1
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <poll.h>
#    include <sys/socket.h>
#    include <unistd.h>
#    include <cerrno>
#else
#    define CY_ASSETS_POSIX_SOCKETS 0
#endif

namespace cy::assets {
namespace {

static_assert(std::endian::native == std::endian::little,
              "the remote protocol is little-endian by definition, and nothing here has run on a "
              "big-endian host; byte-swapping unverified would be worse than refusing to compile");

/// The three operations, which are `RemoteFileProvider`'s three methods and nothing else.
enum class RemoteOp : u16 {
    Stat = 1,
    Fetch = 2,
    List = 3,
};

/// The fixed header every message begins with, in both directions. Packed by hand into a byte
/// buffer rather than memcpy'd as a struct: a struct's padding is the compiler's business and a
/// wire format is not.
constexpr usize kHeaderBytes = 32;

void put_u16(u8* out, u16 value) noexcept {
    std::memcpy(out, &value, sizeof(value));
}
void put_u32(u8* out, u32 value) noexcept {
    std::memcpy(out, &value, sizeof(value));
}
void put_u64(u8* out, u64 value) noexcept {
    std::memcpy(out, &value, sizeof(value));
}
[[nodiscard]] u16 get_u16(const u8* in) noexcept {
    u16 value = 0;
    std::memcpy(&value, in, sizeof(value));
    return value;
}
[[nodiscard]] u32 get_u32(const u8* in) noexcept {
    u32 value = 0;
    std::memcpy(&value, in, sizeof(value));
    return value;
}
[[nodiscard]] u64 get_u64(const u8* in) noexcept {
    u64 value = 0;
    std::memcpy(&value, in, sizeof(value));
    return value;
}

struct Header {
    u32 magic = kRemoteFileMagic;
    u16 version = kRemoteFileProtocolVersion;
    u16 op = 0;
    u32 status = 0;
    u16 path_length = 0;
    u16 flags = 0;
    u64 a = 0;
    u64 b = 0;
};

void encode(const Header& header, u8* out) noexcept {
    put_u32(out + 0, header.magic);
    put_u16(out + 4, header.version);
    put_u16(out + 6, header.op);
    put_u32(out + 8, header.status);
    put_u16(out + 12, header.path_length);
    put_u16(out + 14, header.flags);
    put_u64(out + 16, header.a);
    put_u64(out + 24, header.b);
}

[[nodiscard]] Header decode(const u8* in) noexcept {
    Header header;
    header.magic = get_u32(in + 0);
    header.version = get_u16(in + 4);
    header.op = get_u16(in + 6);
    header.status = get_u32(in + 8);
    header.path_length = get_u16(in + 12);
    header.flags = get_u16(in + 14);
    header.a = get_u64(in + 16);
    header.b = get_u64(in + 24);
    return header;
}

/// The error codes this protocol can carry, and the literal each becomes on the far side.
///
/// `Error::message` is a `const char*` the caller may keep, so it cannot be a string that arrived
/// over a socket. The code crosses the wire; the sentence is the receiver's own.
[[nodiscard]] u32 code_of(ErrorCode code) noexcept {
    return static_cast<u32>(code);
}

[[nodiscard]] Error error_from_wire(u32 code) noexcept {
    switch (static_cast<ErrorCode>(code)) {
        case ErrorCode::NotFound:
            return Error{ErrorCode::NotFound, "the host does not have that file", 0};
        case ErrorCode::OutOfRange:
            return Error{ErrorCode::OutOfRange, "that range lies past the end of the host's file",
                         0};
        case ErrorCode::InvalidArgument:
            return Error{ErrorCode::InvalidArgument, "the host refused that request as malformed",
                         0};
        case ErrorCode::PermissionDenied:
            return Error{ErrorCode::PermissionDenied, "the host refused that path", 0};
        case ErrorCode::Io:
            return Error{ErrorCode::Io, "the host could not read that file", 0};
        default:
            // Anything else is reported as what it is rather than mapped onto a code this end
            // understands: a client that says "Io" for a code it did not recognise is a client that
            // hides a protocol change.
            return Error{ErrorCode::Internal, "the host reported an error this build does not name",
                         static_cast<i64>(code)};
    }
}

#if CY_ASSETS_POSIX_SOCKETS

/// Loop until every byte is written. A stream socket may take fewer than it was given, and treating
/// that as an error is how a transport comes to work on the loopback and fail on a network.
[[nodiscard]] bool write_all(int socket, const void* bytes, usize size) noexcept {
    const auto* cursor = static_cast<const u8*>(bytes);
    usize remaining = size;
    while (remaining > 0) {
        const ssize_t written = ::send(socket, cursor, remaining, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += written;
        remaining -= static_cast<usize>(written);
    }
    return true;
}

/// Loop until every byte is read. Returns false at end of stream, which is a peer that closed.
[[nodiscard]] bool read_all(int socket, void* bytes, usize size) noexcept {
    auto* cursor = static_cast<u8*>(bytes);
    usize remaining = size;
    while (remaining > 0) {
        const ssize_t got = ::recv(socket, cursor, remaining, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            return false;
        }
        cursor += got;
        remaining -= static_cast<usize>(got);
    }
    return true;
}

void close_socket(int& socket) noexcept {
    if (socket >= 0) {
        (void)::close(socket);
        socket = -1;
    }
}

#endif  // CY_ASSETS_POSIX_SOCKETS

#if !CY_ASSETS_POSIX_SOCKETS
/// Only ever reached on a platform with no socket layer; on one that has it, every caller is
/// compiled out and so is this.
[[nodiscard]] Status unsupported_here() noexcept {
    return fail(ErrorCode::Unsupported,
                "this build has no socket layer, so no file can be served or fetched over one; "
                "see the note on Windows in cy/core/assets/remote.h");
}
#endif

}  // namespace

bool remote_serving_available() noexcept {
    return CY_ASSETS_POSIX_SOCKETS != 0;
}

RemoteAddress parse_remote_address(const char* text) noexcept {
    RemoteAddress address;
    if (text == nullptr) {
        return address;
    }
    u32 octets[4] = {};
    u32 value = 0;
    u32 digits = 0;
    u32 index = 0;
    const char* cursor = text;
    for (; *cursor != '\0' && *cursor != ':'; ++cursor) {
        if (*cursor == '.') {
            if (digits == 0 || index >= 3 || value > 255) {
                return RemoteAddress{};
            }
            octets[index++] = value;
            value = 0;
            digits = 0;
            continue;
        }
        if (*cursor < '0' || *cursor > '9') {
            return RemoteAddress{};
        }
        value = (value * 10) + static_cast<u32>(*cursor - '0');
        ++digits;
        if (digits > 3) {
            return RemoteAddress{};
        }
    }
    if (*cursor != ':' || digits == 0 || index != 3 || value > 255) {
        return RemoteAddress{};
    }
    octets[3] = value;

    u32 port = 0;
    ++cursor;
    if (*cursor == '\0') {
        return RemoteAddress{};
    }
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return RemoteAddress{};
        }
        port = (port * 10) + static_cast<u32>(*cursor - '0');
        if (port > 65535) {
            return RemoteAddress{};
        }
    }
    if (port == 0) {
        return RemoteAddress{};
    }

    address.ipv4 = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    address.port = static_cast<u16>(port);
    return address;
}

// --- SocketFileProvider --------------------------------------------------------------------------

SocketFileProvider::SocketFileProvider(Allocator& allocator) noexcept : scratch_(allocator) {}

SocketFileProvider::~SocketFileProvider() {
    disconnect();
}

void SocketFileProvider::disconnect() noexcept {
#if CY_ASSETS_POSIX_SOCKETS
    close_socket(socket_);
#endif
}

Status SocketFileProvider::connect(const char* address_text) noexcept {
#if CY_ASSETS_POSIX_SOCKETS
    const RemoteAddress address = parse_remote_address(address_text);
    if (!address.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a host address is \"a.b.c.d:port\" with a non-zero port");
    }
    disconnect();

    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0) {
        return fail(ErrorCode::Io, "a socket could not be created", errno);
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(address.port);
    target.sin_addr.s_addr = htonl(address.ipv4);
    if (::connect(handle, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0) {
        const int failure = errno;
        int scoped = handle;
        close_socket(scoped);
        return fail(ErrorCode::Unavailable, "the host did not accept the connection", failure);
    }

    // Every message is one request and one reply, so Nagle's algorithm has nothing to coalesce and
    // costs a round trip's worth of latency per fetch.
    int one = 1;
    (void)::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    socket_ = handle;
    return ok();
#else
    (void)address_text;
    return unsupported_here();
#endif
}

Status SocketFileProvider::send_all(const void* bytes, usize size) noexcept {
#if CY_ASSETS_POSIX_SOCKETS
    if (!write_all(socket_, bytes, size)) {
        return transport_failure("the connection to the host failed while sending a request");
    }
    return ok();
#else
    (void)bytes;
    (void)size;
    return unsupported_here();
#endif
}

Status SocketFileProvider::receive_all(void* bytes, usize size) noexcept {
#if CY_ASSETS_POSIX_SOCKETS
    if (!read_all(socket_, bytes, size)) {
        return transport_failure("the connection to the host failed while reading a reply");
    }
    return ok();
#else
    (void)bytes;
    (void)size;
    return unsupported_here();
#endif
}

Status SocketFileProvider::transport_failure(const char* message) noexcept {
    ++stats_.transport_failures;
    disconnect();
    return fail(ErrorCode::Unavailable, message);
}

Status SocketFileProvider::exchange(u16 op, const VirtualPath& path, u64 argument_a, u64 argument_b,
                                    u64& answer) noexcept {
    if (!is_connected()) {
        return fail(ErrorCode::Unavailable, "this provider is not connected to a host");
    }

    u8 header_bytes[kHeaderBytes] = {};
    Header request;
    request.op = op;
    request.path_length = static_cast<u16>(path.size());
    request.a = argument_a;
    request.b = argument_b;
    encode(request, header_bytes);

    if (Status sent = send_all(header_bytes, kHeaderBytes); !sent) {
        return sent;
    }
    if (path.size() != 0) {
        if (Status sent = send_all(path.c_str(), path.size()); !sent) {
            return sent;
        }
    }

    if (Status got = receive_all(header_bytes, kHeaderBytes); !got) {
        return got;
    }
    const Header reply = decode(header_bytes);
    if (reply.magic != kRemoteFileMagic || reply.version != kRemoteFileProtocolVersion) {
        return transport_failure(
            "the host answered with a message this build does not recognise; the two ends are not "
            "speaking the same protocol version");
    }
    if (reply.op != op) {
        return transport_failure(
            "the host answered a different request from the one that was sent");
    }
    if (reply.status != code_of(ErrorCode::None)) {
        ++stats_.refused;
        // The payload of a refusal is empty by construction, so there is nothing to drain: the next
        // request reads the next reply.
        return make_unexpected(error_from_wire(reply.status));
    }
    answer = reply.a;
    return ok();
}

Expected<u64, Error> SocketFileProvider::stat(const VirtualPath& path) noexcept {
    ++stats_.stat_requests;
    u64 size = 0;
    if (Status asked = exchange(static_cast<u16>(RemoteOp::Stat), path, 0, 0, size); !asked) {
        return make_unexpected(asked.error());
    }
    return size;
}

Status SocketFileProvider::fetch(const VirtualPath& path, u64 offset, void* destination,
                                 usize size) noexcept {
    ++stats_.fetch_requests;
    if (destination == nullptr && size != 0) {
        return fail(ErrorCode::InvalidArgument, "a fetch needs somewhere to put the bytes");
    }
    if (size > kRemoteMaxFetchBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "that fetch is larger than one request may carry; read it in ranges");
    }

    u64 answered = 0;
    if (Status asked = exchange(static_cast<u16>(RemoteOp::Fetch), path, offset,
                                static_cast<u64>(size), answered);
        !asked) {
        return asked;
    }
    if (answered != size) {
        return transport_failure("the host answered a fetch with a different number of bytes");
    }
    if (size == 0) {
        return ok();
    }
    if (Status got = receive_all(destination, size); !got) {
        return got;
    }
    stats_.bytes_fetched += size;
    return ok();
}

Status SocketFileProvider::list(const VirtualPath& directory, bool recursive,
                                VirtualVisitor visitor, void* user) noexcept {
    ++stats_.list_requests;
    if (visitor == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a listing needs a visitor");
    }

    u64 entries = 0;
    if (Status asked =
            exchange(static_cast<u16>(RemoteOp::List), directory, recursive ? 1U : 0U, 0, entries);
        !asked) {
        return asked;
    }

    for (u64 index = 0; index < entries; ++index) {
        u8 record[11] = {};
        if (Status got = receive_all(record, sizeof(record)); !got) {
            return got;
        }
        const u16 length = get_u16(record);
        const bool is_directory = record[2] != 0;
        const u64 size = get_u64(record + 3);
        if (length == 0 || length > kMaxPathLength) {
            return transport_failure("the host listed an entry whose path cannot be a path");
        }

        char text[kMaxPathLength + 1] = {};
        if (Status got = receive_all(text, length); !got) {
            return got;
        }
        // Normalised on arrival, exactly as the host normalises what it receives: a path from a
        // socket is untrusted in both directions, and the visitor is handed a `VirtualPath` that
        // has been through the same check every other mount's paths have.
        Expected<VirtualPath, Error> path = VirtualPath::normalise(std::string_view(text, length));
        if (!path) {
            return transport_failure("the host listed a path that does not normalise");
        }

        VirtualEntry entry;
        entry.path = &path.value();
        entry.is_directory = is_directory;
        entry.size = size;
        if (!visitor(user, entry)) {
            // The visitor stopped early. The rest of the listing is still on the wire, and leaving
            // it there would desynchronise every later request, so it is drained rather than
            // abandoned.
            for (u64 rest = index + 1; rest < entries; ++rest) {
                u8 skipped[11] = {};
                if (Status got = receive_all(skipped, sizeof(skipped)); !got) {
                    return got;
                }
                const u16 skip_length = get_u16(skipped);
                if (skip_length > kMaxPathLength) {
                    return transport_failure(
                        "the host listed an entry whose path cannot be a path");
                }
                char discard[kMaxPathLength + 1] = {};
                if (Status got = receive_all(discard, skip_length); !got) {
                    return got;
                }
            }
            return ok();
        }
    }
    return ok();
}

// --- FileServingHost -----------------------------------------------------------------------------

FileServingHost::FileServingHost(VirtualFileSystem& files, Allocator& allocator) noexcept
    : files_(&files), clients_(allocator), scratch_(allocator) {}

FileServingHost::~FileServingHost() {
    close();
}

void FileServingHost::close() noexcept {
#if CY_ASSETS_POSIX_SOCKETS
    for (int& client : clients_) {
        close_socket(client);
        ++stats_.connections_closed;
    }
    clients_.clear();
    close_socket(listener_);
    bound_port_ = 0;
#endif
}

Status FileServingHost::open(u16 port, bool loopback_only) noexcept {
#if CY_ASSETS_POSIX_SOCKETS
    close();

    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0) {
        return fail(ErrorCode::Io, "a listening socket could not be created", errno);
    }
    int one = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int failure = errno;
        int scoped = handle;
        close_socket(scoped);
        return fail(ErrorCode::Unavailable, "that port could not be bound", failure);
    }
    // THE LISTENER IS NON-BLOCKING, and this is not an optimisation. `accept_pending()` drains
    // every waiting connection in a loop, and on a blocking listener the call that finds nothing
    // waiting does not return — it parks the serving thread until somebody else connects, which is
    // a host that answers one client and then wedges. The loop and this flag are one mechanism.
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0) {
        const int failure = errno;
        int scoped = handle;
        close_socket(scoped);
        return fail(ErrorCode::Io, "the listening socket could not be made non-blocking", failure);
    }

    if (::listen(handle, 8) != 0) {
        const int failure = errno;
        int scoped = handle;
        close_socket(scoped);
        return fail(ErrorCode::Io, "the socket could not be listened on", failure);
    }

    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        const int failure = errno;
        int scoped = handle;
        close_socket(scoped);
        return fail(ErrorCode::Io, "the bound port could not be read back", failure);
    }

    listener_ = handle;
    bound_port_ = ntohs(bound.sin_port);
    return ok();
#else
    (void)port;
    (void)loopback_only;
    return unsupported_here();
#endif
}

#if CY_ASSETS_POSIX_SOCKETS

Status FileServingHost::accept_pending() noexcept {
    for (;;) {
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            // EAGAIN is "nothing waiting", which is the ordinary outcome of a poll round in which
            // the listener was not the socket that was ready.
            return ok();
        }
        int one = 1;
        (void)::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // A read deadline on the client, because `serve_one` reads a whole message once `poll` says
        // one has begun to arrive. A peer that sends half a header and stops would otherwise hold
        // the serving thread for ever — which is a denial of service that costs the client nothing,
        // and the loopback is not a reason to leave it open.
        timeval deadline{};
        deadline.tv_sec = 5;
        (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
        if (Status added = clients_.push_back(client); !added) {
            int scoped = client;
            close_socket(scoped);
            return added;
        }
        ++stats_.connections_accepted;
    }
}

void FileServingHost::drop(usize index) noexcept {
    close_socket(clients_[index]);
    ++stats_.connections_closed;
    clients_.erase(index);
}

bool FileServingHost::serve_one(int socket, u32& served) noexcept {
    u8 header_bytes[kHeaderBytes] = {};
    if (!read_all(socket, header_bytes, kHeaderBytes)) {
        return false;  // the peer closed, or the connection failed; the same outcome either way
    }
    const Header request = decode(header_bytes);
    if (request.magic != kRemoteFileMagic || request.version != kRemoteFileProtocolVersion ||
        request.path_length > kMaxPathLength) {
        ++stats_.protocol_errors;
        return false;
    }

    char text[kMaxPathLength + 1] = {};
    if (request.path_length != 0 && !read_all(socket, text, request.path_length)) {
        return false;
    }

    Header reply;
    reply.op = request.op;
    Array<u8>& payload = scratch_;
    payload.clear();

    // THE TRAVERSAL RULE, ENFORCED ON ARRIVAL. A path from a socket has not been through
    // `VirtualPath::normalise`, whatever the client believes it sent, and this is the one place
    // that can be true of.
    Expected<VirtualPath, Error> path =
        VirtualPath::normalise(std::string_view(text, request.path_length));
    if (!path) {
        ++stats_.paths_rejected;
        ++stats_.requests_refused;
        reply.status = code_of(ErrorCode::InvalidArgument);
    } else {
        switch (static_cast<RemoteOp>(request.op)) {
            case RemoteOp::Stat: {
                Expected<u64, Error> size = files_->size_of(path.value());
                if (!size) {
                    ++stats_.requests_refused;
                    reply.status = code_of(size.error().code);
                } else {
                    reply.a = size.value();
                }
                break;
            }
            case RemoteOp::Fetch: {
                const u64 wanted = request.b;
                if (wanted > kRemoteMaxFetchBytes) {
                    ++stats_.requests_refused;
                    reply.status = code_of(ErrorCode::InvalidArgument);
                    break;
                }
                if (Status sized = payload.resize(static_cast<usize>(wanted)); !sized) {
                    ++stats_.requests_refused;
                    reply.status = code_of(ErrorCode::OutOfMemory);
                    break;
                }
                Status read = wanted == 0 ? ok()
                                          : files_->read_range(path.value(), request.a,
                                                               payload.data(), payload.size());
                if (!read) {
                    ++stats_.requests_refused;
                    reply.status = code_of(read.error().code);
                    payload.clear();
                } else {
                    reply.a = wanted;
                    reply.b = wanted;
                    stats_.bytes_served += wanted;
                }
                break;
            }
            case RemoteOp::List: {
                struct Listing {
                    Array<u8>* out;
                    u32 count;
                    bool truncated;
                    bool failed;
                } listing{&payload, 0, false, false};

                const Status walked = files_->enumerate(
                    path.value(), request.a != 0,
                    [](void* user, const VirtualEntry& entry) noexcept -> bool {
                        auto* state = static_cast<Listing*>(user);
                        if (state->count >= kRemoteMaxListEntries) {
                            state->truncated = true;
                            return false;
                        }
                        const usize length = entry.path->size();
                        u8 record[11] = {};
                        put_u16(record, static_cast<u16>(length));
                        record[2] = entry.is_directory ? 1U : 0U;
                        put_u64(record + 3, entry.size);
                        for (const u8 byte : record) {
                            if (!state->out->push_back(byte)) {
                                state->failed = true;
                                return false;
                            }
                        }
                        const auto* bytes = reinterpret_cast<const u8*>(entry.path->c_str());
                        for (usize index = 0; index < length; ++index) {
                            if (!state->out->push_back(bytes[index])) {
                                state->failed = true;
                                return false;
                            }
                        }
                        ++state->count;
                        return true;
                    },
                    &listing);

                if (listing.truncated) {
                    ++stats_.listings_truncated;
                }
                if (listing.failed) {
                    ++stats_.requests_refused;
                    reply.status = code_of(ErrorCode::OutOfMemory);
                    payload.clear();
                } else if (!walked) {
                    ++stats_.requests_refused;
                    reply.status = code_of(walked.error().code);
                    payload.clear();
                } else {
                    reply.a = listing.count;
                    reply.b = payload.size();
                }
                break;
            }
            default:
                ++stats_.protocol_errors;
                return false;
        }
    }

    if (reply.status != code_of(ErrorCode::None)) {
        payload.clear();
        reply.a = 0;
        reply.b = 0;
    }

    encode(reply, header_bytes);
    if (!write_all(socket, header_bytes, kHeaderBytes)) {
        return false;
    }
    if (!payload.empty() && !write_all(socket, payload.data(), payload.size())) {
        return false;
    }
    ++stats_.requests_served;
    ++served;
    return true;
}

Expected<u32, Error> FileServingHost::serve(u32 timeout_ms) noexcept {
    if (listener_ < 0) {
        return fail(ErrorCode::Unavailable, "this host is not open");
    }

    // One descriptor for the listener and one per client, on the stack: a serving round that
    // allocated would allocate on every tick of an editor that is serving nothing.
    constexpr usize kMaxWatched = 32;
    pollfd watched[kMaxWatched] = {};
    usize count = 0;
    watched[count].fd = listener_;
    watched[count].events = POLLIN;
    ++count;
    for (const int client : clients_) {
        if (count == kMaxWatched) {
            break;
        }
        watched[count].fd = client;
        watched[count].events = POLLIN;
        ++count;
    }

    const int ready = ::poll(watched, static_cast<nfds_t>(count), static_cast<int>(timeout_ms));
    if (ready < 0) {
        if (errno == EINTR) {
            return u32{0};
        }
        return fail(ErrorCode::Io, "the serving round could not wait on its sockets", errno);
    }
    if (ready == 0) {
        return u32{0};
    }

    u32 served = 0;
    if ((watched[0].revents & POLLIN) != 0) {
        if (Status accepted = accept_pending(); !accepted) {
            return make_unexpected(accepted.error());
        }
    }

    // Walked backwards so that dropping a connection does not move an index this loop has yet to
    // reach. `clients_` may have grown since the poll — a connection accepted above has no events
    // yet, and `watched` is indexed by the order at poll time, so only the first `count - 1`
    // clients are examined here.
    for (usize index = count - 1; index >= 1; --index) {
        const short events = watched[index].revents;
        if (events == 0) {
            continue;
        }
        const int socket = watched[index].fd;
        usize slot = clients_.size();
        for (usize search = 0; search < clients_.size(); ++search) {
            if (clients_[search] == socket) {
                slot = search;
                break;
            }
        }
        if (slot == clients_.size()) {
            continue;
        }
        if ((events & (POLLHUP | POLLERR | POLLNVAL)) != 0 || !serve_one(socket, served)) {
            drop(slot);
        }
    }
    return served;
}

#else  // CY_ASSETS_POSIX_SOCKETS

Status FileServingHost::accept_pending() noexcept {
    return unsupported_here();
}

void FileServingHost::drop(usize index) noexcept {
    (void)index;
}

bool FileServingHost::serve_one(int socket, u32& served) noexcept {
    (void)socket;
    (void)served;
    return false;
}

Expected<u32, Error> FileServingHost::serve(u32 timeout_ms) noexcept {
    (void)timeout_ms;
    return make_unexpected(unsupported_here().error());
}

#endif  // CY_ASSETS_POSIX_SOCKETS

}  // namespace cy::assets
