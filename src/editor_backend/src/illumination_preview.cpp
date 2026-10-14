// SPDX-License-Identifier: MIT
// The editor's global illumination. See cy/editor/illumination_preview.h.

#include <cy/editor/illumination_preview.h>

#include <cy/core/memory/hash.h>
#include <cy/rendering/gi/classification.h>
#include <cy/rendering/gi/emissive.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace cy::editor {
namespace {

namespace gi = rendering::gi;
namespace bake = rendering::lightmap_bake;

/// Cards every half metre, as the per-pixel resolve's first scenes are carded.
constexpr f32 kCardSpacing = 0.5F;
constexpr u32 kMeshFieldDimension = 24;
constexpr f32 kMeshFieldMargin = 0.5F;
/// A rebuilt GI converges in at most this many frames before it is shown.
constexpr u32 kConvergeFrames = 8;
constexpr f32 kConvergedThreshold = 0.95F;
/// The field's finest level covers at least this much, and the probe window this many probes.
constexpr f32 kMinimumExtent = 8.0F;
constexpr u32 kFieldResolution = 64;
constexpr u32 kProbeHalfExtent = 4;

/// FNV-1a over bytes, folded field by field so padding never enters it.
class Digest {
public:
    Digest& add(const void* data, usize size) noexcept {
        const auto* bytes = static_cast<const u8*>(data);
        for (usize index = 0; index < size; ++index) {
            value_ ^= bytes[index];
            value_ *= 1'099'511'628'211ULL;
        }
        return *this;
    }
    Digest& add(f32 value) noexcept { return add(&value, sizeof(value)); }
    Digest& add(u64 value) noexcept { return add(&value, sizeof(value)); }
    Digest& add(Vec3 value) noexcept { return add(value.x).add(value.y).add(value.z); }
    Digest& add(const std::string& text) noexcept {
        return add(static_cast<u64>(text.size())).add(text.data(), text.size());
    }
    Digest& add(const Mat4& matrix) noexcept {
        for (u32 column = 0; column < 4; ++column) {
            for (u32 row = 0; row < 4; ++row) {
                add(matrix.at(row, column));
            }
        }
        return *this;
    }
    [[nodiscard]] u64 value() const noexcept { return value_; }

private:
    u64 value_ = 14'695'981'039'346'656'037ULL;
};

/// What a rebuild depends on: the triangles, where they are, what they are made of, the lights and
/// which objects move. Settings, volumes, classes and emitter rules are applied in place.
[[nodiscard]] u64 geometry_digest(const IlluminationScene& scene) noexcept {
    Digest digest;
    for (const IlluminationMesh& mesh : scene.meshes) {
        digest.add(mesh.key).add(static_cast<u64>(mesh.positions.size()));
        digest.add(static_cast<u64>(mesh.indices.size()));
        digest.add(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
        digest.add(mesh.indices.data(), mesh.indices.size() * sizeof(u32));
    }
    for (const IlluminationMaterial& material : scene.materials) {
        digest.add(material.name)
            .add(material.albedo)
            .add(material.emission)
            .add(material.roughness);
    }
    for (const IlluminationObject& object : scene.objects) {
        digest.add(object.node)
            .add(static_cast<u64>(object.mesh))
            .add(static_cast<u64>(object.material));
        digest.add(object.transform).add(static_cast<u64>(object.movable ? 1U : 0U));
    }
    for (const gi::GiLight& light : scene.lights) {
        digest.add(light.position).add(light.direction).add(light.colour).add(light.intensity);
        digest.add(light.range).add(static_cast<u64>(light.directional ? 1U : 0U)).add(light.id);
    }
    digest.add(scene.sky.zenith)
        .add(scene.sky.horizon)
        .add(scene.sky.ground)
        .add(scene.sky.intensity);
    return digest.value();
}

/// The largest of a transform's axis scales; one for a rigid one.
[[nodiscard]] f32 largest_scale(const Mat4& transform) noexcept {
    f32 largest = 0.0F;
    for (u32 axis = 0; axis < 3; ++axis) {
        const Vec4 column = transform.columns[axis];
        largest = std::max(largest, length(Vec3{column.x, column.y, column.z}));
    }
    return largest > 1.0e-6F ? largest : 1.0F;
}

/// A GI object id from a node identity: the top bit is the GI's own cell tag.
[[nodiscard]] u64 object_id(u64 node) noexcept {
    return node & ~gi::kObjectCellTag;
}

[[nodiscard]] Aabb mesh_bounds(const IlluminationMesh& mesh) noexcept {
    if (mesh.positions.empty()) {
        return Aabb{};
    }
    Aabb bounds = Aabb::from_min_max(mesh.positions[0], mesh.positions[0]);
    for (const Vec3 position : mesh.positions) {
        bounds =
            Aabb::from_min_max(cwise_min(bounds.min, position), cwise_max(bounds.max, position));
    }
    return bounds;
}

/// The settings a world of `bounds` is lit with: one field level over it, probes a sixteenth of
/// it apart, the cards' own bounce as the frame's resolve uses.
[[nodiscard]] gi::IlluminationSettings settings_for(const Aabb& bounds, const gi::SkyTerm& sky,
                                                    bool hardware) noexcept {
    const Vec3 size = bounds.size();
    const f32 extent = std::max({size.x, size.y, size.z, kMinimumExtent}) * 1.25F;
    gi::IlluminationSettings settings;
    settings.field.levels = 2;
    settings.field.resolution = kFieldResolution;
    settings.field.base_extent_metres = extent;
    settings.probes.levels = 2;
    settings.probes.base_spacing_metres = std::max(extent / 16.0F, 0.5F);
    settings.probes.half_extent_probes = kProbeHalfExtent;
    settings.rays_per_probe = 16;
    settings.max_ray_distance_metres = extent;
    settings.surface_bounce = gi::SurfaceBounce::Cards;
    settings.card_gather.max_distance_metres = extent;
    settings.sky = sky;
    settings.ray_tracing.device_supports_ray_tracing = hardware;
    return settings;
}

/// The world-space bounds of every object.
[[nodiscard]] Aabb scene_bounds(const IlluminationScene& scene) noexcept {
    bool any = false;
    Aabb bounds{};
    for (const IlluminationObject& object : scene.objects) {
        if (object.mesh >= scene.meshes.size()) {
            continue;
        }
        const Aabb placed = transformed(mesh_bounds(scene.meshes[object.mesh]), object.transform);
        bounds = any ? Aabb::from_min_max(cwise_min(bounds.min, placed.min),
                                          cwise_max(bounds.max, placed.max))
                     : placed;
        any = true;
    }
    return any ? bounds : Aabb::from_center_extents(Vec3{}, Vec3{1.0F, 1.0F, 1.0F});
}

[[nodiscard]] Status emitter_rules(const IlluminationScene& scene,
                                   std::vector<gi::EmitterRule>& out) noexcept {
    out.clear();
    for (const IlluminationEmission& entry : scene.emission) {
        if (entry.material >= scene.materials.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "an emitter rule names a material the scene does not have");
        }
        if (entry.mode == gi::EmitterOverride::Automatic) {
            continue;
        }
        // Materials are numbered from one on the cards: zero is "no material".
        out.push_back(gi::EmitterRule{gi::kAnyEmitter, entry.material + 1U, entry.mode});
    }
    return ok();
}

/// A pixel's ray: from the eye through the pixel's point at clip depth one half, which is finite
/// under a conventional, a reversed-Z and an infinite projection alike.
[[nodiscard]] Ray pixel_ray(const IlluminationViewCamera& camera, u32 x, u32 y) noexcept {
    const f32 ndc_x =
        (((static_cast<f32>(x) + 0.5F) / static_cast<f32>(camera.width)) * 2.0F) - 1.0F;
    const f32 ndc_y =
        1.0F - (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(camera.height)) * 2.0F);
    const Vec4 through = camera.clip_to_world * Vec4{ndc_x, ndc_y, 0.5F, 1.0F};
    const Vec3 point = Vec3{through.x, through.y, through.z} * (1.0F / through.w);
    return Ray{camera.eye, normalized_or(point - camera.eye, Vec3{0.0F, 0.0F, -1.0F})};
}

}  // namespace

/// One distance grid per mesh, built once and kept across rebuilds while its key is unchanged.
struct IlluminationPreview::Fields {
    std::map<std::string, std::unique_ptr<gi::MeshDistanceField>> by_key;
};

IlluminationPreview::IlluminationPreview(Allocator& allocator) noexcept
    : allocator_(&allocator),
      system_(std::make_unique<gi::IlluminationSystem>()),
      tracer_(std::make_unique<bake::MeshSceneTracer>()),
      fields_(std::make_unique<Fields>()) {}

IlluminationPreview::~IlluminationPreview() = default;

Status IlluminationPreview::sync(const IlluminationScene& scene) noexcept {
    for (const IlluminationObject& object : scene.objects) {
        if (object.mesh >= scene.meshes.size() || object.material >= scene.materials.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "an object names a mesh or material the scene does not have");
        }
    }
    const u64 digest = geometry_digest(scene);
    if (!ready_ || digest != geometry_digest_) {
        if (Status rebuilt = rebuild(scene); !rebuilt) {
            ready_ = false;
            return rebuilt;
        }
        geometry_digest_ = digest;
        ready_ = true;
        rebuilds_ += 1;
        return ok();
    }
    if (Status applied = apply(scene); !applied) {
        return applied;
    }
    applications_ += 1;
    return ok();
}

Status IlluminationPreview::build_tracer(const IlluminationScene& scene) noexcept {
    bake_meshes_.clear();
    bake_materials_.clear();
    bake_instances_.clear();
    for (const IlluminationMesh& mesh : scene.meshes) {
        bake::BakeMesh entry;
        entry.positions = mesh.positions;
        entry.indices = mesh.indices;
        bake_meshes_.push_back(entry);
    }
    for (const IlluminationMaterial& material : scene.materials) {
        bake::BakeMaterial entry;
        entry.albedo = material.albedo;
        entry.emission = material.emission;
        bake_materials_.push_back(entry);
    }
    for (const IlluminationObject& object : scene.objects) {
        bake::BakeInstance entry;
        entry.mesh = object.mesh;
        entry.material = object.material;
        entry.transform = object.transform;
        entry.receives_lightmap = false;
        entry.id = object.node;
        bake_instances_.push_back(entry);
    }
    bake::LightmapScene traced;
    traced.meshes = {bake_meshes_.data(), bake_meshes_.size()};
    traced.materials = {bake_materials_.data(), bake_materials_.size()};
    traced.instances = {bake_instances_.data(), bake_instances_.size()};
    tracer_ = std::make_unique<bake::MeshSceneTracer>();
    return tracer_->build(traced);
}

Status IlluminationPreview::add_objects(const IlluminationScene& scene) noexcept {
    for (u32 index = 0; index < scene.objects.size(); ++index) {
        const IlluminationObject& object = scene.objects[index];
        const IlluminationMesh& mesh = scene.meshes[object.mesh];
        const IlluminationMaterial& material = scene.materials[object.material];
        std::unique_ptr<gi::MeshDistanceField>& field = fields_->by_key[mesh.key];
        if (field == nullptr) {
            field = std::make_unique<gi::MeshDistanceField>();
            gi::MeshFieldSettings settings;
            settings.dimension = kMeshFieldDimension;
            settings.margin_metres = kMeshFieldMargin;
            if (Status built = field->build(mesh.positions, mesh.indices, settings); !built) {
                field.reset();
                return built;
            }
        }
        gi::MeshCardSettings cards;
        // In the world, not the mesh: a unit plane scaled to an 8 m floor is carded every half
        // metre of the floor, not every half metre of the unit square.
        cards.spacing_metres = kCardSpacing / largest_scale(object.transform);
        cards.albedo = material.albedo;
        cards.emission = material.emission;
        cards.roughness = material.roughness;
        cards.instance_id = index + 1U;
        cards.material_id = object.material + 1U;
        Array<gi::Surfel> surfels(*allocator_);
        if (Status carded = gi::append_mesh_cards(mesh.positions, mesh.indices, Mat4::identity(),
                                                  cards, surfels);
            !carded) {
            return carded;
        }
        const gi::AssetDistanceField asset = field->asset();
        gi::GiObjectDesc desc;
        desc.id = object_id(object.node);
        desc.mobility = object.movable ? gi::ObjectMobility::Movable : gi::ObjectMobility::Static;
        desc.local_bounds = mesh_bounds(mesh);
        desc.transform = object.transform;
        desc.has_class_override = object.has_class;
        desc.class_override = object.klass;
        desc.surfels = surfels.span();
        desc.field = &asset;
        if (Status added = system_->add_object(desc, frame_); !added) {
            return added;
        }
        object_nodes_.push_back(object.node);
    }
    return ok();
}

Status IlluminationPreview::rebuild(const IlluminationScene& scene) noexcept {
    if (scene.objects.empty()) {
        return fail(ErrorCode::InvalidArgument, "the world has no mesh for the GI to light");
    }
    if (Status traced = build_tracer(scene); !traced) {
        return traced;
    }
    system_ = std::make_unique<gi::IlluminationSystem>();
    object_nodes_.clear();
    const Aabb bounds = scene_bounds(scene);
    bounds_ = bounds;
    if (Status configured = system_->configure(settings_for(bounds, scene.sky, true));
        !configured) {
        return configured;
    }
    // The hardware tier traces the same triangles here, on the processor: the editor has no
    // device for it, and the service is the oracle the device tier is held to.
    for (u32 index = 0; index < scene.meshes.size(); ++index) {
        rendering::rt::TriangleGeometry triangles;
        triangles.positions = scene.meshes[index].positions;
        triangles.indices = scene.meshes[index].indices;
        if (Status declared = system_->acceleration().declare_geometry(
                index + 1U, rendering::rt::adapt_static_mesh(triangles, 0));
            !declared) {
            return declared;
        }
    }
    for (u32 index = 0; index < scene.objects.size(); ++index) {
        rendering::rt::InstanceDescriptor instance;
        instance.geometry = scene.objects[index].mesh + 1U;
        instance.transform = scene.objects[index].transform;
        instance.instance_id = index + 1U;
        instance.material_id = scene.objects[index].material + 1U;
        if (auto added = system_->acceleration().add_instance(instance); !added) {
            return make_unexpected(added.error());
        }
    }
    material_names_.clear();
    for (const IlluminationMaterial& material : scene.materials) {
        material_names_.push_back(material.name);
    }
    lights_.assign(scene.lights.begin(), scene.lights.end());
    frame_ += 1;
    if (Status added = add_objects(scene); !added) {
        return added;
    }
    system_->surfaces().set_lookup_radius(kCardSpacing * 1.5F);
    if (Status applied = apply(scene); !applied) {
        return applied;
    }
    eye_ = bounds.center();
    gi::FrameContext context;
    context.camera = eye_;
    context.lights = {lights_.data(), lights_.size()};
    context.frame = ++frame_;
    frame_ += system_->advance_to_convergence(context, kConvergedThreshold, kConvergeFrames);
    return ok();
}

Status IlluminationPreview::apply(const IlluminationScene& scene) noexcept {
    if (Status set = system_->set_runtime_settings(scene.settings); !set) {
        return set;
    }
    if (Status placed = system_->set_volumes(scene.volumes); !placed) {
        return placed;
    }
    std::vector<gi::EmitterRule> rules;
    if (Status built = emitter_rules(scene, rules); !built) {
        return built;
    }
    const Span<const gi::EmitterRule> current = system_->emitter_rules();
    const bool same_rules = current.size() == rules.size() &&
                            std::equal(rules.begin(), rules.end(), current.begin(),
                                       [](const gi::EmitterRule& a, const gi::EmitterRule& b) {
                                           return a.instance_id == b.instance_id &&
                                                  a.material_id == b.material_id &&
                                                  a.mode == b.mode;
                                       });
    if (!same_rules) {
        if (Status ruled = system_->set_emitter_rules({rules.data(), rules.size()}, ++frame_);
            !ruled) {
            return ruled;
        }
    }
    for (const IlluminationObject& object : scene.objects) {
        const gi::GiObject* registered = system_->object(object_id(object.node));
        if (registered == nullptr) {
            continue;
        }
        const bool same = registered->has_class_override == object.has_class &&
                          (!object.has_class || registered->class_override == object.klass);
        if (!same) {
            if (Status classed = system_->set_object_class(object_id(object.node), object.has_class,
                                                           object.klass, ++frame_);
                !classed) {
                return classed;
            }
        }
    }
    return ok();
}

void IlluminationPreview::update(Vec3 eye, u32 frames) noexcept {
    if (!ready_) {
        return;
    }
    eye_ = eye;
    for (u32 index = 0; index < frames; ++index) {
        gi::FrameContext context;
        context.camera = eye;
        context.lights = {lights_.data(), lights_.size()};
        context.frame = ++frame_;
        (void)system_->update(context);
    }
}

bool IlluminationPreview::pick(const Ray& ray, Vec3& point, Vec3& normal) const noexcept {
    if (!ready_) {
        return false;
    }
    gi::SceneHit hit;
    if (!tracer_->trace(ray.origin, normalized_or(ray.direction, Vec3{0.0F, 0.0F, -1.0F}), 1.0e4F,
                        hit)) {
        return false;
    }
    point = hit.position;
    normal = hit.normal;
    return true;
}

Status IlluminationPreview::render_view(gi::GiDebugView view, const IlluminationViewCamera& camera,
                                        Array<u32>& out) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "the GI preview has no world yet");
    }
    if (camera.width == 0 || camera.height == 0 || view >= gi::GiDebugView::Count) {
        return fail(ErrorCode::InvalidArgument, "a GI view needs a size and a view");
    }
    const usize count = static_cast<usize>(camera.width) * camera.height;
    Array<gi::GiViewPixel> pixels(*allocator_);
    if (Status sized = pixels.resize(count); !sized) {
        return sized;
    }
    const Vec3 forward =
        normalized_or(pixel_ray(camera, camera.width / 2U, camera.height / 2U).direction,
                      Vec3{0.0F, 0.0F, -1.0F});
    for (u32 y = 0; y < camera.height; ++y) {
        for (u32 x = 0; x < camera.width; ++x) {
            const Ray ray = pixel_ray(camera, x, y);
            gi::GiViewPixel& pixel = pixels[(static_cast<usize>(y) * camera.width) + x];
            f32 t = 0.0F;
            u32 triangle = 0;
            bool back_face = false;
            if (!tracer_->trace_triangle(ray.origin, ray.direction, 1.0e4F, t, triangle,
                                         back_face)) {
                pixel.depth = 0.0F;  // sky: the view draws nothing here
                continue;
            }
            const bake::WorldTriangle& hit = tracer_->triangles()[triangle];
            pixel.position = ray.at(t);
            pixel.normal = back_face ? -hit.normal : hit.normal;
            pixel.depth = t * std::max(dot(ray.direction, forward), 1.0e-3F);
            if (hit.material < bake_materials_.size()) {
                pixel.emission = bake_materials_[hit.material].emission;
            }
        }
    }
    gi::GiViewCamera view_camera;
    view_camera.width = camera.width;
    view_camera.height = camera.height;
    view_camera.position = camera.eye;
    view_camera.world_to_clip = camera.world_to_clip;
    gi::GiDebugInputs inputs;
    inputs.pixels = pixels.span();
    inputs.frame = frame_;
    inputs.field_slice_height = bounds_.center().y;
    inputs.exposure = camera.exposure > 0.0F ? camera.exposure : automatic_exposure(pixels.span());
    if (Status sized = out.resize(count); !sized) {
        return sized;
    }
    return gi::render_gi_debug_view(*system_, view, view_camera, inputs, out.span());
}

f32 IlluminationPreview::automatic_exposure(Span<const gi::GiViewPixel> pixels) const noexcept {
    // The mean indirect diffuse over every eighth surface pixel, drawn at a third of white: x/(1+x)
    // is a third where x is a half.
    constexpr usize kStride = 8;
    f32 sum = 0.0F;
    u32 count = 0;
    for (usize index = 0; index < pixels.size(); index += kStride) {
        const gi::GiViewPixel& pixel = pixels[index];
        if (pixel.depth <= 0.0F) {
            continue;
        }
        const Vec3 light =
            system_->indirect_diffuse(pixel.position, pixel.normal, gi::SurfaceProperties{})
                .radiance;
        sum += gi::emission_luminance(light);
        count += 1;
    }
    const f32 mean = count == 0 ? 0.0F : sum / static_cast<f32>(count);
    return mean > 1.0e-12F ? 0.5F / mean : 1.0F;
}

IlluminationReport IlluminationPreview::report(const Ray* pick_ray) const noexcept {
    IlluminationReport out;
    out.rebuilds = rebuilds_;
    out.applications = applications_;
    if (!ready_) {
        return out;
    }
    const gi::IlluminationSystem& system = *system_;
    out.settings = system.runtime_settings();
    out.world_tier = system.world_tracer().tier();
    out.convergence = system.convergence().worst_convergence();
    out.cards = system.surfaces().page_count();
    out.probes = system.radiance().probe_count();
    out.volumes = static_cast<u32>(system.volumes().size());
    out.lights = static_cast<u32>(lights_.size());
    out.profile = system.profile();
    out.emissive = system.emitters().report();
    for (const gi::EmitterRecord& record : system.emitters().records()) {
        IlluminationEmitterEntry entry;
        entry.node = record.instance_id >= 1U && record.instance_id <= object_nodes_.size()
                         ? object_nodes_[record.instance_id - 1U]
                         : 0U;
        entry.material = record.material_id >= 1U && record.material_id <= material_names_.size()
                             ? material_names_[record.material_id - 1U]
                             : std::string{};
        entry.classification = record.classification;
        entry.override_mode = record.override_mode;
        entry.cards = record.cards;
        entry.area_m2 = record.area_m2;
        entry.power = record.power;
        out.emitters.push_back(std::move(entry));
    }
    for (const u64 node : object_nodes_) {
        const gi::GiObject* object = system.object(object_id(node));
        if (object != nullptr) {
            out.objects.push_back(
                IlluminationObjectEntry{node, object->klass, object->has_class_override});
        }
    }
    if (pick_ray != nullptr && pick(*pick_ray, out.point, out.normal)) {
        gi::ExplainSettings settings;
        settings.frame = frame_;
        out.query = system.explain(out.point, out.normal, gi::SurfaceProperties{}, settings);
        out.explained = true;
    }
    return out;
}

}  // namespace cy::editor
