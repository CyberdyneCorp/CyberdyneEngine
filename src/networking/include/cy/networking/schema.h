#pragma once
// Replication schemas: compiled, keyed by FieldId, validated against reflection. M9 task 4.3.
//
// ================================================================================================
// COMPILED, NOT INTERPRETED — AND THE REASON IS A LOOP RATHER THAN A TASTE
// ================================================================================================
//
// `networking-and-replication` — "Replication schemas": "Schemas SHALL be **compiled** to
// serialisation and deserialisation code, not interpreted per field per entity, so that replicating
// a thousand instances of a component is a loop over a packed array with a known encoder."
//
// A `CompiledSchema` is therefore an array of `CompiledField` — a byte offset, a width, an encoder
// and its already-computed scale — and `encode_instance()` walks it. Nothing in the hot path
// touches a `TypeInfo`, resolves a `FieldId` or takes a branch on a field name. The reflection
// lookup happens exactly once, in `compile()`, at cook time.
//
// ================================================================================================
// FIELDS ARE KEYED BY FieldId, SO A RENAME IS NOT DRIFT
// ================================================================================================
//
// "Schemas SHALL identify fields by FieldId ... so that renaming a field does not invalidate a
// schema, and so that schema drift means a field genuinely removed or changed rather than merely
// renamed." `core-type-system` assigns those numbers from the committed manifest and nothing here
// derives one from a name — there is no way to, which is the point.
//
// ================================================================================================
// A RANGE THAT CANNOT REPRESENT THE FIELD'S BOUNDS IS A COOK ERROR, NOT A ROUNDING
// ================================================================================================
//
// "a field that no longer exists, or a range that cannot represent the field's declared bounds,
// SHALL be a cook error." So `compile()` reads the field's own `RangeAttribute` when it declares
// one and refuses a quantisation whose interval does not cover it. The alternative — clamping — is
// a position that silently stops moving at the edge of the arena, found by a player rather than by
// a build.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/type_info.h>

namespace cy::net {

/// The largest number of replicated fields one component may declare. Sixty-four because the change
/// mask is one `u64`: a mask that needed two words would make "which fields are present" two
/// comparisons everywhere it is asked, for a component shape nobody has.
inline constexpr u32 kMaxSchemaFields = 64;

/// `networking-and-replication`'s encoder list, verbatim.
enum class FieldEncoder : u8 {
    /// The field's bytes, unchanged. The honest default and the only one for a kind with no range.
    Raw = 0,
    /// A scalar mapped onto `bits` integers across [minimum, maximum].
    QuantisedScalar,
    /// Three scalars, each quantised the same way. A position, a velocity.
    QuantisedVector,
    /// Smallest-three: the largest component is dropped and reconstructed from the other three.
    CompressedQuaternion,
    /// An index into a dictionary sent once and referenced thereafter.
    DictionaryIndex,
    /// An integer narrowed to `bits`.
    Bitfield,
};

const char* field_encoder_name(FieldEncoder encoder) noexcept;

/// When a field is sent.
enum class SendCondition : u8 {
    /// Every update the entity is selected for.
    Always = 0,
    /// Only when it differs from the peer's acknowledged baseline. The default, and what makes
    /// "WHEN an entity's health is unchanged THEN no health data SHALL be transmitted for it" true.
    OnChange,
};

/// Who a field is sent to.
enum class TargetFilter : u8 {
    Everyone = 0,
    /// The peer that owns the entity. A player's own ammunition count, their cooldowns.
    OwnerOnly,
    /// Everyone but the owner — a field the owner already predicts and must not be corrected by.
    ExceptOwner,
};

/// An encoder's parameters. Which members mean anything depends on the encoder, and `compile()`
/// refuses a combination that does not.
struct EncoderParameters {
    f64 minimum = 0.0;
    f64 maximum = 0.0;
    /// Bits per value — per component for a vector or a quaternion.
    u32 bits = 32;
};

/// One replicated field, as declared.
struct FieldSchema {
    reflect::FieldId field;
    FieldEncoder encoder = FieldEncoder::Raw;
    EncoderParameters parameters{};
    SendCondition condition = SendCondition::OnChange;
    TargetFilter filter = TargetFilter::Everyone;
    /// Added to the entity's priority score when this field has changed. `scheduler.h` reads it.
    u32 priority_contribution = 0;
};

/// One replicated component, as declared.
struct ReplicationSchema {
    /// What the component is called, for a diagnostic. Never the key.
    const char* component = "";
    u16 version = 1;
    Span<const FieldSchema> fields;
};

/// Why a schema would not compile. One enumerator per thing a cook error can be about.
enum class SchemaError : u8 {
    None = 0,
    /// The reflected type has no field with that `FieldId`. Genuine drift: removed or
    /// re-identified.
    FieldRemoved,
    /// The field's declared bounds are wider than the encoder's interval.
    RangeCannotRepresent,
    /// The encoder does not apply to the field's storage class — a quantised `bool`.
    EncoderDoesNotApply,
    /// Zero bits, or more than the field's own width can hold.
    BitsOutOfRange,
    TooManyFields,
    /// Two entries of one schema name the same field.
    DuplicateField,
};

const char* schema_error_name(SchemaError error) noexcept;

struct SchemaRejection {
    SchemaError error = SchemaError::None;
    const char* component = "";
    /// The field's name as reflection knows it, or "" when the field is the one that is gone.
    const char* field = "";
    reflect::FieldId field_id;
    /// What was wrong, spelled. Never null.
    const char* detail = "";
};

/// One field, resolved. No pointer into reflection survives compilation: an offset, a width, and
/// the arithmetic already done.
struct CompiledField {
    reflect::FieldId field;
    u32 offset = 0;
    u32 size = 0;
    reflect::FieldKind kind = reflect::FieldKind::Unsupported;
    FieldEncoder encoder = FieldEncoder::Raw;
    /// Bits on the wire for the whole field — three times the per-component count for a vector.
    u32 bits = 32;
    /// Per-component bits, for the encoders that have components.
    u32 component_bits = 32;
    f64 minimum = 0.0;
    /// `(2^component_bits - 1) / (maximum - minimum)`, computed once.
    f64 scale = 0.0;
    f64 inverse_scale = 0.0;
    SendCondition condition = SendCondition::OnChange;
    TargetFilter filter = TargetFilter::Everyone;
    u32 priority_contribution = 0;
};

/// A bit-level writer. `networking-and-replication` — "Bit packing: fields packed to their declared
/// bit widths without byte alignment".
class BitWriter {
public:
    explicit BitWriter(Array<u8>& out) noexcept : out_(&out) {}

    [[nodiscard]] Status write(u64 value, u32 bits) noexcept;
    /// Whole bytes, for `Raw`. Still bit-addressed, so a raw field after a 3-bit one costs three
    /// bits of shifting rather than five bits of padding.
    [[nodiscard]] Status write_bytes(const u8* bytes, u32 count) noexcept;
    /// Round up to the next byte. Called once, at the end of a packet.
    [[nodiscard]] Status flush() noexcept;

    [[nodiscard]] u64 bits_written() const noexcept { return bits_; }

private:
    Array<u8>* out_;
    u64 bits_ = 0;
    u32 partial_ = 0;
    u32 partial_bits_ = 0;
};

class BitReader {
public:
    explicit BitReader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool read(u32 bits, u64& out) noexcept;
    [[nodiscard]] bool read_bytes(u8* out, u32 count) noexcept;

    [[nodiscard]] u64 bits_read() const noexcept { return bits_; }

private:
    Span<const u8> bytes_;
    u64 bits_ = 0;
};

/// The change mask: which of a component's fields are present in this update.
///
/// "Change masks — a bitfield indicating which fields are present, rather than sending
/// identifiers." One `u64`, written at its declared width rather than at 64 — a component with six
/// replicated fields spends six bits on its mask.
using ChangeMask = u64;

/// One component's schema, compiled.
class CompiledSchema {
public:
    explicit CompiledSchema(Allocator& allocator) noexcept : fields_(allocator) {}

    CompiledSchema(const CompiledSchema&) = delete;
    CompiledSchema& operator=(const CompiledSchema&) = delete;

    /// Resolve `schema` against `type`. The only place reflection is read.
    [[nodiscard]] Expected<u32, SchemaRejection> compile(const reflect::TypeInfo& type,
                                                         const ReplicationSchema& schema) noexcept;

    /// Which fields of `instance` differ from `baseline`. `baseline` may be null, which means "no
    /// baseline yet" and marks every field changed — a fresh baseline rather than a delta.
    [[nodiscard]] ChangeMask changes(const void* instance, const void* baseline,
                                     bool owner) const noexcept;

    /// Write the mask and the named fields. Returns the bits written.
    [[nodiscard]] Expected<u32, Error> encode_instance(const void* instance, ChangeMask mask,
                                                       BitWriter& writer) const noexcept;

    /// Read one instance back over `instance`, leaving absent fields as they are — which is what
    /// makes a delta a delta.
    [[nodiscard]] Status decode_instance(BitReader& reader, void* instance,
                                         ChangeMask& mask) const noexcept;

    /// Every field sent, in bits. What the schema tooling reports as the theoretical size.
    [[nodiscard]] u32 theoretical_bits() const noexcept;
    /// The bits `mask` would cost, mask included. What "the added bits per entity per update" is
    /// measured with.
    [[nodiscard]] u32 bits_for(ChangeMask mask) const noexcept;

    [[nodiscard]] u32 field_count() const noexcept { return static_cast<u32>(fields_.size()); }
    [[nodiscard]] const CompiledField& field(u32 index) const noexcept { return fields_[index]; }
    [[nodiscard]] const char* component() const noexcept { return component_; }
    /// The reflected type's own size. Carried because a baseline stores a copy of the instance and
    /// the only other way to know how many bytes that is would be to keep the `TypeInfo` alive.
    [[nodiscard]] u32 instance_size() const noexcept { return instance_size_; }
    [[nodiscard]] u16 version() const noexcept { return version_; }
    [[nodiscard]] reflect::TypeId type() const noexcept { return type_; }

    /// A value identity over what this schema means on the wire: the type, the version, and every
    /// field's identifier, encoder, width and filter. Two builds whose schemas agree produce the
    /// same number; two that do not, do not.
    [[nodiscard]] u64 identity() const noexcept;

private:
    Array<CompiledField> fields_;
    const char* component_ = "";
    reflect::TypeId type_;
    u32 instance_size_ = 0;
    u16 version_ = 1;
};

/// The schema set: every replicated component, and the identity a peer is verified against.
class SchemaSet {
public:
    explicit SchemaSet(Allocator& allocator) noexcept
        : schemas_(allocator), allocator_(&allocator) {}
    ~SchemaSet();

    SchemaSet(const SchemaSet&) = delete;
    SchemaSet& operator=(const SchemaSet&) = delete;

    /// Compile and add. The returned index is this session's dense handle for the component.
    [[nodiscard]] Expected<u32, SchemaRejection> add(const reflect::TypeInfo& type,
                                                     const ReplicationSchema& schema) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(schemas_.size()); }
    [[nodiscard]] const CompiledSchema& at(u32 index) const noexcept { return *schemas_[index]; }
    [[nodiscard]] const CompiledSchema* find(reflect::TypeId type) const noexcept;

    /// **What `mode.h`'s `CompatibilityScope::schema_set_hash` carries.** An empty set hashes to a
    /// non-zero constant, so that "no schemas" and "never computed" are different answers — a zero
    /// there is what `verify_mode()` refuses.
    [[nodiscard]] u64 identity() const noexcept;

private:
    Array<CompiledSchema*> schemas_;
    Allocator* allocator_;
};

/// The identity of the empty schema set. See `SchemaSet::identity()`.
inline constexpr u64 kEmptySchemaSetHash = 0x4359'4E45'5453'0001ULL;

}  // namespace cy::net
