// SPDX-License-Identifier: MIT
#pragma once
// A server and clients over the seeded simulated network, with a clock. Issue #82.
//
// Every key comes from `SeededEntropy`, so a failing session is reproducible packet for packet —
// and `security()` reports the transport as not shipping-grade, which one case asserts on purpose.

#include "fixture.h"

#include <cy/networking/datagram_socket.h>
#include <cy/networking/secure_transport.h>

#include <memory>

namespace cy::net_test {

/// Wraps a socket so a case can see, drop, alter or replay what crosses it.
class TapSocket final : public cy::net::DatagramSocket {
public:
    using Tamper = bool (*)(void* user, cy::Array<cy::u8>& bytes) noexcept;

    TapSocket(cy::Allocator& allocator, cy::net::DatagramSocket& inner) noexcept
        : inner_(&inner), captured_(allocator), scratch_(allocator) {}

    [[nodiscard]] cy::Expected<cy::net::SocketAddress, cy::Error> resolve(
        const char* text) const noexcept override {
        return inner_->resolve(text);
    }

    [[nodiscard]] cy::Status send_to(const cy::net::SocketAddress& to,
                                     cy::Span<const cy::u8> bytes) noexcept override {
        ++sent_;
        if (capture_type_ != 0 && bytes.size() > 40 && bytes[0] == capture_type_) {
            captured_.clear();
            (void)captured_.append(bytes);
            captured_to_ = to;
            capture_type_ = 0;
        }
        if (tamper_ != nullptr) {
            scratch_.clear();
            (void)scratch_.append(bytes);
            if (tamper_(tamper_user_, scratch_)) {
                return inner_->send_to(to, scratch_.span());
            }
        }
        return inner_->send_to(to, bytes);
    }

    [[nodiscard]] bool receive_from(cy::net::SocketAddress& from,
                                    cy::Span<const cy::u8>& bytes) noexcept override {
        return inner_->receive_from(from, bytes);
    }

    void advance(cy::u64 now_ms) noexcept override { inner_->advance(now_ms); }
    [[nodiscard]] bool in_process() const noexcept override { return inner_->in_process(); }
    [[nodiscard]] const char* name() const noexcept override { return "tap"; }

    /// Keep a copy of the next packet of this type the endpoint sends: 4 is a data packet, 3 the
    /// handshake's RESPONSE.
    void capture_next(cy::u8 type) noexcept { capture_type_ = type; }
    void capture_next_data_packet() noexcept { capture_next(4); }
    [[nodiscard]] bool captured() const noexcept {
        return !captured_.empty() && capture_type_ == 0;
    }
    [[nodiscard]] cy::Span<const cy::u8> captured_bytes() const noexcept {
        return captured_.span();
    }
    [[nodiscard]] const cy::net::SocketAddress& captured_to() const noexcept {
        return captured_to_;
    }
    /// Send the captured packet again, byte for byte — a replay.
    [[nodiscard]] cy::Status replay() noexcept {
        return inner_->send_to(captured_to_, captured_.span());
    }
    void set_tamper(Tamper tamper, void* user) noexcept {
        tamper_ = tamper;
        tamper_user_ = user;
    }
    [[nodiscard]] cy::u64 sent() const noexcept { return sent_; }

private:
    cy::net::DatagramSocket* inner_;
    cy::Array<cy::u8> captured_;
    cy::Array<cy::u8> scratch_;
    cy::net::SocketAddress captured_to_{};
    cy::u8 capture_type_ = 0;
    Tamper tamper_ = nullptr;
    void* tamper_user_ = nullptr;
    cy::u64 sent_ = 0;
};

/// One endpoint: a simulated socket, a tap around it, and a secure transport over the tap.
struct SecureEndpoint {
    cy::net::SimulatedSocket socket;
    TapSocket tap;
    cy::net::SecureTransport transport;

    SecureEndpoint(cy::Allocator& allocator, cy::net::LocalNetwork& network, cy::net::PeerId host,
                   const cy::net::SecureConfig& config) noexcept
        : socket(network, host), tap(allocator, socket), transport(allocator, tap, config) {}
};

/// The server's static key pair, from a fixed seed.
[[nodiscard]] inline cy::net::crypto::KeyPair server_keys(cy::u64 seed = 7) noexcept {
    cy::net::crypto::SeededEntropy entropy(seed);
    return cy::net::crypto::generate_key_pair(entropy.source());
}

/// A server and up to four clients on one simulated network.
struct SecureSession {
    cy::net::LocalNetwork network;
    cy::net::crypto::SeededEntropy server_entropy;
    cy::net::crypto::SeededEntropy client_entropy[4] = {
        cy::net::crypto::SeededEntropy(101), cy::net::crypto::SeededEntropy(102),
        cy::net::crypto::SeededEntropy(103), cy::net::crypto::SeededEntropy(104)};
    cy::net::crypto::KeyPair keys;
    std::unique_ptr<SecureEndpoint> server;
    std::unique_ptr<SecureEndpoint> clients[4];
    cy::u32 client_count = 0;
    cy::u64 now_ms = 1000;

    explicit SecureSession(cy::u64 seed, cy::net::SecureConfig server_config = {}) noexcept
        : network(allocator(), seed), server_entropy(seed ^ 0x5EC0ULL), keys(server_keys()) {
        server_config.accept_connections = true;
        server_config.server_secret = keys.secret;
        server_config.entropy = server_entropy.source();
        const cy::net::PeerId host = network.add_host("server").value();
        server = std::make_unique<SecureEndpoint>(allocator(), network, host, server_config);
    }

    ~SecureSession() {
        // Clients first: each says goodbye to a server that is still there to hear it.
        for (auto& client : clients) {
            client.reset();
        }
        server.reset();
    }

    SecureSession(const SecureSession&) = delete;
    SecureSession& operator=(const SecureSession&) = delete;

    /// A client configured to trust this server's key, connected. Returns its index.
    cy::u32 add_client(cy::net::SecureConfig config = {}) noexcept {
        static const char* const names[4] = {"client-0", "client-1", "client-2", "client-3"};
        const cy::u32 index = client_count++;
        config.server_public = keys.public_key;
        config.entropy = client_entropy[index].source();
        const cy::net::PeerId host = network.add_host(names[index]).value();
        clients[index] = std::make_unique<SecureEndpoint>(allocator(), network, host, config);
        return index;
    }

    [[nodiscard]] cy::net::SecureTransport& client(cy::u32 index) noexcept {
        return clients[index]->transport;
    }
    [[nodiscard]] cy::net::SecureTransport& host() const noexcept { return server->transport; }

    void step(cy::u32 milliseconds = 10) noexcept {
        now_ms += milliseconds;
        server->transport.advance(now_ms);
        for (cy::u32 index = 0; index < client_count; ++index) {
            clients[index]->transport.advance(now_ms);
        }
    }

    void run(cy::u32 milliseconds, cy::u32 step_ms = 10) noexcept {
        for (cy::u32 elapsed = 0; elapsed < milliseconds; elapsed += step_ms) {
            step(step_ms);
        }
    }
};

}  // namespace cy::net_test
