// The SPIR-V reflection parser, and the descriptor-set convention check. Task 3.3.
//
// One pass over the instruction stream fills a per-id record table; a second pass turns the
// variables in that table into bindings, push-constant ranges, vertex inputs and specialization
// constants. Two passes rather than one because SPIR-V permits a decoration to precede the
// instruction it decorates and a type to be referenced before it is declared — the format
// guarantees neither ordering, and a parser that assumed one would work on today's Slang and break
// on tomorrow's.
//
// WHAT THIS PARSER DELIBERATELY DOES NOT DO. It does not validate the module: `spirv-val` does
// that, it is part of the toolchain the engine integrates rather than writes, and a second
// half-validator would be worse than none. What this rejects is what it cannot read — a wrong
// magic number, a truncated instruction, an id past the declared bound — because those are the
// three ways a buffer that is not a shader gets this far.

#include <cy/backends/shader/reflection.h>

#include "spirv.h"

#include <algorithm>
#include <cstdio>

namespace cy::shader {
namespace {

constexpr u32 kNone = 0xFFFF'FFFFU;

/// Everything the parser learns about one SPIR-V id.
///
/// A flat record per id rather than a variant: an id is decorated and typed by instructions that
/// may arrive in any order, so the fields have to exist before anybody knows which of them will be
/// used. The table is sized by the module's declared bound, which is a few thousand entries for a
/// real shader.
struct IdRecord {
    u16 opcode = 0;
    /// OpVariable: the pointer type. OpTypePointer: the pointee. OpTypeArray/RuntimeArray: the
    /// element. OpTypeVector/Matrix: the component or column type. OpConstant: the value's type.
    u32 type_id = kNone;
    u32 storage_class = kNone;
    u32 length_id = kNone;    ///< OpTypeArray
    u32 component_count = 0;  ///< vector components, matrix columns
    u32 width = 0;            ///< OpTypeInt / OpTypeFloat, in bits
    bool is_signed = false;
    u32 image_dim = kNone;
    u32 image_sampled = 0;
    u32 constant_value = 0;
    u32 member_first = 0;  ///< into the parser's `members_`
    u32 member_count = 0;
    /// The largest (member offset + member size) over the struct's members, resolved after the
    /// scan. That is a struct's size when the emitter decorated its members with offsets — and a
    /// push-constant block always is.
    u32 struct_extent = 0;
    u32 struct_min_offset = kNone;

    u32 set = kNone;
    u32 binding = kNone;
    u32 location = kNone;
    u32 spec_id = kNone;
    u32 array_stride = 0;
    u32 matrix_stride = 0;
    bool builtin = false;
    bool block = false;
    bool buffer_block = false;
    bool input_attachment = false;
    const char* name = nullptr;
};

[[nodiscard]] rhi::ShaderStage stage_of_model(u32 model) noexcept {
    switch (model) {
        case spirv::ExecutionModelVertex:
            return rhi::ShaderStage::Vertex;
        case spirv::ExecutionModelFragment:
            return rhi::ShaderStage::Fragment;
        case spirv::ExecutionModelGLCompute:
            return rhi::ShaderStage::Compute;
        case spirv::ExecutionModelGeometry:
            return rhi::ShaderStage::Geometry;
        case spirv::ExecutionModelTessellationControl:
            return rhi::ShaderStage::TessellationControl;
        case spirv::ExecutionModelTessellationEvaluation:
            return rhi::ShaderStage::TessellationEvaluation;
        case spirv::ExecutionModelTaskEXT:
            return rhi::ShaderStage::Task;
        case spirv::ExecutionModelMeshEXT:
            return rhi::ShaderStage::Mesh;
        default:
            return rhi::ShaderStage::None;
    }
}

/// A SPIR-V literal string: bytes from `first`, NUL-terminated, padded to a word boundary. The view
/// borrows the module, which outlives the parse.
[[nodiscard]] std::string_view read_string(Span<const u32> words, usize first,
                                           usize limit) noexcept {
    const char* begin = reinterpret_cast<const char*>(words.data() + first);
    const usize max_bytes = (limit - first) * sizeof(u32);
    usize length = 0;
    while (length < max_bytes && begin[length] != '\0') {
        ++length;
    }
    return {begin, length};
}

class Parser {
public:
    Parser(Allocator& allocator, Span<const u32> words) noexcept
        : allocator_(&allocator),
          words_(words),
          ids_(allocator),
          members_(allocator),
          member_offsets_(allocator),
          entry_points_(allocator),
          variables_(allocator) {}

    [[nodiscard]] Expected<Reflection, Error> run() noexcept;

private:
    /// One `OpMemberDecorate ... Offset`, kept out of line.
    ///
    /// The SPIR-V logical layout puts every decoration BEFORE the type declarations, so at the
    /// moment a member offset is seen the struct's member list is still empty and there is nothing
    /// to attach it to. Recording the triple and resolving it after the scan is the only ordering
    /// that works, and it is why these are not fields of `IdRecord`.
    struct MemberOffset {
        u32 struct_id = 0;
        u32 member_index = 0;
        u32 offset = 0;
    };

    struct EntryPointRecord {
        u32 id = kNone;
        rhi::ShaderStage stage = rhi::ShaderStage::None;
        std::string_view name;
        u32 workgroup[3] = {0, 0, 0};
        /// True when `workgroup` holds constant ids rather than literals — see
        /// `scan_execution_mode`.
        bool workgroup_is_id = false;
    };

    [[nodiscard]] Status check_header() noexcept;
    [[nodiscard]] Status scan() noexcept;
    void scan_type(u16 opcode, Span<const u32> operands) noexcept;
    void scan_decoration(u16 opcode, Span<const u32> operands) noexcept;
    [[nodiscard]] Status scan_entry_point(Span<const u32> operands, usize first,
                                          usize limit) noexcept;
    void scan_name(Span<const u32> operands, usize first, usize limit) noexcept;
    [[nodiscard]] Status scan_variable(Span<const u32> operands) noexcept;
    void scan_constant(u16 opcode, Span<const u32> operands) noexcept;
    void scan_execution_mode(Span<const u32> operands) noexcept;
    void resolve_struct_extents() noexcept;

    [[nodiscard]] Status build(Reflection& out) noexcept;
    [[nodiscard]] Status build_variable(u32 id, Reflection& out) noexcept;
    [[nodiscard]] Status build_descriptor(u32 id, u32 pointee, Reflection& out) noexcept;
    void build_vertex_input(u32 id, u32 pointee, Reflection& out, Status& status) noexcept;

    [[nodiscard]] bool valid_id(u32 id) const noexcept { return id < ids_.size(); }
    [[nodiscard]] u32 type_size(u32 type_id, u32 depth = 0) const noexcept;
    [[nodiscard]] rhi::Format vertex_format(u32 type_id) const noexcept;
    [[nodiscard]] Name interned_name(u32 id) const noexcept;

    Allocator* allocator_;
    Span<const u32> words_;
    Array<IdRecord> ids_;
    Array<u32> members_;
    Array<MemberOffset> member_offsets_;
    Array<EntryPointRecord> entry_points_;
    Array<u32> variables_;
    u32 instruction_count_ = 0;
    bool has_vertex_stage_ = false;
};

Status Parser::check_header() noexcept {
    if (words_.size() < spirv::kHeaderWords) {
        return fail(ErrorCode::InvalidArgument, "the SPIR-V module is shorter than its header");
    }
    if (words_[0] != spirv::kMagic) {
        // A byte-swapped magic is a module built for the other endianness. The engine's toolchain
        // never produces one, so naming it is more useful than silently swapping.
        return fail(ErrorCode::InvalidArgument,
                    "not a SPIR-V module: the magic number does not match");
    }
    const u32 bound = words_[3];
    if (bound == 0 || bound > spirv::kMaxIdBound) {
        return fail(ErrorCode::OutOfRange, "the SPIR-V module declares an implausible id bound");
    }
    return ok();
}

void Parser::scan_type(u16 opcode, Span<const u32> operands) noexcept {
    // Every one of these declares its result id first, except OpTypePointer and OpTypeImage, which
    // also do. The differences are in what follows.
    const u32 result = operands[0];
    if (!valid_id(result)) {
        return;
    }
    IdRecord& record = ids_[result];
    record.opcode = opcode;
    switch (opcode) {
        case spirv::OpTypeInt:
            record.width = operands.size() > 1 ? operands[1] : 0;
            record.is_signed = operands.size() > 2 && operands[2] != 0;
            break;
        case spirv::OpTypeFloat:
            record.width = operands.size() > 1 ? operands[1] : 0;
            break;
        case spirv::OpTypeVector:
        case spirv::OpTypeMatrix:
            record.type_id = operands.size() > 1 ? operands[1] : kNone;
            record.component_count = operands.size() > 2 ? operands[2] : 0;
            break;
        case spirv::OpTypeImage:
            record.type_id = operands.size() > 1 ? operands[1] : kNone;
            record.image_dim = operands.size() > 2 ? operands[2] : kNone;
            record.image_sampled = operands.size() > 6 ? operands[6] : 0;
            break;
        case spirv::OpTypeSampledImage:
        case spirv::OpTypeRuntimeArray:
            record.type_id = operands.size() > 1 ? operands[1] : kNone;
            break;
        case spirv::OpTypeArray:
            record.type_id = operands.size() > 1 ? operands[1] : kNone;
            record.length_id = operands.size() > 2 ? operands[2] : kNone;
            break;
        case spirv::OpTypePointer:
            record.storage_class = operands.size() > 1 ? operands[1] : kNone;
            record.type_id = operands.size() > 2 ? operands[2] : kNone;
            break;
        case spirv::OpTypeStruct:
            record.member_first = static_cast<u32>(members_.size());
            record.member_count = static_cast<u32>(operands.size() - 1);
            for (usize index = 1; index < operands.size(); ++index) {
                if (Status pushed = members_.push_back(operands[index]); !pushed) {
                    // The member list is only used to size a push-constant block. Losing it costs a
                    // size of zero, which the caller reports; it is not worth failing the parse.
                    record.member_count = static_cast<u32>(index - 1);
                    break;
                }
            }
            break;
        default:
            break;
    }
}

void Parser::scan_decoration(u16 opcode, Span<const u32> operands) noexcept {
    const bool member = opcode == spirv::OpMemberDecorate;
    const usize decoration_index = member ? 2 : 1;
    if (operands.size() <= decoration_index) {
        return;
    }
    const u32 target = operands[0];
    if (!valid_id(target)) {
        return;
    }
    IdRecord& record = ids_[target];
    const u32 decoration = operands[decoration_index];
    const bool has_literal = operands.size() > decoration_index + 1;
    const u32 literal = has_literal ? operands[decoration_index + 1] : 0;

    if (member) {
        if (decoration == spirv::DecorationOffset && has_literal) {
            record.struct_min_offset =
                (record.struct_min_offset == kNone || literal < record.struct_min_offset)
                    ? literal
                    : record.struct_min_offset;
            // A failed push loses one member's contribution to the block's size, which the caller
            // sees as a smaller push-constant range. Reporting it would mean failing the whole
            // parse over an allocator that is already out of memory.
            (void)member_offsets_.push_back(MemberOffset{target, operands[1], literal});
        } else if (decoration == spirv::DecorationMatrixStride && has_literal) {
            record.matrix_stride = literal;
        }
        return;
    }

    switch (decoration) {
        case spirv::DecorationDescriptorSet:
            record.set = literal;
            break;
        case spirv::DecorationBinding:
            record.binding = literal;
            break;
        case spirv::DecorationLocation:
            record.location = literal;
            break;
        case spirv::DecorationSpecId:
            record.spec_id = literal;
            break;
        case spirv::DecorationArrayStride:
            record.array_stride = literal;
            break;
        case spirv::DecorationMatrixStride:
            record.matrix_stride = literal;
            break;
        case spirv::DecorationBuiltIn:
            record.builtin = true;
            break;
        case spirv::DecorationBlock:
            record.block = true;
            break;
        case spirv::DecorationBufferBlock:
            record.buffer_block = true;
            break;
        case spirv::DecorationInputAttachmentIndex:
            record.input_attachment = true;
            break;
        default:
            break;
    }
}

Status Parser::scan_entry_point(Span<const u32> operands, usize first, usize limit) noexcept {
    if (operands.size() < 3) {
        return ok();
    }
    EntryPointRecord entry;
    entry.stage = stage_of_model(operands[0]);
    entry.id = operands[1];
    entry.name = read_string(words_, first + 2, limit);
    if (entry.stage == rhi::ShaderStage::Vertex) {
        has_vertex_stage_ = true;
    }
    return entry_points_.push_back(entry);
}

void Parser::scan_execution_mode(Span<const u32> operands) noexcept {
    if (operands.size() < 2) {
        return;
    }
    const u32 mode = operands[1];
    if (mode != spirv::ExecutionModeLocalSize && mode != spirv::ExecutionModeLocalSizeId) {
        return;
    }
    for (EntryPointRecord& entry : entry_points_) {
        if (entry.id != operands[0]) {
            continue;
        }
        for (u32 axis = 0; axis < 3; ++axis) {
            const usize index = 2 + axis;
            if (index >= operands.size()) {
                break;
            }
            // LocalSizeId names constant *ids* rather than literals. The constants may not have
            // been scanned yet, so both forms are stored raw here and the id form is resolved in
            // `build`, once the id table is complete.
            entry.workgroup[axis] = operands[index];
        }
        entry.workgroup_is_id = mode == spirv::ExecutionModeLocalSizeId;
        return;
    }
}

void Parser::scan_name(Span<const u32> operands, usize first, usize limit) noexcept {
    if (operands.empty() || !valid_id(operands[0])) {
        return;
    }
    const std::string_view text = read_string(words_, first + 1, limit);
    // The bytes are NUL-terminated inside the module, so the pointer is a C string and interning it
    // later costs no copy here.
    ids_[operands[0]].name = text.empty() ? nullptr : text.data();
}

Status Parser::scan_variable(Span<const u32> operands) noexcept {
    if (operands.size() < 3 || !valid_id(operands[1])) {
        return ok();
    }
    IdRecord& record = ids_[operands[1]];
    record.opcode = spirv::OpVariable;
    record.type_id = operands[0];
    record.storage_class = operands[2];
    return variables_.push_back(operands[1]);
}

void Parser::scan_constant(u16 opcode, Span<const u32> operands) noexcept {
    const bool is_boolean =
        opcode == spirv::OpConstantTrue || opcode == spirv::OpSpecConstantTrue ||
        opcode == spirv::OpConstantFalse || opcode == spirv::OpSpecConstantFalse;
    const usize minimum = is_boolean ? 2 : 3;
    if (operands.size() < minimum || !valid_id(operands[1])) {
        return;
    }
    IdRecord& record = ids_[operands[1]];
    record.opcode = opcode;
    record.type_id = operands[0];
    if (is_boolean) {
        record.constant_value =
            (opcode == spirv::OpConstantTrue || opcode == spirv::OpSpecConstantTrue) ? 1U : 0U;
        return;
    }
    record.constant_value = operands[2];
}

Status Parser::scan() noexcept {
    if (Status sized = ids_.resize(words_[3]); !sized) {
        return sized;
    }

    usize cursor = spirv::kHeaderWords;
    while (cursor < words_.size()) {
        const spirv::InstructionHeader header = spirv::decode_instruction(words_[cursor]);
        if (header.word_count == 0 || cursor + header.word_count > words_.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "the SPIR-V instruction stream is truncated or malformed");
        }
        ++instruction_count_;
        const usize first = cursor + 1;
        const usize limit = cursor + header.word_count;
        const Span<const u32> operands(words_.data() + first, limit - first);

        switch (header.opcode) {
            case spirv::OpName:
                scan_name(operands, first, limit);
                break;
            case spirv::OpEntryPoint:
                if (Status scanned = scan_entry_point(operands, first, limit); !scanned) {
                    return scanned;
                }
                break;
            case spirv::OpExecutionMode:
            case spirv::OpExecutionModeId:
                scan_execution_mode(operands);
                break;
            case spirv::OpDecorate:
            case spirv::OpMemberDecorate:
                scan_decoration(header.opcode, operands);
                break;
            case spirv::OpVariable:
                if (Status scanned = scan_variable(operands); !scanned) {
                    return scanned;
                }
                break;
            case spirv::OpConstant:
            case spirv::OpSpecConstant:
            case spirv::OpConstantTrue:
            case spirv::OpSpecConstantTrue:
            case spirv::OpConstantFalse:
            case spirv::OpSpecConstantFalse:
                scan_constant(header.opcode, operands);
                break;
            // Only the type-declaring opcodes reach the type scanner. A blanket default would
            // hand it OpStore, OpLoad and every control-flow instruction, whose first operand is a
            // pointer or a label rather than a result id — and writing a record for one of those
            // would overwrite the OpVariable record it names. That defect reads as a descriptor
            // that vanishes from the reflection of a shader that got a little longer.
            case spirv::OpTypeVoid:
            case spirv::OpTypeBool:
            case spirv::OpTypeInt:
            case spirv::OpTypeFloat:
            case spirv::OpTypeVector:
            case spirv::OpTypeMatrix:
            case spirv::OpTypeImage:
            case spirv::OpTypeSampler:
            case spirv::OpTypeSampledImage:
            case spirv::OpTypeArray:
            case spirv::OpTypeRuntimeArray:
            case spirv::OpTypeStruct:
            case spirv::OpTypePointer:
            case spirv::OpTypeAccelerationStructureKHR:
                if (!operands.empty()) {
                    scan_type(header.opcode, operands);
                }
                break;
            default:
                break;
        }
        cursor = limit;
    }
    return ok();
}

void Parser::resolve_struct_extents() noexcept {
    for (const MemberOffset& entry : member_offsets_) {
        if (!valid_id(entry.struct_id)) {
            continue;
        }
        IdRecord& record = ids_[entry.struct_id];
        if (record.opcode != spirv::OpTypeStruct || entry.member_index >= record.member_count) {
            continue;
        }
        const u32 member_type = members_[record.member_first + entry.member_index];
        const u32 end = entry.offset + type_size(member_type);
        record.struct_extent = end > record.struct_extent ? end : record.struct_extent;
    }
}

u32 Parser::type_size(u32 type_id, u32 depth) const noexcept {
    // A type graph is a DAG, but a malformed module could still name itself; the depth cap makes
    // that a zero rather than a stack overflow.
    if (!valid_id(type_id) || depth > 16) {
        return 0;
    }
    const IdRecord& record = ids_[type_id];
    switch (record.opcode) {
        case spirv::OpTypeInt:
        case spirv::OpTypeFloat:
            return record.width / 8;
        case spirv::OpTypeBool:
            return 4;
        case spirv::OpTypeVector:
            return record.component_count * type_size(record.type_id, depth + 1);
        case spirv::OpTypeMatrix:
            return record.matrix_stride != 0
                       ? record.component_count * record.matrix_stride
                       : record.component_count * type_size(record.type_id, depth + 1);
        case spirv::OpTypeArray: {
            const u32 length =
                valid_id(record.length_id) ? ids_[record.length_id].constant_value : 0;
            const u32 stride = record.array_stride != 0 ? record.array_stride
                                                        : type_size(record.type_id, depth + 1);
            return length * stride;
        }
        case spirv::OpTypeStruct:
            return record.struct_extent;
        default:
            return 0;
    }
}

rhi::Format Parser::vertex_format(u32 type_id) const noexcept {
    if (!valid_id(type_id)) {
        return rhi::Format::Undefined;
    }
    const IdRecord& record = ids_[type_id];
    u32 components = 1;
    u32 scalar = type_id;
    if (record.opcode == spirv::OpTypeVector) {
        components = record.component_count;
        scalar = record.type_id;
    }
    if (!valid_id(scalar)) {
        return rhi::Format::Undefined;
    }
    const IdRecord& element = ids_[scalar];
    if (element.opcode == spirv::OpTypeFloat && element.width == 32) {
        switch (components) {
            case 1:
                return rhi::Format::R32Sfloat;
            case 2:
                return rhi::Format::Rg32Sfloat;
            case 3:
                return rhi::Format::Rgb32Sfloat;
            case 4:
                return rhi::Format::Rgba32Sfloat;
            default:
                return rhi::Format::Undefined;
        }
    }
    if (element.opcode == spirv::OpTypeInt && element.width == 32 && components == 1) {
        return element.is_signed ? rhi::Format::R32Sint : rhi::Format::R32Uint;
    }
    return rhi::Format::Undefined;
}

Name Parser::interned_name(u32 id) const noexcept {
    if (!valid_id(id) || ids_[id].name == nullptr) {
        return Name{};
    }
    return Name::intern(ids_[id].name);
}

Status Parser::build_descriptor(u32 id, u32 pointee, Reflection& out) noexcept {
    const IdRecord& variable = ids_[id];
    ReflectedBinding binding;
    binding.set = variable.set;
    binding.binding = variable.binding;
    binding.name = interned_name(id);

    u32 inner = pointee;
    if (valid_id(inner) && ids_[inner].opcode == spirv::OpTypeArray) {
        const u32 length_id = ids_[inner].length_id;
        binding.count = valid_id(length_id) ? ids_[length_id].constant_value : 1;
        inner = ids_[inner].type_id;
    } else if (valid_id(inner) && ids_[inner].opcode == spirv::OpTypeRuntimeArray) {
        binding.count = 0;
        binding.runtime_array = true;
        inner = ids_[inner].type_id;
    }
    if (!valid_id(inner)) {
        return fail(ErrorCode::InvalidArgument, "a descriptor variable has no resolvable type");
    }

    const IdRecord& type = ids_[inner];
    switch (type.opcode) {
        case spirv::OpTypeStruct:
            binding.kind =
                (variable.storage_class == spirv::StorageClassStorageBuffer || type.buffer_block)
                    ? rhi::DescriptorKind::StorageBuffer
                    : rhi::DescriptorKind::UniformBuffer;
            break;
        case spirv::OpTypeSampledImage:
            binding.kind = rhi::DescriptorKind::CombinedTextureSampler;
            break;
        case spirv::OpTypeSampler:
            binding.kind = rhi::DescriptorKind::Sampler;
            break;
        case spirv::OpTypeImage:
            if (type.image_dim == spirv::DimSubpassData) {
                binding.kind = rhi::DescriptorKind::InputAttachment;
            } else if (type.image_sampled == spirv::SampledStorage) {
                binding.kind = rhi::DescriptorKind::StorageTexture;
            } else {
                binding.kind = rhi::DescriptorKind::SampledTexture;
            }
            break;
        default:
            // An acceleration structure lands here. `rhi::DescriptorKind` has no enumerator for one
            // because ray tracing is capability-gated and unbuilt; reporting it by name is better
            // than mapping it onto a kind that would bind the wrong thing.
            return fail(ErrorCode::NotImplemented,
                        "the shader declares a descriptor type the RHI does not model yet");
    }
    return out.add_binding(binding);
}

void Parser::build_vertex_input(u32 id, u32 pointee, Reflection& out, Status& status) noexcept {
    const IdRecord& variable = ids_[id];
    if (variable.builtin || variable.location == kNone || !has_vertex_stage_) {
        return;
    }
    ReflectedVertexInput input;
    input.location = variable.location;
    input.format = vertex_format(pointee);
    input.name = interned_name(id);
    if (Status added = out.add_vertex_input(input); !added) {
        status = added;
    }
}

Status Parser::build_variable(u32 id, Reflection& out) noexcept {
    const IdRecord& variable = ids_[id];
    if (!valid_id(variable.type_id) || ids_[variable.type_id].opcode != spirv::OpTypePointer) {
        return ok();
    }
    const u32 pointee = ids_[variable.type_id].type_id;

    if (variable.storage_class == spirv::StorageClassPushConstant) {
        if (!valid_id(pointee)) {
            return ok();
        }
        ReflectedPushConstant range;
        const IdRecord& block = ids_[pointee];
        range.offset = block.struct_min_offset == kNone ? 0 : block.struct_min_offset;
        range.size = block.struct_extent > range.offset ? block.struct_extent - range.offset
                                                        : block.struct_extent;
        return out.add_push_constant(range);
    }

    if (variable.set != kNone && variable.binding != kNone) {
        return build_descriptor(id, pointee, out);
    }

    if (variable.storage_class == spirv::StorageClassInput) {
        Status status = ok();
        build_vertex_input(id, pointee, out, status);
        return status;
    }
    return ok();
}

Status Parser::build(Reflection& out) noexcept {
    for (const EntryPointRecord& entry : entry_points_) {
        ReflectedEntryPoint reflected;
        reflected.name = Name::intern(entry.name);
        reflected.stage = entry.stage;
        if (entry.stage == rhi::ShaderStage::Compute || entry.stage == rhi::ShaderStage::Task ||
            entry.stage == rhi::ShaderStage::Mesh) {
            for (u32 axis = 0; axis < 3; ++axis) {
                const u32 raw = entry.workgroup[axis];
                reflected.workgroup_size[axis] =
                    (entry.workgroup_is_id && valid_id(raw)) ? ids_[raw].constant_value : raw;
            }
        }
        if (Status added = out.add_entry_point(reflected); !added) {
            return added;
        }
    }

    for (const u32 id : variables_) {
        if (Status built = build_variable(id, out); !built) {
            return built;
        }
    }

    for (u32 id = 0; id < ids_.size(); ++id) {
        const IdRecord& record = ids_[id];
        if (record.spec_id == kNone) {
            continue;
        }
        ReflectedSpecConstant constant;
        constant.id = record.spec_id;
        constant.default_value = record.constant_value;
        constant.name = interned_name(id);
        if (Status added = out.add_spec_constant(constant); !added) {
            return added;
        }
    }

    out.assign_stages_to_resources();
    out.set_instruction_count(instruction_count_);
    out.set_spirv_version(words_[1]);
    out.sort();
    return ok();
}

Expected<Reflection, Error> Parser::run() noexcept {
    if (Status checked = check_header(); !checked) {
        return make_unexpected(checked.error());
    }
    if (Status scanned = scan(); !scanned) {
        return make_unexpected(scanned.error());
    }
    resolve_struct_extents();

    Reflection reflection(*allocator_);
    if (Status built = build(reflection); !built) {
        return make_unexpected(built.error());
    }
    return reflection;
}

}  // namespace

// --- Reflection ------------------------------------------------------------------------------

const char* set_name(u32 set) noexcept {
    switch (set) {
        case kSetGlobal:
            return "global (set 0, per frame)";
        case kSetView:
            return "view (set 1, per view)";
        case kSetPass:
            return "pass (set 2, per pass)";
        case kSetDraw:
            return "draw (set 3, per draw)";
        default:
            return "outside the convention";
    }
}

const char* update_frequency_name(UpdateFrequency frequency) noexcept {
    switch (frequency) {
        case UpdateFrequency::PerFrame:
            return "per frame";
        case UpdateFrequency::PerView:
            return "per view";
        case UpdateFrequency::PerPass:
            return "per pass";
        case UpdateFrequency::PerDraw:
            return "per draw";
    }
    return "unknown";
}

Reflection::Reflection(Allocator& allocator) noexcept
    : bindings_(allocator),
      push_constants_(allocator),
      vertex_inputs_(allocator),
      spec_constants_(allocator),
      entry_points_(allocator) {}

Span<const ReflectedBinding> Reflection::bindings() const noexcept {
    return {bindings_.data(), bindings_.size()};
}
Span<const ReflectedPushConstant> Reflection::push_constants() const noexcept {
    return {push_constants_.data(), push_constants_.size()};
}
Span<const ReflectedVertexInput> Reflection::vertex_inputs() const noexcept {
    return {vertex_inputs_.data(), vertex_inputs_.size()};
}
Span<const ReflectedSpecConstant> Reflection::spec_constants() const noexcept {
    return {spec_constants_.data(), spec_constants_.size()};
}
Span<const ReflectedEntryPoint> Reflection::entry_points() const noexcept {
    return {entry_points_.data(), entry_points_.size()};
}

Status Reflection::add_binding(const ReflectedBinding& binding) noexcept {
    for (ReflectedBinding& existing : bindings_) {
        if (existing.set == binding.set && existing.binding == binding.binding) {
            existing.stages = existing.stages | binding.stages;
            return ok();
        }
    }
    return bindings_.push_back(binding);
}

Status Reflection::add_push_constant(const ReflectedPushConstant& range) noexcept {
    return push_constants_.push_back(range);
}

Status Reflection::add_vertex_input(const ReflectedVertexInput& input) noexcept {
    for (const ReflectedVertexInput& existing : vertex_inputs_) {
        if (existing.location == input.location) {
            return ok();
        }
    }
    return vertex_inputs_.push_back(input);
}

Status Reflection::add_spec_constant(const ReflectedSpecConstant& constant) noexcept {
    for (const ReflectedSpecConstant& existing : spec_constants_) {
        if (existing.id == constant.id) {
            return ok();
        }
    }
    return spec_constants_.push_back(constant);
}

Status Reflection::add_entry_point(const ReflectedEntryPoint& entry) noexcept {
    stages_ = stages_ | entry.stage;
    return entry_points_.push_back(entry);
}

void Reflection::assign_stages_to_resources() noexcept {
    // A module's declared bindings belong to every stage the module declares. SPIR-V records which
    // variables an entry point *interfaces* with, and a finer reading is possible; it is not a
    // better one, because a layout that omits a stage is a pipeline the device refuses, while a
    // layout that names one stage too many costs nothing.
    for (ReflectedBinding& binding : bindings_) {
        binding.stages = binding.stages | stages_;
    }
    for (ReflectedPushConstant& range : push_constants_) {
        range.stages = range.stages | stages_;
    }
}

void Reflection::sort() noexcept {
    // Insertion sorts: these arrays hold tens of elements, and an insertion sort is stable, has no
    // allocation, and is the whole of the ordering guarantee this function makes.
    for (usize index = 1; index < bindings_.size(); ++index) {
        ReflectedBinding value = bindings_[index];
        usize position = index;
        while (position > 0 && (bindings_[position - 1].set > value.set ||
                                (bindings_[position - 1].set == value.set &&
                                 bindings_[position - 1].binding > value.binding))) {
            bindings_[position] = bindings_[position - 1];
            --position;
        }
        bindings_[position] = value;
    }
    for (usize index = 1; index < vertex_inputs_.size(); ++index) {
        ReflectedVertexInput value = vertex_inputs_[index];
        usize position = index;
        while (position > 0 && vertex_inputs_[position - 1].location > value.location) {
            vertex_inputs_[position] = vertex_inputs_[position - 1];
            --position;
        }
        vertex_inputs_[position] = value;
    }
    for (usize index = 1; index < spec_constants_.size(); ++index) {
        ReflectedSpecConstant value = spec_constants_[index];
        usize position = index;
        while (position > 0 && spec_constants_[position - 1].id > value.id) {
            spec_constants_[position] = spec_constants_[position - 1];
            --position;
        }
        spec_constants_[position] = value;
    }
}

Status Reflection::merge(const Reflection& other, DiagnosticLog& diagnostics) noexcept {
    for (const ReflectedBinding& incoming : other.bindings_) {
        bool merged = false;
        for (ReflectedBinding& existing : bindings_) {
            if (existing.set != incoming.set || existing.binding != incoming.binding) {
                continue;
            }
            if (existing.kind != incoming.kind || existing.count != incoming.count) {
                char message[256] = {};
                (void)std::snprintf(
                    message, sizeof(message),
                    "set %u binding %u is declared differently by two stages of one pipeline",
                    existing.set, existing.binding);
                if (Status added = diagnostics.add(Severity::Error, message); !added) {
                    return added;
                }
                return fail(ErrorCode::InvalidArgument,
                            "two stages disagree about a descriptor binding");
            }
            existing.stages = existing.stages | incoming.stages;
            merged = true;
            break;
        }
        if (!merged) {
            if (Status added = bindings_.push_back(incoming); !added) {
                return added;
            }
        }
    }
    for (const ReflectedPushConstant& incoming : other.push_constants_) {
        if (Status added = push_constants_.push_back(incoming); !added) {
            return added;
        }
    }
    for (const ReflectedVertexInput& incoming : other.vertex_inputs_) {
        if (Status added = add_vertex_input(incoming); !added) {
            return added;
        }
    }
    for (const ReflectedSpecConstant& incoming : other.spec_constants_) {
        if (Status added = add_spec_constant(incoming); !added) {
            return added;
        }
    }
    for (const ReflectedEntryPoint& incoming : other.entry_points_) {
        if (Status added = entry_points_.push_back(incoming); !added) {
            return added;
        }
        stages_ = stages_ | incoming.stage;
    }
    instruction_count_ += other.instruction_count_;
    spirv_version_ = other.spirv_version_ > spirv_version_ ? other.spirv_version_ : spirv_version_;
    sort();
    return ok();
}

bool Reflection::uses_set(u32 set) const noexcept {
    return std::ranges::any_of(
        bindings_, [set](const ReflectedBinding& binding) noexcept { return binding.set == set; });
}

u32 Reflection::set_count() const noexcept {
    u32 highest = 0;
    for (const ReflectedBinding& binding : bindings_) {
        highest = std::max(highest, binding.set + 1);
    }
    return highest;
}

Status Reflection::set_layout(u32 set, Array<rhi::DescriptorBinding>& out) const noexcept {
    for (const ReflectedBinding& binding : bindings_) {
        if (binding.set != set) {
            continue;
        }
        rhi::DescriptorBinding entry;
        entry.binding = binding.binding;
        entry.kind = binding.kind;
        entry.count = binding.count;
        entry.stages = binding.stages;
        // A runtime-sized array is the bindless table, and a bindless table is exactly the case
        // that needs partial binding: a streaming texture slot may be unwritten as long as no
        // shader reads it.
        entry.partially_bound = binding.runtime_array;
        if (Status pushed = out.push_back(entry); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status Reflection::push_constant_ranges(Array<rhi::PushConstantRange>& out) const noexcept {
    for (const ReflectedPushConstant& range : push_constants_) {
        rhi::PushConstantRange entry;
        entry.stages = range.stages;
        entry.offset = range.offset;
        entry.size = range.size;
        if (Status pushed = out.push_back(entry); !pushed) {
            return pushed;
        }
    }
    return ok();
}

const ReflectedEntryPoint* Reflection::find_entry_point(Name name) const noexcept {
    for (const ReflectedEntryPoint& entry : entry_points_) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

void Reflection::clear() noexcept {
    bindings_.clear();
    push_constants_.clear();
    vertex_inputs_.clear();
    spec_constants_.clear();
    entry_points_.clear();
    stages_ = rhi::ShaderStage::None;
    instruction_count_ = 0;
    spirv_version_ = 0;
}

Expected<Reflection, Error> reflect_spirv(Allocator& allocator, Span<const u32> words) noexcept {
    Parser parser(allocator, words);
    return parser.run();
}

bool validate_set_convention(const Reflection& reflection, DiagnosticLog& diagnostics,
                             std::string_view shader_name) noexcept {
    bool valid = true;
    for (const ReflectedBinding& binding : reflection.bindings()) {
        if (binding.set >= kSetCount) {
            valid = false;
            char message[320] = {};
            (void)std::snprintf(
                message, sizeof(message),
                "shader '%.*s': binding %u is in set %u. The descriptor set convention is "
                "0 global (per frame), 1 view (per view), 2 pass (per pass), 3 draw (per draw); "
                "there is no set %u",
                static_cast<int>(shader_name.size()), shader_name.data(), binding.binding,
                binding.set, binding.set);
            (void)diagnostics.add(Severity::Error, message);
            continue;
        }
        if (binding.runtime_array && binding.set != kSetGlobal) {
            valid = false;
            char message[320] = {};
            (void)std::snprintf(
                message, sizeof(message),
                "shader '%.*s': the runtime-sized array at set %u binding %u is a bindless table, "
                "and bindless arrays belong in set 0 (%s) — a table rebound per %s is not bindless",
                static_cast<int>(shader_name.size()), shader_name.data(), binding.set,
                binding.binding, set_name(kSetGlobal),
                update_frequency_name(frequency_of_set(binding.set)));
            (void)diagnostics.add(Severity::Error, message);
        }
    }
    return valid;
}

}  // namespace cy::shader
