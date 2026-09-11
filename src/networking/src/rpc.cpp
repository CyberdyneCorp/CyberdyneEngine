#include <cy/networking/rpc.h>

#include <algorithm>
#include <cstring>

namespace cy::net {
namespace {

/// The verbs a development build treats as a sign of gameplay intent. Not a rule — see the header.
constexpr const char* kIntentVerbs[] = {"fire",  "shoot",  "move", "use",     "cast",
                                        "build", "attack", "jump", "interact"};

[[nodiscard]] bool contains_word(const char* text, const char* word) noexcept {
    if (text == nullptr || word == nullptr) {
        return false;
    }
    const usize length = std::strlen(word);
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        usize index = 0;
        while (index < length && cursor[index] != '\0') {
            const char left = cursor[index];
            const char lowered =
                (left >= 'A' && left <= 'Z') ? static_cast<char>(left - 'A' + 'a') : left;
            if (lowered != word[index]) {
                break;
            }
            ++index;
        }
        if (index == length) {
            return true;
        }
    }
    return false;
}

}  // namespace

const char* rpc_direction_name(RpcDirection direction) noexcept {
    switch (direction) {
        case RpcDirection::ToServer:
            return "ToServer";
        case RpcDirection::ToClients:
            return "ToClients";
        case RpcDirection::ToOwner:
            return "ToOwner";
        case RpcDirection::ToTarget:
            return "ToTarget";
    }
    return "unknown";
}

const char* rpc_refusal_name(RpcRefusal refusal) noexcept {
    switch (refusal) {
        case RpcRefusal::None:
            return "None";
        case RpcRefusal::UnknownRpc:
            return "UnknownRpc";
        case RpcRefusal::NotPermitted:
            return "NotPermitted";
        case RpcRefusal::PayloadSize:
            return "PayloadSize";
        case RpcRefusal::ParameterOutOfBounds:
            return "ParameterOutOfBounds";
        case RpcRefusal::RateLimited:
            return "RateLimited";
        case RpcRefusal::IntentMustBeACommand:
            return "IntentMustBeACommand";
    }
    return "unknown";
}

bool RpcRegistry::looks_like_intent(const RpcDeclaration& declaration) noexcept {
    if (declaration.direction != RpcDirection::ToServer) {
        return false;
    }
    return std::ranges::any_of(kIntentVerbs, [&declaration](const char* verb) noexcept {
        return contains_word(declaration.name.c_str(), verb);
    });
}

Expected<u32, RpcRefusal> RpcRegistry::declare(const RpcDeclaration& declaration,
                                               bool carries_gameplay_intent) noexcept {
    if (carries_gameplay_intent) {
        // The one refusal this registry makes at declaration time. Intent is a gameplay command —
        // see the header comment, and `gameplay-framework`'s command path.
        return make_unexpected(RpcRefusal::IntentMustBeACommand);
    }
    if (declaration.stable_id == 0 || find(declaration.stable_id) != nullptr) {
        return make_unexpected(RpcRefusal::UnknownRpc);
    }
    if (looks_like_intent(declaration)) {
        ++suspected_intent_;
    }
    if (!declarations_.push_back(declaration)) {
        return make_unexpected(RpcRefusal::UnknownRpc);
    }
    return static_cast<u32>(declarations_.size() - 1);
}

const RpcDeclaration* RpcRegistry::find(u32 stable_id) const noexcept {
    for (const auto& declaration : declarations_) {
        if (declaration.stable_id == stable_id) {
            return &declaration;
        }
    }
    return nullptr;
}

PeerRpcRecord* RpcRegistry::record(PeerId peer) noexcept {
    for (auto& record : peers_) {
        if (record.peer == peer) {
            return &record;
        }
    }
    PeerRpcRecord fresh;
    fresh.peer = peer;
    if (!peers_.push_back(fresh)) {
        return nullptr;
    }
    return &peers_[peers_.size() - 1];
}

const PeerRpcRecord* RpcRegistry::record_for(PeerId peer) const noexcept {
    for (const auto& record : peers_) {
        if (record.peer == peer) {
            return &record;
        }
    }
    return nullptr;
}

RpcRefusal RpcRegistry::validate(const RpcCall& call, const AuthorityRegistry& registry,
                                 PeerId server, u64 now_seconds) noexcept {
    PeerRpcRecord* peer_record = record(call.sender);
    const RpcDeclaration* declaration = find(call.stable_id);
    if (declaration == nullptr) {
        ++rejected_;
        if (peer_record != nullptr) {
            ++peer_record->rejected;
        }
        return RpcRefusal::UnknownRpc;
    }

    // Structural first: may this sender invoke this call on this entity at all?
    bool permitted = true;
    if (declaration->authority == RpcAuthority::ServerOnly) {
        permitted = call.sender == server;
    } else if (declaration->authority == RpcAuthority::EntityOwner) {
        permitted = call.target.valid() && registry.authority_of(call.target) == call.sender;
    }
    if (!permitted) {
        ++rejected_;
        if (peer_record != nullptr) {
            ++peer_record->rejected;
        }
        return RpcRefusal::NotPermitted;
    }

    if (call.payload.size() != declaration->bounds.payload_size) {
        ++rejected_;
        if (peer_record != nullptr) {
            ++peer_record->rejected;
        }
        return RpcRefusal::PayloadSize;
    }
    if (declaration->bounds.checked) {
        if (call.payload.size() < sizeof(i64)) {
            ++rejected_;
            if (peer_record != nullptr) {
                ++peer_record->rejected;
            }
            return RpcRefusal::ParameterOutOfBounds;
        }
        i64 value = 0;
        std::memcpy(static_cast<void*>(&value), static_cast<const void*>(call.payload.data()),
                    sizeof(i64));
        if (value < declaration->bounds.minimum || value > declaration->bounds.maximum) {
            ++rejected_;
            if (peer_record != nullptr) {
                ++peer_record->rejected;
            }
            return RpcRefusal::ParameterOutOfBounds;
        }
    }

    if (peer_record == nullptr) {
        return RpcRefusal::None;
    }
    if (declaration->rate_limit_per_second != 0) {
        if (peer_record->window_second != now_seconds) {
            peer_record->window_second = now_seconds;
            peer_record->in_window = 0;
        }
        if (peer_record->in_window >= declaration->rate_limit_per_second) {
            ++rejected_;
            ++peer_record->rejected;
            ++peer_record->rate_limited;
            return RpcRefusal::RateLimited;
        }
        ++peer_record->in_window;
    }
    ++peer_record->accepted;
    return RpcRefusal::None;
}

}  // namespace cy::net
