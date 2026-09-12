#pragma once
// Wind response: the same field every other consumer reads, evaluated per vertex on the GPU with no
// per-instance CPU work. M10 task 2.4.
//
// `foliage` — "Wind response": "Foliage SHALL deform in response to the WIND FIELD (see
// `environment-fields`), with response detail scaled by distance and importance. Near foliage SHALL
// support HIERARCHICAL RESPONSE — trunk, branch, twig, and leaf motion at different amplitudes and
// frequencies — and distant foliage SHALL receive a simple sway. Wind response SHALL be evaluated
// on the GPU as part of geometry processing, and SHALL NOT REQUIRE PER-INSTANCE CPU WORK. Wind
// response SHALL be CONSISTENT WITH OTHER CONSUMERS of the wind field, so trees, particles, and
// water agree."
//
// ================================================================================================
// "NO PER-INSTANCE CPU WORK" IS A COUNT, NOT A CLAIM
// ================================================================================================
//
// The CPU prepares ONE `ClusterWind` per cluster per frame — a field sample at the cluster centre,
// a derived phase, and the detail level distance and budget allow. `prepare()` returns how many
// preparations it did, and `test_wind.cpp` requires that number to equal the CLUSTER count while
// the instance count is two orders of magnitude larger. Everything else — the trunk bend, the
// branch and twig terms, the per-vertex phase — is a pure function of the vertex, the instance's
// own quantised record and that one struct, and `evaluate_response()` is the CPU statement of it.
//
// WHY THE FUNCTION EXISTS ON THE CPU AT ALL. For the reason `environment::sample_field_image()`
// exists: a shader's algorithm that is only ever written in a shader is an algorithm nothing can
// measure. "A gust crosses a treeline and the response travels with it" is a statement about this
// function's output over positions and time, and it is measured here against a real wind field.
// **The `.slang` module that runs it on a device is NOT in this milestone** — see README.md.
//
// ================================================================================================
// CONSISTENCY WITH OTHER CONSUMERS IS AN INPUT, NOT A CONVENTION
// ================================================================================================
//
// Nothing in this file models wind. `WindSampler` reads the `wind` field through
// `environment::FieldReader`, which is the same handle `water` and `vfx-system` open on the same
// field — one producer, one set of values, `environment-fields`' whole point. A foliage module that
// synthesised its own gusts would make "trees, particles, and water agree" false by construction,
// and it would be invisible until somebody stood beside a tree and a smoke plume at once.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/environment/store.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/species.h>

namespace cy::foliage {

/// Which term of the hierarchy a displacement came from. Returned beside the displacement so a
/// diagnostic — and a test — can see that a `Sway` response really is one term and a `Leaf`
/// response really is four.
struct WindDisplacement {
    /// The displacement in metres, in world axes.
    Vec3 offset{0.0F, 0.0F, 0.0F};
    /// Per-term magnitudes, indexed by `WindTerm`.
    f32 terms[4] = {};
    /// How many terms actually contributed. 1 for a sway, 4 for the full hierarchy.
    u32 term_count = 0;
};

/// The four terms `foliage` names, coarsest first.
enum class WindTerm : u8 { Trunk = 0, Branch, Twig, Leaf, kCount };

inline constexpr u32 kWindTermCount = static_cast<u32>(WindTerm::kCount);

[[nodiscard]] const char* wind_term_name(WindTerm term) noexcept;

/// How many terms a detail level evaluates.
[[nodiscard]] u32 terms_for_detail(WindDetail detail) noexcept;

/// Where on the plant a vertex is, which is what decides how much of each term reaches it.
///
/// It is a PARAMETERISATION and not a position: a shader has the vertex's own height fraction and
/// its distance from the trunk in the mesh's vertex stream, and passing those two numbers is what
/// lets this function be evaluated per vertex without knowing the mesh.
struct VertexParameters {
    /// 0 at the base, 1 at the top of the plant.
    f32 height_fraction = 0.0F;
    /// 0 on the trunk axis, 1 at the widest point of the canopy.
    f32 radial_fraction = 0.0F;
    /// A per-vertex phase offset baked by the cooker, 0..1. What stops every leaf on one tree from
    /// moving together.
    f32 vertex_phase = 0.0F;
};

/// The distance and budget levers, resolved. One of these is prepared per cluster.
struct WindTuning {
    /// Below this many metres, a cluster gets its species' full declared detail.
    f32 hierarchical_metres = 40.0F;
    /// Beyond this, everything gets `Sway`.
    f32 sway_metres = 150.0F;
    /// The budget's own ceiling on detail, whatever the distance says. `FoliageBudget` writes it.
    WindDetail ceiling = WindDetail::Leaf;
    /// Global scale on the amplitude. A calm preset is not a different code path.
    f32 amplitude_scale = 1.0F;
};

/// The detail one cluster gets: the least of the species' declaration, the distance band, and the
/// budget ceiling.
[[nodiscard]] WindDetail resolve_wind_detail(WindDetail declared, f32 distance_metres,
                                             const WindTuning& tuning) noexcept;

/// The per-vertex evaluation. **A pure function of its arguments** — see the header note.
///
/// `time_seconds` is the only thing that moves. Everything else is the cluster's prepared wind, the
/// instance's own record and the vertex's parameters, which is exactly the argument list a vertex
/// shader has.
[[nodiscard]] WindDisplacement evaluate_response(const SpeciesDeclaration& species,
                                                 const ClusterWind& wind,
                                                 const FoliageInstance& instance,
                                                 const VertexParameters& vertex,
                                                 f32 time_seconds) noexcept;

/// What one `prepare()` cost. The measurement behind "SHALL NOT require per-instance CPU work".
struct WindPrepareReport {
    /// Clusters whose `ClusterWind` was written. The CPU cost.
    u32 clusters_prepared = 0;
    /// Instances inside them. The work that did NOT happen.
    u64 instances_covered = 0;
    /// Field samples taken. One per cluster; a second per cluster would mean the sampler is being
    /// called somewhere it should not be.
    u32 field_samples = 0;
    u32 hierarchical = 0;
    u32 sway = 0;
};

/// Reads the wind field and prepares clusters against it.
///
/// It opens an `environment::FieldReader` ONCE, at construction, so the firewall decision
/// (`determinism::may_read()`) is made once for the whole system rather than per sample — which is
/// the same reason `FieldReader` exists at all.
class WindSampler {
public:
    /// `reader_class` is foliage's own simulation class for this read. Wind is a presentation field
    /// in every world that declares it that way, and a foliage system that read it as authoritative
    /// would be refused HERE rather than diverging later.
    [[nodiscard]] static Expected<WindSampler, Error> open(
        const environment::FieldStore& store, environment::FieldId wind_field,
        determinism::SimulationClass reader_class) noexcept;

    /// Prepare every cluster in `clusters` against the field and the view.
    ///
    /// `eye` decides the distance band. One field sample per cluster, at the cluster's centre: a
    /// forest does not need a wind sample per tree, and taking one would be the per-instance CPU
    /// work the requirement forbids.
    [[nodiscard]] Expected<WindPrepareReport, Error> prepare(
        const SpeciesLibrary& library, Span<FoliageCluster> clusters, const world::WorldVec3d& eye,
        const WindTuning& tuning) const noexcept;

    /// The wind at one position, for a caller that is not a cluster — the interaction field's own
    /// advection, a diagnostic probe.
    [[nodiscard]] Vec3 wind_at(const world::WorldVec3d& at) const noexcept;

    [[nodiscard]] environment::FieldId field() const noexcept { return reader_.field(); }

private:
    explicit WindSampler(environment::FieldReader&& reader) noexcept
        : reader_(static_cast<environment::FieldReader&&>(reader)) {}

    environment::FieldReader reader_;
};

/// The declaration of the `wind` field as `environment-fields` describes it, for a world that has
/// no weather row yet.
///
/// **FOLIAGE DOES NOT CLAIM IT.** `environment-fields` names `weather-and-wind` as the wind field's
/// producer, and claiming it here would be exactly the "two systems writing one field" the
/// substrate refuses. This function DECLARES the field so a world without weather still has one to
/// read; whoever produces it claims it. A test in `test_wind.cpp` holds that a second claim after
/// weather's fails, naming both.
[[nodiscard]] environment::FieldDeclaration wind_field_declaration(f32 cell_metres) noexcept;

}  // namespace cy::foliage
