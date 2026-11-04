// SPDX-License-Identifier: MIT
// cy_net_server — the dedicated server, headless. Issue #82.
//
//   cy_net_server [--port 27015] [--units 500] [--tick-rate 30] [--budget 4096] [--seconds 0]
//                 [--rest-after 0] [--key <64 hex digits: the server's static secret>]
//
// `--rest-after S` stops the units wandering after S seconds, so a client's digest can be seen to
// reach the server's.
//
// Binds a UDP port, admits clients through the secure transport's handshake, runs the demo game's
// world at a fixed tick and replicates it. Without `--key` it generates a static key from the
// operating system's generator and prints the PUBLIC half, which a client needs to connect:
//
//   cy_net_server --units 1000
//   cy_net_client --server 127.0.0.1:27015 --key <printed public key>
//
// The process links no renderer, audio, interface or GPU module — `cy::networking`'s dependency
// list makes that a property of the link graph — so it runs on a machine with no display.
// Once a second it prints one status line: peers, the tick, CPU per tick, bytes per second out and
// the replicated digest a client can be compared against.

#include "net_demo.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/networking/secure_transport.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace {

using namespace cy;
using namespace cy::net;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*signal*/) {
    g_stop = 1;
}

[[nodiscard]] u64 microseconds() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

struct Options {
    u16 port = 27015;
    u32 units = 500;
    u32 tick_rate = 30;
    u32 budget = 4096;
    u32 seconds = 0;
    u32 rest_after = 0;
    const char* key = nullptr;
};

[[nodiscard]] bool parse(int argc, char** argv, Options& out) noexcept {
    for (int index = 1; index + 1 < argc; index += 2) {
        const char* name = argv[index];
        const char* value = argv[index + 1];
        const auto number = static_cast<u32>(std::strtoul(value, nullptr, 10));
        if (std::strcmp(name, "--port") == 0) {
            out.port = static_cast<u16>(number);
        } else if (std::strcmp(name, "--units") == 0) {
            out.units = number;
        } else if (std::strcmp(name, "--tick-rate") == 0) {
            out.tick_rate = number;
        } else if (std::strcmp(name, "--budget") == 0) {
            out.budget = number;
        } else if (std::strcmp(name, "--seconds") == 0) {
            out.seconds = number;
        } else if (std::strcmp(name, "--rest-after") == 0) {
            out.rest_after = number;
        } else if (std::strcmp(name, "--key") == 0) {
            out.key = value;
        } else {
            std::fprintf(stderr, "cy_net_server: unknown option %s\n", name);
            return false;
        }
    }
    return (argc % 2) == 1;
}

/// Everything the process runs, built in order and torn down in reverse.
struct ServerProcess {
    UdpSocket socket;
    std::unique_ptr<SecureTransport> transport;
    demo::DemoSchemas schemas;
    ecs::World world;
    WorldReplicator replicator;
    demo::DemoServerGame game;
    std::unique_ptr<ServerHost> host;

    explicit ServerProcess(Allocator& allocator) noexcept
        : socket(allocator),
          schemas(allocator),
          world(allocator),
          replicator(allocator, world, schemas.set()),
          game(world, replicator) {}

    [[nodiscard]] bool start(Allocator& allocator, const Options& options,
                             const crypto::Key& secret) noexcept {
        if (!socket.open(options.port)) {
            std::fprintf(stderr, "cy_net_server: could not bind UDP port %u\n",
                         static_cast<unsigned>(options.port));
            return false;
        }
        SecureConfig secure;
        secure.accept_connections = true;
        secure.server_secret = secret;
        transport = std::make_unique<SecureTransport>(allocator, socket, secure);
        if (!schemas.valid() || !world.initialize() || !game.initialize() ||
            !game.populate(options.units)) {
            std::fprintf(stderr, "cy_net_server: could not build the world\n");
            return false;
        }
        ServerHostConfig config;
        config.scope.build_id = 0x4359'4E45'5453'0082ULL;
        config.scope.schema_set_hash = schemas.set().identity();
        config.tick_rate_hz = options.tick_rate;
        config.budget.bytes_per_tick = options.budget;
        config.simulate = &demo::DemoServerGame::simulate;
        config.simulate_user = &game;
        config.on_peer = &demo::DemoServerGame::on_peer;
        config.on_peer_user = &game;
        host = std::make_unique<ServerHost>(allocator, *transport, world, replicator, config);
        return true;
    }

    void announce(const Options& options, const crypto::Key& secret) const noexcept {
        std::printf("cy_net_server: listening on UDP %u, %u units at %u Hz, budget %u B/tick\n",
                    static_cast<unsigned>(socket.bound_port()), options.units, options.tick_rate,
                    options.budget);
        char published[65];
        demo::key_to_hex(SecureTransport::public_key_for(secret), published);
        std::printf("cy_net_server: public key %s\n", published);
        std::printf("cy_net_server: security: %s\n", transport->security().mechanism);
        std::fflush(stdout);
    }

    void report(u64 busy_us, u64 ticks) const noexcept {
        u64 bytes = 0;
        for (u32 index = 0; index < host->peer_count(); ++index) {
            bytes += transport->stats(host->admission().peer_at(index).peer).bytes_out;
        }
        std::printf("tick %llu  peers %u  cpu %llu us/tick  out %llu bytes total  digest %016llx\n",
                    static_cast<unsigned long long>(host->tick()), host->peer_count(),
                    static_cast<unsigned long long>(ticks == 0 ? 0 : busy_us / ticks),
                    static_cast<unsigned long long>(bytes),
                    static_cast<unsigned long long>(replicated_digest(world, schemas.set())));
        std::fflush(stdout);
    }

    void run(const Options& options) noexcept {
        const u64 start = microseconds();
        const u64 stop = u64{options.seconds} * 1'000'000;
        const u64 rest = u64{options.rest_after} * 1'000'000;
        u64 next_report = start + 1'000'000;
        u64 busy_us = 0;
        u64 ticks_at_report = 0;
        while (g_stop == 0 && (stop == 0 || microseconds() - start < stop)) {
            const u64 before = microseconds();
            game.set_wandering(rest == 0 || before - start < rest);
            host->update(before);
            busy_us += microseconds() - before;
            if (before >= next_report) {
                report(busy_us, host->tick() - ticks_at_report);
                next_report += 1'000'000;
                busy_us = 0;
                ticks_at_report = host->tick();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::printf("cy_net_server: stopped at tick %llu\n",
                    static_cast<unsigned long long>(host->tick()));
    }
};

[[nodiscard]] bool server_secret(const Options& options, crypto::Key& secret) noexcept {
    if (options.key == nullptr) {
        secret = crypto::generate_key_pair(crypto::system_entropy()).secret;
        return true;
    }
    if (!demo::key_from_hex(options.key, secret)) {
        std::fprintf(stderr, "cy_net_server: --key wants 64 hexadecimal digits\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    crypto::Key secret;
    if (!parse(argc, argv, options) || !server_secret(options, secret)) {
        std::fprintf(stderr,
                     "usage: cy_net_server [--port N] [--units N] [--tick-rate HZ] [--budget "
                     "BYTES] [--seconds S] [--rest-after S] [--key HEX]\n");
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    Allocator& allocator = system_allocator(MemoryDomain::World);
    ServerProcess process(allocator);
    if (!process.start(allocator, options, secret)) {
        return 1;
    }
    process.announce(options, secret);
    crypto::wipe(&secret, sizeof(secret));
    process.run(options);
    return 0;
}
