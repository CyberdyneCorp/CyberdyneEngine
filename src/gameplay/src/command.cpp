// The one validated command stream. Task 4.4.3, and design.md §3.

#include <cy/gameplay/command.h>

#include <cy/core/memory/allocator.h>

#include <algorithm>
#include <utility>

namespace cy::gameplay {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 fold(u64 hash, const void* bytes, usize count) noexcept {
    const auto* data = static_cast<const u8*>(bytes);
    for (usize index = 0; index < count; ++index) {
        hash ^= data[index];
        hash *= kFnvPrime;
    }
    return hash;
}

template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object == nullptr) {
        return;
    }
    object->~T();
    allocator.deallocate(object, sizeof(T), alignof(T));
}

}  // namespace

Status CommandBuffer::record(const Command& command) noexcept {
    Command stamped = command;
    stamped.sequence = next_sequence_++;
    return commands_.push_back(stamped);
}

u64 CommandLog::hash() const noexcept {
    // FIELD BY FIELD, AND *WITHOUT* PROVENANCE. `gameplay-framework`: "Provenance SHALL NOT affect
    // validation, ordering, or execution." A log hash that included it would make a replay's log
    // differ from the live one it reproduces — the recorded commands carry `Replay` provenance and
    // the originals carried `Human` — and the first thing anyone would do is "fix" it by making the
    // replay lie about where its commands came from.
    u64 hash = kFnvOffset;
    for (const auto& command : entries_) {
        hash = fold(hash, &command.type, sizeof(command.type));
        hash = fold(hash, &command.tick, sizeof(command.tick));
        const u64 participant = command.participant.bits();
        hash = fold(hash, &participant, sizeof(participant));
        const u64 target = command.target.bits();
        hash = fold(hash, &target, sizeof(target));
        hash = fold(hash, &command.payload_size, sizeof(command.payload_size));
        hash = fold(hash, command.payload, command.payload_size);
    }
    return hash;
}

CommandStream::~CommandStream() {
    for (auto& producer : producers_) {
        unmake(*allocator_, producer);
    }
    producers_.clear();
}

CommandStream::CommandStream(Allocator& allocator, ControlRegistry& control) noexcept
    : allocator_(&allocator),
      control_(&control),
      declarations_(allocator),
      producers_(allocator),
      producer_names_(allocator),
      committed_(allocator),
      rejections_(allocator),
      rules_(allocator),
      capabilities_(allocator),
      log_(allocator) {}

Expected<CommandTypeId, Error> CommandStream::declare(
    const CommandDeclaration& declaration) noexcept {
    if (declaration.stable_id == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "gameplay: a command type needs a stable identifier; zero is the null one");
    }
    if (find(declaration.stable_id) != kInvalidCommandType) {
        return fail(ErrorCode::AlreadyExists,
                    "gameplay: that command type's stable id is already declared");
    }
    const auto id = static_cast<CommandTypeId>(declarations_.size());
    if (Status pushed = declarations_.push_back(declaration); !pushed) {
        return make_unexpected(pushed.error());
    }
    return id;
}

CommandTypeId CommandStream::find(u32 stable_id) const noexcept {
    for (usize index = 0; index < declarations_.size(); ++index) {
        if (declarations_[index].stable_id == stable_id) {
            return static_cast<CommandTypeId>(index);
        }
    }
    return kInvalidCommandType;
}

Expected<u32, Error> CommandStream::open_producer(Name debug_name) noexcept {
    const auto order = static_cast<u32>(producers_.size());
    auto* buffer = make<CommandBuffer>(*allocator_, *allocator_, order);
    if (buffer == nullptr) {
        return fail(ErrorCode::OutOfMemory, "gameplay: could not allocate a command buffer");
    }
    if (Status pushed = producers_.push_back(buffer); !pushed) {
        unmake(*allocator_, buffer);
        return make_unexpected(pushed.error());
    }
    if (Status named = producer_names_.push_back(debug_name); !named) {
        return make_unexpected(named.error());
    }
    return order;
}

Status CommandStream::set_capabilities(ecs::Entity entity, u32 mask) noexcept {
    for (auto& capabilitie : capabilities_) {
        if (capabilitie.entity == entity.bits()) {
            capabilitie.mask = mask;
            return ok();
        }
    }
    return capabilities_.push_back(CapabilityEntry{entity.bits(), mask});
}

u32 CommandStream::capabilities(ecs::Entity entity) const noexcept {
    for (auto capabilitie : capabilities_) {
        if (capabilitie.entity == entity.bits()) {
            return capabilitie.mask;
        }
    }
    return 0;
}

Status CommandStream::add_rule(CommandTypeId type, RuleFn rule, void* user) noexcept {
    if (type >= declarations_.size() || rule == nullptr) {
        return fail(ErrorCode::InvalidArgument, "gameplay: a rule needs a declared command type");
    }
    return rules_.push_back(Rule{type, rule, user});
}

ValidationResult CommandStream::validate(const GameplayContext& context,
                                         const Command& command) const noexcept {
    ValidationResult result;

    if (command.type >= declarations_.size()) {
        result.reject(ReasonTag::UnknownCommand);
        return result;
    }
    const CommandDeclaration& declaration = declarations_[command.type];

    // ===========================================================================================
    // THE STRUCTURAL CHECKS, FIRST AND IN THIS ORDER
    // ===========================================================================================
    //
    // `gameplay-framework`: "The engine SHALL validate structurally before game-specific logic
    // runs: that the participant exists, that it controls the target on the required channel, and
    // that the target accepts the command's capability."
    //
    // The order is not cosmetic. A game rule that ran first would be asked "may this participant
    // build here" about a participant that does not exist, and its answer would be whatever its
    // author happened to write for that case — which is how a client ends up permitted to command
    // units it does not own.
    if (context.session == nullptr ||
        context.session->participant(command.participant) == nullptr) {
        result.reject(ReasonTag::NoSuchParticipant);
        return result;
    }

    if (!declaration.channel.is_empty()) {
        if (control_->source(command.source) == nullptr) {
            result.reject(ReasonTag::NoSuchSource);
            return result;
        }
        if (!command.target.valid()) {
            result.reject(ReasonTag::TargetInvalid);
            return result;
        }
        if (!control_->controls(command.source, command.target, declaration.channel)) {
            result.reject(ValidationReason{ReasonTag::NotControlled, declaration.channel, 0.0F,
                                           0.0F, command.target});
            return result;
        }
    }

    if (declaration.required_capability != 0) {
        if ((capabilities(command.target) & declaration.required_capability) == 0) {
            result.reject(ValidationReason{ReasonTag::CapabilityMissing, declaration.name,
                                           static_cast<f32>(declaration.required_capability),
                                           static_cast<f32>(capabilities(command.target)),
                                           command.target});
            return result;
        }
    }

    // Game-specific logic, only now. A rule may add reasons; it may not remove one.
    for (const auto& rule : rules_) {
        if (rule.type == command.type) {
            rule.rule(context, command, result, rule.user);
        }
    }
    return result;
}

void CommandStream::commit(const GameplayContext& context, u64 tick) noexcept {
    committed_.clear();
    rejections_.clear();

    // The merge. `(producer order, sequence)` and nothing else — never a thread identity, never
    // provenance, never arrival time. Producers are visited in registration order and each one's
    // commands are already in its own record order, so the result is a stable total order without a
    // sort at all.
    for (auto& producer : producers_) {
        CommandBuffer& buffer = *producer;
        for (u32 index = 0; index < buffer.size(); ++index) {
            Command command = buffer.at(index);
            command.tick = tick;
            const ValidationResult result = validate(context, command);
            if (!result.permitted()) {
                (void)rejections_.push_back(Rejection{command, result});
                continue;
            }
            if (Status pushed = committed_.push_back(command); !pushed) {
                continue;
            }
            (void)log_.append(command);
        }
        buffer.clear();
    }
}

Status CommandStream::route_to_group(const Command& prototype, u32 producer,
                                     Array<ecs::Entity>& excluded) noexcept {
    if (producer >= producers_.size()) {
        return fail(ErrorCode::OutOfRange, "gameplay: no such producer");
    }
    if (prototype.type >= declarations_.size()) {
        return fail(ErrorCode::InvalidArgument, "gameplay: undeclared command type");
    }
    const CommandDeclaration& declaration = declarations_[prototype.type];
    const u32 members = control_->group_size(prototype.group);
    for (u32 index = 0; index < members; ++index) {
        const ecs::Entity member = control_->group_member(prototype.group, index);
        if (declaration.required_capability != 0 &&
            (capabilities(member) & declaration.required_capability) == 0) {
            // Reported, not silently ignored. `gameplay-framework`'s "A mixed selection": a build
            // order issued to builders and soldiers reaches the builders, and the exclusion of the
            // soldiers is something the interface can say out loud.
            if (Status pushed = excluded.push_back(member); !pushed) {
                return pushed;
            }
            continue;
        }
        Command command = prototype;
        command.target = member;
        command.group = GroupId{};
        if (Status recorded = producers_[producer]->record(command); !recorded) {
            return recorded;
        }
    }
    return ok();
}

}  // namespace cy::gameplay
