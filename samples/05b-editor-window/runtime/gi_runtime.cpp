// SPDX-License-Identifier: MIT
// The runtime host's GI seam. See gi_runtime.h.

#include "gi_runtime.h"

#include <cy/core/assets/cooked.h>
#include <cy/core/assets/file.h>
#include <cy/core/math/matrix.h>
#include <cy/import/model.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/scene/components.h>
#include <cy/scene/tree.h>
#include <cy/servers/render/picking.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string_view>
#include <utility>

#include "authored_components.h"

namespace cy::sample::editor_window {

namespace gi = rendering::gi;

namespace {

constexpr std::string_view kMeshType = "MeshRenderer";
constexpr std::string_view kSlotsType = "ImportedMaterialSlots";
constexpr std::string_view kLightType = "LightSource";
constexpr std::string_view kSettingsType = "GiSettings";
constexpr std::string_view kVolumeType = "GiVolume";
constexpr std::string_view kObjectType = "GiObject";
constexpr std::string_view kEmissionType = "GiEmission";
constexpr usize kFramesKept = 64;
/// The name the frame's default surface goes by in the report and in an emitter rule.
constexpr std::string_view kDefaultMaterial = "default";
/// The frame's default grey, `apply_standard_defaults`'.
constexpr Vec3 kDefaultAlbedo{0.5F, 0.5F, 0.5F};

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

/// The enumerator of `count` whose engine name, lower-cased with `-` for a word break, is
/// `keyword`; `fallback` for anything else.
template <class Enum, class Namer>
[[nodiscard]] Enum keyword_of(const std::string& keyword, Enum count, Namer name,
                              Enum fallback) noexcept {
    for (u32 index = 0; index < static_cast<u32>(count); ++index) {
        const auto value = static_cast<Enum>(index);
        std::string spelled;
        for (const char c : std::string_view(name(value))) {
            if (c >= 'A' && c <= 'Z' && !spelled.empty()) {
                spelled.push_back('-');
            }
            spelled.push_back(c);
        }
        if (lower(spelled) == keyword) {
            return value;
        }
    }
    return fallback;
}

[[nodiscard]] gi::IlluminationClass class_keyword(const std::string& keyword, bool& has) noexcept {
    // `transient` is the editor's word for TransientIgnored.
    if (keyword == "transient") {
        has = true;
        return gi::IlluminationClass::TransientIgnored;
    }
    const gi::IlluminationClass found =
        keyword_of(keyword, gi::IlluminationClass::Count, gi::illumination_class_name,
                   gi::IlluminationClass::Count);
    has = found != gi::IlluminationClass::Count;
    return has ? found : gi::IlluminationClass::Static;
}

[[nodiscard]] gi::GiRuntimeSettings read_settings(const ComponentReader& reader) {
    gi::GiRuntimeSettings settings;
    settings.mode =
        keyword_of(reader.text("mode"), gi::GiMode::Count, gi::gi_mode_name, settings.mode);
    settings.quality = keyword_of(reader.text("quality"), gi::GiQuality::Count, gi::gi_quality_name,
                                  settings.quality);
    settings.tier = keyword_of(reader.text("tier"), gi::GiTierPreference::Count,
                               gi::gi_tier_preference_name, settings.tier);
    settings.hardware_ray_tracing = reader.flag("hardware_ray_tracing", true);
    const f32 budget = reader.real("budget_ms", settings.budget_ms);
    settings.budget_ms = std::isfinite(budget) && budget > 0.0F ? budget : settings.budget_ms;
    return settings;
}

[[nodiscard]] gi::GiVolume read_volume(const ComponentReader& reader, const Mat4& matrix,
                                       u64 node) {
    gi::GiVolume volume;
    volume.bounds = transformed(Aabb::from_center_extents(Vec3{}, Vec3{0.5F, 0.5F, 0.5F}), matrix);
    volume.priority = static_cast<i32>(reader.integer("priority", 0));
    volume.blend_distance_metres = reader.real("blend_distance", 1.0F);
    volume.weight = reader.real("weight", 1.0F);
    volume.sets = static_cast<u32>(reader.integer("sets", 0)) &
                  ((1U << static_cast<u32>(gi::GiVolumeParameter::Count)) - 1U);
    volume.quality = keyword_of(reader.text("quality"), gi::GiQuality::Count, gi::gi_quality_name,
                                volume.quality);
    volume.tier = keyword_of(reader.text("tier"), gi::GiTierPreference::Count,
                             gi::gi_tier_preference_name, volume.tier);
    volume.probe_density = reader.real("probe_density", 1.0F);
    volume.update_rate = reader.real("update_rate", 1.0F);
    volume.diffuse_intensity = reader.real("diffuse_intensity", 1.0F);
    volume.specular_intensity = reader.real("specular_intensity", 1.0F);
    volume.far_field_begins_metres = reader.real("far_field", 60.0F);
    volume.id = node;
    return volume;
}

/// The node's world matrices, parents before children.
[[nodiscard]] Status world_matrices(const ser::World& world, Array<Mat4>& out) noexcept {
    const Span<const ser::WorldNode> nodes = world.nodes().span();
    if (Status sized = out.resize(nodes.size()); !sized) {
        return sized;
    }
    for (usize row = 0; row < nodes.size(); ++row) {
        Transform local;
        (void)ser::transform_of(world, nodes[row], local);
        out[row] = local.to_matrix();
        const u32 parent = nodes[row].parent;
        if (parent < row && nodes[parent].live) {
            out[row] = out[parent] * out[row];
        }
    }
    return ok();
}

/// A light as the GI's lights are given: in the bake's units, the frame's intensity over pi
/// (gi/bake.h: a card is shaded `albedo * direct`).
[[nodiscard]] bool light_of(const ComponentReader& reader, const Mat4& matrix, u64 id,
                            gi::GiLight& out) noexcept {
    const ser::WorldValue* kind = reader.value("kind");
    if (kind == nullptr || kind->kind != ser::WorldValueKind::Int ||
        !reader.flag("enabled", true)) {
        return false;
    }
    const Vec3 origin = transform_point(matrix, Vec3{});
    out.position = origin;
    out.direction = normalized_or(transform_point(matrix, Vec3{0.0F, 0.0F, -1.0F}) - origin,
                                  Vec3{0.0F, -1.0F, 0.0F});
    out.directional = kind->integer == static_cast<i64>(render::LightKind::Directional);
    out.colour = Vec3{reader.real("color.r", 1.0F), reader.real("color.g", 1.0F),
                      reader.real("color.b", 1.0F)};
    out.intensity = reader.real("intensity", 1000.0F) / std::numbers::pi_v<f32>;
    out.range = out.directional ? 0.0F : reader.real("range", 10.0F);
    out.id = id;
    return true;
}

[[nodiscard]] bool movable(const ser::World& world, const ser::WorldNode& node) noexcept {
    return ComponentReader(world, node, "RigidBody").present() ||
           ComponentReader(world, node, "KinematicBody").present();
}

}  // namespace

AuthoredIllumination::AuthoredIllumination(Allocator& allocator, std::string project) noexcept
    : allocator_(&allocator),
      project_(std::move(project)),
      preview_(std::make_unique<editor::IlluminationPreview>(allocator)) {}

AuthoredIllumination::~AuthoredIllumination() = default;

void AuthoredIllumination::bind(const ser::World* world) noexcept {
    world_ = world;
    stale_ = true;
}

void AuthoredIllumination::record_frame(u64 frame, const render::View& view, Vec3 eye) {
    frames_.push_back(Frame{frame, view, eye});
    if (frames_.size() > kFramesKept) {
        frames_.pop_front();
    }
}

const AuthoredIllumination::LoadedMesh* AuthoredIllumination::mesh(
    const std::string& reference) noexcept {
    const auto found = meshes_.find(reference);
    if (found != meshes_.end()) {
        return found->second.get();
    }
    auto loaded = std::make_unique<LoadedMesh>();
    if (Status read = read_mesh_reference(project_, reference, *allocator_, loaded->data); !read) {
        mesh_failures_ += 1;
        std::fprintf(stderr, "gi source mesh '%s': %s\n", reference.c_str(), read.error().message);
        return nullptr;
    }
    return meshes_.emplace(reference, std::move(loaded)).first->second.get();
}

u32 AuthoredIllumination::material(const std::string& reference) noexcept {
    const std::string name = reference.empty() ? std::string(kDefaultMaterial) : reference;
    const auto found = material_index_.find(name);
    if (found != material_index_.end()) {
        return found->second;
    }
    editor::IlluminationMaterial entry;
    entry.name = name;
    entry.albedo = kDefaultAlbedo;
    if (!reference.empty() && !reference.ends_with(".cygraph")) {
        // A cooked standard material: its base colour and its emission, as the frame draws them.
        Array<u8> cooked(*allocator_);
        const std::string path = project_ + "/.cy/cooked/" + reference + ".cyasset";
        import::StandardMaterial standard;
        if (assets::fs::read_whole(path.c_str(), cooked)) {
            Expected<Span<const u8>, Error> payload =
                assets::read_cooked_payload(cooked.data(), cooked.size(), true);
            if (payload && import::read_cooked_material(*payload, standard)) {
                entry.albedo = Vec3{standard.base_colour[0], standard.base_colour[1],
                                    standard.base_colour[2]} *
                               (1.0F - std::clamp(standard.metallic, 0.0F, 1.0F));
                entry.emission =
                    Vec3{standard.emissive[0], standard.emissive[1], standard.emissive[2]};
                entry.roughness = standard.roughness;
            }
        }
    }
    const auto index = static_cast<u32>(scene_materials_.size());
    scene_materials_.push_back(std::move(entry));
    material_index_.emplace(name, index);
    return index;
}

Status AuthoredIllumination::gather() noexcept {
    scene_meshes_.clear();
    scene_materials_.clear();
    scene_objects_.clear();
    scene_lights_.clear();
    scene_volumes_.clear();
    scene_emission_.clear();
    mesh_index_.clear();
    material_index_.clear();
    scene_ = editor::IlluminationScene{};
    scene_.sky = sky_;
    has_settings_ = false;
    Array<Mat4> matrices(*allocator_);
    if (Status built = world_matrices(*world_, matrices); !built) {
        return built;
    }
    std::vector<std::pair<std::string, gi::EmitterOverride>> emission;
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    for (usize row = 0; row < nodes.size(); ++row) {
        const ser::WorldNode& node = nodes[row];
        if (!node.live) {
            continue;
        }
        if (const ComponentReader settings(*world_, node, kSettingsType); settings.present()) {
            scene_.settings = read_settings(settings);
            has_settings_ = true;
        }
        if (const ComponentReader volume(*world_, node, kVolumeType); volume.present()) {
            scene_volumes_.push_back(read_volume(volume, matrices[row], node.identity));
        }
        if (const ComponentReader rule(*world_, node, kEmissionType); rule.present()) {
            const gi::EmitterOverride mode =
                keyword_of(rule.text("mode"), gi::EmitterOverride::Count, gi::emitter_override_name,
                           gi::EmitterOverride::Automatic);
            emission.emplace_back(rule.text("material"), mode);
        }
        if (const ComponentReader light(*world_, node, kLightType); light.present()) {
            gi::GiLight entry;
            if (light_of(light, matrices[row], node.identity, entry)) {
                scene_lights_.push_back(entry);
            }
        }
        const ComponentReader renderer(*world_, node, kMeshType);
        const std::string reference = renderer.text("mesh");
        const LoadedMesh* loaded = reference.empty() ? nullptr : mesh(reference);
        if (loaded == nullptr) {
            continue;
        }
        const auto [entry, added] =
            mesh_index_.emplace(reference, static_cast<u32>(scene_meshes_.size()));
        if (added) {
            scene_meshes_.push_back(editor::IlluminationMesh{
                reference, loaded->data.positions.span(), loaded->data.indices.span()});
        }
        const std::string slot = ComponentReader(*world_, node, kSlotsType).text("slot_0");
        editor::IlluminationObject object;
        object.node = node.identity;
        object.mesh = entry->second;
        object.material = material(slot.empty() ? renderer.text("material") : slot);
        object.transform = matrices[row];
        object.movable = movable(*world_, node);
        if (const ComponentReader klass(*world_, node, kObjectType); klass.present()) {
            object.klass = class_keyword(klass.text("class"), object.has_class);
        }
        scene_objects_.push_back(object);
    }
    for (const auto& [name, mode] : emission) {
        const auto found =
            material_index_.find(name.empty() ? std::string(kDefaultMaterial) : name);
        if (found != material_index_.end()) {
            scene_emission_.push_back(editor::IlluminationEmission{found->second, mode});
        }
    }
    scene_.meshes = {scene_meshes_.data(), scene_meshes_.size()};
    scene_.materials = {scene_materials_.data(), scene_materials_.size()};
    scene_.objects = {scene_objects_.data(), scene_objects_.size()};
    scene_.lights = {scene_lights_.data(), scene_lights_.size()};
    scene_.volumes = {scene_volumes_.data(), scene_volumes_.size()};
    scene_.emission = {scene_emission_.data(), scene_emission_.size()};
    return ok();
}

editor::IlluminationPreview* AuthoredIllumination::preview() noexcept {
    if (world_ == nullptr) {
        return nullptr;
    }
    if (stale_) {
        if (Status gathered = gather(); !gathered) {
            std::fprintf(stderr, "gi: %s\n", gathered.error().message);
            return nullptr;
        }
        if (scene_objects_.empty()) {
            return nullptr;
        }
        if (Status synced = preview_->sync(scene_); !synced) {
            std::fprintf(stderr, "gi: %s\n", synced.error().message);
            return nullptr;
        }
        stale_ = false;
    }
    return preview_->ready() ? preview_.get() : nullptr;
}

Expected<Ray, Error> AuthoredIllumination::pick_ray(u32, u64 frame, f32 x, f32 y) noexcept {
    for (const Frame& held : frames_) {
        if (held.identity == frame || (frame == 0 && &held == &frames_.back())) {
            Ray ray = render::ray_through_pixel(held.view, x, y);
            ray.origin = ray.origin + held.eye;
            return ray;
        }
    }
    return make_unexpected(
        Error{ErrorCode::Unavailable, "the runtime no longer holds that frame's view"});
}

Status AuthoredIllumination::render_view(render::DebugViewMode mode, const render::View& view,
                                         Vec3 eye, u32 width, u32 height, u32 divisor,
                                         Array<u32>& out) noexcept {
    const gi::GiDebugView which = gi::gi_debug_view_from_name(render::debug_view_mode_name(mode));
    if (which == gi::GiDebugView::Count) {
        return fail(ErrorCode::InvalidArgument, "the view mode is not a GI view");
    }
    editor::IlluminationPreview* lit = preview();
    if (lit == nullptr) {
        return fail(ErrorCode::Unavailable, "the world has nothing for the GI to light");
    }
    lit->update(eye, 1);
    const u32 step = std::max(divisor, 1U);
    editor::IlluminationViewCamera camera;
    camera.width = std::max(width / step, 1U);
    camera.height = std::max(height / step, 1U);
    camera.eye = eye;
    // Automatic: the world's lights may be in any units, and a view drawn at the frame's exposure
    // would be black in most of them.
    camera.exposure = 0.0F;
    camera.world_to_clip = view.view_projection * Mat4::from_translation(-eye);
    Expected<Mat4, Error> inverse_clip = inverse(camera.world_to_clip);
    if (!inverse_clip) {
        return make_unexpected(inverse_clip.error());
    }
    camera.clip_to_world = inverse_clip.value();
    Array<u32> small(*allocator_);
    if (Status drawn = lit->render_view(which, camera, small); !drawn) {
        return drawn;
    }
    if (Status sized = out.resize(static_cast<usize>(width) * height); !sized) {
        return sized;
    }
    // Repeated up to the frame's size: each drawn texel covers a `step`-square of pixels.
    for (u32 y = 0; y < height; ++y) {
        const u32 source_y = std::min(y / step, camera.height - 1U);
        for (u32 x = 0; x < width; ++x) {
            const u32 source_x = std::min(x / step, camera.width - 1U);
            out[(static_cast<usize>(y) * width) + x] =
                small[(static_cast<usize>(source_y) * camera.width) + source_x];
        }
    }
    return ok();
}

bool AuthoredIllumination::lights_frame() noexcept {
    if (preview() == nullptr || !has_settings_) {
        return false;
    }
    // `exclusion_for()`'s answer, as the frame's composite reads it (`gi_gpu::gi_admission`): a
    // mode that keeps the dynamic sources out of every kind of surface lights nothing.
    const gi::GiMode mode = preview_->system().runtime_settings().mode;
    const u32 dynamic = gi::source_bit(gi::RadianceSource::RadianceCache);
    return (gi::exclusion_for(mode, false, false) & dynamic) == 0 ||
           (gi::exclusion_for(mode, true, false) & dynamic) == 0 ||
           (gi::exclusion_for(mode, false, true) & dynamic) == 0;
}

void AuthoredIllumination::set_sky_radiance(Vec3 radiance) noexcept {
    gi::SkyTerm sky;
    sky.zenith = radiance;
    sky.horizon = radiance;
    sky.ground = radiance;
    sky.intensity = 1.0F;
    const auto same = [](Vec3 a, Vec3 b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    if (same(sky.zenith, sky_.zenith) && same(sky.horizon, sky_.horizon) &&
        same(sky.ground, sky_.ground) && sky.intensity == sky_.intensity) {
        return;
    }
    sky_ = sky;
    scene_.sky = sky;
    if (preview_->ready()) {
        preview_->system().set_sky_term(sky);
    }
}

bool AuthoredIllumination::advance(Vec3 eye, AuthoredGi& out) noexcept {
    editor::IlluminationPreview* lit = preview();
    if (lit == nullptr) {
        return false;
    }
    lit->update(eye, 1);
    return current(out);
}

namespace {

/// Every object the GI holds by its Play entity and every material by its authored name.
void bind_names(game_backend::IlluminationAdapter& adapter, const editor::IlluminationPreview& lit,
                const gameplay::PlaySession& play) noexcept {
    const Span<const u64> nodes = lit.object_nodes();
    for (u32 index = 0; index < nodes.size(); ++index) {
        const ecs::Entity entity = play.entity_for(nodes[index]);
        if (entity.valid()) {
            (void)adapter.bind_object(entity.bits(), editor::illumination_object_id(nodes[index]),
                                      index + 1U);
        }
    }
    const Span<const std::string> materials = lit.material_names();
    for (u32 index = 0; index < materials.size(); ++index) {
        (void)adapter.add_material(materials[index].c_str(), index + 1U);
    }
}

}  // namespace

std::unique_ptr<game_backend::IlluminationAdapter> AuthoredIllumination::bind_play(
    const gameplay::PlaySession& play) noexcept {
    editor::IlluminationPreview* lit = preview();
    if (lit == nullptr) {
        return nullptr;
    }
    auto adapter = std::make_unique<game_backend::IlluminationAdapter>(*allocator_, lit->system());
    bind_names(*adapter, *lit, play);
    adapter->begin_frame(lit->frame());
    return adapter;
}

Status AuthoredIllumination::rebind_play(game_backend::IlluminationAdapter& adapter,
                                         const gameplay::PlaySession& play) noexcept {
    editor::IlluminationPreview* lit = preview();
    if (lit == nullptr) {
        return fail(ErrorCode::Unavailable, "the world has nothing for the GI to light");
    }
    adapter.rebind(lit->system());
    bind_names(adapter, *lit, play);
    adapter.begin_frame(lit->frame());
    // The rebuilt GI placed every object where the document puts it: the bodies move them again
    // from the next `follow_play`.
    play_transforms_.clear();
    played_.clear();
    return adapter.restore();
}

u32 AuthoredIllumination::follow_play(gameplay::PlaySession& play) noexcept {
    editor::IlluminationPreview* lit = preview();
    if (lit == nullptr || play.world() == nullptr || play.tree() == nullptr) {
        return 0;
    }
    const scene::SceneComponents& ids = play.tree()->components();
    play_transforms_.resize(scene_objects_.size(), Mat4::identity());
    played_.resize(scene_objects_.size(), false);
    u32 moved = 0;
    for (usize index = 0; index < scene_objects_.size(); ++index) {
        const editor::IlluminationObject& object = scene_objects_[index];
        if (!object.movable) {
            continue;
        }
        const ecs::Entity entity = play.entity_for(object.node);
        const auto* placed =
            entity.valid() ? play.world()->get<scene::WorldTransform>(entity, ids.world_transform)
                           : nullptr;
        if (placed == nullptr) {
            continue;
        }
        const Mat4 transform = placed->value.to_matrix();
        const Mat4& last = played_[index] ? play_transforms_[index] : object.transform;
        if (std::memcmp(&transform, &last, sizeof(Mat4)) == 0) {
            continue;
        }
        if (lit->system()
                .move_object(editor::illumination_object_id(object.node), transform, lit->frame())
                .has_value()) {
            play_transforms_[index] = transform;
            played_[index] = true;
            moved += 1;
        }
    }
    return moved;
}

void AuthoredIllumination::end_play() noexcept {
    editor::IlluminationPreview* lit = preview_->ready() ? preview_.get() : nullptr;
    if (lit != nullptr) {
        // The game's volumes end with it; the document's are the author's and stay.
        (void)lit->system().set_volumes(gi::GiVolumeOwner::Game, {});
    }
    for (usize index = 0; lit != nullptr && index < played_.size(); ++index) {
        if (played_[index] && index < scene_objects_.size()) {
            const editor::IlluminationObject& object = scene_objects_[index];
            (void)lit->system().move_object(editor::illumination_object_id(object.node),
                                            object.transform, lit->frame());
        }
    }
    play_transforms_.clear();
    played_.clear();
}

bool AuthoredIllumination::current(AuthoredGi& out) noexcept {
    editor::IlluminationPreview* lit = preview();
    if (lit == nullptr) {
        return false;
    }
    out.system = &lit->system();
    out.generation = lit->rebuilds();
    out.frame = lit->frame();
    out.field_slice_height = lit->field_slice_height();
    return true;
}

Status AuthoredIllumination::volumes(std::vector<GiVolumeBox>& out) const noexcept {
    out.clear();
    if (world_ == nullptr) {
        return ok();
    }
    Array<Mat4> matrices(*allocator_);
    if (Status built = world_matrices(*world_, matrices); !built) {
        return built;
    }
    const Span<const ser::WorldNode> nodes = world_->nodes().span();
    for (usize row = 0; row < nodes.size(); ++row) {
        const ComponentReader volume(*world_, nodes[row], kVolumeType);
        if (nodes[row].live && volume.present()) {
            out.push_back(
                GiVolumeBox{nodes[row].identity, read_volume(volume, matrices[row], 0).bounds});
        }
    }
    return ok();
}

void draw_gi_volumes(const Canvas& canvas, const render::View& view, Vec3 eye,
                     Span<const GiVolumeBox> boxes) noexcept {
    // The GI's colour in the interface: a cool cyan, distinct from the navigation overlay's teal
    // and the physics layers' greens and oranges.
    constexpr u32 kVolumeColour = 0x52C7F0;
    constexpr u32 kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                   {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const GiVolumeBox& box : boxes) {
        Vec2 corners[8];
        bool seen[8] = {};
        for (u32 index = 0; index < 8; ++index) {
            const Vec3 corner{(index & 1U) != 0U ? box.bounds.max.x : box.bounds.min.x,
                              (index & 2U) != 0U ? box.bounds.max.y : box.bounds.min.y,
                              (index & 4U) != 0U ? box.bounds.max.z : box.bounds.min.z};
            seen[index] = render::project_to_pixel(view, corner - eye, corners[index]);
        }
        for (const auto& edge : kEdges) {
            // An edge with a corner behind the camera is left out rather than drawn wrapped.
            if (seen[edge[0]] && seen[edge[1]]) {
                draw_thin_line(canvas, corners[edge[0]].x, corners[edge[0]].y, corners[edge[1]].x,
                               corners[edge[1]].y, kVolumeColour);
            }
        }
    }
}

}  // namespace cy::sample::editor_window
