// Composable rules, the rules asset, and game phases. M8.b tasks 3.1 and 3.2.

#include <cy/gameplay/rules.h>

#include <cstring>

namespace cy::gameplay {
namespace {

/// FNV-1a over 64 bits. The digest is a value identity over a composition, not a security claim.
constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 mix(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

const char* rule_kind_name(RuleKind kind) noexcept {
    switch (kind) {
        case RuleKind::SpawnSelection:
            return "SpawnSelection";
        case RuleKind::Victory:
            return "Victory";
        case RuleKind::TeamFormation:
            return "TeamFormation";
        case RuleKind::Respawn:
            return "Respawn";
        case RuleKind::Economy:
            return "Economy";
        case RuleKind::Time:
            return "Time";
        case RuleKind::ProjectDefined:
            return "ProjectDefined";
        case RuleKind::Count:
            break;
    }
    return "ProjectDefined";
}

RulesAsset::RulesAsset(Allocator& allocator, Name name) noexcept
    : name_(name), pieces_(allocator), parameters_(allocator) {}

Expected<u32, Error> RulesAsset::add_piece(RuleKind kind, Name name, Name implementation) noexcept {
    RulePiece piece;
    piece.kind = kind;
    piece.name = name;
    piece.implementation = implementation;
    piece.first_parameter = static_cast<u32>(parameters_.size());
    if (Status pushed = pieces_.push_back(piece); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(pieces_.size() - 1);
}

Status RulesAsset::set_parameter(u32 piece, Name parameter, f64 number) noexcept {
    if (piece >= pieces_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such rule piece", 0});
    }
    if (piece + 1 != pieces_.size()) {
        // Parameters are a contiguous run per piece, so they are set while the piece is the last
        // one added. Refusing is better than the alternative, which is a parameter silently
        // attaching to the wrong rule.
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a piece's parameters are set before the next piece", 0});
    }
    RulePiece& target = pieces_[piece];
    for (u32 index = 0; index < target.parameter_count; ++index) {
        RuleParameter& existing = parameters_[target.first_parameter + index];
        if (existing.name == parameter) {
            existing.number = number;
            return ok();
        }
    }
    if (Status pushed = parameters_.push_back(RuleParameter{parameter, number, Name{}}); !pushed) {
        return pushed;
    }
    ++target.parameter_count;
    return ok();
}

Status RulesAsset::set_parameter(u32 piece, Name parameter, Name text) noexcept {
    if (Status set = set_parameter(piece, parameter, 0.0); !set) {
        return set;
    }
    RulePiece& target = pieces_[piece];
    for (u32 index = 0; index < target.parameter_count; ++index) {
        RuleParameter& existing = parameters_[target.first_parameter + index];
        if (existing.name == parameter) {
            existing.text = text;
            return ok();
        }
    }
    return ok();
}

u32 RulesAsset::find(RuleKind kind) const noexcept {
    for (usize index = 0; index < pieces_.size(); ++index) {
        if (pieces_[index].kind == kind) {
            return static_cast<u32>(index);
        }
    }
    return static_cast<u32>(pieces_.size());
}

const RuleParameter* RulesAsset::parameter(u32 piece, Name name) const noexcept {
    if (piece >= pieces_.size()) {
        return nullptr;
    }
    const RulePiece& target = pieces_[piece];
    for (u32 index = 0; index < target.parameter_count; ++index) {
        const RuleParameter& existing = parameters_[target.first_parameter + index];
        if (existing.name == name) {
            return &existing;
        }
    }
    return nullptr;
}

f64 RulesAsset::number(u32 piece, Name name, f64 fallback) const noexcept {
    const RuleParameter* found = parameter(piece, name);
    return found != nullptr ? found->number : fallback;
}

u64 RulesAsset::digest() const noexcept {
    u64 hash = kFnvOffset;
    hash = mix(hash, name_.index());
    for (const RulePiece& piece : pieces_) {
        hash = mix(hash, static_cast<u64>(piece.kind));
        hash = mix(hash, piece.name.index());
        hash = mix(hash, piece.implementation.index());
        for (u32 index = 0; index < piece.parameter_count; ++index) {
            const RuleParameter& parameter = parameters_[piece.first_parameter + index];
            hash = mix(hash, parameter.name.index());
            u64 bits = 0;
            const f64 number = parameter.number;
            std::memcpy(static_cast<void*>(&bits), static_cast<const void*>(&number), sizeof(bits));
            hash = mix(hash, bits);
            hash = mix(hash, parameter.text.index());
        }
    }
    return hash;
}

PhaseController::PhaseController(Allocator& allocator) noexcept
    : transitions_(allocator), events_(allocator) {}

Status PhaseController::allow(TagId from, TagId to) noexcept {
    if (to == kInvalidTag) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a phase is a tag; the null tag is not one", 0});
    }
    return transitions_.push_back(Transition{from, to});
}

Status PhaseController::allow_from_any(TagId to) noexcept {
    if (to == kInvalidTag) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a phase is a tag; the null tag is not one", 0});
    }
    return transitions_.push_back(Transition{kAnyPhase, to});
}

ValidationResult PhaseController::check(TagId to) const noexcept {
    ValidationResult result;
    if (to == kInvalidTag) {
        result.reject(ReasonTag::WrongPhase);
        return result;
    }
    if (to == current_) {
        result.reject(ReasonTag::WrongPhase);
        return result;
    }
    for (const Transition& transition : transitions_) {
        if (transition.to != to) {
            continue;
        }
        if (transition.from == kAnyPhase || transition.from == current_) {
            return result;
        }
    }
    result.reject(ReasonTag::WrongPhase);
    return result;
}

ValidationResult PhaseController::enter(TagId to, u64 tick) noexcept {
    ValidationResult result = check(to);
    if (!result.permitted()) {
        return result;
    }
    if (current_ != kInvalidTag) {
        if (Status pushed = events_.push_back(PhaseEvent{PhaseEventKind::Leaving, current_, tick});
            !pushed) {
            result.reject(ReasonTag::ProjectDefined);
            return result;
        }
    }
    if (Status pushed = events_.push_back(PhaseEvent{PhaseEventKind::Entering, to, tick});
        !pushed) {
        result.reject(ReasonTag::ProjectDefined);
        return result;
    }
    current_ = to;
    return result;
}

bool PhaseController::active_in(const TagRegistry& registry, TagId declared) const noexcept {
    if (declared == kInvalidTag) {
        // A system that declared no phase is active in all of them. That is the default a system
        // gets by not saying anything, and it is the one that keeps existing systems working when
        // a project adds phases.
        return true;
    }
    return registry.matches(declared, current_);
}

}  // namespace cy::gameplay
