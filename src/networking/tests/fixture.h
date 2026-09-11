#pragma once
// The shared fixture for CyberNet's suites. M9 section 4.
//
// WHAT IS ABSENT IS THE SUBJECT, exactly as in src/gameplay/tests/fixture.h and
// src/replay/tests/fixture.h. There is no renderer, no audio device, no interface and no GPU here,
// and `networking-and-replication` says why: a dedicated server "SHALL exclude: the renderer, VFX,
// UI, client audio, and the editor". These suites cannot reach any of them because this module
// links nothing that has one — `tests/test_server.cpp` asserts the same thing from the source side.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/type_info.h>
#include <cy/networking/schema.h>
#include <cy/test/test.h>

#include <cstddef>

namespace cy::net_test {

using namespace cy::net;

inline cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// A replicated component with one field of every shape a schema encodes: a quantised vector, a
/// compressed quaternion, a bitfield and a raw integer. One type rather than four, so that a test
/// of the change mask exercises a mask with holes in it.
struct SampleTransform {
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    f32 rotation[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    u16 health = 100;
    u32 flags = 0;
};

inline constexpr u32 kFieldPosition = 4101;
inline constexpr u32 kFieldRotation = 4102;
inline constexpr u32 kFieldHealth = 4103;
inline constexpr u32 kFieldFlags = 4104;
inline constexpr u32 kSampleTypeId = 4100;

/// The reflected descriptor, built by hand the way a generated one is built by the generator.
/// Static storage, because a `TypeInfo` is a descriptor that outlives every use of it.
[[nodiscard]] inline const reflect::TypeInfo& sample_type() noexcept {
    static reflect::FieldInfo fields[4];
    static reflect::TypeInfo info;
    static bool built = false;
    if (built) {
        return info;
    }

    fields[0].name = "position";
    fields[0].id = reflect::FieldId(kFieldPosition);
    fields[0].kind = reflect::FieldKind::F32;
    fields[0].offset = static_cast<u32>(offsetof(SampleTransform, position));
    fields[0].size = static_cast<u32>(sizeof(SampleTransform::position));
    // A declared range, so that a schema whose quantisation cannot cover it is a cook error rather
    // than a clamp — `networking-and-replication`'s "a range that cannot represent the field's
    // declared bounds SHALL be a cook error".
    fields[0].attributes.declared = reflect::AttributeKind::Range;
    fields[0].attributes.range = reflect::RangeAttribute{-1000.0, 1000.0, 0.0};

    fields[1].name = "rotation";
    fields[1].id = reflect::FieldId(kFieldRotation);
    fields[1].kind = reflect::FieldKind::F32;
    fields[1].offset = static_cast<u32>(offsetof(SampleTransform, rotation));
    fields[1].size = static_cast<u32>(sizeof(SampleTransform::rotation));

    fields[2].name = "health";
    fields[2].id = reflect::FieldId(kFieldHealth);
    fields[2].kind = reflect::FieldKind::U16;
    fields[2].offset = static_cast<u32>(offsetof(SampleTransform, health));
    fields[2].size = static_cast<u32>(sizeof(u16));

    fields[3].name = "flags";
    fields[3].id = reflect::FieldId(kFieldFlags);
    fields[3].kind = reflect::FieldKind::U32;
    fields[3].offset = static_cast<u32>(offsetof(SampleTransform, flags));
    fields[3].size = static_cast<u32>(sizeof(u32));

    info.name = "cy::net_test::SampleTransform";
    info.id = reflect::TypeId(kSampleTypeId);
    info.size = static_cast<u32>(sizeof(SampleTransform));
    info.alignment = static_cast<u32>(alignof(SampleTransform));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 4;
    built = true;
    return info;
}

/// The declared schema: position quantised to 1 cm over a 2 km range, rotation compressed, health a
/// bitfield of ten bits, flags raw and owner-only.
[[nodiscard]] inline ReplicationSchema sample_schema() noexcept {
    static FieldSchema fields[4];
    fields[0].field = reflect::FieldId(kFieldPosition);
    fields[0].encoder = FieldEncoder::QuantisedVector;
    fields[0].parameters = EncoderParameters{-1000.0, 1000.0, 18};
    fields[0].condition = SendCondition::OnChange;
    fields[0].priority_contribution = 4;

    fields[1].field = reflect::FieldId(kFieldRotation);
    fields[1].encoder = FieldEncoder::CompressedQuaternion;
    fields[1].parameters = EncoderParameters{0.0, 0.0, 10};
    fields[1].condition = SendCondition::OnChange;

    fields[2].field = reflect::FieldId(kFieldHealth);
    fields[2].encoder = FieldEncoder::Bitfield;
    fields[2].parameters = EncoderParameters{0.0, 0.0, 10};
    fields[2].condition = SendCondition::OnChange;

    fields[3].field = reflect::FieldId(kFieldFlags);
    fields[3].encoder = FieldEncoder::Raw;
    fields[3].filter = TargetFilter::OwnerOnly;
    fields[3].condition = SendCondition::OnChange;

    ReplicationSchema schema;
    schema.component = "SampleTransform";
    schema.version = 1;
    schema.fields = Span<const FieldSchema>(fields, 4);
    return schema;
}

}  // namespace cy::net_test
