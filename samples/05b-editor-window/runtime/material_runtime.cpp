// SPDX-License-Identifier: MIT
#include "material_runtime.h"

#include <cy/backends/shader/compiler.h>
#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
#    include <cy/backends/shader/slang/slang_compiler.h>
#endif
#include <cy/backends/shader/source.h>
#include <cy/core/assets/vfs.h>
#include <cy/graph/material/lower_material.h>
#include <cy/graph/text.h>
#include <cy/rendering/material/slang_program.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace cy::sample::editor_window {
namespace {

using rendering::ParameterKind;
using rendering::material::CompiledProgram;
using rendering::material::Node;
using rendering::material::Op;
using rendering::material::PreludeOptions;
using rendering::material::ProgramKind;
using rendering::material::QualityTier;
using rendering::material::ValueType;

[[maybe_unused]] inline constexpr const char* kVertexEntry = "editorMaterialVertex";
[[maybe_unused]] inline constexpr const char* kShadowVertexEntry = "editorMaterialShadowVertex";
[[maybe_unused]] inline constexpr const char* kFragmentEntry = "editorMaterialFragment";

class Writer {
public:
    explicit Writer(Array<char>& output) noexcept : output_(&output) {}

    void text(std::string_view value) noexcept {
        if (status_) {
            status_ = output_->append({value.data(), value.size()});
        }
    }

    void number(u32 value) noexcept {
        char buffer[16] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "%u", value);
        text(buffer);
    }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Array<char>* output_;
    Status status_ = ok();
};

[[nodiscard]] Expected<assets::VirtualPath, Error> shader_root() noexcept {
    return assets::VirtualPath::normalise("shaders");
}

struct StandardLibrary {
    explicit StandardLibrary(Allocator& allocator) noexcept : registry(allocator) {}

    static bool resolve(void* user, std::string_view module_name,
                        shader::SourceUnit& out) noexcept {
        auto* self = static_cast<StandardLibrary*>(user);
        shader::SourceResolver inner = self->registry.resolver();
        if (inner(module_name, out)) {
            return true;
        }
        std::string_view suffix = module_name;
        while (true) {
            const usize dot = suffix.find('.');
            if (dot == std::string_view::npos) {
                return false;
            }
            suffix = suffix.substr(dot + 1);
            if (inner(suffix, out)) {
                return true;
            }
        }
    }

    [[nodiscard]] Status start() noexcept {
        auto mount = assets::DirectoryMount::create(CY_SHADER_STAGE_PARENT,
                                                    assets::MountKind::Project, false);
        if (!mount.has_value()) {
            return make_unexpected(mount.error());
        }
        auto mounted = files.mount_owned(std::move(*mount), 0);
        if (!mounted.has_value()) {
            return make_unexpected(mounted.error());
        }
        auto root = shader_root();
        if (!root.has_value()) {
            return make_unexpected(root.error());
        }
        return registry.start(files, *root);
    }

    [[nodiscard]] shader::SourceResolver resolver() noexcept {
        return shader::SourceResolver{&StandardLibrary::resolve, this};
    }

    assets::VirtualFileSystem files;
    shader::SourceRegistry registry;
};

[[nodiscard]] Status append_attribute_bindings(const CompiledProgram& program, Writer& writer,
                                               std::string_view source) noexcept {
    for (const Node& node : program.module.nodes()) {
        if (node.op == Op::Field) {
            return fail(ErrorCode::Unsupported,
                        "the first-light viewport has no environment-field provider");
        }
        if (node.op != Op::Attribute) {
            continue;
        }
        const rendering::material::VertexInterpolant* interpolant = nullptr;
        for (const auto& candidate : program.module.vertex_interpolants()) {
            if (candidate.name == node.symbol) {
                interpolant = &candidate;
                break;
            }
        }
        if (interpolant != nullptr && source == "output") {
            continue;
        }
        writer.text("    ctx.attributes.");
        writer.text(node.symbol.text());
        if (interpolant != nullptr && node.type == interpolant->type) {
            writer.text(" = ");
            writer.text(source);
            writer.text(".vertex_");
            writer.text(node.symbol.text());
            writer.text(";\n");
        } else if (node.symbol == Name::intern("time_seconds") && node.type == ValueType::Float) {
            writer.text(" = editorFrame.frame.shadowControl.z;\n");
        } else if (node.symbol == Name::intern("position") && node.type == ValueType::Vec3) {
            writer.text(" = ");
            writer.text(source);
            writer.text(".positionRelativeToCamera;\n");
        } else if (node.symbol == Name::intern("object_position") && node.type == ValueType::Vec3) {
            writer.text(" = ");
            writer.text(source);
            writer.text(".objectPosition;\n");
        } else if (node.symbol == Name::intern("normal") && node.type == ValueType::Vec3) {
            writer.text(" = ");
            writer.text(source);
            writer.text(".normal;\n");
        } else if ((node.symbol == Name::intern("uv0") || node.symbol == Name::intern("uv1")) &&
                   node.type == ValueType::Vec2) {
            writer.text(" = ");
            writer.text(source);
            writer.text(".uv;\n");
        } else if (node.symbol == Name::intern("tangent") && node.type == ValueType::Vec4) {
            writer.text(" = float4(1.0, 0.0, 0.0, 1.0);\n");
        } else if (node.symbol == Name::intern("color0") && node.type == ValueType::Vec3) {
            writer.text(" = ");
            writer.text(source);
            writer.text(".color.rgb;\n");
        } else {
            return fail(
                ErrorCode::Unsupported,
                "the material requires a vertex attribute this viewport mesh cannot supply");
        }
    }
    return writer.status();
}

[[nodiscard]] Status assemble_unit(const CompiledProgram& program, Array<char>& unit) noexcept {
    unit.clear();
    Writer writer(unit);
    writer.text("#define CY_MATERIAL_PIXEL_STAGE 1\n");
    writer.text(R"(
import cy.material;
struct EditorMaterialTextureTable
{
    Texture2D<float4> textures[16384];
    SamplerState sampler;
};
[[vk::binding(1, 0)]] ParameterBlock<EditorMaterialTextureTable> editorMaterialTextures;
float4 cyMaterialSampleTexture(uint bindlessIndex, float2 uv)
{
    return editorMaterialTextures.textures[bindlessIndex].Sample(editorMaterialTextures.sampler,
                                                                  uv);
}
float4 cyMaterialSampleTextureLevel(uint bindlessIndex, float2 uv, float level)
{
    return editorMaterialTextures.textures[bindlessIndex].SampleLevel(
        editorMaterialTextures.sampler, uv, level);
}
)");
    if (!writer.status()) {
        return writer.status();
    }
    PreludeOptions prelude;
    prelude.material_set = 1;
    prelude.argument_buffer = true;
    auto report = rendering::material::emit_prelude(program.module, prelude, unit);
    if (!report.has_value()) {
        return make_unexpected(report.error());
    }
    writer.text(std::string_view(program.source.text.data(), program.source.text.size()));
    writer.text(
        std::string_view(program.vertex_source.text.data(), program.vertex_source.text.size()));
    Array<char> generated_entry(program.module.allocator());
    if (Status named = rendering::material::entry_point_name(
            program.module.name(), ProgramKind::Primary, QualityTier::High, generated_entry);
        !named) {
        return named;
    }
    writer.text(R"(
struct EditorFrameConstants
{
    float4 viewProjectionRow0; float4 viewProjectionRow1;
    float4 viewProjectionRow2; float4 viewProjectionRow3;
    float4 lightViewProjectionRow0; float4 lightViewProjectionRow1;
    float4 lightViewProjectionRow2; float4 lightViewProjectionRow3;
    float4 sunDirectionAndBias; float4 sunColorAndAmbient; float4 shadowControl;
};
struct EditorFrameGlobals
{
    ConstantBuffer<EditorFrameConstants> frame;
    Texture2D<float4> albedoTexture; SamplerState albedoSampler;
    Texture2D<float> shadowMap; SamplerComparisonState shadowSampler;
};
[[vk::binding(0, 2)]] ParameterBlock<EditorFrameGlobals> editorFrame;
struct EditorObjectPush
{
    float4 modelRow0; float4 modelRow1; float4 modelRow2; float4 baseColor;
};
[[vk::push_constant]] ConstantBuffer<EditorObjectPush> object;
struct EditorVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 color : COLOR0;
};
struct EditorVertexOutput
{
    float4 clip : SV_Position;
    [[vk::location(0)]] float3 positionRelativeToCamera : TEXCOORD1;
    [[vk::location(1)]] float3 normal : TEXCOORD2;
    [[vk::location(2)]] float2 uv : TEXCOORD3;
    [[vk::location(3)]] float3 objectPosition : TEXCOORD4;
    [[vk::location(4)]] float4 color : COLOR1;
)");
    for (usize index = 0; index < program.module.vertex_interpolants().size(); ++index) {
        const auto& interpolant = program.module.vertex_interpolants()[index];
        writer.text("    [[vk::location(");
        writer.number(static_cast<u32>(index) + 5U);
        writer.text(")]] ");
        writer.text(rendering::material::value_type_name(interpolant.type));
        writer.text(" vertex_");
        writer.text(interpolant.name.text());
        writer.text(" : TEXCOORD");
        writer.number(static_cast<u32>(index) + 5U);
        writer.text(";\n");
    }
    writer.text(R"(};
float3 editorPosition(float3 position)
{
    let point = float4(position, 1.0);
    return float3(dot(object.modelRow0, point), dot(object.modelRow1, point),
                  dot(object.modelRow2, point));
}
EditorVertexOutput editorMaterialVertexBase(EditorVertexInput input)
{
    EditorVertexOutput output;
    output.positionRelativeToCamera = editorPosition(input.position);
    output.objectPosition = input.position;
    output.normal = normalize(float3(dot(object.modelRow0.xyz, input.normal),
                                     dot(object.modelRow1.xyz, input.normal),
                                     dot(object.modelRow2.xyz, input.normal)));
    output.uv = input.uv;
    output.color = input.color;
)");
    if (program.module.vertex_offset() != rendering::material::kInvalidNode ||
        !program.module.vertex_interpolants().empty()) {
        writer.text(
            "    CyMaterialContext ctx;\n"
            "    ctx.params = cyMaterialParameters;\n"
            "    ctx.attributes = cyZeroAttributes();\n");
        if (Status attributes = append_attribute_bindings(program, writer, "output"); !attributes) {
            return attributes;
        }
        writer.text("    let materialVertex = ");
        writer.text({generated_entry.data(), generated_entry.size()});
        writer.text(
            "_vertex(ctx);\n"
            "    output.positionRelativeToCamera += materialVertex.offset;\n");
        for (const auto& interpolant : program.module.vertex_interpolants()) {
            writer.text("    output.vertex_");
            writer.text(interpolant.name.text());
            writer.text(" = materialVertex.");
            writer.text(interpolant.name.text());
            writer.text(";\n");
        }
    }
    writer.text(R"(
    return output;
}
[shader("vertex")]
EditorVertexOutput editorMaterialVertex(EditorVertexInput input)
{
    EditorVertexOutput output = editorMaterialVertexBase(input);
    let point = float4(output.positionRelativeToCamera, 1.0);
    output.clip = float4(dot(editorFrame.frame.viewProjectionRow0, point),
                         dot(editorFrame.frame.viewProjectionRow1, point),
                         dot(editorFrame.frame.viewProjectionRow2, point),
                         dot(editorFrame.frame.viewProjectionRow3, point));
    let keepTextureTable = cyMaterialSampleTextureLevel(0, input.uv, 0.0).x;
    if (input.position.x < -1.0e30)
    {
        output.clip.x += keepTextureTable + float(cyMaterialParameters.cyMaterialSlotCount);
    }
    return output;
}
struct EditorShadowOutput { float4 clip : SV_Position; };
[shader("vertex")]
EditorShadowOutput editorMaterialShadowVertex(EditorVertexInput input)
{
    let position = editorMaterialVertexBase(input).positionRelativeToCamera;
    let point = float4(position, 1.0);
    EditorShadowOutput output;
    output.clip = float4(dot(editorFrame.frame.lightViewProjectionRow0, point),
                         dot(editorFrame.frame.lightViewProjectionRow1, point),
                         dot(editorFrame.frame.lightViewProjectionRow2, point),
                         dot(editorFrame.frame.lightViewProjectionRow3, point));
    return output;
}
[shader("fragment")]
float4 editorMaterialFragment(EditorVertexOutput input) : SV_Target
{
    CyMaterialContext ctx;
    ctx.params = cyMaterialParameters;
    ctx.attributes = cyZeroAttributes();
)");
    if (Status attributes = append_attribute_bindings(program, writer, "input"); !attributes) {
        return attributes;
    }
    writer.text("    CySurface compiled = cyDefaultSurface();\n    ");
    writer.text({generated_entry.data(), generated_entry.size()});
    writer.text(R"((ctx, compiled);
    let surface = cyResolveSurface(compiled);
    let normal = normalize(input.normal);
    let lambert = max(dot(normal, editorFrame.frame.sunDirectionAndBias.xyz), 0.0);
    var lit = surface.albedo * ((editorFrame.frame.sunColorAndAmbient.rgb * lambert) +
                                editorFrame.frame.sunColorAndAmbient.w) + surface.emission;
    let keepTextureTable = cyMaterialSampleTextureLevel(0, input.uv, 0.0).x;
    if (input.clip.x < -1.0e30)
    {
        lit += keepTextureTable;
    }
    let encoded = pow(saturate(lit), 1.0 / 2.2);
    return float4(encoded, surface.opacity);
}
)");
    return writer.status();
}

[[nodiscard]] u64 read_u64_le(const u8* bytes) noexcept {
    u64 value = 0;
    for (usize index = 0; index < 8; ++index) {
        value |= static_cast<u64>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] Status convert_parameter_value(first_light::Renderer& renderer,
                                             ParameterKind expected,
                                             const editor::MaterialParameterUpdate& update,
                                             u8 (&converted)[16], u32& converted_size) noexcept {
    switch (update.kind) {
        case 1: {
            if (expected != ParameterKind::Bool || update.value.size() != 1) {
                break;
            }
            const u32 value = update.value[0] == 0 ? 0U : 1U;
            std::memcpy(converted, &value, sizeof(value));
            converted_size = sizeof(value);
            return ok();
        }
        case 2: {
            if (expected != ParameterKind::Int || update.value.size() != sizeof(i64)) {
                break;
            }
            i64 wide = 0;
            std::memcpy(&wide, update.value.data(), sizeof(wide));
            if (wide < std::numeric_limits<i32>::min() || wide > std::numeric_limits<i32>::max()) {
                return fail(ErrorCode::OutOfRange, "the integer material value does not fit int32");
            }
            const i32 value = static_cast<i32>(wide);
            std::memcpy(converted, &value, sizeof(value));
            converted_size = sizeof(value);
            return ok();
        }
        case 3: {
            if (expected != ParameterKind::Float || update.value.size() != sizeof(f64)) {
                break;
            }
            f64 wide = 0.0;
            std::memcpy(&wide, update.value.data(), sizeof(wide));
            if (!std::isfinite(wide) ||
                std::abs(wide) > static_cast<f64>(std::numeric_limits<f32>::max())) {
                return fail(ErrorCode::OutOfRange,
                            "the floating material value does not fit float32");
            }
            const f32 value = static_cast<f32>(wide);
            std::memcpy(converted, &value, sizeof(value));
            converted_size = sizeof(value);
            return ok();
        }
        case 4:
            if ((expected == ParameterKind::Vec4 || expected == ParameterKind::Color) &&
                update.value.size() == 16) {
                std::memcpy(converted, update.value.data(), 16);
                converted_size = 16;
                return ok();
            }
            break;
        case 5: {
            if (expected != ParameterKind::Texture || update.value.size() < 4) {
                break;
            }
            const u32 length = static_cast<u32>(update.value[0]) |
                               (static_cast<u32>(update.value[1]) << 8U) |
                               (static_cast<u32>(update.value[2]) << 16U) |
                               (static_cast<u32>(update.value[3]) << 24U);
            if (length != update.value.size() - 4) {
                return fail(ErrorCode::InvalidArgument,
                            "the texture parameter identity length is malformed");
            }
            const std::string_view identity(reinterpret_cast<const char*>(update.value.data() + 4),
                                            length);
            if (identity != "builtin://checker") {
                return fail(ErrorCode::NotFound,
                            "the texture asset is not resident in this preview runtime");
            }
            const u32 index = renderer.default_texture_index();
            if (index == rhi::kInvalidBindlessIndex) {
                return fail(ErrorCode::Unsupported,
                            "this Metal device has no global texture table");
            }
            std::memcpy(converted, &index, sizeof(index));
            converted_size = sizeof(index);
            return ok();
        }
        default:
            break;
    }
    return fail(ErrorCode::InvalidArgument,
                "the runtime value type does not match the compiled parameter layout");
}

}  // namespace

Status assemble_material_unit(const CompiledProgram& program, Array<char>& unit) noexcept {
    return assemble_unit(program, unit);
}

Expected<rendering::material::CompiledMaterial, Error> compile_scene_graph_material(
    std::string_view source, Allocator& allocator) noexcept {
    graph::NodeRegistry registry(allocator);
    if (Status registered = graph::material::register_material_nodes(registry); !registered) {
        return make_unexpected(registered.error());
    }
    graph::DiagnosticSink diagnostics(allocator);
    auto parsed = graph::parse_graph(source, &registry, allocator, diagnostics);
    if (!parsed) {
        return make_unexpected(parsed.error());
    }
    if (Status checked = graph::validate(*parsed, registry, nullptr, diagnostics); !checked) {
        return make_unexpected(checked.error());
    }
    if (diagnostics.errors() != 0) {
        return fail(ErrorCode::InvalidArgument, "authored scene vertex graph is invalid");
    }
    rendering::material::MaterialGraph lowered(allocator, Name::intern("editor_scene_material"));
    if (Status status = graph::material::lower_material(*parsed, lowered); !status) {
        return make_unexpected(status.error());
    }
    auto module = rendering::material::lower_graph(lowered, allocator);
    if (!module) {
        return make_unexpected(module.error());
    }
    rendering::material::CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    const auto geometry = rendering::material::GeometrySourceKind::StaticMesh;
    options.geometry_paths = {&geometry, 1};
    auto compiled = rendering::material::compile_material(*module, options, allocator);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    if (compiled->failed()) {
        return fail(ErrorCode::InvalidArgument, "authored scene vertex material did not compile");
    }
    return std::move(*compiled);
}

Status assemble_scene_material_vertex_unit(const CompiledProgram& program, Array<char>& unit,
                                           bool argument_buffer) noexcept {
    if (program.vertex_source.text.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "the material has no compiled vertex-stage expression");
    }
    unit.clear();
    Writer writer(unit);
    writer.text("import cy.frame;\nimport cy.packing;\n");
    PreludeOptions prelude;
    prelude.material_set = 3;
    prelude.argument_buffer = argument_buffer;
    prelude.scene_previous_transform = true;
    auto declared = rendering::material::emit_prelude(program.module, prelude, unit);
    if (!declared.has_value()) {
        return make_unexpected(declared.error());
    }
    writer.text(
        std::string_view(program.vertex_source.text.data(), program.vertex_source.text.size()));
    Array<char> generated_entry(program.module.allocator());
    if (Status named = rendering::material::entry_point_name(
            program.module.name(), ProgramKind::Primary, QualityTier::High, generated_entry);
        !named) {
        return named;
    }
    writer.text(R"(
float sceneMaterialTime()
{
#if defined(CY_FRAME_METAL)
    return cyFrameGlobals.globals.timeSeconds;
#else
    return cyGlobalSet.data.timeSeconds;
#endif
}
float sceneMaterialDelta()
{
#if defined(CY_FRAME_METAL)
    return cyFrameGlobals.globals.deltaSeconds;
#else
    return cyGlobalSet.data.deltaSeconds;
#endif
}
float3 sceneMaterialRelative(CyInstanceTransform instance, float3 modelPosition,
                             float4 packedNormal, float2 uv, float timeSeconds)
{
    let relative = transformToRelative(instance, modelPosition);
    let normal = rotateToRelative(instance, decodeOctahedral(packedNormal.xy));
    CyMaterialContext ctx;
    ctx.params = cyMaterialParameters;
    ctx.attributes = cyZeroAttributes();
)");
    for (const Node& node : program.module.nodes()) {
        if (node.op == Op::Field) {
            return fail(ErrorCode::Unsupported,
                        "the authored scene has no environment-field vertex binding");
        }
        if (node.op != Op::Attribute) {
            continue;
        }
        bool interpolant = false;
        for (const auto& root : program.module.vertex_interpolants()) {
            interpolant |= root.name == node.symbol;
        }
        if (interpolant) {
            continue;
        }
        writer.text("    ctx.attributes.");
        writer.text(node.symbol.text());
        if (node.symbol == Name::intern("time_seconds") && node.type == ValueType::Float) {
            writer.text(" = timeSeconds;\n");
        } else if (node.symbol == Name::intern("position") && node.type == ValueType::Vec3) {
            writer.text(" = relative;\n");
        } else if (node.symbol == Name::intern("object_position") && node.type == ValueType::Vec3) {
            writer.text(" = modelPosition;\n");
        } else if (node.symbol == Name::intern("normal") && node.type == ValueType::Vec3) {
            writer.text(" = normal;\n");
        } else if (node.symbol == Name::intern("uv0") && node.type == ValueType::Vec2) {
            writer.text(" = uv;\n");
        } else {
            return fail(ErrorCode::Unsupported,
                        "the authored scene mesh cannot supply a vertex graph attribute");
        }
    }
    writer.text("    return relative + ");
    writer.text({generated_entry.data(), generated_entry.size()});
    writer.text(R"(_vertex_offset(ctx);
}
[shader("vertex")]
CyForwardVertex cySceneMaterialVertex(float3 modelPosition : POSITION,
                                      float4 packedNormal : NORMAL, float2 uv : TEXCOORD0)
{
    let draw = cyFrameView.drawInstances[cyDraw.drawIndex];
    let instance = cyFrameView.instances[draw.instanceSlot];
    CyForwardVertex output;
    output.relativePosition = sceneMaterialRelative(instance, modelPosition, packedNormal, uv,
                                                    sceneMaterialTime());
    output.position = transformToClip(output.relativePosition);
    output.normal = rotateToRelative(instance, decodeOctahedral(packedNormal.xy));
    output.uv = uv;
    output.drawIndex = cyDraw.drawIndex;
    return output;
}
[shader("vertex")]
CyShadowVertex cySceneMaterialShadowVertex(float3 modelPosition : POSITION,
                                           float4 packedNormal : NORMAL, float2 uv : TEXCOORD0)
{
    let draw = cyFrameView.drawInstances[cyDraw.drawIndex];
    let instance = cyFrameView.instances[draw.instanceSlot];
    CyShadowVertex output;
    output.position = transformToShadowClip(sceneMaterialRelative(
        instance, modelPosition, packedNormal, uv, sceneMaterialTime()));
    return output;
}
[shader("vertex")]
CyDepthVertex cySceneMaterialDepthVertex(float3 modelPosition : POSITION,
                                         float4 packedNormal : NORMAL, float2 uv : TEXCOORD0)
{
    let draw = cyFrameView.drawInstances[cyDraw.drawIndex];
    let instance = cyFrameView.instances[draw.instanceSlot];
    CyInstanceTransform previousInstance = instance;
    previousInstance.row0 = cyMaterialPreviousTransform.row0;
    previousInstance.row1 = cyMaterialPreviousTransform.row1;
    previousInstance.row2 = cyMaterialPreviousTransform.row2;
    let current = sceneMaterialRelative(instance, modelPosition, packedNormal, uv,
                                         sceneMaterialTime());
    let previous = sceneMaterialRelative(previousInstance, modelPosition, packedNormal, uv,
                                         sceneMaterialTime() - sceneMaterialDelta());
    let previousPoint = float4(previous, 1.0);
    CyDepthVertex output;
    output.position = transformToClip(current);
    output.currentClip = output.position;
    output.previousClip = float4(
        dot(cyFrameView.frame.previousRelativeToClipRow0, previousPoint),
        dot(cyFrameView.frame.previousRelativeToClipRow1, previousPoint),
        dot(cyFrameView.frame.previousRelativeToClipRow2, previousPoint),
        dot(cyFrameView.frame.previousRelativeToClipRow3, previousPoint));
    output.normal = rotateToRelative(instance, decodeOctahedral(packedNormal.xy));
    return output;
}
)");
    return writer.status();
}

Expected<SceneMaterialVertexArtefacts, Error> compile_scene_material_vertices(
    const CompiledProgram& program, Allocator& allocator, shader::Target target) noexcept {
#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
    if (target != shader::Target::Msl && target != shader::Target::SpirV) {
        return fail(ErrorCode::Unsupported,
                    "authored scene vertex shaders require an MSL or SPIR-V target");
    }
    Array<char> unit(allocator);
    if (Status assembled =
            assemble_scene_material_vertex_unit(program, unit, target == shader::Target::Msl);
        !assembled) {
        return make_unexpected(assembled.error());
    }
    (void)shader::slang::register_slang_backend();
    StandardLibrary library(allocator);
    if (Status started = library.start(); !started) {
        return make_unexpected(started.error());
    }
    auto published = library.registry.add_generated(Name::intern("editor.scene.material.vertex"),
                                                    Name::intern("material-compiler"),
                                                    {unit.data(), unit.size()});
    if (!published.has_value()) {
        return make_unexpected(published.error());
    }
    shader::CompilerSelection selection;
    auto created = shader::create_compiler(allocator, shader::kSlangBackendName, selection);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    struct CompilerHandle {
        Allocator& allocator;
        shader::ShaderCompiler* compiler;
        ~CompilerHandle() { shader::destroy_compiler(allocator, compiler); }
    } compiler{allocator, *created};
    shader::PermutationSet metal(allocator);
    const u32 values[] = {0, 1};
    if (Status axis = metal.add_axis(Name::intern("CY_FRAME_METAL"),
                                     shader::VariationKind::Preprocessor, values);
        !axis) {
        return make_unexpected(axis.error());
    }
    if (Status axis = metal.add_axis(Name::intern("CY_MATERIAL_METAL_ARGUMENT_BUFFER"),
                                     shader::VariationKind::Preprocessor, values);
        !axis) {
        return make_unexpected(axis.error());
    }
    const u32 enabled[] = {target == shader::Target::Msl ? 1U : 0U,
                           target == shader::Target::Msl ? 1U : 0U};
    auto key = metal.encode(enabled);
    if (!key.has_value()) {
        return make_unexpected(key.error());
    }
    const auto compile_stage =
        [&](const char* entry,
            shader::DiagnosticLog& diagnostics) -> Expected<shader::TargetArtefact, Error> {
        shader::CompileRequest request;
        request.source = *published;
        request.entry_point = Name::intern(entry);
        request.stage = rhi::ShaderStage::Vertex;
        request.resolver = library.resolver();
        request.permutations = &metal;
        request.permutation = *key;
        return compiler.compiler->compile_for(request, target, diagnostics);
    };
    shader::DiagnosticLog visible_diagnostics(allocator);
    auto visible = compile_stage("cySceneMaterialVertex", visible_diagnostics);
    if (!visible.has_value()) {
        for (usize index = 0; index < visible_diagnostics.size(); ++index) {
            std::fprintf(stderr, "scene material visible shader: %s\n",
                         visible_diagnostics.at(index).message);
        }
        return fail(ErrorCode::InvalidArgument,
                    "the scene material visible vertex shader did not compile");
    }
    shader::DiagnosticLog depth_diagnostics(allocator);
    auto depth = compile_stage("cySceneMaterialDepthVertex", depth_diagnostics);
    if (!depth.has_value()) {
        for (usize index = 0; index < depth_diagnostics.size(); ++index) {
            std::fprintf(stderr, "scene material depth shader: %s\n",
                         depth_diagnostics.at(index).message);
        }
        return fail(ErrorCode::InvalidArgument,
                    "the scene material depth vertex shader did not compile");
    }
    shader::DiagnosticLog shadow_diagnostics(allocator);
    auto shadow = compile_stage("cySceneMaterialShadowVertex", shadow_diagnostics);
    if (!shadow.has_value()) {
        for (usize index = 0; index < shadow_diagnostics.size(); ++index) {
            std::fprintf(stderr, "scene material shadow shader: %s\n",
                         shadow_diagnostics.at(index).message);
        }
        return fail(ErrorCode::InvalidArgument,
                    "the scene material shadow vertex shader did not compile");
    }
    SceneMaterialVertexArtefacts result(allocator);
    result.depth = std::move(*depth);
    result.visible = std::move(*visible);
    result.shadow = std::move(*shadow);
    return result;
#else
    (void)program;
    (void)allocator;
    (void)target;
    return fail(ErrorCode::Unsupported,
                "scene material variants require the Slang front end in this editor build");
#endif
}

struct MetalMaterialRuntime::Program {
    struct Slot {
        u32 identity = 0;
        ParameterKind kind = ParameterKind::Float;
        u32 offset = 0;
    };

    u64 artefact = 0;
    Slot slots[rendering::kMaxMaterialParameters] = {};
    u32 slot_count = 0;
    u8 parameters[rendering::kMaterialBlockBytes] = {};
};

struct MetalMaterialRuntime::Preview {
    struct Binding {
        u64 entity = 0;
        u32 object = 0;
        u32 material_slot = 0;
        u64 artefact = 0;
    };

    Preview(Allocator& allocator, u64 preview_identity) noexcept
        : bindings(allocator), identity(preview_identity) {}
    Preview(Preview&&) noexcept = default;
    Preview& operator=(Preview&&) noexcept = default;
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    Array<Binding> bindings;
    u64 identity = 0;
};

MetalMaterialRuntime::MetalMaterialRuntime(Allocator& allocator, first_light::Renderer& renderer,
                                           WorldView& world) noexcept
    : allocator_(&allocator),
      renderer_(&renderer),
      world_(&world),
      programs_(allocator),
      previews_(allocator) {}

MetalMaterialRuntime::~MetalMaterialRuntime() = default;

MetalMaterialRuntime::Program* MetalMaterialRuntime::find_program(u64 artefact) noexcept {
    for (Program& program : programs_) {
        if (program.artefact == artefact) {
            return &program;
        }
    }
    return nullptr;
}

MetalMaterialRuntime::Preview* MetalMaterialRuntime::find_preview(u64 preview) noexcept {
    for (Preview& candidate : previews_) {
        if (candidate.identity == preview) {
            return &candidate;
        }
    }
    return nullptr;
}

Status MetalMaterialRuntime::publish(
    u64 artefact, const rendering::material::CompiledMaterial& material) noexcept {
    if (find_program(artefact) != nullptr) {
        return ok();
    }
    if (Status reserved = programs_.reserve(programs_.size() + 1); !reserved) {
        return reserved;
    }
    const CompiledProgram* primary = material.find(ProgramKind::Primary, QualityTier::High);
    if (primary == nullptr || primary->absent) {
        return fail(ErrorCode::InvalidArgument,
                    "the compiled material has no primary high-quality program");
    }

    Array<char> unit(*allocator_);
    if (Status assembled = assemble_material_unit(*primary, unit); !assembled) {
        // Status owns a copied Error whose message uses static storage. The analyzer follows
        // Writer's Array pointer into `unit`, although that pointer is not part of the Status.
        return assembled;  // NOLINT(clang-analyzer-core.StackAddressEscape)
    }
#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
    (void)shader::slang::register_slang_backend();
    StandardLibrary library(*allocator_);
    if (Status started = library.start(); !started) {
        return started;
    }
    auto published = library.registry.add_generated(Name::intern("editor.material.runtime"),
                                                    Name::intern("material-compiler"),
                                                    {unit.data(), unit.size()});
    if (!published.has_value()) {
        return make_unexpected(published.error());
    }
    shader::CompilerSelection selection;
    auto created = shader::create_compiler(*allocator_, shader::kSlangBackendName, selection);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    struct CompilerHandle {
        Allocator& allocator;
        shader::ShaderCompiler* compiler;
        ~CompilerHandle() { shader::destroy_compiler(allocator, compiler); }
    } compiler{*allocator_, *created};
    shader::PermutationSet metal_permutation(*allocator_);
    const u32 macro_values[] = {0, 1};
    if (Status axis = metal_permutation.add_axis(Name::intern("CY_MATERIAL_METAL_ARGUMENT_BUFFER"),
                                                 shader::VariationKind::Preprocessor, macro_values);
        !axis) {
        return axis;
    }
    const u32 enabled[] = {1};
    auto metal_key = metal_permutation.encode(enabled);
    if (!metal_key.has_value()) {
        return make_unexpected(metal_key.error());
    }
    const auto compile_stage =
        [&](const char* entry, rhi::ShaderStage stage,
            shader::DiagnosticLog& diagnostics) -> Expected<shader::TargetArtefact, Error> {
        shader::CompileRequest request;
        request.source = *published;
        request.entry_point = Name::intern(entry);
        request.stage = stage;
        request.resolver = library.resolver();
        request.permutations = &metal_permutation;
        request.permutation = *metal_key;
        return compiler.compiler->compile_for(request, shader::Target::Msl, diagnostics);
    };
    shader::DiagnosticLog vertex_diagnostics(*allocator_);
    auto vertex = compile_stage(kVertexEntry, rhi::ShaderStage::Vertex, vertex_diagnostics);
    if (!vertex.has_value()) {
        return fail(ErrorCode::InvalidArgument,
                    "the generated material vertex program did not compile to MSL");
    }
    shader::DiagnosticLog shadow_diagnostics(*allocator_);
    auto shadow_vertex =
        compile_stage(kShadowVertexEntry, rhi::ShaderStage::Vertex, shadow_diagnostics);
    if (!shadow_vertex.has_value()) {
        return fail(ErrorCode::InvalidArgument,
                    "the generated material shadow vertex program did not compile to MSL");
    }
    shader::DiagnosticLog fragment_diagnostics(*allocator_);
    auto fragment = compile_stage(kFragmentEntry, rhi::ShaderStage::Fragment, fragment_diagnostics);
    if (!fragment.has_value()) {
        return fail(ErrorCode::InvalidArgument,
                    "the generated material fragment program did not compile to MSL");
    }

    Program program;
    program.artefact = artefact;
    for (const rendering::MaterialParameter& parameter : material.layout().parameters()) {
        Program::Slot& slot = program.slots[program.slot_count++];
        slot.identity = parameter.id;
        slot.kind = parameter.kind;
        slot.offset = parameter.offset;
    }
    for (const rendering::material::ParameterDecl& declared : primary->module.parameters()) {
        const rendering::MaterialParameter* parameter =
            material.layout().find(rendering::parameter_id(declared.name.c_str()));
        if (parameter == nullptr) {
            continue;
        }
        u8* destination = program.parameters + parameter->offset;
        switch (parameter->kind) {
            case ParameterKind::Float:
            case ParameterKind::Vec2:
            case ParameterKind::Vec3:
            case ParameterKind::Vec4:
            case ParameterKind::Color:
                std::memcpy(destination, &declared.default_value,
                            rendering::parameter_byte_size(parameter->kind));
                break;
            case ParameterKind::Int: {
                const i32 value = static_cast<i32>(declared.default_value.mask);
                std::memcpy(destination, &value, sizeof(value));
                break;
            }
            case ParameterKind::Bool: {
                const u32 value = declared.default_value.mask == 0 ? 0U : 1U;
                std::memcpy(destination, &value, sizeof(value));
                break;
            }
            case ParameterKind::Texture:
            case ParameterKind::Count:
                break;
        }
    }
    for (const rendering::material::TextureDecl& texture : primary->module.textures()) {
        const rendering::MaterialParameter* parameter =
            material.layout().find(rendering::parameter_id(texture.name.c_str()));
        if (parameter != nullptr) {
            const u32 index = renderer_->default_texture_index();
            std::memcpy(program.parameters + parameter->offset, &index, sizeof(index));
        }
    }
    if (Status retained =
            renderer_->retain_material(artefact, vertex->bytes(), kVertexEntry, fragment->bytes(),
                                       kFragmentEntry, shadow_vertex->bytes(), kShadowVertexEntry,
                                       {program.parameters, sizeof(program.parameters)});
        !retained) {
        return fail(ErrorCode::InvalidArgument,
                    "the Metal renderer rejected the compiled material pipeline or resources");
    }
    return programs_.push_back(std::move(program));
#else
    (void)material;
    return fail(ErrorCode::Unsupported,
                "this runtime was built without the Slang front end required for live materials");
#endif
}

Status MetalMaterialRuntime::create(u64 preview) noexcept {
    if (preview == 0 || find_preview(preview) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "the preview identity is already alive");
    }
    auto created = previews_.emplace_back(*allocator_, preview);
    return created.has_value() ? ok() : make_unexpected(created.error());
}

Status MetalMaterialRuntime::reload(u64 preview, u64 artefact,
                                    Span<const editor::MaterialPreviewTarget> targets) noexcept {
    Preview* state = find_preview(preview);
    if (state == nullptr) {
        return fail(ErrorCode::NotFound, "the preview world was destroyed");
    }
    if (find_program(artefact) == nullptr) {
        return fail(ErrorCode::NotFound, "the compiled material artefact was not published");
    }
    Array<Preview::Binding> next(*allocator_);
    for (const editor::MaterialPreviewTarget& target : targets) {
        if (read_u64_le(target.entity + 8) != 0) {
            return fail(ErrorCode::Unsupported,
                        "this viewport supports 64-bit scene identities only");
        }
        const u64 entity = read_u64_le(target.entity);
        const u32 object = world_->object_for(entity);
        if (object == WorldView::kNoObject) {
            return fail(ErrorCode::NotFound,
                        "the material target is not present in the rendered world");
        }
        if (target.material_slot != 0) {
            return fail(ErrorCode::Unsupported,
                        "the viewport proxy mesh exposes material slot zero only");
        }
        if (Status appended =
                next.push_back(Preview::Binding{entity, object, target.material_slot, artefact});
            !appended) {
            return appended;
        }
    }
    for (const Preview::Binding& binding : state->bindings) {
        renderer_->unbind_material(binding.object, binding.material_slot, binding.artefact);
    }
    for (const Preview::Binding& binding : next) {
        if (Status bound =
                renderer_->bind_material(binding.object, binding.material_slot, binding.artefact);
            !bound) {
            return bound;
        }
    }
    state->bindings = std::move(next);
    return ok();
}

Status MetalMaterialRuntime::update(u64 preview, u64 artefact,
                                    const editor::MaterialParameterUpdate& parameter) noexcept {
    Preview* state = find_preview(preview);
    Program* program = find_program(artefact);
    if (state == nullptr || program == nullptr) {
        return fail(ErrorCode::NotFound, "the preview or compiled artefact is not alive");
    }
    bool applied = false;
    for (const Preview::Binding& binding : state->bindings) {
        applied = applied || binding.artefact == artefact;
    }
    if (!applied) {
        return fail(ErrorCode::InvalidArgument,
                    "the parameter update does not address the preview's applied artefact");
    }
    const Program::Slot* found = nullptr;
    for (u32 index = 0; index < program->slot_count; ++index) {
        if (program->slots[index].identity == parameter.identity) {
            found = &program->slots[index];
            break;
        }
    }
    if (found == nullptr) {
        return fail(ErrorCode::NotFound,
                    "the stable parameter identity is absent from the compiled layout");
    }

    u8 converted[16] = {};
    u32 converted_size = 0;
    if (Status converted_status =
            convert_parameter_value(*renderer_, found->kind, parameter, converted, converted_size);
        !converted_status) {
        return converted_status;
    }
    std::memcpy(program->parameters + found->offset, converted, converted_size);
    return renderer_->update_material(artefact, {program->parameters, sizeof(program->parameters)});
}

Status MetalMaterialRuntime::destroy(u64 preview) noexcept {
    for (usize index = 0; index < previews_.size(); ++index) {
        if (previews_[index].identity != preview) {
            continue;
        }
        for (const Preview::Binding& binding : previews_[index].bindings) {
            renderer_->unbind_material(binding.object, binding.material_slot, binding.artefact);
        }
        previews_.erase(index);
        return ok();
    }
    return ok();
}

}  // namespace cy::sample::editor_window
