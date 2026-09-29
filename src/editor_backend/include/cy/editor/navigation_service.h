// SPDX-License-Identifier: MIT
#pragma once
// The engine's navigation authoring service. Issue #28, task 2.1 of
// `implement-issue-28-navigation-authoring`.
//
// The editor never voxelises, hashes geometry or searches a path. It sends `navigation.*` requests
// and displays what comes back. This service runs the tiling loop (`cy/navigation/bake.h`), owns
// one `NavMesh` per navigation world, and answers path, flow-field and pick queries against it.
// The runtime host supplies the world's data through `NavigationSourceRuntime`, the same way
// `MaterialAuthoringRuntime` supplies the authored scene to `MaterialService`.
//
// The operations, their payloads and their failure codes are listed in
// src/editor_backend/README.md.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/bake.h>
#include <cy/navigation/navmesh.h>

namespace cy::editor {

/// The bake algorithm version that enters every source fingerprint the service computes. Moved
/// when the service's bake changes what it produces from the same inputs.
inline constexpr u32 kNavigationBakeVersion = 1;

/// The `capabilities.get` feature bit a navigation service with a host seam sets.
inline constexpr u64 kNavigationFeature = 0x100U;

/// The sources of one navigation world, as the host gathers them. The service fills a fresh one
/// for every request that reads sources, so the host only appends.
struct NavSourceBuffers {
    Array<Vec3> vertices;
    Array<u32> indices;
    /// One entry per triangle each, or empty for the defaults (layer 0, tag 0, ground).
    Array<u8> layer;
    Array<u8> tag;
    Array<navigation::AreaType> area;
    Array<navigation::NavSurfaceVolume> surfaces;
    Array<navigation::NavAreaVolume> areas;

    explicit NavSourceBuffers(Allocator& allocator) noexcept
        : vertices(allocator),
          indices(allocator),
          layer(allocator),
          tag(allocator),
          area(allocator),
          surfaces(allocator),
          areas(allocator) {}

    /// A view for `bake_tiles` and `source_fingerprint`. Obstacles and links are not part of it:
    /// they are applied to the baked mesh without a rebuild.
    [[nodiscard]] navigation::NavBakeSource source() const noexcept {
        navigation::NavBakeSource out;
        out.geometry = navigation::NavSourceGeometry{vertices.span(), indices.span(), layer.span(),
                                                     tag.span(), area.span()};
        out.surfaces = surfaces.span();
        out.areas = areas.span();
        return out;
    }
};

/// The runtime host's side of navigation authoring. It supplies data and storage; the engine
/// service does every computation. Implemented by samples/05b-editor-window/runtime over the
/// authored world.
class NavigationSourceRuntime {
public:
    virtual ~NavigationSourceRuntime() = default;

    /// The navigation worlds the open scene declares.
    [[nodiscard]] virtual Status worlds(Array<u32>& out) noexcept = 0;
    /// World-space triangles with their per-triangle layer, tag and area, plus the surface and
    /// area volumes, for `world`.
    [[nodiscard]] virtual Status gather(u32 world, NavSourceBuffers& out) noexcept = 0;
    /// The `NavObstacle` footprints of `world`, in world space.
    [[nodiscard]] virtual Status obstacles(u32 world,
                                           Array<navigation::NavObstacleShape>& out) noexcept = 0;
    /// The `NavLink`s of `world`, in world space. `from_poly` and `to_poly` are ignored: the
    /// service snaps each link onto the baked mesh.
    [[nodiscard]] virtual Status links(u32 world, Array<navigation::NavLink>& out) noexcept = 0;
    /// Saves an encoded `.cynavmesh` under its bake identity and names where it went, as the
    /// project-relative path the document records.
    [[nodiscard]] virtual Status store_bake(u64 identity, Span<const u8> bytes,
                                            Array<char>& out_path) noexcept = 0;
    /// Reads back the `.cynavmesh` saved under `identity`.
    [[nodiscard]] virtual Status load_bake(u64 identity, Array<u8>& out) noexcept = 0;
    /// The world-space ray under pixel (`x`, `y`) of `viewport`'s frame `frame`.
    [[nodiscard]] virtual Expected<Ray, Error> pick_ray(u32 viewport, u64 frame, f32 x,
                                                        f32 y) noexcept = 0;
};

/// Engine-owned navigation authoring backend. Each session keeps one context per navigation world
/// (its mesh, settings, area costs, last report and saved identity), has at most one pending
/// request, cancels cooperatively, and ends every request with exactly one terminal event.
class NavigationService final : public abi::EditorServiceBackend {
public:
    /// Without a `runtime`, every `navigation.*` operation fails with `<op>.unavailable` and
    /// `capabilities.get` lists none of them.
    explicit NavigationService(Allocator& allocator,
                               NavigationSourceRuntime* runtime = nullptr) noexcept
        : allocator_(&allocator), runtime_(runtime) {}

    [[nodiscard]] CyResult open(CyServiceSession* out_session) noexcept override;
    void close(CyServiceSession session) noexcept override;
    [[nodiscard]] CyResult submit(CyServiceSession session,
                                  const CyServiceRequest& request) noexcept override;
    [[nodiscard]] CyResult cancel(CyServiceSession session, u64 request_id) noexcept override;
    [[nodiscard]] CyResult poll(CyServiceSession session, CyServiceEvent& out_event,
                                bool& out_has_event) noexcept override;

    /// The baked mesh of `world` in this session, for the host's overlay; null when unbaked.
    [[nodiscard]] static const navigation::NavMesh* mesh(CyServiceSession session,
                                                         u32 world) noexcept;
    /// The overlay flags (`navigation::NavDebugFlags` bits) last set for `world`; zero by default.
    [[nodiscard]] static u32 overlay(CyServiceSession session, u32 world) noexcept;

private:
    Allocator* allocator_;
    NavigationSourceRuntime* runtime_;
};

}  // namespace cy::editor
