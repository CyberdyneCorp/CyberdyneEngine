// SPDX-License-Identifier: MIT
#pragma once
// THE LOCKSTEP RTS SCENARIO. openspec/changes/add-deterministic-math, tasks 4.2 and 7.2.
//
// A game wired the way a lockstep RTS wires the engine, small enough to run in a test and real
// enough that its digest means something:
//
//   * a baked navigation mesh — a square map of 2 m quads with two walls and a central block cut
//     out of it — converted into a `Fixed` world at load, and terrain heights cooked to `Fixed16`;
//   * squads of units in the four corners, each unit a circle in the kinematic mover;
//   * one participant per squad, each issuing a `MoveOrder` command every `order_every` ticks
//     whose target comes from the session's authoritative random stream (`unit_fixed_raw`), carried
//     in the payload as raw `Fixed` values through a `CommandStream` running under `Lockstep`, so
//     a float payload would have been refused at declaration;
//   * executing an order: A* and the funnel from every unit of the squad to its slot in a formation
//     around the target, in the `Fixed` world — or, for the 2 000-unit movement scenario, one path
//     per squad that every unit follows to its own slot;
//   * every tick: commit, execute, follow paths, step the mover, and hash.
//
// TWO PEERS IN ONE PROCESS. `issue()` is the issuing side: it draws the orders and records them.
// `receive()` is every other peer: it records exactly the commands the issuer's log holds for that
// tick, and nothing else. Two sessions built separately — each converts its own mesh, cooks its own
// heights and plans its own paths — and driven by one command log must agree on every tick's hash.
// That is the claim lockstep makes, and `integration.movement_lockstep` checks it on one host;
// `determinism.cross_leg` publishes the final digest so the comparator checks it between
// architectures.
//
// Header-only and free of the test harness, because three places run it: the movement suites,
// tests/determinism/test_cross_leg.cpp and benchmarks/movement/.

#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/determinism/random.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/movement/height_field.h>
#include <cy/movement/mover.h>
#include <cy/movement/nav_mesh.h>
#include <cy/movement/path.h>
#include <cy/navigation/navmesh.h>

#include <memory>
#include <utility>
#include <vector>

namespace cy::movement_test {

using detmath::Fixed;
using detmath::FixedVec2;

/// What a scenario run is. Every field is part of the digest's definition.
struct RtsConfig {
    u32 squads = 4;
    u32 units_per_squad = 64;
    u32 ticks = 480;
    u32 order_every = 60;
    /// Quads per side of the map; each quad is `kRtsCellMetres` on a side.
    u32 map_cells = 32;
    u64 seed = 0x5EED'4757'0001ULL;
    /// True: every unit plans its own path (A* and the funnel per unit). False: each squad plans
    /// one path from its centre and every unit follows it to its own slot, which is how an RTS
    /// moves a large group — and what keeps 2 000 units inside a determinism suite's budget.
    bool per_unit_paths = true;
};

/// The lockstep RTS configuration `detmath-lockstep-digest` publishes: 256 units for 480 ticks.
[[nodiscard]] constexpr RtsConfig lockstep_config() noexcept {
    return RtsConfig{};
}

/// The movement scenario `detmath-movement-digest` publishes (design §10.2: 2 000 units, crowd
/// separation, seeded random orders, 600 ticks).
[[nodiscard]] constexpr RtsConfig movement_config() noexcept {
    RtsConfig config;
    config.units_per_squad = 500;
    config.ticks = 600;
    config.order_every = 100;
    config.map_cells = 64;
    config.seed = 0x5EED'4757'0002ULL;
    config.per_unit_paths = false;
    return config;
}

inline constexpr i32 kRtsCellMetres = 2;
inline constexpr u32 kMoveOrderStableId = 0x4D4F5645U;  // "MOVE"

/// The order a participant gives its squad. Raw `Fixed` values: the issuer converted, every other
/// peer reads these bits.
struct MoveOrder {
    u32 squad = 0;
    u32 reserved = 0;
    i64 target_x = 0;
    i64 target_z = 0;
};

inline constexpr gameplay::PayloadField kMoveOrderFields[] = {
    {"squad", gameplay::PayloadFieldKind::Integer},
    {"reserved", gameplay::PayloadFieldKind::Integer},
    {"target_x", gameplay::PayloadFieldKind::Fixed},
    {"target_z", gameplay::PayloadFieldKind::Fixed},
};

/// Whether the quad at (`row`, `column`) of an `n`-quad map is walkable: two walls and a block.
[[nodiscard]] constexpr bool rts_walkable(u32 n, u32 row, u32 column) noexcept {
    const bool west_wall = column == n / 4 && row >= n / 4 && row < (5 * n) / 8;
    const bool east_wall = column == (3 * n) / 4 && row >= (3 * n) / 8 && row < (3 * n) / 4;
    const bool block =
        row >= (3 * n) / 8 && row < (5 * n) / 8 && column >= (7 * n) / 16 && column < (9 * n) / 16;
    return !(west_wall || east_wall || block);
}

/// The baked mesh: what a bake would have written, in float, as a cooked asset holds it.
[[nodiscard]] inline bool bake_rts_mesh(u32 n, navigation::NavMesh& mesh) noexcept {
    navigation::NavTileData data(mesh.allocator());
    data.coord = navigation::TileCoord{0, 0, 0};
    for (u32 row = 0; row <= n; ++row) {
        for (u32 column = 0; column <= n; ++column) {
            // A gentle, exactly representable slope, so heights are not all zero.
            const f32 y = static_cast<f32>((row + column) % 8) * 0.125F;
            if (!data.vertices().push_back(Vec3{static_cast<f32>(column * kRtsCellMetres), y,
                                                static_cast<f32>(row * kRtsCellMetres)})) {
                return false;
            }
        }
    }
    for (u32 row = 0; row < n; ++row) {
        for (u32 column = 0; column < n; ++column) {
            if (!rts_walkable(n, row, column)) {
                continue;
            }
            navigation::NavPoly poly;
            poly.first_corner = static_cast<u32>(data.corners().size());
            poly.corner_count = 4;
            if (!data.polys().push_back(poly)) {
                return false;
            }
            const u32 base = row * (n + 1);
            const u32 corner[4] = {base + column, base + column + 1, base + n + 1 + column + 1,
                                   base + n + 1 + column};
            for (const u32 index : corner) {
                if (!data.corners().push_back(index)) {
                    return false;
                }
            }
        }
    }
    data.finalise();
    return mesh.add_tile(std::move(data)).has_value();
}

/// One peer's session.
class RtsSession {
public:
    explicit RtsSession(const RtsConfig& config) noexcept
        : config_(config),
          source_(allocator(), Name::intern("rts.nav"),
                  static_cast<f32>(config.map_cells * kRtsCellMetres)),
          mesh_(allocator()),
          heights_(allocator()),
          mover_(allocator(), movement::MoverParams{}),
          search_(allocator()),
          corridor_(allocator()),
          squad_path_(allocator()),
          session_(allocator(), config.seed),
          control_(allocator()),
          commands_(allocator(), control_),
          random_(determinism::RandomSource(config.seed).stream("rts.orders")) {}

    RtsSession(const RtsSession&) = delete;
    RtsSession& operator=(const RtsSession&) = delete;

    /// Load the map, spawn the squads, declare the command and the participants. False on any
    /// refusal — including the command stream refusing the order's payload under `Lockstep`.
    [[nodiscard]] bool setup() noexcept {
        if (!bake_rts_mesh(config_.map_cells, source_) || !mesh_.convert(source_) ||
            !cook_heights() ||
            !commands_.set_determinism_profile(determinism::DeterminismProfile::Lockstep)) {
            return false;
        }
        gameplay::CommandDeclaration order;
        order.name = Name::intern("rts.MoveOrder");
        order.stable_id = kMoveOrderStableId;
        order.payload = gameplay::PayloadLayout{"MoveOrder", kMoveOrderFields, 4};
        auto declared = commands_.declare(order);
        if (!declared) {
            return false;
        }
        order_type_ = *declared;
        for (u32 squad = 0; squad < config_.squads; ++squad) {
            auto participant =
                session_.add_participant(gameplay::ParticipantKind::LocalHuman, Name::intern("p"));
            auto producer = commands_.open_producer(Name::intern("rts.player"));
            if (!participant || !producer) {
                return false;
            }
            participants_.push_back(*participant);
            producers_.push_back(*producer);
        }
        return spawn();
    }

    /// The issuing side: every participant whose turn it is draws an order and records it.
    [[nodiscard]] bool issue(u64 tick) noexcept {
        if (config_.order_every == 0 || tick % config_.order_every != 0) {
            return true;
        }
        const Fixed size = Fixed::from_int(static_cast<i32>(config_.map_cells) * kRtsCellMetres);
        const determinism::SimulationPoint at{determinism::Epoch{0}, tick};
        for (u32 squad = 0; squad < config_.squads; ++squad) {
            MoveOrder payload;
            payload.squad = squad;
            payload.target_x = (Fixed::from_raw(random_.unit_fixed_raw(at, squad, 0)) * size).raw;
            payload.target_z = (Fixed::from_raw(random_.unit_fixed_raw(at, squad, 1)) * size).raw;
            gameplay::Command command;
            command.type = order_type_;
            command.tick = tick;
            command.participant = participants_[squad];
            if (!command.set_payload(payload) ||
                !commands_.producer(producers_[squad]).record(command)) {
                return false;
            }
        }
        return true;
    }

    /// Every other peer: record exactly what `log` holds for `tick`, from `cursor` on.
    [[nodiscard]] bool receive(const gameplay::CommandLog& log, u32& cursor, u64 tick) noexcept {
        for (; cursor < log.size() && log.at(cursor).tick == tick; ++cursor) {
            const gameplay::Command& command = log.at(cursor);
            u32 producer = 0;
            while (producer < participants_.size() &&
                   participants_[producer] != command.participant) {
                ++producer;
            }
            if (producer == participants_.size() ||
                !commands_.producer(producers_[producer]).record(command)) {
                return false;
            }
        }
        return true;
    }

    /// Commit, execute the orders, steer every unit along its path, and step.
    [[nodiscard]] bool advance(u64 tick, jobs::JobSystem* jobs = nullptr) noexcept {
        gameplay::GameplayContext context;
        context.session = &session_;
        context.services = &session_.services();
        context.commands = &commands_;
        context.at = determinism::SimulationPoint{determinism::Epoch{0}, tick};
        commands_.commit(context, tick);
        for (u32 index = 0; index < commands_.committed_count(); ++index) {
            if (!execute(commands_.committed(index))) {
                return false;
            }
        }
        const Fixed arrival = Fixed::from_raw(Fixed::kOneRaw / 4);
        const Fixed rate = Fixed::from_int(mover_.params().tick_rate);
        for (u32 unit = 0; unit < mover_.size(); ++unit) {
            mover_.set_desired_velocity(
                unit, movement::follow_path(paths_[unit].span(), mover_.position(unit),
                                            mover_.max_speed(unit), arrival, rate, cursors_[unit]));
        }
        return mover_.step(jobs).has_value();
    }

    /// The tick's state: the mover's, and where every unit is along its path.
    [[nodiscard]] u64 state_hash() const noexcept {
        u64 fold = mover_.state_hash();
        for (u32 unit = 0; unit < mover_.size(); ++unit) {
            fold = hash_combine(fold, cursors_[unit]);
            fold = hash_combine(fold, paths_[unit].size());
        }
        return fold;
    }

    /// What the session is before tick 0: the converted mesh, the cooked heights, the mover's
    /// parameters and the seed. Peers that disagree here disagree before the first tick.
    [[nodiscard]] u64 world_hash() const noexcept {
        u64 fold = hash_combine(mesh_.digest(), mover_.params_hash());
        for (const detmath::Fixed16 sample : heights_.samples()) {
            fold = hash_combine(fold, static_cast<u64>(static_cast<u32>(sample.raw)));
        }
        return hash_combine(fold, config_.seed);
    }

    [[nodiscard]] const gameplay::CommandLog& log() const noexcept { return commands_.log(); }
    [[nodiscard]] const movement::KinematicMover& mover() const noexcept { return mover_; }
    [[nodiscard]] const movement::FixedNavMesh& mesh() const noexcept { return mesh_; }
    /// Orders executed so far, and how many of their unit paths reached the target's polygon.
    [[nodiscard]] u32 paths_planned() const noexcept { return paths_planned_; }
    [[nodiscard]] u32 paths_found() const noexcept { return paths_found_; }

private:
    [[nodiscard]] static Allocator& allocator() noexcept {
        return system_allocator(MemoryDomain::World);
    }

    [[nodiscard]] bool cook_heights() noexcept {
        // One sample per quad corner, the same exactly representable slope the bake used.
        const u32 side = config_.map_cells + 1;
        std::vector<f32> heights(usize{side} * side);
        for (u32 row = 0; row < side; ++row) {
            for (u32 column = 0; column < side; ++column) {
                heights[(row * side) + column] = static_cast<f32>((row + column) % 8) * 0.125F;
            }
        }
        if (!heights_.cook(Span<const f32>(heights.data(), heights.size()), side, side, FixedVec2{},
                           1)) {
            return false;
        }
        mover_.bind(&mesh_, &heights_);
        return true;
    }

    /// The formation slot of the `k`-th unit of a squad: rows of `width`, 1.25 m apart, centred.
    [[nodiscard]] FixedVec2 slot(u32 k) const noexcept {
        const u32 width = formation_width();
        const Fixed spacing = Fixed::from_raw((Fixed::kOneRaw * 5) / 4);
        const auto half = static_cast<i32>(width / 2);
        return FixedVec2{Fixed::from_int(static_cast<i32>(k % width) - half) * spacing,
                         Fixed::from_int(static_cast<i32>(k / width) - half) * spacing};
    }

    [[nodiscard]] u32 formation_width() const noexcept {
        u32 width = 1;
        while (width * width < config_.units_per_squad) {
            ++width;
        }
        return width;
    }

    /// Each squad in formation in its own corner of the map.
    [[nodiscard]] bool spawn() noexcept {
        const u32 width = formation_width();
        const Fixed spacing = Fixed::from_raw((Fixed::kOneRaw * 5) / 4);
        const Fixed span = Fixed::from_int(static_cast<i32>(width - 1)) * spacing;
        const Fixed size = Fixed::from_int(static_cast<i32>(config_.map_cells) * kRtsCellMetres);
        const Fixed margin = Fixed::from_int(2);
        u64 entity = 1;
        for (u32 squad = 0; squad < config_.squads; ++squad) {
            const Fixed left = (squad % 2 == 0) ? margin : size - margin - span;
            const Fixed near = (squad / 2 % 2 == 0) ? margin : size - margin - span;
            for (u32 k = 0; k < config_.units_per_squad; ++k) {
                movement::UnitDesc unit;
                unit.entity = entity++;
                unit.position =
                    FixedVec2{left + Fixed::from_int(static_cast<i32>(k % width)) * spacing,
                              near + Fixed::from_int(static_cast<i32>(k / width)) * spacing};
                unit.max_speed =
                    Fixed::from_int(4) + Fixed::from_raw((Fixed::kOneRaw / 8) * (k % 5));
                if (!mover_.add(unit)) {
                    return false;
                }
                squads_.push_back(squad);
                paths_.emplace_back(allocator());
                cursors_.push_back(0);
            }
        }
        return true;
    }

    [[nodiscard]] bool execute(const gameplay::Command& command) noexcept {
        MoveOrder order;
        if (command.type != order_type_ || !command.read_payload(order)) {
            return true;
        }
        const FixedVec2 target{Fixed::from_raw(order.target_x), Fixed::from_raw(order.target_z)};
        if (!config_.per_unit_paths && !plan_squad(order.squad, target)) {
            return false;
        }
        u32 k = 0;
        for (u32 unit = 0; unit < mover_.size(); ++unit) {
            if (squads_[unit] != order.squad) {
                continue;
            }
            const FixedVec2 destination = target + slot(k++);
            cursors_[unit] = 0;
            if (config_.per_unit_paths) {
                const movement::FixedPathResult result =
                    movement::find_path(search_, mesh_, mover_.position(unit), destination, 4096,
                                        corridor_, paths_[unit]);
                ++paths_planned_;
                paths_found_ += result.found ? 1U : 0U;
                continue;
            }
            // The squad's path without its first point (the squad's centre), ending at this unit's
            // own slot.
            paths_[unit].clear();
            for (usize point = 1; point + 1 < squad_path_.size(); ++point) {
                if (!paths_[unit].push_back(squad_path_[point])) {
                    return false;
                }
            }
            if (!paths_[unit].push_back(destination)) {
                return false;
            }
        }
        return true;
    }

    /// One path for the whole squad, from the mean position of its units.
    [[nodiscard]] bool plan_squad(u32 squad, FixedVec2 target) noexcept {
        Fixed sum_x;
        Fixed sum_z;
        i32 count = 0;
        for (u32 unit = 0; unit < mover_.size(); ++unit) {
            if (squads_[unit] == squad) {
                sum_x += mover_.position(unit).x;
                sum_z += mover_.position(unit).y;
                ++count;
            }
        }
        if (count == 0) {
            return true;
        }
        const FixedVec2 centre{sum_x / Fixed::from_int(count), sum_z / Fixed::from_int(count)};
        const movement::FixedPathResult result =
            movement::find_path(search_, mesh_, centre, target, 4096, corridor_, squad_path_);
        ++paths_planned_;
        paths_found_ += result.found ? 1U : 0U;
        return true;
    }

    RtsConfig config_;
    navigation::NavMesh source_;
    movement::FixedNavMesh mesh_;
    movement::FixedHeightField heights_;
    movement::KinematicMover mover_;
    movement::FixedPathSearch search_;
    Array<movement::FixedPolyIndex> corridor_;
    Array<FixedVec2> squad_path_;
    gameplay::GameSession session_;
    gameplay::ControlRegistry control_;
    gameplay::CommandStream commands_;
    determinism::RandomStream random_;
    gameplay::CommandTypeId order_type_ = gameplay::kInvalidCommandType;
    std::vector<gameplay::ParticipantId> participants_;
    std::vector<u32> producers_;
    std::vector<u32> squads_;
    std::vector<Array<FixedVec2>> paths_;
    std::vector<u32> cursors_;
    u32 paths_planned_ = 0;
    u32 paths_found_ = 0;
};

/// The seed the per-tick fold starts from. A committed constant: a digest whose seed moved is a
/// digest of a different question.
inline constexpr u64 kRtsFoldSeed = 0xC1'BE'12'0A'5EED'0007ULL;

/// One run of `config` on one peer, its per-tick state hashes folded in tick order with
/// `hash_combine` — order-sensitive, so the same hashes in another order are another digest.
struct RtsDigest {
    u64 world = 0;
    u64 digest = 0;
    u64 final_hash = 0;
    u32 units = 0;
    u32 ticks = 0;
    u32 paths_planned = 0;
    u32 paths_found = 0;
    bool complete = false;
};

[[nodiscard]] inline RtsDigest run_rts(const RtsConfig& config,
                                       jobs::JobSystem* jobs = nullptr) noexcept {
    RtsDigest result;
    auto session = std::make_unique<RtsSession>(config);
    if (!session->setup()) {
        return result;
    }
    result.world = session->world_hash();
    u64 fold = hash_combine(kRtsFoldSeed, result.world);
    for (u64 tick = 0; tick < config.ticks; ++tick) {
        if (!session->issue(tick) || !session->advance(tick, jobs)) {
            return result;
        }
        result.final_hash = session->state_hash();
        fold = hash_combine(fold, result.final_hash);
    }
    result.digest = fold;
    result.units = session->mover().size();
    result.ticks = config.ticks;
    result.paths_planned = session->paths_planned();
    result.paths_found = session->paths_found();
    result.complete = true;
    return result;
}

/// Two peers of one lockstep session: the issuer, and a follower driven by the issuer's command log
/// alone. Each digest is that peer's own fold; `disagreements` counts the ticks whose state hashes
/// differed, which a lockstep session requires to be none.
struct RtsPairDigest {
    RtsDigest issuer;
    RtsDigest follower;
    u32 disagreements = 0;
};

[[nodiscard]] inline RtsPairDigest run_lockstep_pair(const RtsConfig& config) noexcept {
    RtsPairDigest pair;
    auto issuer = std::make_unique<RtsSession>(config);
    auto follower = std::make_unique<RtsSession>(config);
    if (!issuer->setup() || !follower->setup()) {
        return pair;
    }
    pair.issuer.world = issuer->world_hash();
    pair.follower.world = follower->world_hash();
    u64 issuer_fold = hash_combine(kRtsFoldSeed, pair.issuer.world);
    u64 follower_fold = hash_combine(kRtsFoldSeed, pair.follower.world);
    u32 cursor = 0;
    for (u64 tick = 0; tick < config.ticks; ++tick) {
        if (!issuer->issue(tick) || !issuer->advance(tick) ||
            !follower->receive(issuer->log(), cursor, tick) || !follower->advance(tick)) {
            return pair;
        }
        pair.issuer.final_hash = issuer->state_hash();
        pair.follower.final_hash = follower->state_hash();
        pair.disagreements += pair.issuer.final_hash == pair.follower.final_hash ? 0U : 1U;
        issuer_fold = hash_combine(issuer_fold, pair.issuer.final_hash);
        follower_fold = hash_combine(follower_fold, pair.follower.final_hash);
    }
    for (RtsDigest* peer : {&pair.issuer, &pair.follower}) {
        peer->units = issuer->mover().size();
        peer->ticks = config.ticks;
        peer->complete = true;
    }
    pair.issuer.digest = issuer_fold;
    pair.follower.digest = follower_fold;
    pair.issuer.paths_planned = issuer->paths_planned();
    pair.issuer.paths_found = issuer->paths_found();
    pair.follower.paths_planned = follower->paths_planned();
    pair.follower.paths_found = follower->paths_found();
    return pair;
}

}  // namespace cy::movement_test
