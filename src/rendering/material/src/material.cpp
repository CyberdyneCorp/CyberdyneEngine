// Material programs, their parameter layouts, and the GPU material table. See
// cy/rendering/material/material.h.

#include <cy/rendering/material/material.h>

#include <cy/core/base/assert.h>

#include <cstring>

namespace cy::rendering {
namespace {

constexpr const char* kParameterKindNames[] = {"Float", "Vec2", "Vec3", "Vec4",
                                               "Color", "Int",  "Bool", "Texture"};
static_assert(sizeof(kParameterKindNames) / sizeof(kParameterKindNames[0]) ==
              static_cast<usize>(ParameterKind::Count));

[[nodiscard]] u32 align_to(u32 value, u32 alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

}  // namespace

const char* parameter_kind_name(ParameterKind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    return (index < static_cast<usize>(ParameterKind::Count)) ? kParameterKindNames[index]
                                                              : "<invalid>";
}

u32 parameter_byte_size(ParameterKind kind) noexcept {
    switch (kind) {
        case ParameterKind::Float:
        case ParameterKind::Int:
        case ParameterKind::Bool:
        case ParameterKind::Texture:
            // A bool is four bytes and not one. std430 has no one-byte scalar a shader can read
            // portably, and a packed bool would make the block's layout depend on the rules of
            // whichever compiler read it.
            return 4;
        case ParameterKind::Vec2:
            return 8;
        case ParameterKind::Vec3:
            // Twelve bytes, aligned to sixteen by `add_parameter` — std430's rule for a three-
            // component vector, applied where the offset is assigned rather than left to the
            // shader compiler and the CPU to agree about by luck.
            return 12;
        case ParameterKind::Vec4:
        case ParameterKind::Color:
            return 16;
        case ParameterKind::Count:
            break;
    }
    return 0;
}

// --- Programs -------------------------------------------------------------------------------

MaterialProgram::MaterialProgram(Allocator& allocator) noexcept : parameters_(allocator) {}

Status MaterialProgram::initialize(Name name, ShadingModel model, BlendMode blend) noexcept {
    name_ = name;
    model_ = model;
    blend_ = blend;
    parameters_.clear();
    used_bytes_ = 0;
    return ok();
}

Expected<ParameterId, Error> MaterialProgram::add_parameter(const char* name, ParameterKind kind,
                                                            bool is_static) noexcept {
    if (name == nullptr || *name == '\0') {
        return fail(ErrorCode::InvalidArgument, "a parameter needs a name to be addressed by");
    }
    if (parameters_.size() >= kMaxMaterialParameters) {
        return fail(ErrorCode::OutOfRange, "too many parameters for one material program");
    }
    const ParameterId id = parameter_id(name);
    const Name interned = Name::intern(name);
    for (const MaterialParameter& existing : parameters_) {
        if (existing.id != id) {
            continue;
        }
        // Same hash: either the same parameter declared twice, or two names that collide. Both are
        // refused, and they are refused HERE because this is the only moment either is cheap — at
        // run time a collision addresses the wrong bytes and looks like a shading bug.
        return (existing.name == interned)
                   ? fail(ErrorCode::AlreadyExists, "that parameter is already declared")
                   : fail(ErrorCode::AlreadyExists,
                          "two parameter names hash to one identifier; rename one");
    }

    const u32 size = parameter_byte_size(kind);
    // std430: a scalar aligns to 4, a vec2 to 8, and vec3 and vec4 both to 16.
    u32 alignment = 4U;
    if (size >= 12U) {
        alignment = 16U;
    } else if (size == 8U) {
        alignment = 8U;
    }
    const u32 offset = align_to(used_bytes_, alignment);
    if (offset + size > kMaterialBlockBytes) {
        return fail(ErrorCode::OutOfRange,
                    "the material's parameter block is full; a program that needs more is a "
                    "program whose parameters belong in a texture");
    }

    MaterialParameter parameter;
    parameter.name = interned;
    parameter.id = id;
    parameter.kind = kind;
    parameter.offset = offset;
    parameter.is_static = is_static;
    if (Status pushed = parameters_.push_back(parameter); !pushed) {
        return make_unexpected(pushed.error());
    }
    used_bytes_ = offset + size;
    return id;
}

const MaterialParameter* MaterialProgram::find(ParameterId id) const noexcept {
    for (const MaterialParameter& parameter : parameters_) {
        if (parameter.id == id) {
            return &parameter;
        }
    }
    return nullptr;
}

u32 MaterialProgram::static_bool_count() const noexcept {
    u32 count = 0;
    for (const MaterialParameter& parameter : parameters_) {
        if (parameter.is_static && parameter.kind == ParameterKind::Bool) {
            ++count;
        }
    }
    return count;
}

u64 MaterialProgram::permutation_count() const noexcept {
    u64 total = 1;
    for (const MaterialParameter& parameter : parameters_) {
        if (!parameter.is_static) {
            continue;
        }
        // Two per static boolean. A static parameter of another kind is a specialization constant
        // whose value set the program does not enumerate, so it contributes one — counting it as
        // unbounded would make the budget check useless rather than strict.
        if (parameter.kind == ParameterKind::Bool) {
            total *= 2;
        }
    }
    return total;
}

void MaterialProgram::mark_referenced(ParameterId id, bool referenced) noexcept {
    for (MaterialParameter& parameter : parameters_) {
        if (parameter.id == id) {
            parameter.referenced = referenced;
            return;
        }
    }
}

// --- The table ------------------------------------------------------------------------------

MaterialTable::MaterialTable(Allocator& allocator) noexcept
    : storage_(allocator), free_slots_(allocator) {}

Status MaterialTable::initialize(u32 capacity) noexcept {
    if (capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "a material table with no slots holds no material");
    }
    if (Status sized = storage_.resize(static_cast<usize>(capacity) * kMaterialBlockBytes);
        !sized) {
        return sized;
    }
    std::memset(storage_.data(), 0, storage_.size());
    free_slots_.clear();
    capacity_ = capacity;
    next_ = 0;
    live_ = 0;
    clear_dirty();
    return ok();
}

Expected<u32, Error> MaterialTable::allocate() noexcept {
    u32 index = 0;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else if (next_ < capacity_) {
        index = next_++;
    } else {
        return fail(ErrorCode::OutOfMemory, "the material table is full");
    }
    std::memset(storage_.data() + (static_cast<usize>(index) * kMaterialBlockBytes), 0,
                kMaterialBlockBytes);
    mark_dirty(index * kMaterialBlockBytes, kMaterialBlockBytes);
    ++live_;
    return index;
}

Expected<u32, Error> MaterialTable::instantiate(u32 parent) noexcept {
    if (parent >= capacity_) {
        return fail(ErrorCode::OutOfRange, "no such parent material");
    }
    Expected<u32, Error> index = allocate();
    if (!index) {
        return index;
    }
    // "Instances SHALL be creatable at runtime without compilation, and any number SHALL share one
    // program." A copy of 256 bytes, and nothing to invalidate.
    std::memcpy(storage_.data() + (static_cast<usize>(*index) * kMaterialBlockBytes),
                storage_.data() + (static_cast<usize>(parent) * kMaterialBlockBytes),
                kMaterialBlockBytes);
    mark_dirty(*index * kMaterialBlockBytes, kMaterialBlockBytes);
    return index;
}

Status MaterialTable::release(u32 index) noexcept {
    if (index >= capacity_) {
        return fail(ErrorCode::OutOfRange, "no such material slot");
    }
    std::memset(storage_.data() + (static_cast<usize>(index) * kMaterialBlockBytes), 0,
                kMaterialBlockBytes);
    mark_dirty(index * kMaterialBlockBytes, kMaterialBlockBytes);
    if (live_ > 0) {
        --live_;
    }
    return free_slots_.push_back(index);
}

Expected<u32, Error> MaterialTable::resolve_offset(const MaterialProgram& program, u32 index,
                                                   ParameterId id,
                                                   ParameterKind expected) const noexcept {
    if (index >= capacity_) {
        return fail(ErrorCode::OutOfRange, "no such material slot");
    }
    const MaterialParameter* parameter = program.find(id);
    if (parameter == nullptr) {
        return fail(ErrorCode::NotFound, "that program declares no such parameter");
    }
    if (parameter->kind != expected) {
        // A `Vec3` written where a `Float` was declared overwrites the two parameters after it, and
        // the symptom is a material whose roughness changes when its tint does.
        return fail(ErrorCode::InvalidArgument, "the parameter was declared with a different kind");
    }
    return (index * kMaterialBlockBytes) + parameter->offset;
}

void MaterialTable::mark_dirty(u32 offset, u32 size) noexcept {
    if (dirty_end_ == 0 && dirty_begin_ == 0) {
        dirty_begin_ = offset;
        dirty_end_ = offset + size;
        return;
    }
    dirty_begin_ = (offset < dirty_begin_) ? offset : dirty_begin_;
    dirty_end_ = ((offset + size) > dirty_end_) ? (offset + size) : dirty_end_;
}

bool MaterialTable::dirty_range(u32& out_offset, u32& out_size) const noexcept {
    if (dirty_end_ <= dirty_begin_) {
        return false;
    }
    out_offset = dirty_begin_;
    out_size = dirty_end_ - dirty_begin_;
    return true;
}

void MaterialTable::clear_dirty() noexcept {
    dirty_begin_ = 0;
    dirty_end_ = 0;
}

namespace {

template <class T>
[[nodiscard]] Status write_at(Array<u8>& storage, u32 offset, const T& value) noexcept {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
    return ok();
}

template <class T>
[[nodiscard]] T read_at(const Array<u8>& storage, u32 offset) noexcept {
    T value{};
    std::memcpy(&value, storage.data() + offset, sizeof(T));
    return value;
}

}  // namespace

Status MaterialTable::set_float(const MaterialProgram& program, u32 index, ParameterId id,
                                f32 value) noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Float);
    if (!offset) {
        return Status{make_unexpected(offset.error())};
    }
    mark_dirty(*offset, sizeof(f32));
    return write_at(storage_, *offset, value);
}

Status MaterialTable::set_vec4(const MaterialProgram& program, u32 index, ParameterId id,
                               Vec4 value) noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Vec4);
    if (!offset) {
        return Status{make_unexpected(offset.error())};
    }
    mark_dirty(*offset, sizeof(Vec4));
    return write_at(storage_, *offset, value);
}

Status MaterialTable::set_color(const MaterialProgram& program, u32 index, ParameterId id,
                                Vec4 linear_color) noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Color);
    if (!offset) {
        return Status{make_unexpected(offset.error())};
    }
    mark_dirty(*offset, sizeof(Vec4));
    return write_at(storage_, *offset, linear_color);
}

Status MaterialTable::set_texture(const MaterialProgram& program, u32 index, ParameterId id,
                                  u32 bindless_index) noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Texture);
    if (!offset) {
        return Status{make_unexpected(offset.error())};
    }
    mark_dirty(*offset, sizeof(u32));
    return write_at(storage_, *offset, bindless_index);
}

Status MaterialTable::set_int(const MaterialProgram& program, u32 index, ParameterId id,
                              i32 value) noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Int);
    if (!offset) {
        return Status{make_unexpected(offset.error())};
    }
    mark_dirty(*offset, sizeof(i32));
    return write_at(storage_, *offset, value);
}

Status MaterialTable::set_bool(const MaterialProgram& program, u32 index, ParameterId id,
                               bool value) noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Bool);
    if (!offset) {
        return Status{make_unexpected(offset.error())};
    }
    mark_dirty(*offset, sizeof(u32));
    return write_at(storage_, *offset, value ? 1U : 0U);
}

Expected<f32, Error> MaterialTable::get_float(const MaterialProgram& program, u32 index,
                                              ParameterId id) const noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Float);
    if (!offset) {
        return make_unexpected(offset.error());
    }
    return read_at<f32>(storage_, *offset);
}

Expected<Vec4, Error> MaterialTable::get_vec4(const MaterialProgram& program, u32 index,
                                              ParameterId id) const noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Vec4);
    if (!offset) {
        // A colour is stored identically; asking for one as a `Vec4` is refused so that a caller
        // reading a colour says so and validation can see what a slot is used as.
        return make_unexpected(offset.error());
    }
    return read_at<Vec4>(storage_, *offset);
}

Expected<u32, Error> MaterialTable::get_texture(const MaterialProgram& program, u32 index,
                                                ParameterId id) const noexcept {
    Expected<u32, Error> offset = resolve_offset(program, index, id, ParameterKind::Texture);
    if (!offset) {
        return make_unexpected(offset.error());
    }
    return read_at<u32>(storage_, *offset);
}

}  // namespace cy::rendering
