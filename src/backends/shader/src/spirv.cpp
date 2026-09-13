// The SPIR-V reflector. Tasks 3.1 and 3.3.
//
// One linear pass over the word stream fills per-id tables; a second turns those tables into a
// `Reflection`. There is no recursion over the instruction stream and no allocation per
// instruction: the id tables are sized once from the header's id bound, which is what makes
// reflecting a thousand-variant library a linear cost rather than a quadratic one.
//
// --- READING THIS FILE
// ------------------------------------------------------------------------------
//
// SPIR-V is a stream of instructions, each `word[0] = (word_count << 16) | opcode`. Every constant
// below is from the SPIR-V specification's Binary Form section, and each is spelled with its
// specification name so it can be looked up. Nothing here is Vulkan: SPIR-V is a Khronos
// interchange format that Vulkan consumes, and the distinction is what lets this file compile and
// be tested on a machine with no Vulkan headers and no device.
//
// --- THE ONE APPROXIMATION, STATED
// ------------------------------------------------------------------
//
// A binding's `block_size` is computed as `offset of the last member + size of the last member`,
// using the `Offset` decorations the compiler emitted. That is exact for the layouts Slang produces
// and it is *not* a std140/std430 layout engine — the engine deliberately does not have one,
// because the compiler already laid the block out and recomputing it would be a second opinion that
// can disagree. A block ending in a runtime array reports 0, which is the honest answer: its size
// is not static.

#include <cy/shader/spirv.h>

#include <cy/core/base/assert.h>

#include <cstring>

namespace cy::shader {
namespace {

// --- The parts of the SPIR-V binary form this file reads
// ------------------------------------------

enum : u32 {
    kOpName = 5,
    kOpMemberName = 6,
    kOpEntryPoint = 15,
    kOpExecutionMode = 16,
    kOpTypeVoid = 19,
    kOpTypeBool = 20,
    kOpTypeInt = 21,
    kOpTypeFloat = 22,
    kOpTypeVector = 23,
    kOpTypeMatrix = 24,
    kOpTypeImage = 25,
    kOpTypeSampler = 26,
    kOpTypeSampledImage = 27,
    kOpTypeArray = 28,
    kOpTypeRuntimeArray = 29,
    kOpTypeStruct = 30,
    kOpTypePointer = 32,
    kOpConstantTrue = 41,
    kOpConstantFalse = 42,
    kOpConstant = 43,
    kOpSpecConstantTrue = 48,
    kOpSpecConstantFalse = 49,
    kOpSpecConstant = 50,
    kOpVariable = 59,
    kOpDecorate = 71,
    kOpMemberDecorate = 72,
    kOpExecutionModeId = 331,
    kOpTypeAccelerationStructure = 5341,
};

enum : u32 {
    kDecorationSpecId = 1,
    kDecorationBlock = 2,
    kDecorationBufferBlock = 3,
    kDecorationArrayStride = 6,
    kDecorationMatrixStride = 7,
    kDecorationBuiltIn = 11,
    kDecorationLocation = 30,
    kDecorationBinding = 33,
    kDecorationDescriptorSet = 34,
    kDecorationOffset = 35,
};

enum : u32 {
    kStorageUniformConstant = 0,
    kStorageInput = 1,
    kStorageUniform = 2,
    kStorageOutput = 3,
    kStoragePushConstant = 9,
    kStorageStorageBuffer = 12,
};

enum : u32 {
    kExecutionModeLocalSize = 17,
    kExecutionModeLocalSizeId = 38,
};

/// `Dim` operand of `OpTypeImage`. Only `Buffer` changes the classification — a `Buffer`-dimension
/// image is a texel buffer, which is a different descriptor type entirely.
enum : u32 { kDimBuffer = 5, kDimSubpassData = 6 };

inline constexpr u32 kNoValue = 0xFFFF'FFFFU;

/// The engine's `Stage` for a SPIR-V execution model. Unknown models map to `Vertex` and are
/// reported by the caller as an unparsed entry point rather than silently mislabelled.
bool stage_of_execution_model(u32 model, Stage& out) noexcept {
    switch (model) {
        case 0:
            out = Stage::Vertex;
            return true;
        case 1:
            out = Stage::TessellationControl;
            return true;
        case 2:
            out = Stage::TessellationEvaluation;
            return true;
        case 3:
            out = Stage::Geometry;
            return true;
        case 4:
            out = Stage::Fragment;
            return true;
        case 5:
            out = Stage::Compute;
            return true;
        case 5267:  // TaskNV
        case 5364:  // TaskEXT
            out = Stage::Task;
            return true;
        case 5268:  // MeshNV
        case 5365:  // MeshEXT
            out = Stage::Mesh;
            return true;
        case 5313:
            out = Stage::RayGeneration;
            return true;
        case 5314:
            out = Stage::Intersection;
            return true;
        case 5315:
            out = Stage::AnyHit;
            return true;
        case 5316:
            out = Stage::ClosestHit;
            return true;
        case 5317:
            out = Stage::Miss;
            return true;
        case 5318:
            out = Stage::Callable;
            return true;
        default:
            return false;
    }
}

/// Words a null-terminated literal string occupies, four characters to a word.
u32 literal_string_words(const u32* words, u32 available) noexcept {
    for (u32 index = 0; index < available; ++index) {
        const u32 word = words[index];
        for (u32 byte = 0; byte < 4; ++byte) {
            if (((word >> (byte * 8)) & 0xFFU) == 0) {
                return index + 1;
            }
        }
    }
    return available;
}

/// Everything the first pass records about one result id.
struct IdInfo {
    u32 opcode = 0;
    /// Type-specific operands, verbatim from the defining instruction. Which slot means what
    /// depends on `opcode`, and every reader below is next to the opcode it reads for.
    u32 operand[4] = {kNoValue, kNoValue, kNoValue, kNoValue};
    u32 name = kNoValue;  // offset into the name arena
    u32 set = kNoValue;
    u32 binding = kNoValue;
    u32 location = kNoValue;
    u32 spec_id = kNoValue;
    u32 array_stride = 0;
    /// Value of an `OpConstant`, used to resolve an array length.
    u32 constant = 0;
    bool block = false;
    bool buffer_block = false;
    bool builtin = false;
};

/// One `OpMemberDecorate ... Offset` record. A flat array rather than a per-struct list: a module
/// has a handful of blocks and scanning is cheaper than the structure that would avoid it.
struct MemberOffset {
    u32 struct_id = 0;
    u32 member = 0;
    u32 offset = 0;
};

struct EntryPointRecord {
    u32 id = 0;
    Stage stage = Stage::Vertex;
    u32 name = kNoValue;
    u32 first_interface = 0;
    u32 interface_count = 0;
    u32 workgroup[3] = {0, 0, 0};
    u32 workgroup_spec[3] = {0, 0, 0};
};

/// The first pass's tables, and the second pass over them.
class Reflector {
public:
    Reflector(Span<const u32> words, Allocator& allocator) noexcept
        : words_(words),
          allocator_(&allocator),
          ids_(allocator),
          names_(allocator),
          member_offsets_(allocator),
          entry_points_(allocator),
          interfaces_(allocator) {}

    [[nodiscard]] Status collect(const SpirvHeader& header) noexcept;
    [[nodiscard]] Expected<Reflection, Error> build() noexcept;

    [[nodiscard]] u32 instruction_count() const noexcept { return instruction_count_; }
    [[nodiscard]] Span<const EntryPointRecord> entry_points() const noexcept {
        return entry_points_.span();
    }
    [[nodiscard]] std::string_view name_at(u32 offset) const noexcept {
        return offset == kNoValue ? std::string_view{} : std::string_view(names_.data() + offset);
    }

private:
    [[nodiscard]] Status record_name(const u32* operands, u32 count) noexcept;
    [[nodiscard]] Status record_entry_point(const u32* operands, u32 count) noexcept;
    void record_execution_mode(const u32* operands, u32 count, bool by_id) noexcept;
    void record_decoration(const u32* operands, u32 count) noexcept;
    [[nodiscard]] Status record_member_decoration(const u32* operands, u32 count) noexcept;
    void record_type_or_constant(u32 opcode, const u32* operands, u32 count) noexcept;

    /// Bytes one type occupies. Zero for a type whose size is not static.
    [[nodiscard]] u32 type_size(u32 type) const noexcept;
    /// Strip arrays from a pointee type, reporting the element type and the array length.
    /// A length of 0 is a runtime array — a bindless table.
    void unwrap_array(u32 type, u32& element, u32& count) const noexcept;
    [[nodiscard]] bool classify(u32 element_type, BindingKind& out) const noexcept;
    [[nodiscard]] VertexFormat vertex_format(u32 type) const noexcept;
    [[nodiscard]] StageMask stages_using(u32 variable) const noexcept;

    [[nodiscard]] Status add_binding(Reflection& reflection, u32 variable) noexcept;
    [[nodiscard]] Status add_interface_variable(Reflection& reflection, u32 variable,
                                                u32 storage) noexcept;

    Span<const u32> words_;
    Allocator* allocator_;
    Array<IdInfo> ids_;
    Array<char> names_;
    Array<MemberOffset> member_offsets_;
    Array<EntryPointRecord> entry_points_;
    /// Interface ids, flattened; each entry point names a range.
    Array<u32> interfaces_;
    u32 instruction_count_ = 0;
    /// True when the module declares exactly one entry point, which is what lets a pre-1.4 module —
    /// whose interface list holds only Input and Output variables — still attribute its bindings.
    bool single_entry_point_ = false;
};

Status Reflector::record_name(const u32* operands, u32 count) noexcept {
    if (count < 2) {
        return ok();
    }
    const u32 target = operands[0];
    if (target >= ids_.size()) {
        return ok();
    }
    const char* text = reinterpret_cast<const char*>(operands + 1);
    const usize length = std::strlen(text);
    ids_[target].name = static_cast<u32>(names_.size());
    const Status appended = names_.append(Span<const char>(text, length + 1));
    return appended;
}

Status Reflector::record_entry_point(const u32* operands, u32 count) noexcept {
    if (count < 3) {
        return ok();
    }
    EntryPointRecord record;
    if (!stage_of_execution_model(operands[0], record.stage)) {
        // An execution model this engine has no stage for. Skipped rather than failed: a module may
        // legitimately carry a kernel entry point the renderer never uses.
        return ok();
    }
    record.id = operands[1];
    record.name = static_cast<u32>(names_.size());
    const char* text = reinterpret_cast<const char*>(operands + 2);
    const usize length = std::strlen(text);
    const Status appended = names_.append(Span<const char>(text, length + 1));
    if (!appended.has_value()) {
        return appended;
    }

    const u32 name_words = literal_string_words(operands + 2, count - 2);
    record.first_interface = static_cast<u32>(interfaces_.size());
    for (u32 index = 2 + name_words; index < count; ++index) {
        const Status pushed = interfaces_.push_back(operands[index]);
        if (!pushed.has_value()) {
            return pushed;
        }
        ++record.interface_count;
    }
    return entry_points_.push_back(record);
}

void Reflector::record_execution_mode(const u32* operands, u32 count, bool by_id) noexcept {
    if (count < 2) {
        return;
    }
    const u32 mode = operands[1];
    const bool is_local_size =
        (!by_id && mode == kExecutionModeLocalSize) || (by_id && mode == kExecutionModeLocalSizeId);
    if (!is_local_size || count < 5) {
        return;
    }
    for (EntryPointRecord& record : entry_points_) {
        if (record.id != operands[0]) {
            continue;
        }
        for (u32 axis = 0; axis < 3; ++axis) {
            if (by_id) {
                // A specialized workgroup size: the operands are ids of specialization constants.
                // Both the current value and the constant id are reported, because a pipeline that
                // overrides the constant changes the size and the renderer needs to know which.
                const u32 id = operands[2 + axis];
                record.workgroup_spec[axis] = id < ids_.size() ? ids_[id].spec_id : kNoValue;
                record.workgroup[axis] = id < ids_.size() ? ids_[id].constant : 0;
            } else {
                record.workgroup[axis] = operands[2 + axis];
            }
        }
        return;
    }
}

void Reflector::record_decoration(const u32* operands, u32 count) noexcept {
    if (count < 2) {
        return;
    }
    const u32 target = operands[0];
    if (target >= ids_.size()) {
        return;
    }
    IdInfo& info = ids_[target];
    const u32 decoration = operands[1];
    const bool has_literal = count >= 3;
    switch (decoration) {
        case kDecorationDescriptorSet:
            info.set = has_literal ? operands[2] : 0;
            break;
        case kDecorationBinding:
            info.binding = has_literal ? operands[2] : 0;
            break;
        case kDecorationLocation:
            info.location = has_literal ? operands[2] : 0;
            break;
        case kDecorationSpecId:
            info.spec_id = has_literal ? operands[2] : 0;
            break;
        case kDecorationArrayStride:
            info.array_stride = has_literal ? operands[2] : 0;
            break;
        case kDecorationBlock:
            info.block = true;
            break;
        case kDecorationBufferBlock:
            info.buffer_block = true;
            break;
        case kDecorationBuiltIn:
            info.builtin = true;
            break;
        default:
            break;
    }
}

Status Reflector::record_member_decoration(const u32* operands, u32 count) noexcept {
    if (count < 4 || operands[2] != kDecorationOffset) {
        return ok();
    }
    return member_offsets_.push_back(MemberOffset{operands[0], operands[1], operands[3]});
}

void Reflector::record_type_or_constant(u32 opcode, const u32* operands, u32 count) noexcept {
    // Every instruction handled here defines a result id in `operands[0]`, except the constants,
    // whose result id is `operands[1]` because they carry a result *type* first.
    const bool is_constant = opcode == kOpConstant || opcode == kOpSpecConstant ||
                             opcode == kOpConstantTrue || opcode == kOpConstantFalse ||
                             opcode == kOpSpecConstantTrue || opcode == kOpSpecConstantFalse;
    const u32 result = is_constant ? (count > 1 ? operands[1] : kNoValue) : operands[0];
    if (result == kNoValue || result >= ids_.size()) {
        return;
    }

    IdInfo& info = ids_[result];
    info.opcode = opcode;
    const u32 first = is_constant ? 2 : 1;
    for (u32 index = 0; index + first < count && index < 4; ++index) {
        info.operand[index] = operands[first + index];
    }
    if (opcode == kOpConstant && count > 2) {
        info.constant = operands[2];
    } else if (opcode == kOpSpecConstant && count > 2) {
        info.constant = operands[2];
    } else if (opcode == kOpConstantTrue || opcode == kOpSpecConstantTrue) {
        info.constant = 1;
    } else if (opcode == kOpConstantFalse || opcode == kOpSpecConstantFalse) {
        info.constant = 0;
    }
    if (is_constant && count > 0) {
        // The constant's *type* id, kept in the last slot so `OpSpecConstant` can report its width.
        info.operand[3] = operands[0];
    }
}

Status Reflector::collect(const SpirvHeader& header) noexcept {
    const Status sized = ids_.resize(header.id_bound);
    if (!sized.has_value()) {
        return sized;
    }

    u32 at = kSpirvHeaderWords;
    while (at < words_.size()) {
        const u32 head = words_[at];
        const u32 word_count = head >> 16;
        const u32 opcode = head & 0xFFFFU;
        if (word_count == 0) {
            return fail(ErrorCode::InvalidArgument,
                        "a SPIR-V instruction claims zero words, which cannot be advanced past");
        }
        if (at + word_count > words_.size()) {
            return fail(ErrorCode::InvalidArgument, "a SPIR-V instruction runs past the module");
        }
        ++instruction_count_;

        const u32* operands = words_.data() + at + 1;
        const u32 operand_count = word_count - 1;
        Status recorded = ok();
        switch (opcode) {
            case kOpName:
                recorded = record_name(operands, operand_count);
                break;
            case kOpEntryPoint:
                recorded = record_entry_point(operands, operand_count);
                break;
            case kOpExecutionMode:
                record_execution_mode(operands, operand_count, false);
                break;
            case kOpExecutionModeId:
                record_execution_mode(operands, operand_count, true);
                break;
            case kOpDecorate:
                record_decoration(operands, operand_count);
                break;
            case kOpMemberDecorate:
                recorded = record_member_decoration(operands, operand_count);
                break;
            case kOpMemberName:
                break;  // member names are not reflected; the block's own name is enough
            default:
                if (operand_count != 0) {
                    record_type_or_constant(opcode, operands, operand_count);
                }
                break;
        }
        if (!recorded.has_value()) {
            return recorded;
        }
        at += word_count;
    }

    single_entry_point_ = entry_points_.size() == 1;
    return ok();
}

u32 Reflector::type_size(u32 type) const noexcept {
    if (type >= ids_.size()) {
        return 0;
    }
    const IdInfo& info = ids_[type];
    switch (info.opcode) {
        case kOpTypeBool:
            return 4;
        case kOpTypeInt:
        case kOpTypeFloat:
            return info.operand[0] == kNoValue ? 0 : info.operand[0] / 8;
        case kOpTypeVector:
        case kOpTypeMatrix:
            return type_size(info.operand[0]) * (info.operand[1] == kNoValue ? 0 : info.operand[1]);
        case kOpTypeArray: {
            const u32 length_id = info.operand[1];
            const u32 length = length_id < ids_.size() ? ids_[length_id].constant : 0;
            const u32 stride =
                info.array_stride != 0 ? info.array_stride : type_size(info.operand[0]);
            return length * stride;
        }
        case kOpTypeRuntimeArray:
            return 0;
        case kOpTypeStruct: {
            // The last member's offset plus its size. See the file header for why this is not a
            // layout engine: the compiler already laid the block out and these are its numbers.
            u32 highest_offset = 0;
            u32 highest_member = kNoValue;
            for (const MemberOffset& member : member_offsets_) {
                if (member.struct_id == type &&
                    (highest_member == kNoValue || member.offset >= highest_offset)) {
                    highest_offset = member.offset;
                    highest_member = member.member;
                }
            }
            if (highest_member == kNoValue) {
                return 0;
            }
            const u32 member_type = highest_member < 4 ? info.operand[highest_member] : kNoValue;
            const u32 member_size = member_type == kNoValue ? 0 : type_size(member_type);
            return member_size == 0 ? 0 : highest_offset + member_size;
        }
        default:
            return 0;
    }
}

void Reflector::unwrap_array(u32 type, u32& element, u32& count) const noexcept {
    element = type;
    count = 1;
    while (element < ids_.size()) {
        const IdInfo& info = ids_[element];
        if (info.opcode == kOpTypeArray) {
            const u32 length_id = info.operand[1];
            const u32 length = length_id < ids_.size() ? ids_[length_id].constant : 0;
            count = count * (length == 0 ? 1 : length);
            element = info.operand[0];
            continue;
        }
        if (info.opcode == kOpTypeRuntimeArray) {
            // A runtime array is the bindless case, and it is reported as zero rather than as a
            // very large number: `reflection.h` documents zero as "unbounded", and the convention
            // validator refuses one outside set 0.
            count = 0;
            element = info.operand[0];
            return;
        }
        return;
    }
}

bool Reflector::classify(u32 element_type, BindingKind& out) const noexcept {
    if (element_type >= ids_.size()) {
        return false;
    }
    const IdInfo& info = ids_[element_type];
    switch (info.opcode) {
        case kOpTypeSampler:
            out = BindingKind::Sampler;
            return true;
        case kOpTypeSampledImage:
            out = BindingKind::CombinedImageSampler;
            return true;
        case kOpTypeAccelerationStructure:
            out = BindingKind::AccelerationStructure;
            return true;
        case kOpTypeImage: {
            // OpTypeImage: SampledType, Dim, Depth, Arrayed, MS, Sampled, Format. `Sampled == 1`
            // means it is read through a sampler; `2` means it is a storage image. `Dim == Buffer`
            // overrides both — a texel buffer is its own descriptor type.
            const u32 dim = info.operand[1];
            const u32 sampled = info.operand[5];
            if (dim == kDimBuffer) {
                out = sampled == 2 ? BindingKind::StorageTexelBuffer
                                   : BindingKind::UniformTexelBuffer;
                return true;
            }
            if (dim == kDimSubpassData) {
                out = BindingKind::InputAttachment;
                return true;
            }
            out = sampled == 2 ? BindingKind::StorageImage : BindingKind::SampledImage;
            return true;
        }
        default:
            return false;
    }
}

VertexFormat Reflector::vertex_format(u32 type) const noexcept {
    if (type >= ids_.size()) {
        return VertexFormat::Unknown;
    }
    const IdInfo& info = ids_[type];
    u32 scalar = type;
    u32 components = 1;
    if (info.opcode == kOpTypeVector) {
        scalar = info.operand[0];
        components = info.operand[1];
    }
    if (scalar >= ids_.size() || components == 0 || components > 4) {
        return VertexFormat::Unknown;
    }
    const IdInfo& scalar_info = ids_[scalar];
    if (scalar_info.opcode == kOpTypeFloat) {
        return static_cast<VertexFormat>(static_cast<u32>(VertexFormat::Float32) + components - 1);
    }
    if (scalar_info.opcode == kOpTypeInt) {
        const bool is_signed = scalar_info.operand[1] == 1;
        const VertexFormat base = is_signed ? VertexFormat::Sint32 : VertexFormat::Uint32;
        return static_cast<VertexFormat>(static_cast<u32>(base) + components - 1);
    }
    return VertexFormat::Unknown;
}

StageMask Reflector::stages_using(u32 variable) const noexcept {
    StageMask mask = StageMask::None;
    for (const EntryPointRecord& record : entry_points_) {
        for (u32 index = 0; index < record.interface_count; ++index) {
            if (interfaces_[record.first_interface + index] == variable) {
                mask |= stage_bit(record.stage);
                break;
            }
        }
    }
    // SPIR-V before 1.4 lists only Input and Output variables in an entry point's interface, so a
    // uniform is named by nothing. With one entry point the attribution is unambiguous and this is
    // the right answer; with several it would be a guess, and reporting no stage is the honest
    // outcome — the layout unions stages across entry points anyway.
    if (!any(mask) && single_entry_point_) {
        mask = stage_bit(entry_points_[0].stage);
    }
    return mask;
}

Status Reflector::add_binding(Reflection& reflection, u32 variable) noexcept {
    const IdInfo& info = ids_[variable];
    const u32 pointer = info.operand[3] != kNoValue ? info.operand[3] : kNoValue;
    (void)pointer;

    // OpVariable's result type is a pointer; its pointee is what is actually bound.
    const u32 pointer_type = ids_[variable].operand[3];
    const u32 pointee = pointer_type < ids_.size() ? ids_[pointer_type].operand[1] : kNoValue;
    if (pointee == kNoValue) {
        return ok();
    }

    u32 element = pointee;
    u32 count = 1;
    unwrap_array(pointee, element, count);

    BindingReflection binding;
    binding.name.assign(name_at(info.name));
    binding.set = info.set == kNoValue ? 0 : info.set;
    binding.binding = info.binding == kNoValue ? 0 : info.binding;
    binding.count = count;
    binding.stages = stages_using(variable);

    const u32 storage = ids_[pointer_type].operand[0];
    if (!classify(element, binding.kind)) {
        // Not an image, sampler or acceleration structure, so it is a block. Which kind depends on
        // the storage class and, for the pre-1.3 spelling, on the BufferBlock decoration.
        if (storage == kStorageStorageBuffer || ids_[element].buffer_block) {
            binding.kind = BindingKind::StorageBuffer;
        } else if (storage == kStorageUniform && ids_[element].block) {
            binding.kind = BindingKind::UniformBuffer;
        } else {
            return ok();  // not a descriptor: a workgroup variable, a private, a function local
        }
        binding.block_size = type_size(element);
    }

    return reflection.bindings.push_back(binding);
}

Status Reflector::add_interface_variable(Reflection& reflection, u32 variable,
                                         u32 storage) noexcept {
    const IdInfo& info = ids_[variable];
    if (info.builtin || info.location == kNoValue) {
        return ok();  // gl_Position and friends are not vertex attributes
    }
    const u32 pointer_type = info.operand[3];
    const u32 pointee = pointer_type < ids_.size() ? ids_[pointer_type].operand[1] : kNoValue;
    if (pointee == kNoValue) {
        return ok();
    }

    if (storage == kStorageInput) {
        VertexInputReflection input;
        input.name.assign(name_at(info.name));
        input.location = info.location;
        input.format = vertex_format(pointee);
        return reflection.vertex_inputs.push_back(input);
    }

    FragmentOutputReflection output;
    output.name.assign(name_at(info.name));
    output.location = info.location;
    output.format = vertex_format(pointee);
    return reflection.fragment_outputs.push_back(output);
}

Expected<Reflection, Error> Reflector::build() noexcept {
    Reflection reflection(*allocator_);

    for (const EntryPointRecord& record : entry_points_) {
        EntryPointReflection entry;
        entry.name.assign(name_at(record.name));
        entry.stage = record.stage;
        for (u32 axis = 0; axis < 3; ++axis) {
            entry.workgroup[axis] = record.workgroup[axis];
            entry.workgroup_spec_ids[axis] = record.workgroup_spec[axis];
        }
        const Status pushed = reflection.entry_points.push_back(entry);
        if (!pushed.has_value()) {
            return make_unexpected(pushed.error());
        }
    }

    // Only the vertex stage's inputs are attributes. A fragment stage's inputs are interpolants
    // from the stage before it, and reporting them as vertex inputs would make a mesh layout check
    // compare a vertex buffer against a varying.
    const bool has_vertex_stage = [this]() noexcept {
        for (const EntryPointRecord& record : entry_points_) {
            if (record.stage == Stage::Vertex) {
                return true;
            }
        }
        return false;
    }();
    const bool has_fragment_stage = [this]() noexcept {
        for (const EntryPointRecord& record : entry_points_) {
            if (record.stage == Stage::Fragment) {
                return true;
            }
        }
        return false;
    }();

    for (u32 id = 0; id < ids_.size(); ++id) {
        const IdInfo& info = ids_[id];
        if (info.opcode == kOpSpecConstant || info.opcode == kOpSpecConstantTrue ||
            info.opcode == kOpSpecConstantFalse) {
            if (info.spec_id == kNoValue) {
                continue;
            }
            SpecializationConstantReflection constant;
            constant.name.assign(name_at(info.name));
            constant.constant_id = info.spec_id;
            constant.is_boolean = info.opcode != kOpSpecConstant;
            constant.size = constant.is_boolean ? 1 : type_size(info.operand[3]);
            constant.default_value = info.constant;
            const Status pushed = reflection.specialization_constants.push_back(constant);
            if (!pushed.has_value()) {
                return make_unexpected(pushed.error());
            }
            continue;
        }

        if (info.opcode != kOpVariable) {
            continue;
        }
        const u32 pointer_type = info.operand[3];
        if (pointer_type >= ids_.size() || ids_[pointer_type].opcode != kOpTypePointer) {
            continue;
        }
        const u32 storage = ids_[pointer_type].operand[0];

        Status recorded = ok();
        if (storage == kStorageUniform || storage == kStorageStorageBuffer ||
            storage == kStorageUniformConstant) {
            recorded = add_binding(reflection, id);
        } else if (storage == kStoragePushConstant) {
            const u32 pointee = ids_[pointer_type].operand[1];
            PushConstantReflection range;
            range.name.assign(name_at(info.name));
            range.offset = 0;
            range.size = type_size(pointee);
            range.stages = stages_using(id);
            recorded = reflection.push_constants.push_back(range);
        } else if (storage == kStorageInput && has_vertex_stage) {
            recorded = add_interface_variable(reflection, id, storage);
        } else if (storage == kStorageOutput && has_fragment_stage) {
            recorded = add_interface_variable(reflection, id, storage);
        }
        if (!recorded.has_value()) {
            return make_unexpected(recorded.error());
        }
    }

    reflection.instruction_count = instruction_count_;
    reflection.canonicalise();
    return reflection;
}

}  // namespace

Expected<SpirvHeader, Error> spirv_header(Span<const u32> words) noexcept {
    if (words.size() < kSpirvHeaderWords) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a SPIR-V module is at least five words", 0});
    }
    if (words[0] != kSpirvMagic) {
        // A byte-swapped module has the magic reversed. It is rejected rather than converted: the
        // engine never produces one, so seeing one means a build targeted the wrong endianness and
        // silently fixing it would hide that.
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "not a SPIR-V module, or byte-swapped for the wrong host", 0});
    }

    SpirvHeader header;
    header.version = words[1];
    header.generator = words[2];
    header.id_bound = words[3];

    // An id bound larger than the module has words cannot be honest — every id is defined by an
    // instruction of at least two words — and it is what a length mistake looks like. Checked
    // before the id table is sized, because that table is the one allocation proportional to the
    // bound.
    if (header.id_bound == 0 || header.id_bound > words.size()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "the SPIR-V id bound is impossible for a module this size",
                                     static_cast<i64>(header.id_bound)});
    }
    return header;
}

Expected<Reflection, Error> reflect_spirv(Span<const u32> words, Allocator& allocator) noexcept {
    const Expected<SpirvHeader, Error> header = spirv_header(words);
    if (!header.has_value()) {
        return make_unexpected(header.error());
    }

    Reflector reflector(words, allocator);
    const Status collected = reflector.collect(*header);
    if (!collected.has_value()) {
        return make_unexpected(collected.error());
    }

    Expected<Reflection, Error> reflection = reflector.build();
    if (reflection.has_value()) {
        reflection->spirv_version = header->version;
    }
    return reflection;
}

Expected<u32, Error> spirv_instruction_count(Span<const u32> words) noexcept {
    const Expected<SpirvHeader, Error> header = spirv_header(words);
    if (!header.has_value()) {
        return make_unexpected(header.error());
    }
    u32 count = 0;
    u32 at = kSpirvHeaderWords;
    while (at < words.size()) {
        const u32 word_count = words[at] >> 16;
        if (word_count == 0 || at + word_count > words.size()) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "a SPIR-V instruction runs past the module", 0});
        }
        ++count;
        at += word_count;
    }
    return count;
}

Expected<u32, Error> spirv_entry_points(Span<const u32> words,
                                        Array<EntryPointReflection>& out) noexcept {
    const Expected<SpirvHeader, Error> header = spirv_header(words);
    if (!header.has_value()) {
        return make_unexpected(header.error());
    }

    Reflector reflector(words, out.allocator());
    const Status collected = reflector.collect(*header);
    if (!collected.has_value()) {
        return make_unexpected(collected.error());
    }

    out.clear();
    for (const EntryPointRecord& record : reflector.entry_points()) {
        EntryPointReflection entry;
        entry.name.assign(reflector.name_at(record.name));
        entry.stage = record.stage;
        for (u32 axis = 0; axis < 3; ++axis) {
            entry.workgroup[axis] = record.workgroup[axis];
            entry.workgroup_spec_ids[axis] = record.workgroup_spec[axis];
        }
        const Status pushed = out.push_back(entry);
        if (!pushed.has_value()) {
            return make_unexpected(pushed.error());
        }
    }
    return static_cast<u32>(out.size());
}

}  // namespace cy::shader
