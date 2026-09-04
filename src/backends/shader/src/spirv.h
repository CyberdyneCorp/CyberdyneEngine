#pragma once
// The subset of the SPIR-V binary format the reflection parser reads. Task 3.3.
//
// PRIVATE TO src/backends/shader/. These are the numeric values the SPIR-V specification assigns;
// they are written out here rather than pulled from SPIRV-Headers for one reason worth stating:
// this is the only file in the engine that needs them, the values are frozen by the specification
// and can never change, and the alternative is a fetched dependency, a second include path, and a
// version to keep in step — for a table of forty constants.
//
// If ray tracing or mesh shading later needs a wider slice of the format, the argument flips and
// SPIRV-Headers becomes the right answer. The line to watch is this file growing a *category* it
// does not have today, not it growing a few more rows.
//
// Reference: the SPIR-V Specification, "Binary Form" and "Appendix A: Changes".

#include <cy/core/base/types.h>

namespace cy::shader::spirv {

inline constexpr u32 kMagic = 0x0723'0203U;
/// Words before the first instruction: magic, version, generator, bound, schema.
inline constexpr u32 kHeaderWords = 5;

/// The largest id bound the parser will allocate a table for. A module past this is not a shader;
/// it is a buffer that happened to start with the right four bytes.
inline constexpr u32 kMaxIdBound = 1U << 22;

// --- Opcodes ---------------------------------------------------------------------------------

enum Op : u16 {
    OpName = 5,
    OpMemberName = 6,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstantTrue = 41,
    OpConstantFalse = 42,
    OpConstant = 43,
    OpSpecConstantTrue = 48,
    OpSpecConstantFalse = 49,
    OpSpecConstant = 50,
    OpVariable = 59,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpExecutionModeId = 331,
    OpTypeAccelerationStructureKHR = 5341,
};

// --- Execution models ------------------------------------------------------------------------

enum ExecutionModel : u32 {
    ExecutionModelVertex = 0,
    ExecutionModelTessellationControl = 1,
    ExecutionModelTessellationEvaluation = 2,
    ExecutionModelGeometry = 3,
    ExecutionModelFragment = 4,
    ExecutionModelGLCompute = 5,
    ExecutionModelTaskEXT = 5364,
    ExecutionModelMeshEXT = 5365,
};

enum ExecutionMode : u32 {
    ExecutionModeLocalSize = 17,
    ExecutionModeLocalSizeId = 38,
};

// --- Storage classes -------------------------------------------------------------------------

enum StorageClass : u32 {
    StorageClassUniformConstant = 0,
    StorageClassInput = 1,
    StorageClassUniform = 2,
    StorageClassOutput = 3,
    StorageClassWorkgroup = 4,
    StorageClassPushConstant = 9,
    StorageClassStorageBuffer = 12,
};

// --- Decorations -----------------------------------------------------------------------------

enum Decoration : u32 {
    DecorationSpecId = 1,
    DecorationBlock = 2,
    DecorationBufferBlock = 3,
    DecorationArrayStride = 6,
    DecorationMatrixStride = 7,
    DecorationBuiltIn = 11,
    DecorationLocation = 30,
    DecorationBinding = 33,
    DecorationDescriptorSet = 34,
    DecorationOffset = 35,
    DecorationInputAttachmentIndex = 43,
};

/// `Dim` values the parser distinguishes. Only SubpassData changes the descriptor kind; the rest
/// are the same kind with a different view type, which is the RHI's business and not reflection's.
enum Dim : u32 {
    DimSubpassData = 6,
};

/// `OpTypeImage`'s `Sampled` operand. 1 means "used with a sampler", 2 means "used as storage",
/// 0 means "either", which only occurs in kernels.
enum Sampled : u32 {
    SampledWithSampler = 1,
    SampledStorage = 2,
};

/// Split an instruction's first word. `word_count` counts the first word itself, so an instruction
/// with a word count of zero would not advance and is rejected by the caller.
struct InstructionHeader {
    u16 opcode = 0;
    u16 word_count = 0;
};

[[nodiscard]] constexpr InstructionHeader decode_instruction(u32 word) noexcept {
    return InstructionHeader{static_cast<u16>(word & 0xFFFFU), static_cast<u16>(word >> 16U)};
}

}  // namespace cy::shader::spirv
