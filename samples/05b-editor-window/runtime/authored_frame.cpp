// SPDX-License-Identifier: MIT
#include "authored_frame.h"

#include <cy/core/assets/cooked.h>
#include <cy/core/assets/file.h>
#include <cy/core/math/projection.h>
#include <cy/import/mesh.h>
#include <cy/import/model.h>
#include <cy/import/primitive.h>
#include <cy/import/texture.h>
#include <cy/rendering/material/standard.h>
#include <cy/servers/render/sort.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace cy::sample::editor_window {
namespace {

namespace ser = scene::serialization;
using namespace rendering;
using namespace rendering::assembly;
using namespace rendering::pipeline;

Vec3 point(const Mat4& matrix, Vec3 model) noexcept;

constexpr u32 kCapacity = 4096;
constexpr u32 kMaterialCapacity = 128;
constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;

u32 read_u32(const u8* bytes) noexcept {
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8U) |
           (static_cast<u32>(bytes[2]) << 16U) | (static_cast<u32>(bytes[3]) << 24U);
}

Expected<render::TextureFormat, Error> cooked_texture_format(import::TextureFormat format,
                                                             bool encoded, bool srgb) noexcept {
    if (format == import::TextureFormat::BC7 && encoded) {
        return srgb ? render::TextureFormat::Bc7Srgb : render::TextureFormat::Bc7Unorm;
    }
    if (format == import::TextureFormat::RGBA8 && !encoded) {
        return srgb ? render::TextureFormat::Rgba8Srgb : render::TextureFormat::Rgba8Unorm;
    }
    return fail(ErrorCode::Unsupported, "authored frame: cooked texture format is not sampleable");
}

const ser::WorldValue* field_value(const ser::World& world, const ser::WorldNode& node,
                                   std::string_view type_name, std::string_view field_name) {
    for (const ser::WorldComponent& component : node.components()) {
        const ser::WorldTypeDecl* type = world.type(component.file_type);
        if (type == nullptr || world.text(type->name) != type_name) {
            continue;
        }
        for (const ser::WorldFieldDecl& declaration : type->fields()) {
            if (world.text(declaration.name) != field_name) {
                continue;
            }
            const ser::WorldField* field = component.find(declaration.file_field);
            return field == nullptr ? nullptr : &field->value;
        }
    }
    return nullptr;
}

std::string field_reference(const ser::World& world, const ser::WorldNode& node,
                            std::string_view type_name, std::string_view field_name) {
    const ser::WorldValue* value = field_value(world, node, type_name, field_name);
    if (value == nullptr || value->kind != ser::WorldValueKind::Text) {
        return {};
    }
    const Span<const u8> bytes = world.blob(*value);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

f32 light_float(const ser::World& world, const ser::WorldNode& node, std::string_view name,
                f32 fallback) noexcept {
    const ser::WorldValue* value = field_value(world, node, "LightSource", name);
    return value != nullptr && value->kind == ser::WorldValueKind::Float ? value->lanes[0]
                                                                         : fallback;
}

bool light_enabled(const ser::World& world, const ser::WorldNode& node) noexcept {
    const ser::WorldValue* value = field_value(world, node, "LightSource", "enabled");
    return value == nullptr || (value->kind == ser::WorldValueKind::Bool && value->integer != 0);
}

bool has_light_source(const ser::World& world, const ser::WorldNode& node) noexcept {
    return field_value(world, node, "LightSource", "kind") != nullptr;
}

bool append_light(const ser::World& world, const ser::WorldNode& node, const Mat4& matrix,
                  Array<render::LightDescription>& lights) noexcept {
    const ser::WorldValue* kind = field_value(world, node, "LightSource", "kind");
    if (kind == nullptr || kind->kind != ser::WorldValueKind::Int || !light_enabled(world, node)) {
        return true;
    }
    if (kind->integer < 0 || kind->integer >= static_cast<i64>(render::LightKind::Count)) {
        return true;
    }
    render::LightDescription light;
    light.kind = static_cast<render::LightKind>(kind->integer);
    light.transform.translation = point(matrix, Vec3{});
    const Vec3 forward =
        normalize(point(matrix, Vec3{0.0F, 0.0F, -1.0F}) - light.transform.translation);
    const Vec3 up = normalize(point(matrix, Vec3{0.0F, 1.0F, 0.0F}) - light.transform.translation);
    light.transform.rotation = Quat::look_rotation(forward, up);
    light.color[0] = light_float(world, node, "color.r", 1.0F);
    light.color[1] = light_float(world, node, "color.g", 1.0F);
    light.color[2] = light_float(world, node, "color.b", 1.0F);
    light.intensity = light_float(world, node, "intensity", 1000.0F);
    light.range = light_float(world, node, "range", 10.0F);
    light.inner_cone_radians = light_float(world, node, "inner_cone", 0.0F);
    light.outer_cone_radians = light_float(world, node, "outer_cone", 0.7853981634F);
    const ser::WorldValue* casts_shadow = field_value(world, node, "LightSource", "casts_shadow");
    light.casts_shadow =
        casts_shadow == nullptr ||
        (casts_shadow->kind == ser::WorldValueKind::Bool && casts_shadow->integer != 0);
    light.stable_id = node.identity;
    return static_cast<bool>(lights.push_back(light));
}

std::string mesh_reference(const ser::World& world, const ser::WorldNode& node) {
    return field_reference(world, node, "MeshRenderer", "mesh");
}

[[nodiscard]] Expected<rhi::BufferHandle, Error> upload(rhi::Device& device, const char* name,
                                                        const void* source, u64 bytes,
                                                        rhi::BufferUsage usage) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = std::max<u64>(bytes, 16);
    description.usage = usage;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer) {
        return buffer;
    }
    void* mapped = device.buffer_mapped_pointer(*buffer);
    if (mapped == nullptr) {
        device.destroy_buffer(*buffer);
        return fail(ErrorCode::Internal, "authored frame: upload buffer was not mapped");
    }
    if (bytes != 0) {
        std::memcpy(mapped, source, static_cast<usize>(bytes));
    }
    return buffer;
}

Vec3 point(const Mat4& matrix, Vec3 model) noexcept {
    return (matrix * Vec4{model.x, model.y, model.z, 1.0F}).xyz();
}

Aabb transformed_bounds(const Aabb& model, const Mat4& matrix) noexcept {
    Aabb result = Aabb::empty();
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 local{(corner & 1U) != 0U ? model.max.x : model.min.x,
                         (corner & 2U) != 0U ? model.max.y : model.min.y,
                         (corner & 4U) != 0U ? model.max.z : model.min.z};
        const Vec3 world = point(matrix, local);
        result.min = Vec3{std::min(result.min.x, world.x), std::min(result.min.y, world.y),
                          std::min(result.min.z, world.z)};
        result.max = Vec3{std::max(result.max.x, world.x), std::max(result.max.y, world.y),
                          std::max(result.max.z, world.z)};
    }
    return result;
}

f32 radius_of(const Aabb& bounds) noexcept {
    return length((bounds.max - bounds.min) * 0.5F);
}

InstanceTransform relative_transform(const Mat4& matrix, Vec3 eye) noexcept {
    InstanceTransform transform{};
    for (u32 axis = 0; axis < 3; ++axis) {
        f32* destination = transform.row0;
        if (axis == 1) {
            destination = transform.row1;
        } else if (axis == 2) {
            destination = transform.row2;
        }
        for (u32 column = 0; column < 4; ++column) {
            destination[column] = matrix.at(axis, column);
        }
    }
    transform.row0[3] -= eye.x;
    transform.row1[3] -= eye.y;
    transform.row2[3] -= eye.z;
    transform.tint[0] = 1.0F;
    transform.tint[1] = 1.0F;
    transform.tint[2] = 1.0F;
    transform.tint[3] = 1.0F;
    return transform;
}

}  // namespace

struct AuthoredFrame::Mesh {
    std::string reference;
    import::MeshData data;
    Aabb bounds = Aabb::empty();
    u32 first_index = 0;
    u32 index_count = 0;
    i32 vertex_offset = 0;
};

struct AuthoredFrame::Instance {
    u64 identity = 0;
    u32 mesh = 0;
    Aabb bounds = Aabb::empty();
    std::vector<u32> materials;
};

struct AuthoredFrame::Readback {
    ResourceId output = kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

AuthoredFrame::AuthoredFrame(Allocator& allocator, rhi::Device& device) noexcept
    : allocator_(&allocator),
      device_(&device),
      assembly_(allocator),
      index_(allocator),
      graph_(allocator),
      material_program_(allocator),
      texture_server_(allocator),
      transforms_(allocator),
      lights_(allocator),
      pixels_(allocator) {}

AuthoredFrame::~AuthoredFrame() {
    (void)device_->wait_idle();
    texture_table_.shutdown();
    texture_server_.shutdown();
    release_geometry();
    if (!readback_.is_null()) {
        device_->destroy_buffer(readback_);
    }
    if (!output_.is_null()) {
        device_->destroy_texture(output_);
    }
    bindings_.shutdown();
    pipelines_.shutdown();
}

Status AuthoredFrame::initialize(u32 width, u32 height, const char* project) noexcept {
    if (initialized_ || width == 0 || height == 0 || project == nullptr) {
        return fail(ErrorCode::InvalidArgument, "authored frame: invalid initialization");
    }
    width_ = width;
    height_ = height;
    project_ = project;

    AssemblyDescription description;
    description.width = width;
    description.height = height;
    description.near_plane = 0.1F;
    description.far_plane = 10000.0F;
    description.clusters = ClusterGridConfig{16, 12, 16};
    description.material_capacity = kMaterialCapacity;
    description.max_draws = kCapacity * 16U;
    description.max_instances = kCapacity;
    description.gpu_culling = false;
    description.post.temporal_antialiasing = true;
    if (Status status = assembly_.initialize(description); !status) {
        return status;
    }
    if (Status status = assembly_.attach_device(*device_); !status) {
        return status;
    }

    PipelineSetup setup;
    setup.color_format = description.color_format;
    setup.depth_format = description.depth_format;
    setup.output_format = kOutputFormat;
    setup.prepass_normal = true;
    setup.prepass_velocity = true;
    if (Status status = pipelines_.initialize(*device_, setup); !status) {
        return status;
    }
    Expected<ClusterGrid, Error> grid = make_cluster_grid(
        description.clusters, width, height, description.near_plane, description.far_plane);
    if (!grid) {
        return make_unexpected(grid.error());
    }
    const BindingCapacity capacity =
        BindingCapacity::for_grid(*grid, kCapacity, kCapacity * 16U, kMaterialCapacity, 16);
    if (Status status = bindings_.initialize(*device_, pipelines_, capacity); !status) {
        return status;
    }
    if (Status status = recorder_.initialize(pipelines_, bindings_); !status) {
        return status;
    }
    if (Status status = create_materials(); !status) {
        return status;
    }
    render::RenderServerConfig texture_config;
    texture_config.debug_primitive_capacity = 16;
    texture_config.debug_label_capacity = 4;
    if (Status status = texture_server_.configure(texture_config); !status) {
        return status;
    }
    if (Status status = texture_server_.initialize(); !status) {
        return status;
    }
    rhi::SamplerDescription texture_sampler;
    texture_sampler.name = "editor authored material textures";
    if (Status status = texture_table_.initialize(*device_, *allocator_, texture_sampler);
        !status) {
        return status;
    }
    if (Status status = upload_geometry(); !status) {
        return status;
    }

    rhi::BufferDescription readback;
    readback.name = "editor authored frame readback";
    readback.size = u64{width} * height * sizeof(u32);
    readback.usage = rhi::BufferUsage::TransferDestination;
    readback.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(readback);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    readback_ = *buffer;

    rhi::TextureDescription output;
    output.name = "editor authored frame output";
    output.format = kOutputFormat;
    output.extent = rhi::Extent3D{width, height, 1};
    output.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> texture = device_->create_texture(output);
    if (!texture) {
        return make_unexpected(texture.error());
    }
    output_ = *texture;

    initialized_ = true;
    return ok();
}

Status AuthoredFrame::create_materials() noexcept {
    if (Status status =
            describe_standard_material(material_program_, Name::intern("standard"),
                                       render::ShadingModel::Lit, render::BlendMode::Opaque);
        !status) {
        return status;
    }
    const StandardParameters ids;
    const ParameterId wanted[4] = {ids.base_color_factor, ids.roughness_factor, ids.metallic_factor,
                                   ids.emission_color};
    for (u32 index = 0; index < 4; ++index) {
        const MaterialParameter* parameter = material_program_.find(wanted[index]);
        if (parameter == nullptr) {
            return fail(ErrorCode::NotFound, "authored frame: material parameter missing");
        }
        material_offsets_[index] = parameter->offset / 4U;
    }
    const MaterialParameter* texture = material_program_.find(ids.base_color_texture);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "authored frame: base-colour texture parameter missing");
    }
    base_color_texture_offset_ = texture->offset / 4U;
    MaterialTable& table = assembly_.materials();
    for (u32 slot = 0; slot < kMaterialCapacity; ++slot) {
        Expected<u32, Error> allocated = table.allocate();
        if (!allocated) {
            return make_unexpected(allocated.error());
        }
        if (Status status = apply_standard_defaults(material_program_, table, *allocated, ids);
            !status) {
            return status;
        }
    }
    return ok();
}

Expected<u32, Error> AuthoredFrame::material_slot(const std::string& reference) noexcept {
    if (reference.empty()) {
        return 0U;
    }
    const auto found = std::ranges::find_if(
        material_slots_, [&](const auto& entry) { return entry.first == reference; });
    if (found != material_slots_.end()) {
        return found->second;
    }
    if (material_slots_.size() + 1 >= kMaterialCapacity) {
        return fail(ErrorCode::OutOfRange, "authored frame: material capacity exceeded");
    }
    const std::string path = project_ + "/.cy/cooked/" + reference + ".cyasset";
    Array<u8> cooked(*allocator_);
    if (Status status = assets::fs::read_whole(path.c_str(), cooked); !status) {
        return make_unexpected(status.error());
    }
    Expected<Span<const u8>, Error> payload =
        assets::read_cooked_payload(cooked.data(), cooked.size(), true);
    if (!payload) {
        return make_unexpected(payload.error());
    }
    import::StandardMaterial material;
    if (Status status = import::read_cooked_material(*payload, material); !status) {
        return make_unexpected(status.error());
    }
    const u32 slot = static_cast<u32>(material_slots_.size() + 1);
    MaterialTable& table = assembly_.materials();
    const StandardParameters ids;
    if (Status status = table.set_color(material_program_, slot, ids.base_color_factor,
                                        Vec4{material.base_colour[0], material.base_colour[1],
                                             material.base_colour[2], material.base_colour[3]});
        !status) {
        return make_unexpected(status.error());
    }
    if (Status status =
            table.set_float(material_program_, slot, ids.roughness_factor, material.roughness);
        !status) {
        return make_unexpected(status.error());
    }
    if (Status status =
            table.set_float(material_program_, slot, ids.metallic_factor, material.metallic);
        !status) {
        return make_unexpected(status.error());
    }
    if (Status status = table.set_color(
            material_program_, slot, ids.emission_color,
            Vec4{material.emissive[0], material.emissive[1], material.emissive[2], 1.0F});
        !status) {
        return make_unexpected(status.error());
    }
    if (!material.base_color_texture.is_nil()) {
        Expected<rhi::BindlessIndex, Error> texture = texture_slot(material.base_color_texture);
        if (!texture) {
            return make_unexpected(texture.error());
        }
        if (Status status =
                table.set_texture(material_program_, slot, ids.base_color_texture, *texture);
            !status) {
            return make_unexpected(status.error());
        }
    }
    material_slots_.emplace_back(reference, slot);
    return slot;
}

Expected<rhi::BindlessIndex, Error> AuthoredFrame::texture_slot(AssetId identity) noexcept {
    for (const auto& entry : texture_handles_) {
        if (entry.first == identity) {
            return texture_table_.slot_of(entry.second);
        }
    }
    char id_text[AssetId::kTextLength + 1] = {};
    (void)identity.format(id_text);
    const std::string path = project_ + "/.cy/cooked/" + id_text + ".cyasset";
    Array<u8> cooked(*allocator_);
    if (Status status = assets::fs::read_whole(path.c_str(), cooked); !status) {
        return make_unexpected(status.error());
    }
    Expected<Span<const u8>, Error> payload =
        assets::read_cooked_payload(cooked.data(), cooked.size(), true);
    if (!payload) {
        return make_unexpected(payload.error());
    }
    if (payload->size() < 20 || read_u32(payload->data()) != import::CookedTexture::kVersion) {
        return fail(ErrorCode::InvalidArgument, "authored frame: invalid cooked texture header");
    }
    const auto format = static_cast<import::TextureFormat>(static_cast<u16>((*payload)[4]) |
                                                           (static_cast<u16>((*payload)[5]) << 8U));
    Expected<render::TextureFormat, Error> device_format =
        cooked_texture_format(format, (*payload)[6] != 0, (*payload)[7] != 0);
    if (!device_format) {
        return make_unexpected(device_format.error());
    }
    render::TextureRecord record;
    record.name = Name::intern(id_text);
    record.format = *device_format;
    record.usage_class = render::TextureUsageClass::Color;
    record.width = read_u32(payload->data() + 8);
    record.height = read_u32(payload->data() + 12);
    record.mip_levels = static_cast<u16>(read_u32(payload->data() + 16));
    if (record.width == 0 || record.height == 0 || record.mip_levels == 0) {
        return fail(ErrorCode::InvalidArgument, "authored frame: invalid cooked texture extent");
    }
    Expected<render::TextureHandle, Error> handle = texture_server_.create_texture(record);
    if (!handle) {
        return make_unexpected(handle.error());
    }
    const render::TextureRecord* stored = texture_server_.texture(*handle);
    const Span<const u8> pixels(payload->data() + 20, payload->size() - 20);
    if (stored == nullptr || stored->bytes != pixels.size()) {
        texture_server_.destroy_texture(*handle);
        return fail(ErrorCode::InvalidArgument, "authored frame: cooked texture mip size mismatch");
    }
    const TextureUpload upload{*handle, pixels};
    if (Status status =
            texture_table_.upload(texture_server_, Span<const TextureUpload>(&upload, 1));
        !status) {
        texture_server_.destroy_texture(*handle);
        return make_unexpected(status.error());
    }
    texture_handles_.emplace_back(identity, *handle);
    return texture_table_.slot_of(*handle);
}

Status AuthoredFrame::load_mesh(const std::string& reference) noexcept {
    auto mesh = std::make_unique<Mesh>();
    mesh->reference = reference;
    if (reference.ends_with(".cyprim")) {
        Array<u8> source(*allocator_);
        const std::string path = project_ + "/" + reference;
        if (Status status = assets::fs::read_whole(path.c_str(), source); !status) {
            return status;
        }
        const std::string_view text(reinterpret_cast<const char*>(source.data()), source.size());
        Expected<import::PrimitiveSpec, Error> spec = import::parse_primitive_source(text);
        if (!spec) {
            return make_unexpected(spec.error());
        }
        if (Status status = import::build_primitive_mesh(*spec, mesh->data); !status) {
            return status;
        }
    } else {
        const std::string path = project_ + "/.cy/cooked/" + reference + ".cyasset";
        Array<u8> cooked(*allocator_);
        if (Status status = assets::fs::read_whole(path.c_str(), cooked); !status) {
            return status;
        }
        Expected<Span<const u8>, Error> payload =
            assets::read_cooked_payload(cooked.data(), cooked.size(), true);
        if (!payload) {
            return make_unexpected(payload.error());
        }
        if (Status status = import::read_cooked_mesh(*payload, mesh->data); !status) {
            return status;
        }
    }
    if (Status status = mesh->data.validate(); !status) {
        return status;
    }
    mesh->bounds = mesh->data.bounds();
    meshes_.push_back(std::move(mesh));
    return ok();
}

Status AuthoredFrame::resolve_meshes(const ser::World& world) noexcept {
    bool added = false;
    for (const ser::WorldNode& node : world.nodes()) {
        if (!node.live) {
            continue;
        }
        const std::string reference = mesh_reference(world, node);
        if (reference.empty()) {
            continue;
        }
        const auto found = std::ranges::find_if(
            meshes_, [&](const auto& mesh) { return mesh->reference == reference; });
        if (found != meshes_.end()) {
            continue;
        }
        if (Status status = load_mesh(reference); !status) {
            std::fprintf(stderr, "editor authored mesh '%s': %s\n", reference.c_str(),
                         status.error().message);
            return status;
        }
        added = true;
    }
    return added ? upload_geometry() : ok();
}

Status AuthoredFrame::prepare_world(const ser::World& world) noexcept {
    if (Status status = resolve_meshes(world); !status) {
        return status;
    }
    return build_instances(world, Vec3{});
}

void AuthoredFrame::release_geometry() noexcept {
    for (rhi::BufferHandle* buffer : {&positions_, &normals_, &uvs_, &indices_}) {
        if (!buffer->is_null()) {
            device_->destroy_buffer(*buffer);
            *buffer = {};
        }
    }
}

Status AuthoredFrame::upload_geometry() noexcept {
    Array<f32> positions(*allocator_);
    Array<u16> normals(*allocator_);
    Array<f32> uvs(*allocator_);
    Array<u32> indices(*allocator_);
    for (const auto& mesh : meshes_) {
        mesh->first_index = static_cast<u32>(indices.size());
        mesh->index_count = static_cast<u32>(mesh->data.indices.size());
        mesh->vertex_offset = static_cast<i32>(positions.size() / 3U);
        for (usize vertex = 0; vertex < mesh->data.positions.size(); ++vertex) {
            const Vec3 p = mesh->data.positions[vertex];
            if (Status status = positions.push_back(p.x); !status) {
                return status;
            }
            if (Status status = positions.push_back(p.y); !status) {
                return status;
            }
            if (Status status = positions.push_back(p.z); !status) {
                return status;
            }
            const Vec3 normal =
                mesh->data.normals.empty() ? Vec3{0, 1, 0} : mesh->data.normals[vertex];
            const Vec3 tangent =
                mesh->data.tangents.empty() ? Vec3{1, 0, 0} : mesh->data.tangents[vertex].xyz();
            u16 packed[4];
            pack_normal_stream(normal, tangent, packed);
            for (u16 lane : packed) {
                if (Status status = normals.push_back(lane); !status) {
                    return status;
                }
            }
            const Vec2 uv = mesh->data.uvs.empty() ? Vec2{} : mesh->data.uvs[vertex];
            if (Status status = uvs.push_back(uv.x); !status) {
                return status;
            }
            if (Status status = uvs.push_back(uv.y); !status) {
                return status;
            }
        }
        for (u32 index : mesh->data.indices) {
            if (Status status = indices.push_back(index); !status) {
                return status;
            }
        }
    }
    (void)device_->wait_idle();
    release_geometry();
    struct Request {
        const char* name;
        const void* data;
        u64 bytes;
        rhi::BufferUsage usage;
        rhi::BufferHandle* out;
    };
    const Request requests[] = {
        {"editor positions", positions.data(), positions.size() * sizeof(f32),
         rhi::BufferUsage::Vertex, &positions_},
        {"editor normals", normals.data(), normals.size() * sizeof(u16), rhi::BufferUsage::Vertex,
         &normals_},
        {"editor uvs", uvs.data(), uvs.size() * sizeof(f32), rhi::BufferUsage::Vertex, &uvs_},
        {"editor indices", indices.data(), indices.size() * sizeof(u32), rhi::BufferUsage::Index,
         &indices_},
    };
    for (const Request& request : requests) {
        Expected<rhi::BufferHandle, Error> buffer =
            upload(*device_, request.name, request.data, request.bytes, request.usage);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        *request.out = *buffer;
    }
    GeometrySource source;
    source.streams[kPositionStream] = positions_;
    source.streams[kNormalStream] = normals_;
    source.streams[kUvStream] = uvs_;
    source.geometry = &AuthoredFrame::geometry;
    source.user = this;
    recorder_.set_geometry(source);
    return ok();
}

Status AuthoredFrame::build_instances(const ser::World& world, Vec3 eye,
                                      bool editor_lighting) noexcept {
    index_.reset();
    instances_.clear();
    pivots_.clear();
    light_markers_.clear();
    camera_markers_.clear();
    transforms_.clear();
    lights_.clear();
    bool authored_light_present = false;
    const Span<const ser::WorldNode> nodes = world.nodes().span();
    Array<Mat4> matrices(*allocator_);
    if (Status status = matrices.resize(nodes.size()); !status) {
        return status;
    }
    for (usize row = 0; row < nodes.size(); ++row) {
        const ser::WorldNode& node = nodes[row];
        Transform local;
        (void)ser::transform_of(world, node, local);
        matrices[row] = local.to_matrix();
        if (node.parent < row && nodes[node.parent].live) {
            matrices[row] = matrices[node.parent] * matrices[row];
        }
        if (node.live) {
            const bool light_source = has_light_source(world, node);
            authored_light_present |= light_source;
            pivots_.emplace_back(node.identity, point(matrices[row], Vec3{}));
            if (!append_light(world, node, matrices[row], lights_)) {
                return fail(ErrorCode::OutOfMemory, "authored frame: cannot append light");
            }
            if (light_source) {
                const ser::WorldValue* kind = field_value(world, node, "LightSource", "kind");
                if (kind != nullptr && kind->kind == ser::WorldValueKind::Int &&
                    kind->integer >= 0 &&
                    kind->integer < static_cast<i64>(render::LightKind::Count)) {
                    const Vec3 origin = point(matrices[row], Vec3{});
                    const Vec3 forward = normalize(point(matrices[row], Vec3{0, 0, -1}) - origin);
                    light_markers_.push_back(
                        LightMarker{node.identity, static_cast<render::LightKind>(kind->integer),
                                    origin, forward});
                }
            }
            if (field_value(world, node, "Camera", "projection.fov_y") != nullptr) {
                const Vec3 origin = point(matrices[row], Vec3{});
                const Vec3 forward = normalize(point(matrices[row], Vec3{0, 0, -1}) - origin);
                camera_markers_.push_back(CameraMarker{node.identity, origin, forward});
            }
            if (Status status = append_instance(world, node, matrices[row], eye); !status) {
                return status;
            }
        }
    }
    if (lights_.empty() && editor_lighting && !authored_light_present) {
        render::LightDescription preview;
        preview.kind = render::LightKind::Directional;
        preview.intensity = 22000.0F;
        preview.transform.rotation =
            Quat::look_rotation(normalize(Vec3{-0.28F, -0.82F, -0.50F}), Vec3{0, 1, 0});
        preview.color[1] = 0.96F;
        preview.color[2] = 0.88F;
        preview.stable_id = 1;
        if (Status status = lights_.push_back(preview); !status) {
            return status;
        }
    }
    return ok();
}

Status AuthoredFrame::append_instance(const ser::World& world, const ser::WorldNode& node,
                                      const Mat4& matrix, Vec3 eye) noexcept {
    const std::string reference = mesh_reference(world, node);
    if (reference.empty()) {
        return ok();
    }
    const auto found = std::ranges::find_if(
        meshes_, [&](const auto& mesh) { return mesh->reference == reference; });
    if (found == meshes_.end()) {
        return ok();
    }
    if (instances_.size() >= kCapacity) {
        return fail(ErrorCode::OutOfRange, "authored frame: more than 4096 mesh instances");
    }

    const Aabb bounds = transformed_bounds((*found)->bounds, matrix);
    Instance instance;
    instance.identity = node.identity;
    instance.mesh = static_cast<u32>(found - meshes_.begin());
    instance.bounds = bounds;
    const std::string primary = field_reference(world, node, "MeshRenderer", "material");
    const usize sections = std::max<usize>((*found)->data.sections.size(), 1);
    for (usize section = 0; section < sections; ++section) {
        const u32 material_index =
            (*found)->data.sections.empty() ? 0U : (*found)->data.sections[section].material;
        std::string material_reference = field_reference(world, node, "ImportedMaterialSlots",
                                                         "slot_" + std::to_string(material_index));
        if (material_reference.empty() && material_index == 0) {
            material_reference = primary;
        }
        Expected<u32, Error> material = material_slot(material_reference);
        if (!material) {
            return make_unexpected(material.error());
        }
        instance.materials.push_back(*material);
    }
    SpatialEntry entry;
    entry.bounds = bounds;
    entry.stable_id = node.identity;
    entry.gpu_slot = static_cast<u32>(instances_.size());
    entry.radius = radius_of(bounds);
    if (Expected<u32, Error> inserted = index_.insert(entry); !inserted) {
        return make_unexpected(inserted.error());
    }
    instances_.push_back(std::move(instance));
    return transforms_.push_back(relative_transform(matrix, eye));
}

Span<const DrawSurface> AuthoredFrame::surfaces(const VisibleInstance& instance,
                                                void* user) noexcept {
    static thread_local std::vector<DrawSurface> surfaces;
    const auto& frame = *static_cast<const AuthoredFrame*>(user);
    if (instance.gpu_slot >= frame.instances_.size()) {
        return {};
    }
    const Instance& placed = frame.instances_[instance.gpu_slot];
    surfaces.resize(placed.materials.size());
    for (usize section = 0; section < surfaces.size(); ++section) {
        DrawSurface& surface = surfaces[section];
        surface = DrawSurface{};
        surface.mesh = placed.mesh + 1U;
        surface.material = placed.materials[section];
        surface.surface = static_cast<u32>(section);
        surface.blend = render::BlendMode::Opaque;
    }
    return {surfaces.data(), surfaces.size()};
}

bool AuthoredFrame::geometry(const render::DrawItem& item, const GpuDrawInstance& instance,
                             void* user, DrawGeometry& out) noexcept {
    const auto& frame = *static_cast<const AuthoredFrame*>(user);
    if (instance.instance_slot >= frame.instances_.size()) {
        return false;
    }
    const Mesh& mesh = *frame.meshes_[frame.instances_[instance.instance_slot].mesh];
    out.indices = frame.indices_;
    out.wide_indices = true;
    out.first_index = mesh.first_index;
    out.index_count = mesh.index_count;
    out.vertex_offset = mesh.vertex_offset;
    if (!mesh.data.sections.empty()) {
        if (item.surface >= mesh.data.sections.size()) {
            return false;
        }
        const import::MeshSection& section = mesh.data.sections[item.surface];
        out.first_index += section.first_index;
        out.index_count = section.index_count;
    }
    return true;
}

void AuthoredFrame::readback(const PassContext& context, void* user) noexcept {
    const auto& read = *static_cast<const Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{read.width, read.height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(read.output), read.buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

Status AuthoredFrame::render(const ser::World& world, const first_light::Camera& camera,
                             bool editor_lighting) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "authored frame: not initialized");
    }
    if (Status status = resolve_meshes(world); !status) {
        return status;
    }
    const Vec3 eye{static_cast<f32>(camera.position[0]), static_cast<f32>(camera.position[1]),
                   static_cast<f32>(camera.position[2])};
    if (Status status = build_instances(world, eye, editor_lighting); !status) {
        return status;
    }
    MaterialTextureSlot resident[kMaterialTextureSlots];
    const usize count =
        texture_table_.slots(Span<MaterialTextureSlot>(resident, kMaterialTextureSlots));
    if (count > kMaterialTextureSlots) {
        return fail(ErrorCode::OutOfRange, "authored frame: too many resident material textures");
    }
    if (Status status =
            bindings_.set_material_textures(Span<const MaterialTextureSlot>(resident, count));
        !status) {
        return status;
    }
    Expected<u32, Error> begun = device_->begin_frame();
    if (!begun) {
        return make_unexpected(begun.error());
    }
    Status result = capture(*begun, camera, editor_lighting);
    if (Status ended = device_->end_frame(); !ended && result) {
        return ended;
    }
    return result;
}

Status AuthoredFrame::publish(const first_light::Camera& camera,
                              Array<render::GpuInstance>& instances,
                              Array<render::DrawItem>& draws) const noexcept {
    instances.clear();
    draws.clear();
    const Vec3 eye{static_cast<f32>(camera.position[0]), static_cast<f32>(camera.position[1]),
                   static_cast<f32>(camera.position[2])};
    for (const Instance& visible : instances_) {
        const Vec3 centre = (visible.bounds.min + visible.bounds.max) * 0.5F - eye;
        render::GpuInstance record;
        record.bounds_center[0] = centre.x;
        record.bounds_center[1] = centre.y;
        record.bounds_center[2] = centre.z;
        record.bounds_radius = radius_of(visible.bounds);
        record.set_stable_id(visible.identity);
        record.flags = render::kInstanceActive;
        record.layer_mask = 0xFFFF'FFFFU;
        const u32 slot = static_cast<u32>(instances.size());
        if (Status status = instances.push_back(record); !status) {
            return status;
        }
        render::DrawKeyInputs key;
        key.layer = render::SortLayer::Opaque;
        key.view_depth = length(centre);
        render::DrawItem item;
        item.key = render::make_sort_key(key);
        item.stable_id = visible.identity;
        item.instance_slot = slot;
        if (Status status = draws.push_back(item); !status) {
            return status;
        }
    }
    render::sort_draws(draws.span());
    return ok();
}

bool AuthoredFrame::pivot_for(u64 identity, Vec3& pivot) const noexcept {
    for (const auto& entry : pivots_) {
        if (entry.first == identity) {
            pivot = entry.second;
            return true;
        }
    }
    return false;
}

bool AuthoredFrame::scene_camera(const ser::World& world, u64 identity,
                                 first_light::Camera& camera) const noexcept {
    const Span<const ser::WorldNode> nodes = world.nodes().span();
    Array<Mat4> matrices(*allocator_);
    if (Status status = matrices.resize(nodes.size()); !status) {
        return false;
    }
    for (usize row = 0; row < nodes.size(); ++row) {
        const ser::WorldNode& node = nodes[row];
        Transform local;
        (void)ser::transform_of(world, node, local);
        matrices[row] = local.to_matrix();
        if (node.parent < row && nodes[node.parent].live) {
            matrices[row] = matrices[node.parent] * matrices[row];
        }
        const ser::WorldValue* enabled = field_value(world, node, "Camera", "enabled");
        const ser::WorldValue* fov = field_value(world, node, "Camera", "projection.fov_y");
        if (!node.live || fov == nullptr || fov->kind != ser::WorldValueKind::Float ||
            (enabled != nullptr && enabled->kind == ser::WorldValueKind::Bool &&
             enabled->integer == 0) ||
            (identity != 0 && identity != node.identity)) {
            continue;
        }
        const Vec3 origin = point(matrices[row], Vec3{});
        const Vec3 forward = normalize(point(matrices[row], Vec3{0.0F, 0.0F, -1.0F}) - origin);
        const Vec3 up = normalize(point(matrices[row], Vec3{0.0F, 1.0F, 0.0F}) - origin);
        camera.position[0] = static_cast<f64>(origin.x);
        camera.position[1] = static_cast<f64>(origin.y);
        camera.position[2] = static_cast<f64>(origin.z);
        camera.forward = forward;
        camera.up = up;
        camera.fov_y_radians = fov->lanes[0];
        const ser::WorldValue* near = field_value(world, node, "Camera", "projection.near");
        camera.near_plane =
            near != nullptr && near->kind == ser::WorldValueKind::Float ? near->lanes[0] : 0.1F;
        return true;
    }
    return false;
}

first_light::Camera AuthoredFrame::framing(const first_light::Camera& fallback) const noexcept {
    if (instances_.empty()) {
        return fallback;
    }
    Aabb bounds = Aabb::empty();
    for (const Instance& visible : instances_) {
        bounds.min = Vec3{std::min(bounds.min.x, visible.bounds.min.x),
                          std::min(bounds.min.y, visible.bounds.min.y),
                          std::min(bounds.min.z, visible.bounds.min.z)};
        bounds.max = Vec3{std::max(bounds.max.x, visible.bounds.max.x),
                          std::max(bounds.max.y, visible.bounds.max.y),
                          std::max(bounds.max.z, visible.bounds.max.z)};
    }
    const Vec3 centre = (bounds.min + bounds.max) * 0.5F;
    const Vec3 size = bounds.max - bounds.min;
    const f32 extent = std::max({size.x, size.y, size.z, 1.0F});
    const f32 distance = (extent * 1.1F) + 1.0F;
    first_light::Camera camera = fallback;
    camera.position[0] = static_cast<f64>(centre.x + (distance * 0.65F));
    camera.position[1] = static_cast<f64>(centre.y + (distance * 0.55F));
    camera.position[2] = static_cast<f64>(centre.z + (distance * 0.65F));
    camera.forward = normalize(centre - Vec3{static_cast<f32>(camera.position[0]),
                                             static_cast<f32>(camera.position[1]),
                                             static_cast<f32>(camera.position[2])});
    return camera;
}

Status AuthoredFrame::capture(u32 slot, const first_light::Camera& camera,
                              bool editor_lighting) noexcept {
    graph_.reset();
    const Vec3 eye{static_cast<f32>(camera.position[0]), static_cast<f32>(camera.position[1]),
                   static_cast<f32>(camera.position[2])};
    const Vec3 forward = camera.forward;
    const Mat4 view_matrix = look_at(eye, eye + forward, camera.up);
    const Mat4 relative_view = look_at(Vec3{}, forward, camera.up);
    const f32 fov = camera.fov_y_radians > 0.0F ? camera.fov_y_radians : 0.9F;
    const f32 near_plane = camera.near_plane > 0.0F ? camera.near_plane : 0.1F;
    const Mat4 projection = perspective_reversed_z(
        fov, static_cast<f32>(width_) / static_cast<f32>(height_), near_plane, 10000.0F);
    AssemblyView view;
    view.fov_y_radians = fov;
    view.view = view_matrix;
    view.projection = projection;
    view.cull.frustum = Frustum::from_view_projection(projection * view_matrix);
    view.cull.camera_position = eye;
    view.cull.camera_forward = forward;
    view.cull.fov_y_radians = view.fov_y_radians;
    view.lights = lights_.span();
    view.sun_direction = Vec3{0.28F, 0.82F, 0.50F};
    // Edits can insert geometry into a previously empty history. Reset it for the editor frame.
    view.cut = true;
    TextureRequest output;
    output.name = "editor authored output";
    output.format = kOutputFormat;
    output.width = width_;
    output.height = height_;
    output.extra_usage = rhi::TextureUsage::TransferSource;
    view.output = graph_.import_texture(output, output_, rhi::ImageUse::Undefined);
    if (Status status = recorder_.bind(assembly_); !status) {
        return status;
    }
    FrameSinks sinks = recorder_.sinks();
    sinks.surfaces = &AuthoredFrame::surfaces;
    sinks.surfaces_user = this;
    AssemblyReport report;
    if (Status status = assembly_.assemble(index_, view, sinks, graph_, report); !status) {
        return status;
    }
    GlobalsData globals;
    globals.exposure_stops = -16.0F;
    FrameUpload data = upload_for(assembly_, report, projection * relative_view, relative_view,
                                  transforms_.span(), globals, material_offsets_);
    // The studio fill belongs to Editor preview only. Game renders the authored lighting without
    // silently adding a light the scene does not contain.
    if (editor_lighting) {
        for (u32 channel = 0; channel < 3; ++channel) {
            data.view.ambient_and_occlusion[channel] += 500.0F;
        }
    }
    if (!texture_handles_.empty()) {
        data.view.material_textures[0] = base_color_texture_offset_;
    }
    if (Status status = bindings_.upload(slot, data); !status) {
        return status;
    }
    Readback read{assembly_.resources().output, readback_, width_, height_};
    BufferRequest request;
    request.name = "editor authored capture";
    request.size = u64{width_} * height_ * sizeof(u32);
    request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId destination = graph_.import_buffer(request, readback_);
    graph_.add_pass("editor capture", rhi::QueueKind::Graphics)
        .read(read.output, rhi::Access::TransferRead)
        .write(destination, rhi::Access::TransferWrite)
        .record(&AuthoredFrame::readback, &read);
    graph_.add_pass("editor capture host", rhi::QueueKind::Graphics)
        .read(destination, rhi::Access::HostRead)
        .side_effect();
    GraphExecutor executor(*allocator_, *device_);
    Status executed = assembly_.execute(executor, graph_, report);
    if (executed) {
        executed = device_->wait_idle();
    }
    if (executed) {
        if (Status status = pixels_.resize(usize{width_} * height_); !status) {
            executed = status;
        } else {
            const void* mapped = device_->buffer_mapped_pointer(readback_);
            if (mapped == nullptr) {
                executed = fail(ErrorCode::Internal, "authored frame: readback not mapped");
            } else {
                std::memcpy(pixels_.data(), mapped, pixels_.size() * sizeof(u32));
            }
        }
    }
    executor.release();
    return executed;
}

}  // namespace cy::sample::editor_window
