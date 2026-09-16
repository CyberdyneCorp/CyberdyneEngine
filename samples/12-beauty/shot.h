#pragma once
// samples/12-beauty — the committed scene, the content it names, and the device that photographs
// it. M11.c section 7.
//
// ================================================================================================
// WHAT THIS ARTEFACT IS, AND WHAT MAKES IT DIFFERENT FROM EVERY OTHER PICTURE IN THIS REPOSITORY
// ================================================================================================
//
// It is the first frame this engine has ever drawn in which a MATERIAL decided a pixel. Every other
// published capture — `docs/design/virtual-geometry.md`'s normals view, `m10-world.png`, the
// fidelity ladder — shades from constants or from a debug channel, and M11.c's own ledger says so:
// "outside `docs/`, the tree holds six image files, four of them editor identity marks and two of
// them golden references. There is no albedo, normal, roughness or mask texture anywhere."
//
// The path this program runs, end to end, is the one M11.c's spike measured and found broken at both
// ends:
//
//   1 AUTHOR    a material graph, placed and wired on the editor's own `GraphCanvas`
//   2 COMPILE   `lower_material` -> `lower_graph` -> `compile_material` -> the emitter's Slang
//   3 ENCODE    `cy::import::TextureImporter` — PNG in, BC7 and BC5 blocks with a mip chain out
//   4 BIND      the cooked blocks uploaded and bound at (set 0, binding 1), where
//               `cy/material.slang` declares `cyMaterialTextures[]`
//   5 ASSEMBLE  `cy::rendering::assembly::FrameAssembly` — the post chain, the exposure, the tonemap
//   6 CAPTURE   `tests/render/golden.cpp`'s PNG writer, and a manifest built from the frame's own
//               report rather than from what this file believes
//
// ================================================================================================
// WHAT IT DOES NOT CLAIM, AND THE LIST IS THE ARTEFACT'S HONESTY CONTRACT
// ================================================================================================
//
// design.md §7: "a beautiful picture with an unstated provenance is the most efficient way to make
// this whole record dishonest". So, stated here and published beside the image:
//
//  * **The geometry is procedural.** Six `.cyprim` sources, built by the engine's own primitive
//    generator. No mesh was modelled, no mesh file is committed, and nothing here is a scan.
//  * **The normal map is sampled by the FRAME and not by the material.** `CyClosure` has five terms
//    and none of them is a normal, so a compiled material cannot express a normal-mapped surface.
//    `beauty.slang` samples it from the material's own table at the material's own slot, and says so.
//  * **There is no global illumination and no ambient occlusion pass.** The ambient term is the
//    engine's own sky irradiance, hemispherically weighted. `cy::rendering-gi` is not linked.
//  * **There is no anti-aliasing stage.** `FramePassKind::Temporal` is declared by the frame and
//    nothing in this tree records it; the frame is drawn at 2x and box-filtered down, which is
//    supersampling and is named as such in the manifest rather than called TAA.
//  * **One shadow map.** 2048x2048, one cascade, recorded by THIS program into the frame's graph.
//    The engine's own shadow cache — `cy::rendering-shadows`' pages and budget — is initialised by
//    the assembly and spends nothing, because this program draws its own geometry.
//  * **No particles.** `vfx-system` is Working, not Complete; `src/vfx/README.md`'s four recorded
//    absences are why, and a shot with sprites in it would not have made any of them false.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/import/mesh.h>
#include <cy/rendering/assembly/capture_manifest.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy::sample::beauty {

/// One vertex of the shot: position, normal, tangent, and one texture coordinate set.
///
/// CAMERA-RELATIVE POSITIONS, baked at load. The scene's transforms are applied on the processor in
/// f64 and the camera's own position subtracted before the result narrows to f32, so nothing in any
/// shader here ever holds a world coordinate — design.md §3, and the same arrangement
/// samples/03-first-light makes for the same reason.
struct Vertex {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec4 tangent{1.0F, 0.0F, 0.0F, 1.0F};
    Vec2 uv{0.0F, 0.0F};
};

static_assert(sizeof(Vertex) == 48, "binding 0's stride is declared as 48 in create_pipelines()");

/// One material the shot uses: what was authored, what was cooked, and what the device holds.
struct ShotMaterial {
    std::string key;
    /// The `.cygraph` the editor's canvas produced.
    std::string graph_path;
    /// The three source images, in the order the frame binds them.
    std::string albedo_path;
    std::string normal_path;
    std::string data_path;

    /// What `cy_material author` reported about the compiled module — read from the sidecar and
    /// CHECKED, because the parameter block this program uploads is laid out from it.
    std::string entry_point;
    std::vector<std::string> parameters;
    /// The authored default of each parameter, in the same order. Uploaded into the material's own
    /// block: a zeroed block draws a black material however the graph was authored.
    std::vector<Vec4> parameter_defaults;
    std::vector<std::string> textures;
    u64 cook_key = 0;

    /// The compiled fragment program, as SPIR-V words.
    std::vector<u32> spirv;

    /// What the importer produced for each of the three images, for the manifest.
    struct Cooked {
        u32 width = 0;
        u32 height = 0;
        u32 mip_count = 0;
        u32 format = 0;
        bool encoded = false;
        usize payload_bytes = 0;
        usize source_bytes = 0;
        /// Where in the frame's table it landed.
        u32 slot = 0;
    };
    Cooked albedo;
    Cooked normal;
    Cooked data;
};

/// One thing in the scene.
struct Instance {
    std::string mesh;
    std::string material;
    Vec3 position{0.0F, 0.0F, 0.0F};
    f32 yaw_degrees = 0.0F;
    f32 uv_scale = 1.0F;
    f32 normal_strength = 1.0F;
};

/// The committed scene: `content/beauty/shot.cyshot`, parsed.
struct Shot {
    std::string name;
    Vec3 camera_position{0.0F, 1.6F, 0.0F};
    Vec3 camera_target{0.0F, 1.6F, 1.0F};
    f32 field_of_view_degrees = 42.0F;
    f32 near_plane = 0.08F;

    f32 sun_elevation_degrees = 8.0F;
    f32 sun_azimuth_degrees = 120.0F;
    f32 shadow_extent_metres = 24.0F;
    f32 shadow_bias = 0.0015F;
    /// How far along the surface normal the shadow lookup moves, in metres.
    f32 shadow_normal_offset = 0.04F;

    f32 cloud_cover = 0.4F;
    f32 cloud_density = 0.25F;
    u32 cloud_steps = 48;
    u64 seed = 0x5EEDB107ULL;

    f32 exposure_stops = 12.0F;

    std::vector<ShotMaterial> materials;
    std::vector<std::pair<std::string, std::string>> meshes;
    std::vector<Instance> instances;

    /// Parse the committed file. Every refusal names the line.
    [[nodiscard]] static Expected<Shot, Error> read(const char* path, std::string& problem);

    [[nodiscard]] const ShotMaterial* material(std::string_view key) const noexcept;
};

/// What one run measured and produced.
struct ShotReport {
    u32 validation_errors = 0;
    u32 instances = 0;
    u32 triangles = 0;
    u32 materials = 0;
    u32 textures = 0;
    u64 texture_source_bytes = 0;
    u64 texture_cooked_bytes = 0;
    u32 frame_passes = 0;
    u32 post_stages = 0;
    u32 supersample = 1;
    f64 build_ms = 0.0;
    f64 submit_ms = 0.0;
    f64 sky_ms = 0.0;
    /// The sun and sky the engine's own atmosphere answered for this elevation.
    Vec3 sun_illuminance{0.0F, 0.0F, 0.0F};
    Vec3 sky_irradiance{0.0F, 0.0F, 0.0F};
    rendering::assembly::CaptureManifest manifest;
    bool manifest_valid = false;
};

/// The device, the content on it, and the frame.
class Stage {
public:
    /// One run of the scene pass: a contiguous index range shaded by one material's own pipeline.
    /// Public because the record callback is a free function — a graph pass's `RecordFn` is, by
    /// construction, not a member.
    struct Batch;
    struct Device;

    explicit Stage(Allocator& allocator) noexcept : allocator_(&allocator) {}
    ~Stage();

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    /// Open a device. An ABSENT device is success with `available()` false, the same arrangement
    /// samples/07-fidelity, 08-vertical-slice, 09b and 10-world use: a machine with no graphics
    /// device says what it is missing and the program still reports what it loaded.
    [[nodiscard]] Status open(u32 width, u32 height, u32 supersample) noexcept;
    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] const char* absence() const noexcept;

    /// Build the vertex and index buffers, cook and upload the textures, and create the pipelines.
    /// `shot` is non-const because the cook writes back what the importer produced — the
    /// format, the mip count and the table slot each texture landed in, which is a fact about this
    /// run and is what the manifest publishes.
    [[nodiscard]] Status stage_shot(Shot& shot, ShotReport& report) noexcept;

    /// Draw ONE frame and write BOTH images out of it.
    ///
    /// `png_path` is the tonemapped 8-bit image the resolve wrote; `linear_path` is the linear HDR
    /// scene colour the resolve READ, with the display transfer and nothing else. That is the
    /// before/after pair task 7.4 asks for, and taking both out of one frame is what stops the two
    /// pictures differing in anything but the chain.
    [[nodiscard]] Status render(const Shot& shot, const char* png_path, const char* linear_path,
                                ShotReport& report) noexcept;

    /// The same frame from somewhere else, which is what a turntable is. World coordinates; the
    /// geometry stays baked against the shot's own camera and this is expressed as an offset.
    [[nodiscard]] Status render_from(const Shot& shot, Vec3 eye_world, Vec3 target_world,
                                     const char* png_path, const char* linear_path,
                                     ShotReport& report) noexcept;

    [[nodiscard]] Status write_manifest(const Shot& shot, const ShotReport& report,
                                        const char* path) const noexcept;

private:
    [[nodiscard]] Status create_pipelines(const Shot& shot) noexcept;
    [[nodiscard]] Status create_frame() noexcept;
    [[nodiscard]] Status cook_textures(Shot& shot, ShotReport& report) noexcept;
    [[nodiscard]] Status build_geometry(const Shot& shot, ShotReport& report) noexcept;
    [[nodiscard]] Status prepare_shadow() noexcept;
    [[nodiscard]] Status write_png(const char* path) noexcept;
    [[nodiscard]] Status write_linear_png(const char* path) noexcept;

    Allocator* allocator_ = nullptr;
    Device* device_ = nullptr;
    /// What the engine's own atmosphere answered for this shot's sun, kept between staging and
    /// rendering so the picture and the manifest cannot disagree about the light.
    Vec3 sun_direction_{0.0F, 1.0F, 0.0F};
    Vec3 sun_illuminance_{0.0F, 0.0F, 0.0F};
    Vec3 sky_irradiance_{0.0F, 0.0F, 0.0F};
    u32 width_ = 0;
    u32 height_ = 0;
    u32 supersample_ = 1;
    bool available_ = false;
    Array<u32> pixels_;
};

/// Load a `.cyprim` and build its mesh, with normals and a tangent basis.
[[nodiscard]] Expected<import::MeshData, Error> load_primitive(const char* path,
                                                               std::string& problem);

}  // namespace cy::sample::beauty
