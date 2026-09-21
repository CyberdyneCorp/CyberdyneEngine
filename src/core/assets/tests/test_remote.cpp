// SPDX-License-Identifier: MIT
// The remote mount's transport, both ends, over a real socket. M11.d task 6.1.
//
// `core-assets-and-io` — "Development file serving": "**WHEN** a device runs with a remote mount
// configured, **THEN** assets SHALL be fetched from the host machine on demand, so iteration does
// not require repackaging." M1 built `RemoteMount` against a `RemoteFileProvider` and left the
// transport out, saying why: a protocol nobody has spoken is a protocol that is wrong. Ten
// milestones later the only implementation in the tree was still `FakeHost` in `test_vfs.cpp`.
//
// THIS SUITE IS WHY THE TRANSPORT CAN BE WRITTEN NOW: both ends run here, in one process, over the
// loopback — a real socket, a real protocol, and a test that fails when either end is wrong. The
// host runs on its own thread because `serve()` waits on a poll; the client calls block, which is
// what the asset system's read stage does on the one thread where blocking is legal.
//
// A SEPARATE SUITE, for `integration.networking_udp`'s reason: a machine that refuses to bind a
// loopback socket loses this suite rather than the whole asset layer's.

#include <cy/core/assets/remote.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#    define CY_ASSETS_TEST_SOCKETS 1
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#else
#    define CY_ASSETS_TEST_SOCKETS 0
#endif

using namespace cy::assets;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

VirtualPath path_of(const char* raw) {
    auto path = VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

/// A host, serving a memory-mounted namespace on its own thread.
///
/// The thread is the TEST's, not the transport's: `FileServingHost` owns no schedule, so something
/// has to drive `serve()`, and in an editor that something is the frame loop.
class HostThread {
public:
    explicit HostThread(VirtualFileSystem& files) : host_(files, cy::current_allocator()) {}

    ~HostThread() { stop(); }

    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;

    [[nodiscard]] bool start() {
        if (!host_.open(0).has_value()) {
            return false;
        }
        port_ = host_.bound_port();
        running_.store(true);
        thread_ = std::thread([this]() {
            while (running_.load()) {
                const auto served = host_.serve(20);
                if (!served.has_value()) {
                    break;
                }
            }
        });
        return true;
    }

    void stop() {
        if (thread_.joinable()) {
            running_.store(false);
            thread_.join();
        }
        host_.close();
    }

    [[nodiscard]] std::string address() const {
        return std::string("127.0.0.1:") + std::to_string(port_);
    }

    [[nodiscard]] const RemoteHostStats& stats() const noexcept { return host_.stats(); }

private:
    FileServingHost host_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    cy::u16 port_ = 0;
};

/// What the whole stack looks like from the device's side: a namespace whose only mount is a
/// `RemoteMount` over a socket.
struct Device {
    explicit Device(cy::Allocator& allocator) : provider(allocator) {}

    [[nodiscard]] bool connect(const std::string& address) {
        if (!provider.connect(address.c_str()).has_value()) {
            return false;
        }
        auto mount = cy::make_unique<RemoteMount>(cy::current_allocator(), provider);
        CY_REQUIRE(mount.has_value());
        return files.mount_owned(std::move(mount.value()), mount_priority::kRemote).has_value();
    }

    SocketFileProvider provider;
    VirtualFileSystem files;
};

struct Listing {
    std::vector<std::string> paths;

    static bool visit(void* user, const VirtualEntry& entry) noexcept {
        auto* self = static_cast<Listing*>(user);
        self->paths.emplace_back(entry.path->c_str());
        return true;
    }
};

}  // namespace

CY_TEST_CASE("this build has a socket layer") {
    // Stated as a case rather than assumed by the ones below: on a platform with no socket half
    // every case here would be vacuous, and a suite that passes vacuously is the defect this
    // project has paid for nine times. On Windows this case fails and says what is missing.
    CY_CHECK(remote_serving_available());
}

CY_TEST_CASE("a host address parses, and anything that is not one does not") {
    const RemoteAddress good = parse_remote_address("127.0.0.1:41000");
    CY_CHECK(good.valid());
    CY_CHECK_EQ(good.ipv4, 0x7F000001U);
    CY_CHECK_EQ(good.port, 41000U);

    CY_CHECK_FALSE(parse_remote_address("127.0.0.1").valid());      // no port
    CY_CHECK_FALSE(parse_remote_address("127.0.0.1:0").valid());    // port zero is not a port
    CY_CHECK_FALSE(parse_remote_address("127.0.0:41000").valid());  // three octets
    CY_CHECK_FALSE(parse_remote_address("300.1.1.1:41000").valid());
    CY_CHECK_FALSE(parse_remote_address("localhost:41000").valid());
    CY_CHECK_FALSE(parse_remote_address(nullptr).valid());
}

CY_TEST_CASE("a device fetches from the host on demand, and only what it reads") {
    // THE SCENARIO, over a socket: assets are fetched on demand, so iteration does not require
    // repackaging. The counters are what make "on demand" an assertion rather than a description —
    // this is `test_vfs.cpp`'s FakeHost case with a real transport underneath it.
    VirtualFileSystem served;
    MemoryMount* memory = nullptr;
    {
        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "project");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(served.mount_owned(std::move(mount.value()), 0).has_value());
    }
    const std::string first = "the first file's bytes";
    const std::string second = "a second file, longer than the first one is";
    CY_REQUIRE(memory->add(path_of("textures/stone.ktx2"), first.data(), first.size()).has_value());
    CY_REQUIRE(
        memory->add(path_of("textures/wood.ktx2"), second.data(), second.size()).has_value());

    HostThread host(served);
    CY_REQUIRE(host.start());

    Device device(cy::current_allocator());
    CY_REQUIRE(device.connect(host.address()));

    // Mounting fetched nothing: a remote mount is a promise, not a copy.
    CY_CHECK_EQ(device.provider.stats().fetch_requests, 0U);
    CY_CHECK_EQ(device.provider.stats().bytes_fetched, 0U);

    const auto size = device.files.size_of(path_of("textures/stone.ktx2"));
    CY_REQUIRE(size.has_value());
    CY_CHECK_EQ(size.value(), first.size());
    CY_CHECK_EQ(device.provider.stats().fetch_requests, 0U);  // a stat is not a fetch

    cy::Array<u8> bytes(cy::current_allocator());
    CY_REQUIRE(device.files.read(path_of("textures/stone.ktx2"), bytes).has_value());
    CY_CHECK_EQ(bytes.size(), first.size());
    CY_CHECK(std::memcmp(bytes.data(), first.data(), first.size()) == 0);
    CY_CHECK_EQ(device.provider.stats().fetch_requests, 1U);
    CY_CHECK_EQ(device.provider.stats().bytes_fetched, first.size());

    // The second file was served by the same host on the same connection and was not fetched until
    // it was read — which is the whole of "on demand".
    CY_REQUIRE(device.files.read(path_of("textures/wood.ktx2"), bytes).has_value());
    CY_CHECK_EQ(bytes.size(), second.size());
    CY_CHECK(std::memcmp(bytes.data(), second.data(), second.size()) == 0);
    CY_CHECK_EQ(device.provider.stats().fetch_requests, 2U);
    CY_CHECK_EQ(device.provider.stats().bytes_fetched, first.size() + second.size());

    // A partial read is one request for that range and not for the file.
    u8 window[64] = {};
    CY_REQUIRE(device.files.read_range(path_of("textures/wood.ktx2"), 2, window, 8).has_value());
    CY_CHECK(std::memcmp(window, second.data() + 2, 8) == 0);
    CY_CHECK_EQ(device.provider.stats().fetch_requests, 3U);
    CY_CHECK_EQ(device.provider.stats().bytes_fetched, first.size() + second.size() + 8);

    // A file the host does not have is NotFound and not a transport failure: the connection is
    // still good, and the next read proves it.
    const auto missing = device.files.read(path_of("textures/absent.ktx2"), bytes);
    CY_CHECK_FALSE(missing.has_value());
    CY_CHECK(missing.error().code == cy::ErrorCode::NotFound);
    CY_CHECK_EQ(device.provider.stats().transport_failures, 0U);
    CY_REQUIRE(device.files.read(path_of("textures/stone.ktx2"), bytes).has_value());

    // A range past the end is refused rather than short-read, which is what every other mount does.
    const auto past = device.files.read_range(path_of("textures/stone.ktx2"), 4, window, 64);
    CY_CHECK_FALSE(past.has_value());
    CY_CHECK_EQ(device.provider.stats().transport_failures, 0U);

    CY_CHECK(host.stats().requests_served > 0);
    CY_CHECK_EQ(host.stats().connections_accepted, 1U);
}

CY_TEST_CASE("a directory listing crosses the wire, and a visitor may stop early") {
    VirtualFileSystem served;
    MemoryMount* memory = nullptr;
    {
        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "project");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(served.mount_owned(std::move(mount.value()), 0).has_value());
    }
    for (const char* name : {"meshes/a.cymesh", "meshes/b.cymesh", "meshes/c.cymesh"}) {
        CY_REQUIRE(memory->add(path_of(name), "x", 1).has_value());
    }

    HostThread host(served);
    CY_REQUIRE(host.start());
    Device device(cy::current_allocator());
    CY_REQUIRE(device.connect(host.address()));

    Listing listing;
    CY_REQUIRE(
        device.files.enumerate(path_of("meshes"), true, &Listing::visit, &listing).has_value());
    CY_CHECK_EQ(listing.paths.size(), usize{3});
    CY_CHECK(listing.paths[0] == "meshes/a.cymesh");

    // A visitor that stops early leaves the rest of the listing on the wire. The transport drains
    // it, so the NEXT request still reads its own reply rather than the tail of this one — which is
    // the failure a stream protocol has and a request-per-connection one does not.
    struct StopAfterOne {
        u32 seen = 0;
        static bool visit(void* user, const VirtualEntry&) noexcept {
            auto* self = static_cast<StopAfterOne*>(user);
            ++self->seen;
            return false;
        }
    } stopper;
    CY_REQUIRE(device.files.enumerate(path_of("meshes"), true, &StopAfterOne::visit, &stopper)
                   .has_value());
    CY_CHECK_EQ(stopper.seen, 1U);

    cy::Array<u8> bytes(cy::current_allocator());
    CY_REQUIRE(device.files.read(path_of("meshes/b.cymesh"), bytes).has_value());
    CY_CHECK_EQ(bytes.size(), usize{1});
    CY_CHECK_EQ(device.provider.stats().transport_failures, 0U);
}

CY_TEST_CASE("the host refuses a path that does not normalise, rather than serving it") {
    // The path arriving over the socket is untrusted input. `VirtualPath::normalise` is the one
    // place the traversal rule lives, and the host runs it on arrival rather than trusting that the
    // client did — so a client that hand-writes a request cannot reach outside the namespace.
    //
    // The request below is built by hand, over a raw socket, because a `SocketFileProvider` cannot
    // send it: its argument is a `VirtualPath`, and a `VirtualPath` cannot hold a traversal. The
    // hostile client is the one this rule exists for, and it is not the one in this file.
#if CY_ASSETS_TEST_SOCKETS
    VirtualFileSystem served;
    {
        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "project");
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(mount.value()->add(path_of("inside.txt"), "secret", 6).has_value());
        CY_REQUIRE(served.mount_owned(std::move(mount.value()), 0).has_value());
    }

    HostThread host(served);
    CY_REQUIRE(host.start());
    const RemoteAddress address = parse_remote_address(host.address().c_str());
    CY_REQUIRE(address.valid());

    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);
    CY_REQUIRE(handle >= 0);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(address.port);
    target.sin_addr.s_addr = htonl(address.ipv4);
    CY_REQUIRE(::connect(handle, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0);

    const std::string hostile = "../../etc/passwd";
    u8 header[32] = {};
    const u32 magic = kRemoteFileMagic;
    std::memcpy(header, &magic, 4);
    const cy::u16 version = kRemoteFileProtocolVersion;
    std::memcpy(header + 4, &version, 2);
    const cy::u16 op = 1;  // STAT
    std::memcpy(header + 6, &op, 2);
    const auto length = static_cast<cy::u16>(hostile.size());
    std::memcpy(header + 12, &length, 2);

    CY_REQUIRE(::send(handle, header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header)));
    CY_REQUIRE(::send(handle, hostile.data(), hostile.size(), 0) ==
               static_cast<ssize_t>(hostile.size()));

    u8 reply[32] = {};
    usize read = 0;
    while (read < sizeof(reply)) {
        const ssize_t got = ::recv(handle, reply + read, sizeof(reply) - read, 0);
        CY_REQUIRE(got > 0);
        read += static_cast<usize>(got);
    }
    u32 status = 0;
    std::memcpy(&status, reply + 8, 4);
    CY_CHECK(static_cast<cy::ErrorCode>(status) == cy::ErrorCode::InvalidArgument);
    (void)::close(handle);

    CY_CHECK_EQ(host.stats().paths_rejected, 1U);
    CY_CHECK_EQ(host.stats().bytes_served, 0U);
#endif
}

CY_TEST_CASE("a fetch larger than one request may carry is refused by both ends") {
    VirtualFileSystem served;
    {
        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "project");
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(mount.value()->add(path_of("small.bin"), "abc", 3).has_value());
        CY_REQUIRE(served.mount_owned(std::move(mount.value()), 0).has_value());
    }
    HostThread host(served);
    CY_REQUIRE(host.start());
    Device device(cy::current_allocator());
    CY_REQUIRE(device.connect(host.address()));

    std::vector<u8> destination(16);
    const cy::Status refused = device.provider.fetch(path_of("small.bin"), 0, destination.data(),
                                                     kRemoteMaxFetchBytes + 1);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::InvalidArgument);
    // Refused before it was sent: the host never saw it, so the connection is untouched.
    CY_CHECK_EQ(device.provider.stats().transport_failures, 0U);
    CY_REQUIRE(device.provider.fetch(path_of("small.bin"), 0, destination.data(), 3).has_value());
}

CY_TEST_CASE("a host that goes away is reported as a transport failure, not as a missing file") {
    // The distinction a developer needs at three in the morning: "your host is gone" and "that file
    // is not there" are different sentences, and a transport that says the second for the first
    // sends people to look at their content.
    VirtualFileSystem served;
    {
        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "project");
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(mount.value()->add(path_of("thing.bin"), "1234", 4).has_value());
        CY_REQUIRE(served.mount_owned(std::move(mount.value()), 0).has_value());
    }

    auto host = std::make_unique<HostThread>(served);
    CY_REQUIRE(host->start());
    Device device(cy::current_allocator());
    CY_REQUIRE(device.connect(host->address()));

    cy::Array<u8> bytes(cy::current_allocator());
    CY_REQUIRE(device.files.read(path_of("thing.bin"), bytes).has_value());

    host.reset();  // the developer closed their editor

    // AT THE TRANSPORT, the two are different and the difference is reported: `Unavailable` rather
    // than `NotFound`, and a `transport_failures` count that a missing file does not touch.
    u8 destination[4] = {};
    const cy::Status direct = device.provider.fetch(path_of("thing.bin"), 0, destination, 4);
    CY_CHECK_FALSE(direct.has_value());
    CY_CHECK(direct.error().code == cy::ErrorCode::Unavailable);
    CY_CHECK_EQ(device.provider.stats().transport_failures, 1U);
    // And the provider dropped the socket rather than leaving a half-finished conversation behind.
    CY_CHECK_FALSE(device.provider.is_connected());

    // AT THE NAMESPACE, the distinction is LOST, and that is a property of `Mount` rather than of
    // this transport: `Mount::contains()` returns `bool`, so `VirtualFileSystem::resolve` asks
    // every mount "do you have this" and a mount that cannot answer at all is indistinguishable
    // from one that says no. A dead host therefore reads as a missing file one layer up. Asserted
    // here rather than filed as a wish, because it is what a caller sees today and because the fix
    // is an interface change to `Mount` that every mount pays for — see src/core/assets/README.md.
    const auto after = device.files.read(path_of("thing.bin"), bytes);
    CY_CHECK_FALSE(after.has_value());
    CY_CHECK(after.error().code == cy::ErrorCode::NotFound);
}
