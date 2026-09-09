#ifndef CY_IMPORT_PRIMITIVE_H
#define CY_IMPORT_PRIMITIVE_H
// Generated primitives — box, sphere, cylinder, plane and capsule — as an IMPORTER. M8.a task 2.1.
//
// design.md §2, which refuses the obvious design and makes the refusal the requirement:
//
// > The temptation is a `PrimitiveNode` with a shape enum. Refuse it. A created box is a **mesh
// > instance whose mesh the engine generated rather than imported** — the same components, the same
// > asset handles, the same cooking path. The requirement says every later system must be unable to
// > tell the difference, and the way to guarantee that is to give it nothing to tell apart.
// >
// > Generation belongs beside the importers, not in the editor.
//
// --- WHY THIS IS AN `Importer` AND NOT A FUNCTION SOMEBODY CALLS ---------------------------------
//
// The strongest available reading of "give it nothing to tell apart" is that generation is not a
// second producer at all: a primitive is a SOURCE ASSET — a `.cyprim` file in the project, beside
// the `.gltf` and the `.fbx` — and the thing that turns it into cooked bytes is an entry in the
// same `ImporterRegistry`, reached by the same `ImportPipeline`, keyed by the same
// `import_derivation_key`, stored in the same `DerivedCache`, published through the same sidecars.
//
// That settles four tasks at once and leaves nothing to police:
//
//   * 2.1 — the five shapes, with their parameters in the source file, so editing a parameter is
//     editing an asset and re-cooking it is the ordinary re-cook of a changed source.
//   * 2.3 — no consumer can tell a generated mesh from an imported one, because after step 1 there
//     is only one code path: `finish_mesh`, `emit_mesh_with_lods`, `emit_collision` and
//     `emit_prefab` from model.h, in that order, with the same options under the same names. Even
//     the collision convention is the same one — see below.
//   * 2.4 — the derivation key is `import_derivation_key`'s, which is the single key M7 unified.
//     There is no second cache, no second key, and nowhere to put one.
//   * `asset-import-pipeline`'s own rule that "a project SHALL be able to register its own
//     importers with the same weight as a built-in" — a primitive is exactly as privileged as a
//     glTF, which is to say not at all.
//
// The alternative — a `generate_primitive()` the editor calls, writing a cooked mesh somewhere —
// would need its own key, its own cache entry, its own identity minting and its own re-cook rule,
// and every one of those is a place the two producers could disagree. M7 spent its first section
// repairing exactly that class of defect.
//
// --- THE SOURCE FORMAT ---------------------------------------------------------------------------
//
// A `.cyprim` is a few lines of text, because a parameter a person cannot read in a diff is a
// parameter nobody can review:
//
//     cyprim 1
//     shape box
//     name Crate
//     extent 1 2 1
//     origin base
//
// The first line is the magic and the version. Every other line is a keyword and its values;
// `#` begins a comment; blank lines are ignored. A keyword the shape does not take is an ERROR
// rather than a warning, because a parameter that is written down and does nothing is the shape of
// bug a cache cannot save you from — you change it, nothing re-cooks, and both of those are
// correct.
//
// The parameters, per shape, are the table in `PrimitiveParameters`. `name` and `origin` are common
// to all five.
//
// --- COLLISION USES THE IMPORTERS' CONVENTION, NOT A PARAMETER OF ITS OWN ------------------------
//
// A model importer generates a collider when a node's name ends with the `collision-suffix` option
// ("WHEN a node is named with the configured collision suffix THEN a collider SHALL be generated
// from it and the node excluded from rendering"). A primitive obeys the same rule with the same
// option: `name Crate_collision` produces a collider and no render mesh.
//
// A `collision: convex` line in the source would have been friendlier and would also have been a
// second convention for the same thing — the first place where a downstream system could tell a
// generated mesh from an imported one. An artist who wants a crate and its hull authors two nodes;
// a designer who wants a box and its hull creates two primitives. That is the same workflow, which
// is the whole point.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/import/importer.h>
#include <cy/import/mesh.h>

#include <string>
#include <string_view>

namespace cy::import {

/// The five shapes `editor-ui-ux` names, and nothing else.
///
/// This enumeration exists in the GENERATOR, which is the one place design.md §2 allows it: "no
/// `PrimitiveNode`, no shape enum the rest of the editor branches on". Nothing downstream of
/// `PrimitiveImporter::import` ever sees it — what leaves this module is a cooked mesh, a cooked
/// collider and a cooked node table, exactly as a glTF leaves the module beside it.
enum class PrimitiveShape : u8 {
    Box = 0,
    Sphere = 1,
    Cylinder = 2,
    Plane = 3,
    Capsule = 4,
};

/// The keyword the source file uses. Never null.
[[nodiscard]] const char* primitive_shape_name(PrimitiveShape shape) noexcept;

/// The shape a keyword names, or `NotFound` naming what was written.
[[nodiscard]] Expected<PrimitiveShape, Error> primitive_shape_from_name(
    std::string_view name) noexcept;

/// Where the shape's own origin sits relative to its geometry.
///
/// `Base` is not a convenience: a designer dropping a box on a floor wants its bottom at the floor,
/// and the alternative is that every created primitive is half-buried and has to be nudged. It is
/// resolved HERE, in the geometry, rather than as a transform on the node, so that the mesh's own
/// bounds are what a physics body, a bounding-volume test and a framing operation all read.
enum class PrimitiveOrigin : u8 {
    /// The centre of the shape's bounds. What a modelling package produces.
    Centre = 0,
    /// The centre of its footprint, with the lowest point at y = 0.
    Base = 1,
};

/// The keyword the source file uses. Never null.
[[nodiscard]] const char* primitive_origin_name(PrimitiveOrigin origin) noexcept;

/// Everything the five generators read.
///
/// One struct rather than a variant per shape: the fields a shape does not use are not written to
/// its source file and are refused if they are, so the unused ones cost nothing and the parser has
/// one table to check against.
///
/// | shape    | parameters                                        |
/// |----------|---------------------------------------------------|
/// | box      | `extent x y z`                                    |
/// | sphere   | `radius r`, `segments n`, `rings n`               |
/// | cylinder | `radius r`, `height h`, `segments n`              |
/// | plane    | `extent x z`, `subdivisions u v`                  |
/// | capsule  | `radius r`, `height h`, `segments n`, `rings n`   |
///
/// `height` on a capsule is the length of its cylindrical section, so its total height is
/// `height + 2 * radius`. That is the convention every physics engine uses for a capsule shape,
/// including the one behind `cy::physics`, and picking the other one would make a generated capsule
/// and its collider disagree the day somebody puts a body on it.
struct PrimitiveParameters {
    /// Box: the full size along each axis. Plane: `x` and `z`; `y` is unused.
    Vec3 extent{1.0f, 1.0f, 1.0f};
    f32 radius = 0.5f;
    f32 height = 1.0f;
    /// Divisions around the axis of revolution. Sphere, cylinder and capsule.
    u32 segments = 32;
    /// Divisions along the axis of revolution: a sphere's latitudes, a capsule's per-hemisphere
    /// latitudes.
    u32 rings = 16;
    /// Plane: quads along X and along Z.
    u32 subdivisions_x = 1;
    u32 subdivisions_z = 1;
};

/// The smallest and largest a parameter may be, so that a mistyped source fails at the parse rather
/// than at a vertex count nothing can allocate.
///
/// The upper bounds are generous — 512 segments on a sphere is 262,144 triangles — and the point of
/// them is that they exist and are stated, not that they are tight.
inline constexpr f32 kPrimitiveMinExtent = 1.0e-4f;
inline constexpr f32 kPrimitiveMaxExtent = 1.0e5f;
inline constexpr u32 kPrimitiveMinSegments = 3;
inline constexpr u32 kPrimitiveMaxSegments = 512;
inline constexpr u32 kPrimitiveMinRings = 2;
inline constexpr u32 kPrimitiveMaxRings = 512;
inline constexpr u32 kPrimitiveMaxSubdivisions = 512;

/// The longest a primitive's node name may be. Long enough for any name a person types, short
/// enough that the parser never allocates unboundedly from a malformed file.
inline constexpr usize kPrimitiveMaxNameLength = 96;

/// A whole `.cyprim`, parsed.
struct PrimitiveSpec {
    PrimitiveShape shape = PrimitiveShape::Box;
    /// The node's name, which is also the stem of every sub-asset name. Defaults to the shape's own
    /// keyword capitalised — "Box", "Sphere" — so a source file that says only `shape box` is
    /// complete.
    std::string name;
    PrimitiveOrigin origin = PrimitiveOrigin::Centre;
    PrimitiveParameters parameters;
};

/// The source format's version, written on the first line and refused when it is not this.
inline constexpr u32 kPrimitiveSourceVersion = 1;

/// The extension a primitive source carries, dot included.
inline constexpr std::string_view kPrimitiveExtension = ".cyprim";

/// Parse a `.cyprim`.
///
/// Fails with `InvalidArgument` and, in `out_line`, the one-based line the failure is about —
/// because `Error::message` is a literal by construction and "which line" is the half of the
/// diagnostic a person acts on. `PrimitiveImporter::import` joins the two into an
/// `ImportDiagnostic` rather than returning an error: a malformed source is a fact about the
/// source, and the importer framework is explicit that those are diagnostics.
[[nodiscard]] Expected<PrimitiveSpec, Error> parse_primitive_source(
    std::string_view text, u32* out_line = nullptr) noexcept;

/// Write the canonical text of a spec: the bytes `parse_primitive_source` reads back unchanged.
///
/// THE EDITOR WRITES THIS SAME TEXT, IN RUST, AND THAT IS A CONTRACT RATHER THAN A COINCIDENCE. The
/// source bytes are hashed into the derivation key, so two producers that agree on the parameters
/// and disagree on the spelling would cook the same primitive twice under two keys. Both sides pin
/// the identical golden string in their own test suite — `tools/import/tests/test_primitive.cpp`
/// and `editor/crates/cy-editor-services/tests/primitives_are_ordinary_mesh_instances.rs` — so a
/// change to either spelling fails on the other side of the process boundary.
///
/// Numbers are written with up to six decimal places, trailing zeros and a trailing point removed,
/// which is a rule both languages implement identically and neither's default formatter does.
[[nodiscard]] Status write_primitive_source(const PrimitiveSpec& spec, std::string& out);

/// Format one number the way the canonical text does. Public because both the writer and its test
/// need it, and a second copy of a rounding rule is a second rounding rule.
[[nodiscard]] std::string primitive_number(f64 value);

/// Generate the mesh a spec describes: positions, normals and texture coordinates, one section,
/// counter-clockwise front faces, in metres.
///
/// Deterministic and machine-independent by construction — it is `sin`, `cos` and arithmetic over
/// the parameters, with no ordering that depends on an allocation address. `asset-import-pipeline`
/// requires byte-identical output for identical input and the test asserts it over the cooked
/// bytes rather than over this.
///
/// Tangents are NOT generated here: `finish_mesh` does that, with the same convention, at the same
/// point in the sequence, for every importer. A generator that produced its own would be the first
/// difference a downstream system could see.
[[nodiscard]] Status build_primitive_mesh(const PrimitiveSpec& spec, MeshData& out) noexcept;

/// The primitive importer's option schema.
///
/// Every option is one a model importer already declares, under the same name and with the same
/// default, for the reason model.h gives about glTF and FBX: a project that changes its mind about
/// welding or about levels of detail sets one value, not three. What is absent is as deliberate:
/// there is no `scale` and no `source-up-axis`, because a generated shape has no source frame to
/// convert from and an option that cannot be wrong is an option nobody should have to read.
[[nodiscard]] OptionsSchema primitive_options() noexcept;

/// The built-in primitive importer, for `.cyprim`.
///
/// Holds no state, so one instance serves every worker — the same argument, and the same shape, as
/// every other built-in.
class PrimitiveImporter final : public Importer {
public:
    [[nodiscard]] ImporterInfo info() const noexcept override;
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

}  // namespace cy::import

#endif  // CY_IMPORT_PRIMITIVE_H
