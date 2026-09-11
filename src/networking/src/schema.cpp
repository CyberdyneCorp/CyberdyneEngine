#include <cy/networking/schema.h>

#include <cmath>
#include <cstring>
#include <utility>

namespace cy::net {
namespace {

template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object == nullptr) {
        return;
    }
    object->~T();
    allocator.deallocate(object, sizeof(T), alignof(T));
}

inline constexpr u64 kFnvOffset = 0xcbf29ce484222325ULL;
inline constexpr u64 kFnvPrime = 0x100000001b3ULL;

[[nodiscard]] u64 fold(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] constexpr bool is_floating(reflect::FieldKind kind) noexcept {
    return kind == reflect::FieldKind::F32 || kind == reflect::FieldKind::F64;
}

[[nodiscard]] constexpr bool is_integral(reflect::FieldKind kind) noexcept {
    switch (kind) {
        case reflect::FieldKind::Bool:
        case reflect::FieldKind::I8:
        case reflect::FieldKind::I16:
        case reflect::FieldKind::I32:
        case reflect::FieldKind::I64:
        case reflect::FieldKind::U8:
        case reflect::FieldKind::U16:
        case reflect::FieldKind::U32:
        case reflect::FieldKind::U64:
        case reflect::FieldKind::Enum:
        case reflect::FieldKind::Flags:
            return true;
        default:
            return false;
    }
}

/// The unsigned value of an integral field of `size` bytes at `address`.
[[nodiscard]] u64 read_integer(const u8* address, u32 size) noexcept {
    u64 value = 0;
    const u32 width = size > 8 ? 8 : size;
    std::memcpy(static_cast<void*>(&value), static_cast<const void*>(address), width);
    return value;
}

void write_integer(u8* address, u32 size, u64 value) noexcept {
    const u32 width = size > 8 ? 8 : size;
    std::memcpy(static_cast<void*>(address), static_cast<const void*>(&value), width);
}

[[nodiscard]] f64 read_float(const u8* address, reflect::FieldKind kind) noexcept {
    if (kind == reflect::FieldKind::F32) {
        f32 value = 0.0F;
        std::memcpy(static_cast<void*>(&value), static_cast<const void*>(address), sizeof(f32));
        return static_cast<f64>(value);
    }
    f64 value = 0.0;
    std::memcpy(static_cast<void*>(&value), static_cast<const void*>(address), sizeof(f64));
    return value;
}

void write_float(u8* address, reflect::FieldKind kind, f64 value) noexcept {
    if (kind == reflect::FieldKind::F32) {
        const f32 narrow = static_cast<f32>(value);
        std::memcpy(static_cast<void*>(address), static_cast<const void*>(&narrow), sizeof(f32));
        return;
    }
    std::memcpy(static_cast<void*>(address), static_cast<const void*>(&value), sizeof(f64));
}

/// The largest integer `bits` can hold. `bits` is validated to be 1..64 before this is called.
[[nodiscard]] u64 bit_limit(u32 bits) noexcept {
    return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1ULL);
}

/// Quantise one scalar. **`std::floor` and nothing else**: `simulation-and-determinism` and M9's
/// spike (design.md §1.3) permit `floor`, `fabs`, `nearbyint` and their measured-exact neighbours
/// to authoritative paths and forbid the thirteen this libm does not round correctly. A quantiser
/// written with `std::round`'s cousins would be a cross-build difference in the one number a peer
/// compares.
[[nodiscard]] u64 quantise(f64 value, f64 minimum, f64 scale, u64 limit) noexcept {
    const f64 shifted = (value - minimum) * scale;
    if (!(shifted > 0.0)) {
        return 0;
    }
    const f64 floored = std::floor(shifted + 0.5);
    const f64 capped = static_cast<f64>(limit);
    return floored >= capped ? limit : static_cast<u64>(floored);
}

[[nodiscard]] f64 dequantise(u64 raw, f64 minimum, f64 inverse_scale) noexcept {
    return minimum + (static_cast<f64>(raw) * inverse_scale);
}

}  // namespace

const char* field_encoder_name(FieldEncoder encoder) noexcept {
    switch (encoder) {
        case FieldEncoder::Raw:
            return "Raw";
        case FieldEncoder::QuantisedScalar:
            return "QuantisedScalar";
        case FieldEncoder::QuantisedVector:
            return "QuantisedVector";
        case FieldEncoder::CompressedQuaternion:
            return "CompressedQuaternion";
        case FieldEncoder::DictionaryIndex:
            return "DictionaryIndex";
        case FieldEncoder::Bitfield:
            return "Bitfield";
    }
    return "unknown";
}

const char* schema_error_name(SchemaError error) noexcept {
    switch (error) {
        case SchemaError::None:
            return "None";
        case SchemaError::FieldRemoved:
            return "FieldRemoved";
        case SchemaError::RangeCannotRepresent:
            return "RangeCannotRepresent";
        case SchemaError::EncoderDoesNotApply:
            return "EncoderDoesNotApply";
        case SchemaError::BitsOutOfRange:
            return "BitsOutOfRange";
        case SchemaError::TooManyFields:
            return "TooManyFields";
        case SchemaError::DuplicateField:
            return "DuplicateField";
    }
    return "unknown";
}

// --- BitWriter / BitReader -----------------------------------------------------------------------

Status BitWriter::write(u64 value, u32 bits) noexcept {
    for (u32 index = 0; index < bits; ++index) {
        const u32 bit = static_cast<u32>((value >> index) & 1ULL);
        partial_ |= bit << partial_bits_;
        ++partial_bits_;
        ++bits_;
        if (partial_bits_ == 8) {
            if (Status pushed = out_->push_back(static_cast<u8>(partial_)); !pushed) {
                return pushed;
            }
            partial_ = 0;
            partial_bits_ = 0;
        }
    }
    return ok();
}

Status BitWriter::write_bytes(const u8* bytes, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (Status written = write(bytes[index], 8); !written) {
            return written;
        }
    }
    return ok();
}

Status BitWriter::flush() noexcept {
    if (partial_bits_ == 0) {
        return ok();
    }
    const Status pushed = out_->push_back(static_cast<u8>(partial_));
    partial_ = 0;
    partial_bits_ = 0;
    return pushed;
}

bool BitReader::read(u32 bits, u64& out) noexcept {
    u64 value = 0;
    for (u32 index = 0; index < bits; ++index) {
        const u64 position = bits_ + index;
        const auto byte = static_cast<usize>(position / 8);
        if (byte >= bytes_.size()) {
            return false;
        }
        const u64 bit = (bytes_[byte] >> (position % 8)) & 1ULL;
        value |= bit << index;
    }
    bits_ += bits;
    out = value;
    return true;
}

bool BitReader::read_bytes(u8* out, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        u64 byte = 0;
        if (!read(8, byte)) {
            return false;
        }
        out[index] = static_cast<u8>(byte);
    }
    return true;
}

// --- Compilation ---------------------------------------------------------------------------------

namespace {

struct Resolution {
    SchemaError error = SchemaError::None;
    const char* detail = "";
};

/// The per-encoder half of `compile()`. Separated so the outer function is the sequence of checks
/// it is, rather than a switch nested inside a loop nested inside a validation.
[[nodiscard]] Resolution resolve_encoder(const reflect::FieldInfo& info,
                                         const FieldSchema& declared, CompiledField& out) noexcept {
    out.encoder = declared.encoder;
    out.component_bits = declared.parameters.bits;
    out.minimum = declared.parameters.minimum;

    switch (declared.encoder) {
        case FieldEncoder::Raw:
            out.bits = info.size * 8;
            out.component_bits = out.bits;
            return {};

        case FieldEncoder::Bitfield:
        case FieldEncoder::DictionaryIndex: {
            if (!is_integral(info.kind)) {
                return {SchemaError::EncoderDoesNotApply,
                        "a bitfield or dictionary index encodes an integral field"};
            }
            if (declared.parameters.bits == 0 || declared.parameters.bits > 64 ||
                declared.parameters.bits > info.size * 8) {
                return {SchemaError::BitsOutOfRange,
                        "between one bit and the field's own width, inclusive"};
            }
            out.bits = declared.parameters.bits;
            return {};
        }

        case FieldEncoder::QuantisedScalar:
        case FieldEncoder::QuantisedVector:
        case FieldEncoder::CompressedQuaternion: {
            if (!is_floating(info.kind)) {
                return {SchemaError::EncoderDoesNotApply,
                        "a quantised encoder needs a floating-point field"};
            }
            if (declared.parameters.bits == 0 || declared.parameters.bits > 32) {
                return {SchemaError::BitsOutOfRange,
                        "between one and thirty-two bits per component"};
            }
            // One component for a scalar; three for a vector, and three for a quaternion because
            // smallest-three drops the fourth and reconstructs it. The two threes are the same
            // number for different reasons, which is why this is a comparison rather than a table.
            const u32 components = declared.encoder == FieldEncoder::QuantisedScalar ? 1U : 3U;
            if (declared.encoder != FieldEncoder::QuantisedScalar) {
                const u32 element = info.kind == reflect::FieldKind::F32 ? 4U : 8U;
                const u32 wanted = declared.encoder == FieldEncoder::QuantisedVector ? 3U : 4U;
                if (info.size != element * wanted) {
                    return {SchemaError::EncoderDoesNotApply,
                            "a vector encoder needs three components and a quaternion four"};
                }
            }
            if (declared.encoder == FieldEncoder::CompressedQuaternion) {
                // Smallest-three: two bits name the dropped component, and the other three are
                // quantised across [-1/sqrt(2), 1/sqrt(2)].
                out.bits = 2 + (components * declared.parameters.bits);
                out.minimum = -0.7071067811865476;
                const f64 span = 2.0 * 0.7071067811865476;
                out.scale = static_cast<f64>(bit_limit(declared.parameters.bits)) / span;
                out.inverse_scale = span / static_cast<f64>(bit_limit(declared.parameters.bits));
                return {};
            }
            const f64 span = declared.parameters.maximum - declared.parameters.minimum;
            if (!(span > 0.0)) {
                return {SchemaError::RangeCannotRepresent,
                        "a quantised range needs a maximum above its minimum"};
            }
            out.bits = components * declared.parameters.bits;
            out.scale = static_cast<f64>(bit_limit(declared.parameters.bits)) / span;
            out.inverse_scale = span / static_cast<f64>(bit_limit(declared.parameters.bits));
            return {};
        }
    }
    return {SchemaError::EncoderDoesNotApply, "unknown encoder"};
}

/// The declared-bounds check. `networking-and-replication`: "a range that cannot represent the
/// field's declared bounds SHALL be a cook error".
[[nodiscard]] bool range_covers_declared_bounds(const reflect::FieldInfo& info,
                                                const FieldSchema& declared) noexcept {
    if (declared.encoder != FieldEncoder::QuantisedScalar &&
        declared.encoder != FieldEncoder::QuantisedVector) {
        return true;
    }
    if (!info.attributes.declares(reflect::AttributeKind::Range)) {
        return true;
    }
    return declared.parameters.minimum <= info.attributes.range.minimum &&
           declared.parameters.maximum >= info.attributes.range.maximum;
}

}  // namespace

Expected<u32, SchemaRejection> CompiledSchema::compile(const reflect::TypeInfo& type,
                                                       const ReplicationSchema& schema) noexcept {
    SchemaRejection rejection;
    rejection.component = schema.component;

    if (schema.fields.size() > kMaxSchemaFields) {
        rejection.error = SchemaError::TooManyFields;
        rejection.detail = "a replicated component declares at most sixty-four fields";
        return make_unexpected(rejection);
    }

    component_ = schema.component;
    version_ = schema.version;
    type_ = type.id;
    instance_size_ = type.size;
    fields_.clear();

    for (const auto& declared : schema.fields) {
        rejection.field_id = declared.field;

        for (auto& field : fields_) {
            if (field.field == declared.field) {
                rejection.error = SchemaError::DuplicateField;
                rejection.detail = "two entries of one schema name the same field";
                return make_unexpected(rejection);
            }
        }

        const reflect::FieldInfo* info = type.find_field(declared.field);
        if (info == nullptr) {
            rejection.error = SchemaError::FieldRemoved;
            rejection.detail =
                "the reflected type has no field with that identifier; a rename would have kept it";
            return make_unexpected(rejection);
        }
        rejection.field = info->name;

        if (!range_covers_declared_bounds(*info, declared)) {
            rejection.error = SchemaError::RangeCannotRepresent;
            rejection.detail =
                "the encoder's interval is narrower than the field's own declared range";
            return make_unexpected(rejection);
        }

        CompiledField compiled;
        compiled.field = declared.field;
        compiled.offset = info->offset;
        compiled.size = info->size;
        compiled.kind = info->kind;
        compiled.condition = declared.condition;
        compiled.filter = declared.filter;
        compiled.priority_contribution = declared.priority_contribution;

        const Resolution resolution = resolve_encoder(*info, declared, compiled);
        if (resolution.error != SchemaError::None) {
            rejection.error = resolution.error;
            rejection.detail = resolution.detail;
            return make_unexpected(rejection);
        }
        if (!fields_.push_back(compiled)) {
            rejection.error = SchemaError::TooManyFields;
            rejection.detail = "out of memory compiling the schema";
            return make_unexpected(rejection);
        }
    }
    return static_cast<u32>(fields_.size());
}

ChangeMask CompiledSchema::changes(const void* instance, const void* baseline,
                                   bool owner) const noexcept {
    ChangeMask mask = 0;
    const auto* current = static_cast<const u8*>(instance);
    const auto* previous = static_cast<const u8*>(baseline);
    for (usize index = 0; index < fields_.size(); ++index) {
        const CompiledField& field = fields_[index];
        if (field.filter == TargetFilter::OwnerOnly && !owner) {
            continue;
        }
        if (field.filter == TargetFilter::ExceptOwner && owner) {
            continue;
        }
        if (field.condition == SendCondition::Always || previous == nullptr) {
            mask |= 1ULL << index;
            continue;
        }
        if (std::memcmp(current + field.offset, previous + field.offset, field.size) != 0) {
            mask |= 1ULL << index;
        }
    }
    return mask;
}

namespace {

/// Encode one field's value. Extracted so `encode_instance()` is a loop and a mask rather than a
/// loop around a switch around four arithmetic cases.
[[nodiscard]] Status encode_field(const CompiledField& field, const u8* address,
                                  BitWriter& writer) noexcept {
    switch (field.encoder) {
        case FieldEncoder::Raw:
            return writer.write_bytes(address, field.size);

        case FieldEncoder::Bitfield:
        case FieldEncoder::DictionaryIndex:
            return writer.write(read_integer(address, field.size), field.bits);

        case FieldEncoder::QuantisedScalar:
            return writer.write(quantise(read_float(address, field.kind), field.minimum,
                                         field.scale, bit_limit(field.component_bits)),
                                field.component_bits);

        case FieldEncoder::QuantisedVector: {
            const u32 element = field.kind == reflect::FieldKind::F32 ? 4U : 8U;
            for (u32 component = 0; component < 3; ++component) {
                const f64 value =
                    read_float(address + (static_cast<usize>(component) * element), field.kind);
                if (Status written = writer.write(quantise(value, field.minimum, field.scale,
                                                           bit_limit(field.component_bits)),
                                                  field.component_bits);
                    !written) {
                    return written;
                }
            }
            return ok();
        }

        case FieldEncoder::CompressedQuaternion: {
            const u32 element = field.kind == reflect::FieldKind::F32 ? 4U : 8U;
            f64 values[4] = {};
            u32 largest = 0;
            for (u32 component = 0; component < 4; ++component) {
                values[component] =
                    read_float(address + (static_cast<usize>(component) * element), field.kind);
                if (std::fabs(values[component]) > std::fabs(values[largest])) {
                    largest = component;
                }
            }
            // The dropped component's sign is recovered by convention: the kept three are negated
            // when the largest is negative, so the reconstruction always takes the positive root.
            const f64 sign = values[largest] < 0.0 ? -1.0 : 1.0;
            if (Status written = writer.write(largest, 2); !written) {
                return written;
            }
            for (u32 component = 0; component < 4; ++component) {
                if (component == largest) {
                    continue;
                }
                if (Status written =
                        writer.write(quantise(values[component] * sign, field.minimum, field.scale,
                                              bit_limit(field.component_bits)),
                                     field.component_bits);
                    !written) {
                    return written;
                }
            }
            return ok();
        }
    }
    return fail(ErrorCode::Internal, "networking: unknown encoder");
}

[[nodiscard]] bool decode_field(const CompiledField& field, BitReader& reader,
                                u8* address) noexcept {
    switch (field.encoder) {
        case FieldEncoder::Raw:
            return reader.read_bytes(address, field.size);

        case FieldEncoder::Bitfield:
        case FieldEncoder::DictionaryIndex: {
            u64 raw = 0;
            if (!reader.read(field.bits, raw)) {
                return false;
            }
            write_integer(address, field.size, raw);
            return true;
        }

        case FieldEncoder::QuantisedScalar: {
            u64 raw = 0;
            if (!reader.read(field.component_bits, raw)) {
                return false;
            }
            write_float(address, field.kind, dequantise(raw, field.minimum, field.inverse_scale));
            return true;
        }

        case FieldEncoder::QuantisedVector: {
            const u32 element = field.kind == reflect::FieldKind::F32 ? 4U : 8U;
            for (u32 component = 0; component < 3; ++component) {
                u64 raw = 0;
                if (!reader.read(field.component_bits, raw)) {
                    return false;
                }
                write_float(address + (static_cast<usize>(component) * element), field.kind,
                            dequantise(raw, field.minimum, field.inverse_scale));
            }
            return true;
        }

        case FieldEncoder::CompressedQuaternion: {
            const u32 element = field.kind == reflect::FieldKind::F32 ? 4U : 8U;
            u64 largest = 0;
            if (!reader.read(2, largest)) {
                return false;
            }
            f64 values[4] = {};
            f64 sum = 0.0;
            for (u32 component = 0; component < 4; ++component) {
                if (component == largest) {
                    continue;
                }
                u64 raw = 0;
                if (!reader.read(field.component_bits, raw)) {
                    return false;
                }
                values[component] = dequantise(raw, field.minimum, field.inverse_scale);
                sum += values[component] * values[component];
            }
            const f64 remainder = sum >= 1.0 ? 0.0 : 1.0 - sum;
            values[largest] = std::sqrt(remainder);
            for (u32 component = 0; component < 4; ++component) {
                write_float(address + (static_cast<usize>(component) * element), field.kind,
                            values[component]);
            }
            return true;
        }
    }
    return false;
}

}  // namespace

Expected<u32, Error> CompiledSchema::encode_instance(const void* instance, ChangeMask mask,
                                                     BitWriter& writer) const noexcept {
    const u64 before = writer.bits_written();
    if (Status written = writer.write(mask, static_cast<u32>(fields_.size())); !written) {
        return make_unexpected(written.error());
    }
    const auto* bytes = static_cast<const u8*>(instance);
    for (usize index = 0; index < fields_.size(); ++index) {
        if ((mask & (1ULL << index)) == 0) {
            continue;
        }
        const CompiledField& field = fields_[index];
        if (Status written = encode_field(field, bytes + field.offset, writer); !written) {
            return make_unexpected(written.error());
        }
    }
    return static_cast<u32>(writer.bits_written() - before);
}

Status CompiledSchema::decode_instance(BitReader& reader, void* instance,
                                       ChangeMask& mask) const noexcept {
    u64 read_mask = 0;
    if (!reader.read(static_cast<u32>(fields_.size()), read_mask)) {
        return fail(ErrorCode::OutOfRange, "networking: a delta shorter than its change mask");
    }
    mask = read_mask;
    auto* bytes = static_cast<u8*>(instance);
    for (usize index = 0; index < fields_.size(); ++index) {
        if ((mask & (1ULL << index)) == 0) {
            continue;
        }
        const CompiledField& field = fields_[index];
        if (!decode_field(field, reader, bytes + field.offset)) {
            return fail(ErrorCode::OutOfRange,
                        "networking: a delta that ends inside one of its own fields");
        }
    }
    return ok();
}

u32 CompiledSchema::theoretical_bits() const noexcept {
    u32 total = static_cast<u32>(fields_.size());
    for (const auto& field : fields_) {
        total += field.bits;
    }
    return total;
}

u32 CompiledSchema::bits_for(ChangeMask mask) const noexcept {
    u32 total = static_cast<u32>(fields_.size());
    for (usize index = 0; index < fields_.size(); ++index) {
        if ((mask & (1ULL << index)) != 0) {
            total += fields_[index].bits;
        }
    }
    return total;
}

u64 CompiledSchema::identity() const noexcept {
    u64 hash = fold(kFnvOffset, type_.value());
    hash = fold(hash, version_);
    for (const auto& field : fields_) {
        hash = fold(hash, field.field.value());
        hash = fold(hash, static_cast<u64>(field.encoder));
        hash = fold(hash, field.bits);
        hash = fold(hash, static_cast<u64>(field.filter));
        hash = fold(hash, static_cast<u64>(field.condition));
    }
    return hash;
}

// --- SchemaSet -----------------------------------------------------------------------------------

SchemaSet::~SchemaSet() {
    for (auto& schema : schemas_) {
        unmake(*allocator_, schema);
    }
}

Expected<u32, SchemaRejection> SchemaSet::add(const reflect::TypeInfo& type,
                                              const ReplicationSchema& schema) noexcept {
    auto* compiled = make<CompiledSchema>(*allocator_, *allocator_);
    if (compiled == nullptr) {
        SchemaRejection rejection;
        rejection.error = SchemaError::TooManyFields;
        rejection.component = schema.component;
        rejection.detail = "out of memory";
        return make_unexpected(rejection);
    }
    Expected<u32, SchemaRejection> result = compiled->compile(type, schema);
    if (!result) {
        const SchemaRejection rejection = result.error();
        unmake(*allocator_, compiled);
        return make_unexpected(rejection);
    }
    if (!schemas_.push_back(compiled)) {
        unmake(*allocator_, compiled);
        SchemaRejection rejection;
        rejection.error = SchemaError::TooManyFields;
        rejection.component = schema.component;
        rejection.detail = "out of memory";
        return make_unexpected(rejection);
    }
    return static_cast<u32>(schemas_.size() - 1);
}

const CompiledSchema* SchemaSet::find(reflect::TypeId type) const noexcept {
    for (auto* schema : schemas_) {
        if (schema->type() == type) {
            return schema;
        }
    }
    return nullptr;
}

u64 SchemaSet::identity() const noexcept {
    if (schemas_.empty()) {
        return kEmptySchemaSetHash;
    }
    u64 hash = kEmptySchemaSetHash;
    for (auto* schema : schemas_) {
        hash = fold(hash, schema->identity());
    }
    // Zero is reserved for "never computed" — `mode.h`'s `verify_mode()` refuses it — so a set
    // whose fold happens to land there is nudged rather than silently reported as unconfigured.
    return hash == 0 ? kEmptySchemaSetHash : hash;
}

}  // namespace cy::net
