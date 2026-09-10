// The game, built: the level's geometry and entities, and the navigation mesh over them.
// M8.b section 12. `systems.cpp` is the other half — the programs, the characters and the tick.

#include "internals.h"

#include <cy/core/determinism/commit.h>
#include <cy/graph/audit.h>

#include "presentation.h"

namespace cy::sample::slice {

const char* mesh_kind_name(MeshKind kind) noexcept {
    switch (kind) {
        case MeshKind::Ground:
            return "ground";
        case MeshKind::Wall:
            return "wall";
        case MeshKind::Crate:
            return "crate";
        case MeshKind::Pillar:
            return "pillar";
        case MeshKind::Character:
            return "character";
        case MeshKind::Count:
            break;
    }
    return "unknown";
}

const char* material_kind_name(MaterialKind kind) noexcept {
    switch (kind) {
        case MaterialKind::Ground:
            return "ground";
        case MaterialKind::Wall:
            return "wall";
        case MaterialKind::Crate:
            return "crate";
        case MaterialKind::Pillar:
            return "pillar";
        case MaterialKind::TeamBlue:
            return "team-blue";
        case MaterialKind::TeamRed:
            return "team-red";
        case MaterialKind::Count:
            break;
    }
    return "unknown";
}

Characters::Characters(Allocator& allocator) noexcept
    : entities(allocator),
      positions(allocator),
      forward(allocator),
      goals(allocator),
      crowd(allocator),
      animation_slot(allocator),
      team(allocator) {}

// --- Construction
// ----------------------------------------------------------------------------------

Slice::Slice(Allocator& allocator) noexcept
    : allocator_(&allocator), characters_(allocator), shot_(allocator) {}

Slice::~Slice() {
    delete presentation_;
    delete kit_;
    delete brain_;
    delete level_;
}

Status Slice::build(const Options& options) noexcept {
    options_ = options;
    report_.agents = options.agents;
    report_.ticks = options.ticks;

    const f32 arena = arena_half_extent(options.agents);
    level_ = new (std::nothrow) Level(*allocator_, arena);
    brain_ = new (std::nothrow) Brain(*allocator_, crowd_cell_size(options.agents, arena));
    kit_ = new (std::nothrow) Kit(*allocator_, options.seed);
    presentation_ = new (std::nothrow) Presentation(*allocator_);
    if (level_ == nullptr || brain_ == nullptr || kit_ == nullptr || presentation_ == nullptr) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the slice could not allocate its subsystems");
    }
    if (Status started = level_->initialize(); !started) {
        return started;
    }
    if (Status built = build_level(); !built) {
        return built;
    }
    if (Status navigable = build_navmesh(); !navigable) {
        return navigable;
    }
    if (Status compiled = build_programs(); !compiled) {
        return compiled;
    }
    if (Status kitted = build_abilities(); !kitted) {
        return kitted;
    }
    if (Status mustered = build_characters(); !mustered) {
        return mustered;
    }
    // BIND LAST, AND ONCE. Every drawable thing in the world exists by now, and a binding pass is
    // over the world rather than over a list — which is what makes it the same call a host makes
    // after a load or a streaming step. Its report is the artefact's evidence for task 11.3: an
    // authored reference became a handle, and the number that were not answered is visible rather
    // than absent.
    cy::rendering::BindReport bound;
    if (Status resolved = level_->bind_assets(bound); !resolved) {
        return resolved;
    }
    report_.meshes_bound = bound.meshes_bound;
    report_.meshes_unresolved = bound.unresolved_meshes;
    report_.materials_bound = bound.materials_bound;
    report_.mesh_assets = static_cast<u32>(level_->meshes.size());
    return presentation_->build(options_, report_);
}

// --- The level
// -------------------------------------------------------------------------------------

Status Slice::build_level() noexcept {
    Level& level = *level_;
    Status status = cy::ok();

    const f32 extent = level.half_extent;
    const f32 step = (2.0F * extent) / static_cast<f32>(level.ground_cells);
    for (u32 row = 0; row < level.ground_cells; ++row) {
        for (u32 column = 0; column < level.ground_cells; ++column) {
            const f32 x0 = -extent + (static_cast<f32>(column) * step);
            const f32 z0 = -extent + (static_cast<f32>(row) * step);
            add_quad(level.vertices, level.indices, level.areas, Vec3{x0, 0.0F, z0},
                     Vec3{x0 + step, 0.0F, z0}, Vec3{x0 + step, 0.0F, z0 + step},
                     Vec3{x0, 0.0F, z0 + step}, status);
        }
    }
    if (!status) {
        return status;
    }
    if (Status placed = add_prop(Vec3{0.0F, 0.0F, 0.0F}, MeshKind::Ground, MaterialKind::Ground,
                                 Vec3{extent, 0.05F, extent});
        !placed) {
        return placed;
    }

    // Four walls, twelve pillars and eighteen crates. The pillars are the level's occluders, which
    // is what makes the perception pass's line of sight answer something.
    const Vec3 wall_at[4] = {Vec3{0.0F, 1.5F, -extent}, Vec3{0.0F, 1.5F, extent},
                             Vec3{-extent, 1.5F, 0.0F}, Vec3{extent, 1.5F, 0.0F}};
    const Vec3 wall_half[4] = {Vec3{extent, 1.5F, 0.5F}, Vec3{extent, 1.5F, 0.5F},
                               Vec3{0.5F, 1.5F, extent}, Vec3{0.5F, 1.5F, extent}};
    for (u32 index = 0; index < 4U; ++index) {
        if (Status placed =
                add_prop(wall_at[index], MeshKind::Wall, MaterialKind::Wall, wall_half[index]);
            !placed) {
            return placed;
        }
    }
    for (u32 index = 0; index < 12U; ++index) {
        const f32 angle = static_cast<f32>(index) * 0.5235987756F;
        const f32 ring = (extent * 0.38F) + (static_cast<f32>(index % 3U) * extent * 0.19F);
        const Vec3 at{ring * std::cos(angle), 1.4F, ring * std::sin(angle)};
        if (Status placed =
                add_prop(at, MeshKind::Pillar, MaterialKind::Pillar, Vec3{0.7F, 1.4F, 0.7F});
            !placed) {
            return placed;
        }
        if (Status remembered = level.pillars.push_back(Vec3{at.x, 0.0F, at.z}); !remembered) {
            return remembered;
        }
    }
    for (u32 index = 0; index < 18U; ++index) {
        const f32 angle = (static_cast<f32>(index) * 0.3490658504F) + 0.4F;
        const f32 ring = (extent * 0.17F) + (static_cast<f32>(index % 5U) * extent * 0.125F);
        const Vec3 at{ring * std::cos(angle), 0.5F, ring * std::sin(angle)};
        if (Status placed =
                add_prop(at, MeshKind::Crate, MaterialKind::Crate, Vec3{0.5F, 0.5F, 0.5F});
            !placed) {
            return placed;
        }
    }

    report_.level_entities = static_cast<u32>(level.props.size());
    report_.level_triangles = static_cast<u32>(level.indices.size() / 3U);
    return cy::ok();
}

Status Slice::add_prop(Vec3 at, MeshKind kind, MaterialKind paint, Vec3 half) noexcept {
    Level& level = *level_;
    Expected<Entity, Error> entity = level.place(at, kind, paint, half);
    if (!entity) {
        return Status{cy::make_unexpected(entity.error())};
    }
    if (Status pushed = level.props.push_back(*entity); !pushed) {
        return pushed;
    }
    if (Status pushed = level.prop_kind.push_back(kind); !pushed) {
        return pushed;
    }
    return level.prop_bounds.push_back(Aabb::from_center_extents(at, half));
}

Status Slice::build_navmesh() noexcept {
    Level& level = *level_;
    cy::navigation::NavBuildParams params;
    params.cell_size = 0.4F;
    params.cell_height = 0.25F;
    params.agent_radius = kCharacterRadius;
    params.agent_height = 1.8F;

    cy::navigation::NavSourceGeometry geometry;
    geometry.vertices = level.vertices.span();
    geometry.indices = level.indices.span();
    geometry.area = level.areas.span();

    const Aabb bounds = Aabb::from_min_max(Vec3{-level.half_extent, -1.0F, -level.half_extent},
                                           Vec3{level.half_extent, 4.0F, level.half_extent});
    cy::navigation::NavBuildReport build_report;
    Expected<cy::navigation::NavTileData, Error> tile = cy::navigation::build_tile(
        *allocator_, params, geometry, cy::navigation::TileCoord{0, 0, 0}, bounds, build_report);
    if (!tile) {
        return Status{cy::make_unexpected(tile.error())};
    }
    cy::navigation::AgentProfile profile;
    profile.radius = kCharacterRadius;
    profile.height = 1.8F;
    level.mesh.set_profile(profile);
    Expected<cy::navigation::TileChange, Error> change = level.mesh.add_tile(std::move(*tile));
    if (!change) {
        return Status{cy::make_unexpected(change.error())};
    }
    report_.navmesh_polys = build_report.polys;
    report_.navmesh_tiles = level.mesh.tile_count();
    report_.navmesh_recast = build_report.backend == cy::navigation::NavBuildBackend::Recast;
    return cy::ok();
}

}  // namespace cy::sample::slice
