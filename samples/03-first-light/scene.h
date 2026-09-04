#pragma once
// The content: geometry, objects, the sun, the camera path, and the one texture. No device.
//
// Everything in this file is arithmetic. It builds the same numbers on a machine with a GPU and on
// one without, which is what lets `tests/render/` photograph this scene on Vulkan and compile the
// identical frame on the null backend — and it is why the file that owns the content owns no
// handles.
//
// ================================================================================================
// WORLD POSITIONS ARE f64 AND THE GPU NEVER SEES ONE
// ================================================================================================
//
// design.md §3: camera-relative rendering lands with the first draw. `Object::world_position` and
// `Camera::position` are `f64` because the subtraction between them is the whole mechanism: at the
// sample's `--origin 1000000`, an `f32` world position has a spacing of about 0.06 units and the
// scene's own features are smaller than that, so the difference computed in `f32` would already
// have lost them. Computed in `f64` and narrowed afterwards, the difference is small and exact, and
// `render.golden`'s far case renders byte-for-byte the same image as its near one.
//
// `cy::Vec3` is deliberately not used for these two fields: it is `f32`, and a type that cannot
// hold the value is a worse home for it than three named doubles.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::sample::first_light {

/// The vertex the shader's three inputs read. 32 bytes, and the layout is stated once here and once
/// in `renderer.cpp`'s vertex attributes; a `static_assert` below pins the size so the two cannot
/// drift by an accidental member.
struct Vertex {
    f32 position[3] = {};
    f32 normal[3] = {};
    f32 uv[2] = {};
};

static_assert(sizeof(Vertex) == 32, "the vertex attribute offsets in renderer.cpp assume this");

/// One drawable: a range of the shared index buffer, placed in the world.
///
/// Rotation is a yaw alone. A sample that carried a full quaternion here would have to explain what
/// happens to the normal under non-uniform scale, and it has nothing to say about it that the
/// shader's `rotateNormal` does not already say.
struct Object {
    f64 world_position[3] = {};
    f32 yaw_radians = 0.0F;
    f32 scale = 1.0F;
    f32 base_color[3] = {1.0F, 1.0F, 1.0F};
    u32 first_index = 0;
    u32 index_count = 0;
};

/// Where the camera is and what it is looking at, for one frame.
struct Camera {
    f64 position[3] = {};
    /// The unit direction the camera looks along, and its up vector. Directions are differences
    /// already, so they are `f32` without losing anything.
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    f32 fov_y_radians = 0.0F;
    /// Reversed-Z with an infinite far plane, so there is no far distance to carry.
    f32 near_plane = 0.1F;
};

struct SceneDescription {
    /// Boxes in the ring around the pillar. Each is 12 triangles.
    u32 box_count = 6;
    /// The world offset applied to every position, in metres along all three axes. The point of the
    /// switch is `--origin 1000000`, which is task 5.3's scene.
    f64 origin = 0.0;
    /// Whether the sun casts a shadow. Off is the control the golden suite compares against: an
    /// image with no shadow in it proves that the shadowed one is doing something.
    bool sun_shadows = true;
};

/// The sun, in physical-ish terms a sample can afford: a direction, a colour, and an ambient floor.
struct Sun {
    /// The unit direction TOWARDS the sun, which is the direction the shading dots against.
    Vec3 direction{0.0F, 1.0F, 0.0F};
    Vec3 color{1.0F, 1.0F, 1.0F};
    f32 ambient = 0.03F;
    /// The depth bias added to the shadow reference, in light-clip units, and the normal offset
    /// applied before the lookup, in world units. Both are properties of this scene's scale and of
    /// the shadow map's resolution, which is why they live with the content rather than in the
    /// renderer.
    f32 shadow_depth_bias = 0.0015F;
    f32 shadow_normal_offset = 0.02F;
};

inline constexpr u32 kCheckerExtent = 64;

/// The scene: one vertex buffer, one index buffer, a handful of objects and a sun.
///
/// Built once and never mutated. The camera is the only thing that changes between frames, and it
/// is computed rather than stored — `camera_at()` is a pure function of the frame's phase, which is
/// what makes the whole run reproducible from the frame index alone.
class Scene {
public:
    explicit Scene(Allocator& allocator) noexcept;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] Status build(const SceneDescription& description) noexcept;

    [[nodiscard]] Span<const Vertex> vertices() const noexcept { return vertices_.span(); }
    [[nodiscard]] Span<const u16> indices() const noexcept { return indices_.span(); }
    [[nodiscard]] Span<const Object> objects() const noexcept { return objects_.span(); }
    [[nodiscard]] const Sun& sun() const noexcept { return sun_; }
    [[nodiscard]] const SceneDescription& description() const noexcept { return description_; }

    /// The camera at a phase of its orbit, in turns: 0 and 1 are the same position. A test picks a
    /// phase and gets the same frame every time, on any machine.
    [[nodiscard]] Camera camera_at(f32 phase_turns) const noexcept;

    /// The radius of a sphere around everything the scene contains, centred on `centre()`. The
    /// shadow projection is fitted to it, which is what keeps the shadow map's texels the same size
    /// whatever `--boxes` is set to.
    [[nodiscard]] f32 bounding_radius() const noexcept { return bounding_radius_; }
    /// The scene's centre in world space, which is `origin` plus the ring's own centre.
    [[nodiscard]] const f64* centre() const noexcept { return centre_; }

    /// The checkerboard, as Rgba8Unorm texels. Generated rather than loaded: an asset file would
    /// make the sample depend on a content pipeline it is not demonstrating, and a texture whose
    /// contents are a function makes "textured" checkable rather than merely visible.
    [[nodiscard]] Span<const u32> checker_texels() const noexcept { return checker_.span(); }

private:
    Status build_geometry() noexcept;
    Status build_checker() noexcept;
    /// Append a box centred on the origin with the given half extents, returning its index range.
    Status append_box(Vec3 half_extents, f32 uv_scale, u32& first_index, u32& index_count) noexcept;

    Array<Vertex> vertices_;
    Array<u16> indices_;
    Array<Object> objects_;
    Array<u32> checker_;
    SceneDescription description_{};
    Sun sun_{};
    f64 centre_[3] = {};
    f32 bounding_radius_ = 1.0F;
};

/// The camera-relative view-projection for one camera and one viewport.
///
/// Reversed-Z with an infinite far plane, which is `core-math`'s convention and the one every
/// number in this sample assumes.
[[nodiscard]] Mat4 view_projection(const Camera& camera, u32 width, u32 height) noexcept;

/// The sun's camera-relative-to-clip matrix: an orthographic reversed-Z projection fitted to the
/// scene's bounding sphere, with the eye placed behind it along the sun's direction.
///
/// Camera-relative like everything else — the light's own position is never a world coordinate
/// either, which is what keeps the shadow projection exact at `--origin 1000000`.
[[nodiscard]] Mat4 sun_view_projection(const Scene& scene, const Camera& camera) noexcept;

}  // namespace cy::sample::first_light
