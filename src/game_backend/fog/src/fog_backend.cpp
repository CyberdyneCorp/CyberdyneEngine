// SPDX-License-Identifier: MIT
#include <cy/game_backend/fog_backend.h>

#include <cy/abi/errors.h>

#include <algorithm>

namespace cy::game_backend {
namespace {

namespace fog = cy::gameplay::fog;

// The ABI's enumerators are the simulation's, value for value; this adapter names both.
static_assert(static_cast<u32>(fog::CellState::Unexplored) == CY_FOG_CELL_UNEXPLORED);
static_assert(static_cast<u32>(fog::CellState::Explored) == CY_FOG_CELL_EXPLORED);
static_assert(static_cast<u32>(fog::CellState::Visible) == CY_FOG_CELL_VISIBLE);
static_assert(static_cast<u32>(fog::UnitSight::Hidden) == CY_FOG_SIGHT_HIDDEN);
static_assert(static_cast<u32>(fog::UnitSight::Ghost) == CY_FOG_SIGHT_GHOST);
static_assert(static_cast<u32>(fog::UnitSight::Visible) == CY_FOG_SIGHT_VISIBLE);
static_assert(fog::kMaxFogTeams == 32U, "fog_set_vision's team bound is the grid's");

[[nodiscard]] bool by_entity(const auto& a, CyEntity entity) noexcept {
    return a.entity < entity;
}

}  // namespace

FogAdapter::FogAdapter(Allocator& allocator) noexcept
    : fog_(allocator), visions_(allocator), units_(allocator) {}

Status FogAdapter::create(const gameplay::fog::FogGridDescription& description) noexcept {
    return fog_.create(description);
}

void FogAdapter::set_placer(FogPlacer placer, void* user) noexcept {
    placer_ = placer;
    placer_user_ = user;
}

Status FogAdapter::share_vision(gameplay::TeamId a, gameplay::TeamId b) noexcept {
    return fog_.share_vision(a, b);
}

usize FogAdapter::find(CyEntity entity) const noexcept {
    const auto found =
        std::lower_bound(visions_.begin(), visions_.end(), entity,
                         [](const Vision& a, CyEntity e) { return by_entity(a, e); });
    return found != visions_.end() && found->entity == entity
               ? static_cast<usize>(found - visions_.begin())
               : visions_.size();
}

CyResult FogAdapter::set_vision(CyEntity entity, const CyFogVision& vision) noexcept {
    if (placer_ != nullptr) {
        i64 x = 0;
        i64 z = 0;
        if (!placer_(entity, x, z, placer_user_)) {
            return abi::report(CY_RESULT_NOT_FOUND,
                               "fog_set_vision: the host places no such entity");
        }
    }
    const usize index = find(entity);
    if (index < visions_.size()) {
        visions_[index].vision = vision;
        return CY_RESULT_OK;
    }
    if (Status added = visions_.push_back(Vision{entity, vision}); !added) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "fog_set_vision: no room for another vision");
    }
    std::rotate(std::upper_bound(visions_.begin(), visions_.end() - 1, entity,
                                 [](CyEntity e, const Vision& b) { return e < b.entity; }),
                visions_.end() - 1, visions_.end());
    return CY_RESULT_OK;
}

void FogAdapter::forget(CyEntity entity) noexcept {
    const usize index = find(entity);
    if (index < visions_.size()) {
        visions_.erase(index);
    }
}

CyResult FogAdapter::vision(CyEntity entity, CyFogVision& out) const noexcept {
    const usize index = find(entity);
    if (index == visions_.size()) {
        return abi::report(CY_RESULT_NOT_FOUND, "fog_get_vision: the entity has no vision");
    }
    out = visions_[index].vision;
    out.struct_size = static_cast<u32>(sizeof(CyFogVision));
    return CY_RESULT_OK;
}

Status FogAdapter::update(u64 tick) noexcept {
    units_.clear();
    if (Status reserved = units_.reserve(visions_.size()); !reserved) {
        return reserved;
    }
    for (const Vision& seen : visions_) {
        fog::FogUnit unit;
        if (placer_ == nullptr || !placer_(seen.entity, unit.x_raw, unit.z_raw, placer_user_)) {
            continue;
        }
        unit.id = seen.entity;
        unit.team = seen.vision.team;
        unit.reveal_radius_raw = seen.vision.reveal_radius;
        unit.leaves_ghost = (seen.vision.flags & CY_FOG_LEAVES_GHOST) != 0U;
        (void)units_.push_back(unit);
    }
    return fog_.update(units_.span(), tick);
}

CyResult FogAdapter::cell(u32 team, const CyFixedVec2& position, u32& out) const noexcept {
    if (!fog_.created()) {
        return abi::report(CY_RESULT_UNAVAILABLE, "fog_cell: the host has not sized the grid");
    }
    out = static_cast<u32>(fog_.at(team, position.x, position.y));
    return CY_RESULT_OK;
}

CyResult FogAdapter::sight(u32 team, CyEntity entity, u32& out) const noexcept {
    out = static_cast<u32>(fog_.sight(team, entity));
    return CY_RESULT_OK;
}

CyResult FogAdapter::status(CyFogStatus& out) const noexcept {
    const fog::FogStatistics& statistics = fog_.statistics();
    out.width = fog_.width();
    out.height = fog_.height();
    out.units = statistics.units;
    out.tick = statistics.tick;
    out.visible_cells = statistics.cells_visible_any;
    out.ghosts = statistics.ghosts;
    out.digest = fog_.digest();
    return CY_RESULT_OK;
}

void bind_fog(cy::abi::Host& host, FogAdapter* adapter) noexcept {
    host.game.fog = adapter;
}

}  // namespace cy::game_backend
