// SPDX-License-Identifier: MIT
// The lockstep session. See cy/game_backend/lockstep_session.h. Integer arithmetic only, after the
// one conversion of the baked mesh in `load()`.

#include <cy/core/determinism/epoch.h>
#include <cy/core/determinism/profile.h>
#include <cy/core/memory/hash.h>
#include <cy/game_backend/lockstep_session.h>
#include <cy/movement/determinism.h>
#include <cy/navigation/determinism.h>

#include <initializer_list>

namespace cy::game_backend {
namespace {

constexpr gameplay::PayloadField kOrderFields[] = {
    {"kind", gameplay::PayloadFieldKind::Integer},
    {"group", gameplay::PayloadFieldKind::Integer},
    {"target_x", gameplay::PayloadFieldKind::Fixed},
    {"target_z", gameplay::PayloadFieldKind::Fixed},
};

/// The seed the per-tick fold starts from.
constexpr u64 kDigestSeed = 0x10C5'7E90'0001'0008ULL;

[[nodiscard]] Error refused(ErrorCode code, const char* message) noexcept {
    return Error{code, message};
}

}  // namespace

LockstepSession::LockstepSession(Allocator& allocator, const LockstepConfig& config) noexcept
    : allocator_(allocator),
      config_(config),
      mesh_(allocator),
      mover_(allocator, config.mover),
      crowd_(allocator, Fixed::from_int(4)),
      search_(allocator),
      corridor_(allocator),
      session_(allocator, config.seed),
      control_(allocator),
      commands_(allocator, control_),
      groups_(allocator),
      entities_(allocator),
      paths_(allocator),
      cursors_(allocator),
      moving_(allocator),
      arrived_(allocator) {}

Status LockstepSession::load(const navigation::NavMesh& baked) noexcept {
    if (started_) {
        return make_unexpected(
            refused(ErrorCode::PermissionDenied, "the world is loaded before the first tick"));
    }
    if (!mesh_.convert(baked)) {
        return make_unexpected(
            refused(ErrorCode::InvalidArgument, "the baked mesh did not convert to a Fixed world"));
    }
    mover_.bind(&mesh_, nullptr);
    loaded_ = true;
    return ok();
}

Expected<u32, Error> LockstepSession::enlist(const LockstepUnitSpec& spec) noexcept {
    if (started_) {
        return make_unexpected(
            refused(ErrorCode::PermissionDenied, "a unit is enlisted before the first tick"));
    }
    movement::UnitDesc unit;
    // The unit's identity in the session is its index: entity ids are a peer's own business.
    unit.entity = static_cast<u64>(mover_.size()) + 1U;
    unit.position = spec.position;
    if (spec.radius.raw > 0) {
        unit.radius = spec.radius;
    }
    if (spec.max_speed.raw > 0) {
        unit.max_speed = spec.max_speed;
    }
    auto added = mover_.add(unit);
    if (!added) {
        return make_unexpected(added.error());
    }
    if (!groups_.push_back(spec.group) || !entities_.push_back(spec.entity) ||
        !paths_.emplace_back(allocator_) || !cursors_.push_back(0) || !moving_.push_back(0) ||
        !arrived_.push_back(0)) {
        return make_unexpected(refused(ErrorCode::OutOfMemory, "enlisting a unit"));
    }
    return *added;
}

Status LockstepSession::admit_profile() noexcept {
    determinism::DeterminismConfiguration registry(allocator_);
    const navigation::NavWorldDeclaration world{"lockstep", navigation::NavArithmetic::Fixed, true,
                                                false};
    if (!registry.declare(movement::movement_determinism()) ||
        !registry.declare(navigation::navigation_determinism(
            Span<const navigation::NavWorldDeclaration>(&world, 1))) ||
        !registry.declare(commands_.determinism_declaration())) {
        return make_unexpected(
            refused(ErrorCode::Internal, "the session's subsystems could not be declared"));
    }
    const auto verdict =
        registry.require(determinism::DeterminismProfile::Lockstep, movement::movement_build());
    if (!verdict) {
        return make_unexpected(
            refused(ErrorCode::PermissionDenied, "the Lockstep profile check refused the session"));
    }
    admitted_subsystems_ = verdict->authoritative_subsystems;
    return ok();
}

Status LockstepSession::start() noexcept {
    if (started_) {
        return ok();
    }
    if (!loaded_) {
        return make_unexpected(
            refused(ErrorCode::InvalidArgument, "a lockstep session needs a world: load() it"));
    }
    if (Status profile =
            commands_.set_determinism_profile(determinism::DeterminismProfile::Lockstep);
        !profile) {
        return profile;
    }
    gameplay::CommandDeclaration order;
    order.name = Name::intern("lockstep.Order");
    order.stable_id = kLockstepOrderStableId;
    order.payload = gameplay::PayloadLayout{"LockstepOrder", kOrderFields, 4};
    auto declared = commands_.declare(order);
    if (!declared) {
        return make_unexpected(declared.error());
    }
    order_type_ = *declared;
    if (Status admitted = admit_profile(); !admitted) {
        return admitted;
    }
    auto participant =
        session_.add_participant(gameplay::ParticipantKind::LocalHuman, Name::intern("orders"));
    auto producer = commands_.open_producer(Name::intern("lockstep.orders"));
    if (!participant || !producer) {
        return make_unexpected(
            refused(ErrorCode::OutOfMemory, "opening the session's participant"));
    }
    participant_ = *participant;
    producer_ = *producer;
    world_hash_ = compute_world_hash();
    state_hash_ = world_hash_;
    digest_ = hash_combine(kDigestSeed, world_hash_);
    started_ = true;
    return ok();
}

bool LockstepSession::has_group(u32 group) const noexcept {
    for (const u32 member : groups_.span()) {
        if (member == group) {
            return true;
        }
    }
    return false;
}

Status LockstepSession::record_command(gameplay::Command command) noexcept {
    if (Status begun = start(); !begun) {
        return begun;
    }
    command.tick = tick_;
    command.participant = participant_;
    if (!commands_.producer(producer_).record(command)) {
        return make_unexpected(refused(ErrorCode::OutOfMemory, "recording an order"));
    }
    return ok();
}

Status LockstepSession::record(const LockstepOrderPayload& order) noexcept {
    if (order.kind > static_cast<u32>(LockstepOrderKind::Stop)) {
        return make_unexpected(
            refused(ErrorCode::InvalidArgument, "an order's kind is Move or Stop"));
    }
    if (!has_group(order.group)) {
        return make_unexpected(refused(ErrorCode::NotFound, "no unit is in that group"));
    }
    if (Status begun = start(); !begun) {
        return begun;
    }
    gameplay::Command command;
    command.type = order_type_;
    if (!command.set_payload(order)) {
        return make_unexpected(refused(ErrorCode::Internal, "the order does not fit a command"));
    }
    return record_command(command);
}

Status LockstepSession::receive(const gameplay::CommandLog& log, u32& cursor) noexcept {
    if (Status begun = start(); !begun) {
        return begun;
    }
    for (; cursor < log.size() && log.at(cursor).tick == tick_; ++cursor) {
        if (Status recorded = record_command(log.at(cursor)); !recorded) {
            return recorded;
        }
    }
    return ok();
}

FixedVec2 LockstepSession::slot(u32 k, u32 group_size) const noexcept {
    u32 width = 1;
    while (width * width < group_size) {
        ++width;
    }
    const auto half = static_cast<i32>(width / 2);
    const Fixed spacing = config_.formation_spacing;
    return FixedVec2{Fixed::from_int(static_cast<i32>(k % width) - half) * spacing,
                     Fixed::from_int(static_cast<i32>(k / width) - half) * spacing};
}

Status LockstepSession::move_group(u32 group, FixedVec2 target) noexcept {
    u32 size = 0;
    for (const u32 member : groups_.span()) {
        size += member == group ? 1U : 0U;
    }
    u32 k = 0;
    for (u32 unit = 0; unit < mover_.size(); ++unit) {
        if (groups_[unit] != group) {
            continue;
        }
        const movement::FixedPathResult result =
            movement::find_path(search_, mesh_, mover_.position(unit), target + slot(k++, size),
                                config_.node_budget, corridor_, paths_[unit]);
        ++paths_planned_;
        paths_found_ += result.found ? 1U : 0U;
        cursors_[unit] = 0;
        moving_[unit] = paths_[unit].empty() ? 0U : 1U;
    }
    return ok();
}

Status LockstepSession::execute(const gameplay::Command& command) noexcept {
    LockstepOrderPayload order;
    if (command.type != order_type_ || !command.read_payload(order)) {
        return ok();
    }
    ++orders_executed_;
    if (order.kind == static_cast<u32>(LockstepOrderKind::Move)) {
        return move_group(order.group, FixedVec2{Fixed::from_raw(order.target_x),
                                                 Fixed::from_raw(order.target_z)});
    }
    for (u32 unit = 0; unit < mover_.size(); ++unit) {
        if (groups_[unit] == order.group) {
            paths_[unit].clear();
            cursors_[unit] = 0;
            moving_[unit] = 0;
        }
    }
    return ok();
}

Status LockstepSession::advance(jobs::JobSystem* jobs) noexcept {
    if (Status begun = start(); !begun) {
        return begun;
    }
    gameplay::GameplayContext context;
    context.session = &session_;
    context.services = &session_.services();
    context.commands = &commands_;
    context.at = determinism::SimulationPoint{determinism::Epoch{0}, tick_};
    commands_.commit(context, tick_);
    for (u32 index = 0; index < commands_.committed_count(); ++index) {
        if (Status executed = execute(commands_.committed(index)); !executed) {
            return executed;
        }
    }
    const Fixed rate = Fixed::from_int(mover_.params().tick_rate);
    for (u32 unit = 0; unit < mover_.size(); ++unit) {
        arrived_[unit] = 0;
        mover_.set_desired_velocity(
            unit,
            movement::follow_path(paths_[unit].span(), mover_.position(unit),
                                  mover_.max_speed(unit), config_.arrival, rate, cursors_[unit]));
        if (moving_[unit] != 0U && cursors_[unit] >= paths_[unit].size()) {
            moving_[unit] = 0;
            arrived_[unit] = 1;
        }
    }
    if (config_.avoidance) {
        navigation::CrowdReport report;
        if (Status avoided = movement::avoid(mover_, crowd_, config_.avoidance_params, report);
            !avoided) {
            return avoided;
        }
    }
    if (auto stepped = mover_.step(jobs); !stepped) {
        return make_unexpected(stepped.error());
    }
    ++tick_;
    state_hash_ = compute_state_hash();
    digest_ = hash_combine(digest_, state_hash_);
    return ok();
}

Expected<LockstepUnitState, Error> LockstepSession::unit(u32 index) const noexcept {
    if (index >= mover_.size()) {
        return make_unexpected(refused(ErrorCode::NotFound, "no unit was enlisted at that index"));
    }
    LockstepUnitState state;
    state.group = groups_[index];
    state.entity = entities_[index];
    state.position = mover_.position(index);
    state.velocity = mover_.velocity(index);
    state.height = mover_.height(index);
    state.heading = mover_.heading(index);
    state.moving = moving_[index] != 0U;
    state.just_arrived = arrived_[index] != 0U;
    return state;
}

u64 LockstepSession::compute_state_hash() const noexcept {
    u64 fold = hash_combine(mover_.state_hash(), tick_);
    for (u32 unit = 0; unit < mover_.size(); ++unit) {
        fold = hash_combine(fold, cursors_[unit]);
        fold = hash_combine(fold, paths_[unit].size());
        fold = hash_combine(fold, moving_[unit]);
    }
    return fold;
}

u64 LockstepSession::compute_world_hash() const noexcept {
    u64 fold = hash_combine(hash_combine(mesh_.digest(), mover_.params_hash()), config_.seed);
    fold = hash_combine(fold, config_.avoidance ? 1U : 0U);
    const movement::FixedAvoidanceParams& crowd = config_.avoidance_params;
    for (const Fixed value : {crowd.radius, crowd.max_speed, crowd.max_acceleration,
                              crowd.neighbour_distance, crowd.time_horizon, crowd.separation_weight,
                              config_.arrival, config_.formation_spacing}) {
        fold = hash_combine(fold, static_cast<u64>(value.raw));
    }
    fold = hash_combine(fold, crowd.max_neighbours);
    fold = hash_combine(fold, crowd.priority);
    fold = hash_combine(fold, config_.node_budget);
    for (u32 unit = 0; unit < mover_.size(); ++unit) {
        fold = hash_combine(fold, groups_[unit]);
    }
    return hash_combine(fold, mover_.state_hash());
}

}  // namespace cy::game_backend
