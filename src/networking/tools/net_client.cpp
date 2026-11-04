// SPDX-License-Identifier: MIT
// cy_net_client — a headless client host for the dedicated server. Issue #82.
//
//   cy_net_client --server 127.0.0.1:27015 --key <the server's public key> [--seconds 10]
//
// Connects through the secure transport, joins, mirrors the server's world into its own and steers
// its avatar with move commands. Once a second it prints what it holds: the phase, the entities,
// the snapshots applied, the round trip and loss the transport measured, and the replicated digest,
// which equals the server's once the server's world is at rest and every snapshot has arrived.
// Exits 0 when it was joined at the end, 1 otherwise.

#include "net_demo.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/networking/secure_transport.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace {

using namespace cy;
using namespace cy::net;

[[nodiscard]] u64 microseconds() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

struct Options {
    const char* server = nullptr;
    const char* key = nullptr;
    u32 seconds = 10;
};

[[nodiscard]] bool parse(int argc, char** argv, Options& out) noexcept {
    for (int index = 1; index + 1 < argc; index += 2) {
        const char* name = argv[index];
        const char* value = argv[index + 1];
        if (std::strcmp(name, "--server") == 0) {
            out.server = value;
        } else if (std::strcmp(name, "--key") == 0) {
            out.key = value;
        } else if (std::strcmp(name, "--seconds") == 0) {
            out.seconds = static_cast<u32>(std::strtoul(value, nullptr, 10));
        } else {
            std::fprintf(stderr, "cy_net_client: unknown option %s\n", name);
            return false;
        }
    }
    return (argc % 2) == 1 && out.server != nullptr && out.key != nullptr;
}

/// Everything the process runs.
struct ClientProcess {
    UdpSocket socket;
    std::unique_ptr<SecureTransport> transport;
    demo::DemoSchemas schemas;
    ecs::World world;
    ReplicaWorld replica;
    std::unique_ptr<ClientHost> host;
    u64 round = 0;

    explicit ClientProcess(Allocator& allocator) noexcept
        : socket(allocator),
          schemas(allocator),
          world(allocator),
          replica(allocator, world, schemas.set()) {}

    [[nodiscard]] bool start(Allocator& allocator, const Options& options,
                             const crypto::Key& server_public) noexcept {
        if (!socket.open(0)) {
            std::fprintf(stderr, "cy_net_client: could not open a UDP socket\n");
            return false;
        }
        SecureConfig secure;
        secure.server_public = server_public;
        transport = std::make_unique<SecureTransport>(allocator, socket, secure);
        if (!schemas.valid() || !world.initialize() || !demo::register_client_components(world) ||
            !replica.initialize()) {
            std::fprintf(stderr, "cy_net_client: could not build the client world\n");
            return false;
        }
        ClientHostConfig config;
        config.scope.build_id = 0x4359'4E45'5453'0082ULL;
        config.scope.schema_set_hash = schemas.set().identity();
        host = std::make_unique<ClientHost>(allocator, *transport, replica, config);
        if (!host->connect(options.server)) {
            std::fprintf(stderr, "cy_net_client: %s is not an address\n", options.server);
            return false;
        }
        return true;
    }

    /// Steer the avatar a little: left and right, and always forward.
    void steer() noexcept {
        const NetworkId avatar = demo::avatar_of(world, host->self());
        if (avatar.valid()) {
            const auto dx = static_cast<i16>((round++ % 40) < 20 ? 2 : -2);
            (void)host->submit(demo::move(avatar, host->server_tick() + 1, dx, 1));
        }
    }

    void report() const noexcept {
        const ConnectionStats stats = transport->stats(host->server());
        std::printf("%-12s entities %u  snapshots %llu  rtt %u us  loss %u/1000  digest %016llx\n",
                    client_phase_name(host->phase()), replica.entity_count(),
                    static_cast<unsigned long long>(host->snapshots_applied()), stats.round_trip_us,
                    stats.loss_per_mille,
                    static_cast<unsigned long long>(replicated_digest(world, schemas.set())));
        std::fflush(stdout);
    }

    [[nodiscard]] bool run(const Options& options) noexcept {
        const u64 start = microseconds();
        u64 next_report = start + 1'000'000;
        u64 next_command = start;
        while (microseconds() - start < u64{options.seconds} * 1'000'000) {
            const u64 now = microseconds();
            host->update(now);
            if (host->phase() == ClientPhase::Joined && now >= next_command) {
                steer();
                next_command = now + 100'000;
            }
            if (now >= next_report) {
                report();
                next_report += 1'000'000;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool joined = host->phase() == ClientPhase::Joined;
        host->disconnect();
        host->update(microseconds());
        return joined;
    }
};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    crypto::Key server_public;
    if (!parse(argc, argv, options) || !demo::key_from_hex(options.key, server_public)) {
        std::fprintf(stderr,
                     "usage: cy_net_client --server a.b.c.d:port --key <64 hex digits> "
                     "[--seconds S]\n");
        return 2;
    }
    Allocator& allocator = system_allocator(MemoryDomain::World);
    ClientProcess process(allocator);
    if (!process.start(allocator, options, server_public)) {
        return 1;
    }
    return process.run(options) ? 0 : 1;
}
