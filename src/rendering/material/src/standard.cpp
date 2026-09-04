// The standard material's slot set and the fallback materials. See
// cy/rendering/material/standard.h.

#include <cy/rendering/material/standard.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

constexpr const char* kFallbackNames[] = {"MissingMaterial", "ShaderCompilationFailed",
                                          "MissingTexture"};
static_assert(sizeof(kFallbackNames) / sizeof(kFallbackNames[0]) ==
              static_cast<usize>(FallbackKind::Count));

/// One row of the slot table. Written as data so the declaration order — which fixes every offset
/// the shader is compiled against — is visible in one place rather than spread over fifty calls.
struct SlotDeclaration {
    const char* name;
    ParameterKind kind;
    bool is_static;
};

constexpr SlotDeclaration kStandardSlots[] = {
    // Runtime parameters, in the order the specification lists the slots.
    {"base_color_factor", ParameterKind::Color, false},
    {"metallic_factor", ParameterKind::Float, false},
    {"roughness_factor", ParameterKind::Float, false},
    {"specular_factor", ParameterKind::Float, false},
    {"normal_scale", ParameterKind::Float, false},
    {"occlusion_strength", ParameterKind::Float, false},
    {"opacity", ParameterKind::Float, false},
    {"alpha_cutoff", ParameterKind::Float, false},
    {"height_scale", ParameterKind::Float, false},
    {"clearcoat", ParameterKind::Float, false},
    {"clearcoat_roughness", ParameterKind::Float, false},
    {"anisotropy", ParameterKind::Float, false},
    {"vertex_color_modulation", ParameterKind::Float, false},
    {"emission_color", ParameterKind::Color, false},
    {"subsurface_color", ParameterKind::Color, false},
    {"anisotropy_direction", ParameterKind::Vec2, false},
    {"detail_uv_scale", ParameterKind::Vec2, false},
    {"distance_fade", ParameterKind::Vec2, false},
    {"proximity_fade", ParameterKind::Vec2, false},
    {"uv_tiling_offset", ParameterKind::Vec4, false},

    // Texture slots: a bindless index each. `orm_texture` is one slot and not three — see the
    // header comment for why that is the difference between one sample and three.
    {"base_color_texture", ParameterKind::Texture, false},
    {"orm_texture", ParameterKind::Texture, false},
    {"normal_texture", ParameterKind::Texture, false},
    {"emission_texture", ParameterKind::Texture, false},
    {"height_texture", ParameterKind::Texture, false},
    {"thickness_texture", ParameterKind::Texture, false},
    {"detail_texture", ParameterKind::Texture, false},
    {"uv_channel_mask", ParameterKind::Int, false},

    // Static parameters. Each doubles the permutation count, which is why there are four.
    {"use_triplanar", ParameterKind::Bool, true},
    {"use_world_space_uv", ParameterKind::Bool, true},
    {"use_detail_layer", ParameterKind::Bool, true},
    {"two_sided", ParameterKind::Bool, true},
};

}  // namespace

Status describe_standard_material(MaterialProgram& program, Name name, ShadingModel model,
                                  BlendMode blend) noexcept {
    if (Status started = program.initialize(name, model, blend); !started) {
        return started;
    }
    for (const SlotDeclaration& slot : kStandardSlots) {
        if (auto declared = program.add_parameter(slot.name, slot.kind, slot.is_static);
            !declared) {
            return Status{make_unexpected(declared.error())};
        }
    }
    return ok();
}

Status apply_standard_defaults(const MaterialProgram& program, MaterialTable& table, u32 index,
                               const StandardParameters& ids) noexcept {
    // A plausible surface rather than a black one: a black default reads as "the lighting is
    // broken" and sends the reader looking in the wrong place.
    if (Status set =
            table.set_color(program, index, ids.base_color_factor, Vec4{0.5F, 0.5F, 0.5F, 1.0F});
        !set) {
        return set;
    }
    struct FloatDefault {
        ParameterId id;
        f32 value;
    };
    const FloatDefault defaults[] = {
        {ids.metallic_factor, 0.0F},
        // Not zero. Perfectly smooth is a delta lobe, and a default material that showed a
        // single-pixel highlight would look like an aliasing bug rather than a default.
        {ids.roughness_factor, 0.5F},
        {ids.specular_factor, 1.0F},
        {ids.normal_scale, 1.0F},
        {ids.occlusion_strength, 1.0F},
        {ids.opacity, 1.0F},
        {ids.alpha_cutoff, 0.5F},
        {ids.height_scale, 0.0F},
        {ids.clearcoat, 0.0F},
        {ids.clearcoat_roughness, 0.1F},
        {ids.anisotropy, 0.0F},
        {ids.vertex_color_modulation, 0.0F},
    };
    for (const FloatDefault& entry : defaults) {
        if (Status set = table.set_float(program, index, entry.id, entry.value); !set) {
            return set;
        }
    }
    if (Status set =
            table.set_color(program, index, ids.emission_color, Vec4{0.0F, 0.0F, 0.0F, 0.0F});
        !set) {
        return set;
    }
    // A tiling of (1, 1) and an offset of (0, 0): the identity, spelled rather than assumed,
    // because a zeroed block would mean a tiling of zero and a material that samples one texel.
    return table.set_vec4(program, index, ids.uv_tiling_offset, Vec4{1.0F, 1.0F, 0.0F, 0.0F});
}

// --- Fallbacks ------------------------------------------------------------------------------

const char* fallback_kind_name(FallbackKind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    return (index < static_cast<usize>(FallbackKind::Count)) ? kFallbackNames[index] : "<invalid>";
}

void fallback_colors(FallbackKind kind, Vec4& out_primary, Vec4& out_secondary) noexcept {
    // Magenta is the primary in all three: it is the specification's word, and it is the one hue
    // that appears in almost no authored content, so a magenta surface is never ambiguous.
    out_primary = Vec4{1.0F, 0.0F, 1.0F, 1.0F};
    switch (kind) {
        case FallbackKind::MissingMaterial:
            out_secondary = Vec4{0.0F, 0.0F, 0.0F, 1.0F};
            return;
        case FallbackKind::ShaderCompilationFailed:
            out_secondary = Vec4{1.0F, 0.0F, 0.0F, 1.0F};
            return;
        case FallbackKind::MissingTexture:
            out_secondary = Vec4{1.0F, 1.0F, 1.0F, 1.0F};
            return;
        case FallbackKind::Count:
            break;
    }
    out_secondary = Vec4{0.0F, 0.0F, 0.0F, 1.0F};
}

Status generate_fallback_texture(FallbackKind kind, u32 size, u32 checker,
                                 Array<u8>& out) noexcept {
    if (size == 0 || checker == 0) {
        return fail(ErrorCode::InvalidArgument, "a checkerboard needs an extent and a square size");
    }
    if (Status sized = out.resize(static_cast<usize>(size) * size * 4U); !sized) {
        return sized;
    }
    Vec4 primary;
    Vec4 secondary;
    fallback_colors(kind, primary, secondary);

    const auto channel = [](f32 value) noexcept {
        // `lround` rather than `+ 0.5` and a truncating cast: the latter rounds a negative value
        // the wrong way and is a documented bug pattern. The clamp above it means nothing negative
        // reaches this, which is exactly why the difference would never have been noticed here and
        // would have been copied to somewhere it matters.
        return static_cast<u8>(std::lround(math::saturate(value) * 255.0F));
    };
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const bool second = (((x / checker) + (y / checker)) & 1U) != 0U;
            const Vec4& colour = second ? secondary : primary;
            const usize texel = ((static_cast<usize>(y) * size) + x) * 4U;
            out[texel + 0] = channel(colour.x);
            out[texel + 1] = channel(colour.y);
            out[texel + 2] = channel(colour.z);
            out[texel + 3] = channel(colour.w);
        }
    }
    return ok();
}

Status describe_fallback_material(MaterialProgram& program, FallbackKind kind) noexcept {
    // Unlit and opaque on purpose. An error material that needed a light to be visible would be
    // invisible in exactly the frames where something is wrong — which is every frame this material
    // is used in.
    if (Status started = program.initialize(Name::intern(fallback_kind_name(kind)),
                                            ShadingModel::Unlit, BlendMode::Opaque);
        !started) {
        return started;
    }
    if (auto declared = program.add_parameter("emission_color", ParameterKind::Color); !declared) {
        return Status{make_unexpected(declared.error())};
    }
    if (auto declared = program.add_parameter("base_color_texture", ParameterKind::Texture);
        !declared) {
        return Status{make_unexpected(declared.error())};
    }
    return ok();
}

}  // namespace cy::rendering
