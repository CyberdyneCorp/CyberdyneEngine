#pragma once
// Remote procedure calls, and the boundary that keeps gameplay intent out of them. M9 task 4.3.
//
// ================================================================================================
// RPCs ARE NOT THE CHANNEL FOR GAMEPLAY INTENT, AND THAT IS CHECKED RATHER THAN ADVISED
// ================================================================================================
//
// `networking-and-replication` states it in capitals and gives the reason: "**RPCs SHALL NOT be the
// channel for gameplay intent.** Client-to-server gameplay intent travels as **gameplay commands**
// ..., which carry prediction, structural authority validation, and replay recording. ... Intent
// sent by RPC would bypass prediction, replay, and command validation."
//
// That last clause is M9's whole subject. A game whose "fire" went out as an RPC would have a
// replay that did not contain firing, a rollback that could not re-simulate it, and a lockstep
// session that diverged the first time a player shot. So `RpcRegistry::declare()` refuses a
// declaration whose `carries_gameplay_intent` is set — there is no way to register one — and
// `looks_like_intent()` is the development-build heuristic the specification asks for
// ("development builds SHOULD report an RPC that appears to carry gameplay intent"), reported as a
// count rather than as a refusal, because a heuristic that could refuse would eventually refuse
// something legitimate.
//
// ================================================================================================
// EVERY RECEIPT IS VALIDATED: SENDER AUTHORITY, THEN PARAMETER BOUNDS
// ================================================================================================
//
// "The engine SHALL validate on receipt: that the sender is permitted to invoke this RPC on this
// entity, and that parameters are within declared bounds." Both, in that order, and the order is
// the same one `gameplay-framework` fixes for commands: structural before specific. A rejected call
// is counted per peer, which is what a rate-limit or disconnect policy is built on.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/networking/authority.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// Where an RPC goes.
enum class RpcDirection : u8 {
    ToServer = 0,
    ToClients,
    ToOwner,
    ToTarget,
};

const char* rpc_direction_name(RpcDirection direction) noexcept;

/// What the sender must be.
enum class RpcAuthority : u8 {
    /// Anyone connected. Chat.
    AnyPeer = 0,
    /// The peer that owns the target entity.
    EntityOwner,
    /// The session's authority.
    ServerOnly,
};

/// One declared parameter's bounds, checked on receipt.
struct RpcParameterBounds {
    /// Bytes the payload must be, exactly. Zero means "no payload".
    u16 payload_size = 0;
    /// A signed inclusive range applied to the first `i64` of the payload when `checked` is set.
    /// Deliberately narrow: a general bounds language here would be a second serialisation format,
    /// and the general answer is the reflected schema in `schema.h`.
    bool checked = false;
    i64 minimum = 0;
    i64 maximum = 0;
};

struct RpcDeclaration {
    Name name;
    /// Persistent identity, from the project's manifest. Never derived from the name.
    u32 stable_id = 0;
    RpcDirection direction = RpcDirection::ToServer;
    DeliveryMode delivery = DeliveryMode::ReliableOrdered;
    RpcAuthority authority = RpcAuthority::AnyPeer;
    ChannelId channel = 0;
    RpcParameterBounds bounds{};
    /// Calls per second one peer may make. Zero is unlimited, which is only correct for a
    /// server-to-client call.
    u32 rate_limit_per_second = 0;
};

/// Why a call was refused.
enum class RpcRefusal : u8 {
    None = 0,
    UnknownRpc,
    NotPermitted,
    PayloadSize,
    ParameterOutOfBounds,
    RateLimited,
    /// The declaration itself said it carries gameplay intent. Refused at declaration, and the
    /// enumerator exists so the refusal has a name rather than a generic invalid-argument.
    IntentMustBeACommand,
};

const char* rpc_refusal_name(RpcRefusal refusal) noexcept;

/// One call, as received.
struct RpcCall {
    u32 stable_id = 0;
    PeerId sender;
    NetworkId target;
    Span<const u8> payload;
};

/// What one peer has done. `networking-and-replication`'s "Unauthorised RPC": the server "SHALL
/// reject it, log it, and optionally apply a rate-limit or disconnect policy" — this is the state
/// such a policy reads.
struct PeerRpcRecord {
    PeerId peer;
    u64 accepted = 0;
    u64 rejected = 0;
    u64 rate_limited = 0;
    /// Calls in the current second, and which second that is.
    u32 in_window = 0;
    u64 window_second = 0;
};

class RpcRegistry {
public:
    explicit RpcRegistry(Allocator& allocator) noexcept
        : declarations_(allocator), peers_(allocator) {}

    /// Declare an RPC. Refuses a duplicate stable id and a declaration that admits to carrying
    /// gameplay intent — see the header comment.
    [[nodiscard]] Expected<u32, RpcRefusal> declare(const RpcDeclaration& declaration,
                                                    bool carries_gameplay_intent) noexcept;

    [[nodiscard]] const RpcDeclaration* find(u32 stable_id) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(declarations_.size()); }

    /// Validate a received call. `now_seconds` drives the rate limit; `registry` answers the
    /// authority question, and is const because validating a call may not change who owns anything.
    [[nodiscard]] RpcRefusal validate(const RpcCall& call, const AuthorityRegistry& registry,
                                      PeerId server, u64 now_seconds) noexcept;

    [[nodiscard]] const PeerRpcRecord* record_for(PeerId peer) const noexcept;
    [[nodiscard]] u64 rejected_total() const noexcept { return rejected_; }

    /// The development-build heuristic: does this declaration look like gameplay intent?
    ///
    /// A name containing a verb the engine associates with intent — fire, move, use, cast, build —
    /// on a `ToServer` call from a client. A heuristic, reported and never enforced.
    [[nodiscard]] static bool looks_like_intent(const RpcDeclaration& declaration) noexcept;
    [[nodiscard]] u32 suspected_intent() const noexcept { return suspected_intent_; }

private:
    [[nodiscard]] PeerRpcRecord* record(PeerId peer) noexcept;

    Array<RpcDeclaration> declarations_;
    Array<PeerRpcRecord> peers_;
    u64 rejected_ = 0;
    u32 suspected_intent_ = 0;
};

}  // namespace cy::net
