// Global shader parameters: the std140 layout, the block's bytes, and its layout hash. Task 3.6.

#include <cy/backends/shader/globals.h>

#include <cstdio>
#include <cstring>

namespace cy::shader {
namespace {

/// Round `value` up to the next multiple of `alignment`, which is always a power of two here.
[[nodiscard]] constexpr u32 align_up(u32 value, u32 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] const char* slang_type_name(GlobalType type) noexcept {
    switch (type) {
        case GlobalType::Float:
            return "float";
        case GlobalType::Vec2:
            return "float2";
        case GlobalType::Vec3:
            return "float3";
        case GlobalType::Vec4:
            return "float4";
        case GlobalType::Int:
            return "int";
        case GlobalType::UInt:
            return "uint";
        case GlobalType::Bool:
            return "bool";
        case GlobalType::Mat4:
            return "float4x4";
    }
    return "float";
}

}  // namespace

const char* global_type_name(GlobalType type) noexcept {
    switch (type) {
        case GlobalType::Float:
            return "float";
        case GlobalType::Vec2:
            return "vec2";
        case GlobalType::Vec3:
            return "vec3";
        case GlobalType::Vec4:
            return "vec4";
        case GlobalType::Int:
            return "int";
        case GlobalType::UInt:
            return "uint";
        case GlobalType::Bool:
            return "bool";
        case GlobalType::Mat4:
            return "mat4";
    }
    return "unknown";
}

u32 global_type_size(GlobalType type) noexcept {
    switch (type) {
        case GlobalType::Float:
        case GlobalType::Int:
        case GlobalType::UInt:
        case GlobalType::Bool:
            return 4;
        case GlobalType::Vec2:
            return 8;
        case GlobalType::Vec3:
            return 12;
        case GlobalType::Vec4:
            return 16;
        case GlobalType::Mat4:
            return 64;
    }
    return 0;
}

u32 global_type_alignment(GlobalType type) noexcept {
    switch (type) {
        case GlobalType::Float:
        case GlobalType::Int:
        case GlobalType::UInt:
        case GlobalType::Bool:
            return 4;
        case GlobalType::Vec2:
            return 8;
        // std140: a three-component vector aligns as a four-component one, and a matrix aligns as
        // its columns do. This is the rule shading languages agree on, and it is why a block laid
        // out by naive packing reads garbage in the shader.
        case GlobalType::Vec3:
        case GlobalType::Vec4:
        case GlobalType::Mat4:
            return 16;
    }
    return 4;
}

GlobalParameterBlock::GlobalParameterBlock(Allocator& allocator) noexcept
    : parameters_(allocator), storage_(allocator) {}

Expected<GlobalParameterId, Error> GlobalParameterBlock::declare(Name name,
                                                                 GlobalType type) noexcept {
    if (frozen_) {
        return fail(ErrorCode::PermissionDenied,
                    "the global parameter block is frozen; declaring one now would move the layout "
                    "out from under every shader already compiled against it");
    }
    if (name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "a global shader parameter must be named");
    }
    if (find(name).is_valid()) {
        return fail(ErrorCode::AlreadyExists, "a global shader parameter with that name exists");
    }

    const u32 alignment = global_type_alignment(type);
    const u32 offset = align_up(static_cast<u32>(storage_.size()), alignment);
    const u32 end = offset + global_type_size(type);
    if (end > kMaxGlobalBlockBytes) {
        return fail(ErrorCode::OutOfRange,
                    "the global parameter block is full; a project needing more has data rather "
                    "than project-wide constants");
    }
    if (Status sized = storage_.resize(end); !sized) {
        return make_unexpected(sized.error());
    }
    // The padding a new parameter's alignment introduced is left zeroed rather than undefined, so
    // the block's bytes are a function of its declarations and its values and of nothing else —
    // which is what lets two runs upload identical bytes.
    std::memset(storage_.data() + offset, 0, end - offset);

    if (Status pushed = parameters_.push_back(GlobalParameter{name, type, offset}); !pushed) {
        return make_unexpected(pushed.error());
    }
    return GlobalParameterId{static_cast<u32>(parameters_.size() - 1)};
}

GlobalParameterId GlobalParameterBlock::find(Name name) const noexcept {
    for (usize index = 0; index < parameters_.size(); ++index) {
        if (parameters_[index].name == name) {
            return GlobalParameterId{static_cast<u32>(index)};
        }
    }
    return GlobalParameterId{};
}

const GlobalParameter* GlobalParameterBlock::parameter_at(GlobalParameterId id) const noexcept {
    return id.value < parameters_.size() ? &parameters_[id.value] : nullptr;
}

Status GlobalParameterBlock::write(GlobalParameterId id, GlobalType expected, const void* data,
                                   u32 size) noexcept {
    const GlobalParameter* parameter = parameter_at(id);
    if (parameter == nullptr) {
        return fail(ErrorCode::NotFound, "no such global shader parameter");
    }
    if (parameter->type != expected) {
        return fail(ErrorCode::InvalidArgument,
                    "a global shader parameter was set through the wrong type");
    }
    if (parameter->offset + size > storage_.size()) {
        return fail(ErrorCode::OutOfRange, "the global parameter block is smaller than its layout");
    }
    std::memcpy(storage_.data() + parameter->offset, data, size);
    ++revision_;
    return ok();
}

Status GlobalParameterBlock::set_float(GlobalParameterId id, f32 value) noexcept {
    return write(id, GlobalType::Float, &value, sizeof(value));
}

Status GlobalParameterBlock::set_int(GlobalParameterId id, i32 value) noexcept {
    return write(id, GlobalType::Int, &value, sizeof(value));
}

Status GlobalParameterBlock::set_uint(GlobalParameterId id, u32 value) noexcept {
    return write(id, GlobalType::UInt, &value, sizeof(value));
}

Status GlobalParameterBlock::set_bool(GlobalParameterId id, bool value) noexcept {
    // A `bool` in a uniform block is four bytes, not one: the shading languages agree on that and
    // writing one byte would leave three bytes of whatever was there before.
    const u32 encoded = value ? 1U : 0U;
    return write(id, GlobalType::Bool, &encoded, sizeof(encoded));
}

Status GlobalParameterBlock::set_vec(GlobalParameterId id, Span<const f32> components) noexcept {
    const GlobalParameter* parameter = parameter_at(id);
    if (parameter == nullptr) {
        return fail(ErrorCode::NotFound, "no such global shader parameter");
    }
    if (global_type_size(parameter->type) != components.size() * sizeof(f32) ||
        parameter->type == GlobalType::Mat4) {
        return fail(ErrorCode::InvalidArgument,
                    "the component count does not match the declared vector type");
    }
    return write(id, parameter->type, components.data(), global_type_size(parameter->type));
}

Status GlobalParameterBlock::set_mat4(GlobalParameterId id, Span<const f32> column_major) noexcept {
    if (column_major.size() != 16) {
        return fail(ErrorCode::InvalidArgument, "a mat4 is sixteen floats, column-major");
    }
    return write(id, GlobalType::Mat4, column_major.data(), 64);
}

assets::ContentHash GlobalParameterBlock::layout_hash() const noexcept {
    assets::ContentHasher hasher;
    for (const GlobalParameter& parameter : parameters_) {
        const std::string_view text = parameter.name.text();
        const u64 length = text.size();
        hasher.update(&length, sizeof(length));
        hasher.update(text.data(), text.size());
        const u32 type = static_cast<u32>(parameter.type);
        hasher.update(&type, sizeof(type));
        hasher.update(&parameter.offset, sizeof(parameter.offset));
    }
    return hasher.finish();
}

bool GlobalParameterBlock::validate_against(const Reflection& reflection,
                                            DiagnosticLog& diagnostics,
                                            std::string_view shader_name) const noexcept {
    for (const ReflectedBinding& binding : reflection.bindings()) {
        if (binding.set != kGlobalsSet || binding.binding != kGlobalsBinding) {
            continue;
        }
        if (binding.kind == rhi::DescriptorKind::UniformBuffer) {
            return true;
        }
        char message[256] = {};
        (void)std::snprintf(message, sizeof(message),
                            "shader '%.*s': set %u binding %u is the global parameter block (%u "
                            "bytes, %u parameters) and must be a uniform buffer",
                            static_cast<int>(shader_name.size()), shader_name.data(), kGlobalsSet,
                            kGlobalsBinding, byte_size(), static_cast<u32>(parameters_.size()));
        (void)diagnostics.add(Severity::Error, message);
        return false;
    }
    // A shader that names no global is not wrong: most compute utilities do not. The check is about
    // disagreement, not about absence.
    return true;
}

Status GlobalParameterBlock::emit_slang_declaration(Array<char>& out) const noexcept {
    const auto append = [&out](std::string_view text) noexcept -> Status {
        return out.append(Span<const char>(text.data(), text.size()));
    };
    if (Status written = append("// Generated by cy::shader::GlobalParameterBlock. Do not edit.\n"
                                "[[vk::binding(0, 0)]]\ncbuffer CyGlobals {\n");
        !written) {
        return written;
    }
    for (const GlobalParameter& parameter : parameters_) {
        char line[256] = {};
        const int length = std::snprintf(line, sizeof(line), "    %s %s;  // offset %u\n",
                                         slang_type_name(parameter.type), parameter.name.c_str(),
                                         parameter.offset);
        if (length <= 0) {
            return fail(ErrorCode::Internal, "a global parameter declaration could not be written");
        }
        if (Status written = append(std::string_view(line, static_cast<usize>(length))); !written) {
            return written;
        }
    }
    return append("};\n");
}

}  // namespace cy::shader
