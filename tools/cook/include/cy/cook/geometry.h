#pragma once
// The virtual geometry cook: a mesh becomes a cluster hierarchy, addressed by the engine's one
// derivation key. M7 task 7.5.
//
// `asset-import-pipeline` — "Virtual geometry cooking": the requirement that has kept that
// capability's row at M8. What it asks for is a cook that is DETERMINISTIC and CACHE-FRIENDLY at
// cluster granularity, and both halves are properties of `src/rendering/virtual_geometry/` that
// this file exercises rather than provides. What this file adds is the third thing the requirement
// needs and the library cannot have: a place in the cook where the ONE derivation key is computed,
// so that two builds of the engine cannot serve each other's geometry.
//
// ================================================================================================
// IT USES THE KEY M7 TASK 1.1 MADE REACHABLE, AND THAT IS THE WHOLE POINT
// ================================================================================================
//
// M6 found two derivation keys and two caches, and the one that cooked real content was the blind
// one: `cy::import::import_derivation_key` contributed no compiler, no flags and no library
// versions, so an artefact built at `-O0` was served from a cache populated at `-O2`. Task 1.1
// moved `cy::assets::ToolchainFingerprint` to layer 0 so that every producer in the tree can reach
// it, and left a rule behind: **any new cook must call `toolchain.contribute(builder)`**.
//
// `cy::rendering::vg::derive_geometry_key` is where this cook does that, and it is the only place
// this cook's key is assembled. A key built where each input is discovered would miss its own cache
// the first time somebody reordered the discovery — which derivation.h names as the trap.
//
// ================================================================================================
// WHY THE COOK RUNS THE WATERTIGHTNESS CHECK
// ================================================================================================
//
// `virtual-geometry` — "Watertightness is tested": "WHEN an asset is cooked THEN an automated check
// SHALL verify that adjacent clusters across levels share consistent boundaries." The word is
// COOKED, not tested, so the check runs here and its result is in the report — a cook that produced
// a cracked asset fails rather than shipping one that a golden image might or might not catch.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/virtual_geometry/asset.h>
#include <cy/rendering/virtual_geometry/build.h>

#include <string_view>

namespace cy::cook {

/// What one virtual geometry cook was asked to do.
struct GeometryCookRequest {
    rendering::vg::SourceMesh mesh;
    rendering::vg::BuildOptions options;
    /// The platform-and-feature key the asset is stamped with; part of the derivation key.
    std::string_view variant = "desktop";
    /// Refuse to produce an asset whose cut is not watertight. On by default: a cracked asset is
    /// worse than a failed cook, because the crack appears at one threshold on one view.
    bool fail_on_cracks = true;
};

/// `virtual-geometry` — "Authoring experience": "Import SHALL report: source triangle count,
/// cluster count, hierarchy depth, cooked size, resident size, bytes per triangle, and any warnings
/// about content suitability." Every one of those is here, and so are the two figures the collision
/// and tangent requirements ask for separately.
struct GeometryCookReport {
    assets::DerivationKey key;
    assets::ContentHash content;
    u32 source_triangles = 0;
    u32 clusters = 0;
    u32 pages = 0;
    u32 levels = 0;
    u32 cooked_bytes = 0;
    u32 resident_bytes = 0;
    u32 resident_pages = 0;
    u32 cluster_metadata_bytes = 0;
    u32 tangent_bytes_saved = 0;
    f32 bytes_per_triangle = 0.0F;
    f32 quantisation_error = 0.0F;
    /// `virtual-geometry` — "Counts are reported separately": render and collision complexity are
    /// distinct figures, and a mismatch has to be visible. The collision proxy is `physics`'s to
    /// generate and this cook does not; the field is zero and README.md says so rather than the
    /// number being quietly the render one.
    u32 collision_triangles = 0;
    bool watertight = false;
    bool closed_source = false;
    u32 thresholds_tested = 0;
    bool suitability_warning = false;
    const char* suitability_reason = "";
};

/// Cook one mesh. Deterministic: the same request produces the same bytes and the same key.
///
/// Fails when the toolchain fingerprint is incomplete — a key that did not name the toolchain would
/// let one build's geometry pages be served to another's — and, unless `fail_on_cracks` is cleared,
/// when the hierarchy's cut is not watertight.
[[nodiscard]] Expected<GeometryCookReport, Error> cook_virtual_geometry(
    const GeometryCookRequest& request, Array<u8>& out,
    Allocator& allocator = current_allocator()) noexcept;

}  // namespace cy::cook
