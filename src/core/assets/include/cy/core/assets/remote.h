#ifndef CY_CORE_ASSETS_REMOTE_H
#define CY_CORE_ASSETS_REMOTE_H
// The transport behind the remote mount: a host machine serving files to a device. M11.d task 6.1.
//
// `core-assets-and-io` — "Virtual filesystem", the remote mount row, and its scenario in full:
// "**WHEN** a device runs with a remote mount configured, **THEN** assets SHALL be fetched from the
// host machine on demand, so iteration does not require repackaging."
//
// M1 wrote the seam and said so in as many words — `vfs.h`: "THE TRANSPORT IS NOT HERE: there is no
// socket, no protocol and no host discovery at M1, because none of them can be tested on one
// machine and a protocol nobody has spoken is a protocol that is wrong." Ten milestones later the
// only implementation of `RemoteFileProvider` in the tree was still `FakeHost` in
// `tests/test_vfs.cpp`, so the scenario was satisfied by a test double and by nothing a device
// could run. This file is the transport, and the reason it can be written now is the reason M1 gave
// for not writing it then: **both ends run on one machine over the loopback**, which is a real
// socket, a real protocol and a test that fails when either is wrong.
//
// --- WHAT THIS IS FOR, AND WHAT IT IS NOT -------------------------------------------------------
//
// It is a DEVELOPMENT transport. A developer's machine serves its project directory; a device
// mounts it at `mount_priority::kRemote`, above the packages and below the project, and reads what
// it needs when it needs it. Editing a texture and pressing play costs one fetch, not one cook and
// one deploy.
//
// It is therefore **not** authenticated and **not** encrypted, and that is a property of the thing
// rather than an omission to be discovered later. `FileServingHost::open()` binds the LOOPBACK by
// default and taking a route off it is an explicit argument, so serving a project directory to a
// local network is a decision somebody makes rather than a default they inherit. A shipping build
// has no reason to mount one; nothing here is on a shipping path.
//
// --- ONE ARCHITECTURE, ONE OPERATING SYSTEM -----------------------------------------------------
//
// The socket half is POSIX and is compiled on Linux and macOS, exactly as `net::UdpTransport`'s is.
// On every other platform `connect()` and `open()` return `Unsupported` rather than failing to
// link, `remote_serving_available()` answers false, and **Windows is reported unverified** — this
// machine has one operating system and nothing here has run on another. The Windows implementation
// is WinSock with the same protocol; it is deliberately not written blind.
//
// --- THE PROTOCOL, IN ONE PARAGRAPH -------------------------------------------------------------
//
// Three operations, because `RemoteFileProvider` has three: STAT a path, FETCH a byte range, LIST a
// directory. Every message is a fixed 32-byte header followed by a variable tail, little-endian,
// with a magic and a version in every request and every response so that two ends that disagree say
// so on the first message rather than by misreading the fourth. A response carries an `ErrorCode`
// as a number and no message: `Error::message` is a `const char*` that must outlive the call, so a
// string from the wire could not be one and the client supplies its own literal for the code it
// received.
//
// **EVERY PATH THAT ARRIVES IS RE-NORMALISED THROUGH `VirtualPath`.** A path from a socket is
// untrusted input, and `VirtualPath::normalise` is the one place the traversal rule is enforced —
// so the host cannot be talked into serving `../../etc/passwd` by a client that did not construct
// its path the way the mount does. The server also caps the bytes one FETCH may ask for, because a
// request for four gigabytes is a denial of service that costs the client one message.

#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::assets {

/// True when this build has a socket implementation. False on Windows, where `open()` and
/// `connect()` refuse rather than pretending.
[[nodiscard]] bool remote_serving_available() noexcept;

/// "CYRF" — Cyberdyne remote files. First four bytes of every message in both directions.
inline constexpr u32 kRemoteFileMagic = 0x43'59'52'46U;
/// Bumped when the wire format changes in a way an older end would misread.
inline constexpr u16 kRemoteFileProtocolVersion = 1;

/// The largest payload one FETCH may ask for. A mount reads an asset in one call, so this is also
/// the largest file this transport serves in a single request; a larger one is fetched in ranges,
/// which is what `RemoteMount` does when the asset system asks for part of an entry.
inline constexpr usize kRemoteMaxFetchBytes = usize{8} * 1024 * 1024;

/// The largest number of entries one LIST answers with. A directory with more is reported as far as
/// this and then truncated — `RemoteHostStats::listings_truncated` counts it, because a listing
/// that quietly stopped is how a file comes to look as if it were not there.
inline constexpr u32 kRemoteMaxListEntries = 4096;

/// An address, parsed. IPv4 only, for `net::UdpAddress`'s reason: an IPv6 path would be a second
/// code path with no test on this host.
struct RemoteAddress {
    u32 ipv4 = 0;
    u16 port = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return port != 0; }
};

/// Parse "a.b.c.d:port". Returns an invalid address on anything else, rather than a partial one.
[[nodiscard]] RemoteAddress parse_remote_address(const char* text) noexcept;

struct RemoteClientStats {
    u64 stat_requests = 0;
    u64 fetch_requests = 0;
    u64 list_requests = 0;
    u64 bytes_fetched = 0;
    /// Requests the host answered with an error. Not a transport failure: a missing file is one.
    u64 refused = 0;
    /// Requests that failed because the connection did, which is the one a developer needs to see
    /// separately — the host went away, and every asset after this one will fail too.
    u64 transport_failures = 0;
};

/// A `RemoteFileProvider` that fetches from a host over a TCP connection.
///
/// **Every call blocks until the host answers.** That is correct for the one place the engine calls
/// a provider from — `AssetSystem`'s read stage, which runs on `jobs::AsyncService`, the single
/// thread where blocking is legal — and it is why nothing here spawns a thread of its own. A caller
/// on a job worker is a defect the job system already reports.
///
/// Not thread-safe: one connection, one conversation. A second caller would interleave its request
/// with the first's reply. Two threads that both need the host take two providers.
class SocketFileProvider final : public RemoteFileProvider {
public:
    explicit SocketFileProvider(Allocator& allocator) noexcept;
    ~SocketFileProvider() override;

    /// Connect to "a.b.c.d:port". Fails with `Unsupported` where there is no socket layer.
    [[nodiscard]] Status connect(const char* address) noexcept;
    void disconnect() noexcept;
    [[nodiscard]] bool is_connected() const noexcept { return socket_ >= 0; }

    [[nodiscard]] Expected<u64, Error> stat(const VirtualPath& path) noexcept override;
    [[nodiscard]] Status fetch(const VirtualPath& path, u64 offset, void* destination,
                               usize size) noexcept override;
    [[nodiscard]] Status list(const VirtualPath& directory, bool recursive, VirtualVisitor visitor,
                              void* user) noexcept override;

    [[nodiscard]] const RemoteClientStats& stats() const noexcept { return stats_; }

private:
    /// Write a whole request and read a whole response header. A short write or a short read is
    /// looped rather than reported: a stream socket is entitled to move fewer bytes than it was
    /// given, and treating that as an error is the classic way to write a transport that works on
    /// the loopback and fails on a network.
    [[nodiscard]] Status exchange(u16 op, const VirtualPath& path, u64 argument_a, u64 argument_b,
                                  u64& answer) noexcept;
    [[nodiscard]] Status send_all(const void* bytes, usize size) noexcept;
    [[nodiscard]] Status receive_all(void* bytes, usize size) noexcept;
    /// Drop the connection and count it. Every transport failure goes through here, so a caller
    /// cannot be left with a socket that is half a conversation behind.
    [[nodiscard]] Status transport_failure(const char* message) noexcept;

    Allocator* allocator_;
    Array<u8> scratch_;
    RemoteClientStats stats_{};
    int socket_ = -1;
};

struct RemoteHostStats {
    u64 connections_accepted = 0;
    u64 connections_closed = 0;
    u64 requests_served = 0;
    /// Requests answered with an error code: a path that does not normalise, a file the namespace
    /// does not have, a range past the end.
    u64 requests_refused = 0;
    /// Requests refused because the path could not be normalised. A subset of the above, counted
    /// separately because it is the only one that means somebody sent something strange.
    u64 paths_rejected = 0;
    u64 bytes_served = 0;
    u64 listings_truncated = 0;
    /// Messages whose magic or version did not match. The connection is closed on the first one.
    u64 protocol_errors = 0;
};

/// The host half: serves a mounted namespace to connected devices.
///
/// **It owns no thread and no schedule.** `serve()` does one round of accept-and-answer with a
/// timeout the caller chooses, so the editor ticks it from its own loop and a tool calls it in one.
/// A class here that started a thread would be the second place in the engine deciding when I/O
/// happens — the same reason `FileWatcher` does not poll itself.
class FileServingHost {
public:
    FileServingHost(VirtualFileSystem& files, Allocator& allocator) noexcept;
    ~FileServingHost();

    FileServingHost(const FileServingHost&) = delete;
    FileServingHost& operator=(const FileServingHost&) = delete;

    /// Bind and listen. Port zero takes an ephemeral one, which `bound_port()` then reports — the
    /// only way a test can run a host and a client without agreeing a port number in advance.
    ///
    /// `loopback_only` defaults to true: a development file server that binds every interface by
    /// default is a project directory published to the network by a developer who did not ask for
    /// that. Serving a device on the same network is the explicit `false`.
    [[nodiscard]] Status open(u16 port, bool loopback_only = true) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return listener_ >= 0; }
    [[nodiscard]] u16 bound_port() const noexcept { return bound_port_; }
    [[nodiscard]] u32 connection_count() const noexcept { return static_cast<u32>(clients_.size()); }

    /// Accept what is waiting and answer what has arrived, then return. `timeout_ms` is how long to
    /// wait for the first thing to happen; zero polls and returns at once.
    ///
    /// Returns the number of requests answered, so a caller can tell a quiet round from a busy one.
    [[nodiscard]] Expected<u32, Error> serve(u32 timeout_ms) noexcept;

    [[nodiscard]] const RemoteHostStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] Status accept_pending() noexcept;
    /// Answer one request on one client. Returns false when the connection should be dropped —
    /// closed by the peer, or a protocol error, which are the same outcome and different counters.
    [[nodiscard]] bool serve_one(int socket, u32& served) noexcept;
    void drop(usize index) noexcept;

    VirtualFileSystem* files_;
    Allocator* allocator_;
    Array<int> clients_;
    Array<u8> scratch_;
    RemoteHostStats stats_{};
    int listener_ = -1;
    u16 bound_port_ = 0;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_REMOTE_H
