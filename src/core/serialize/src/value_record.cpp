#include <cy/core/serialize/value_record.h>

namespace cy::serialize {
namespace {

/// The visitor that turns a traversal into a record. One per call, on the stack.
class RecordBuilder final : public FieldVisitor {
public:
    explicit RecordBuilder(ValueRecord& out) noexcept : out_(&out) {}

    Status begin_object(const reflect::TypeInfo& type, u32 field_count) noexcept override {
        (void)field_count;
        out_->clear();
        out_->set_type(type.id);
        return ok();
    }

    Status visit_field(const reflect::FieldInfo& field, const void* value) noexcept override {
        return out_->set_scalar(field.id, wire_type_of(field.kind), value, field.size);
    }

    Status end_object(const reflect::TypeInfo&) noexcept override { return ok(); }

private:
    ValueRecord* out_;
};

}  // namespace

usize ValueRecord::lower_bound(reflect::FieldId id) const noexcept {
    usize low = 0;
    usize high = fields_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (fields_[middle].id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

const FieldValue* ValueRecord::find(reflect::FieldId id) const noexcept {
    const usize index = lower_bound(id);
    if (index < fields_.size() && fields_[index].id == id) {
        return &fields_[index];
    }
    return nullptr;
}

Span<const u8> ValueRecord::bytes(const FieldValue& value) const noexcept {
    if (value.offset + value.size > pool_.size()) {
        return {};
    }
    return {pool_.data() + value.offset, value.size};
}

Span<const u8> ValueRecord::bytes(reflect::FieldId id) const noexcept {
    const FieldValue* value = find(id);
    return (value == nullptr) ? Span<const u8>{} : bytes(*value);
}

Status ValueRecord::set(reflect::FieldId id, WireType wire, const void* bytes, u32 size) noexcept {
    if (!id.valid()) {
        return fail(ErrorCode::InvalidArgument, "a value record addresses fields by FieldId");
    }
    if (wire >= WireType::Count) {
        return fail(ErrorCode::InvalidArgument, "unknown wire type");
    }

    const u32 offset = static_cast<u32>(pool_.size());
    if (size != 0) {
        if (Status appended = pool_.append(Span<const u8>(static_cast<const u8*>(bytes), size));
            !appended) {
            return appended;
        }
    }

    const usize index = lower_bound(id);
    if (index < fields_.size() && fields_[index].id == id) {
        fields_[index] = FieldValue{id, wire, offset, size};
        return ok();
    }

    // Insert in identifier order. `Array` has no insert-at, and adding one for this would be a
    // container change for a control-plane caller: a record holds a component's fields, so the
    // shift is over a handful of eight-byte descriptors.
    if (Status grown = fields_.push_back(FieldValue{id, wire, offset, size}); !grown) {
        return grown;
    }
    for (usize position = fields_.size() - 1; position > index; --position) {
        FieldValue moved = fields_[position - 1];
        fields_[position - 1] = fields_[position];
        fields_[position] = moved;
    }
    return ok();
}

Status ValueRecord::set_scalar(reflect::FieldId id, WireType wire, const void* value,
                               u32 size) noexcept {
    if (is_reference(wire)) {
        return fail(ErrorCode::InvalidArgument,
                    "a reference is stored through set_local_reference or set_external_reference");
    }
    Array<u8> scratch(allocator());
    ByteWriter writer(scratch);
    if (Status written = writer.write_scalar(wire, value, size); !written) {
        return written;
    }
    return set(id, wire, scratch.data(), static_cast<u32>(scratch.size()));
}

Status ValueRecord::set_local_reference(reflect::FieldId id, u32 local) noexcept {
    Array<u8> scratch(allocator());
    ByteWriter writer(scratch);
    if (Status written = writer.write_u32(local); !written) {
        return written;
    }
    return set(id, WireType::LocalRef, scratch.data(), static_cast<u32>(scratch.size()));
}

Status ValueRecord::set_external_reference(reflect::FieldId id, u64 asset_high, u64 asset_low,
                                           u32 local) noexcept {
    Array<u8> scratch(allocator());
    ByteWriter writer(scratch);
    if (Status written = writer.write_u64(asset_high); !written) {
        return written;
    }
    if (Status written = writer.write_u64(asset_low); !written) {
        return written;
    }
    if (Status written = writer.write_u32(local); !written) {
        return written;
    }
    return set(id, WireType::ExternalRef, scratch.data(), static_cast<u32>(scratch.size()));
}

Expected<u32, Error> ValueRecord::local_reference(reflect::FieldId id) const noexcept {
    const FieldValue* value = find(id);
    if (value == nullptr) {
        return fail(ErrorCode::NotFound, "no such field in this value record");
    }
    if (value->wire != WireType::LocalRef) {
        return fail(ErrorCode::InvalidArgument, "field is not a local entity reference");
    }
    const Span<const u8> raw = bytes(*value);
    if (raw.size() != 4) {
        return fail(ErrorCode::Internal, "a local reference is four bytes");
    }
    ByteReader reader(raw.data(), raw.size());
    return reader.read_u32();
}

bool ValueRecord::remove(reflect::FieldId id) noexcept {
    const usize index = lower_bound(id);
    if (index >= fields_.size() || fields_[index].id != id) {
        return false;
    }
    fields_.erase(index);
    return true;
}

Status ValueRecord::retarget(reflect::FieldId from, reflect::FieldId to) noexcept {
    if (!to.valid()) {
        return fail(ErrorCode::InvalidArgument, "cannot retarget a field onto a null FieldId");
    }
    const FieldValue* source = find(from);
    if (source == nullptr) {
        return fail(ErrorCode::NotFound, "no such field in this value record");
    }
    if (from == to) {
        return ok();
    }
    if (contains(to)) {
        return fail(ErrorCode::AlreadyExists,
                    "the migration target already carries a value; merging would lose one");
    }
    // The payload is copied out before anything is mutated. Not defensiveness: `set` appends to the
    // same pool the payload lives in, and appending may reallocate it — so passing a span of the
    // pool straight back into `set` reads freed memory on exactly the calls that grow.
    const FieldValue moved = *source;
    Array<u8> payload(allocator());
    if (Status copied = payload.append(bytes(moved)); !copied) {
        return copied;
    }
    (void)remove(from);
    return set(to, moved.wire, payload.data(), static_cast<u32>(payload.size()));
}

void ValueRecord::clear() noexcept {
    fields_.clear();
    pool_.clear();
    schema_version_ = 0;
}

Status ValueRecord::clone_into(ValueRecord& out) const noexcept {
    out.clear();
    out.type_ = type_;
    out.schema_version_ = schema_version_;
    for (const FieldValue& value : fields_) {
        const Span<const u8> payload = bytes(value);
        if (Status copied =
                out.set(value.id, value.wire, payload.data(), static_cast<u32>(payload.size()));
            !copied) {
            return copied;
        }
    }
    return ok();
}

Status ValueRecord::overlay(const ValueRecord& other) noexcept {
    for (const FieldValue& value : other.fields()) {
        const Span<const u8> payload = other.bytes(value);
        if (Status copied =
                set(value.id, value.wire, payload.data(), static_cast<u32>(payload.size()));
            !copied) {
            return copied;
        }
    }
    return ok();
}

Status record_from_object(const reflect::TypeInfo& type, const void* object, Purpose purpose,
                          ValueRecord& out) noexcept {
    RecordBuilder builder(out);
    return visit_object(type, object, purpose, builder);
}

Status record_to_object(const ValueRecord& record, const reflect::FieldIndex& fields, void* object,
                        u32* applied, u32* skipped) noexcept {
    if (object == nullptr) {
        return fail(ErrorCode::InvalidArgument, "cannot apply a value record to a null object");
    }
    u32 written = 0;
    u32 preserved = 0;
    u8* base = static_cast<u8*>(object);

    for (const FieldValue& value : record.fields()) {
        const reflect::FieldInfo* field = fields.find(value.id);
        if (field == nullptr) {
            // A field this build's schema does not have. Preserved by virtue of still being in the
            // record; skipped here because there is nowhere to put it.
            ++preserved;
            continue;
        }
        if (is_reference(value.wire)) {
            // A reference is resolved by whoever knows what a document is; this layer cannot turn a
            // local id into anything, and writing the id into the field would be a plausible-
            // looking wrong answer.
            //
            // Checked *before* the width, and that ordering is load-bearing: a `LocalRef` is four
            // bytes on the wire while the field it addresses is an eight-byte entity slot, so a
            // width comparison here would reject every reference in every cooked component.
            ++preserved;
            continue;
        }
        const Span<const u8> payload = record.bytes(value);
        if (payload.size() != field->size) {
            return fail(ErrorCode::InvalidArgument,
                        "field width in the data does not match the field width in the schema");
        }
        if (Status decoded = decode_scalar(value.wire, payload.data(),
                                           static_cast<u32>(payload.size()), base + field->offset);
            !decoded) {
            return decoded;
        }
        ++written;
    }

    if (applied != nullptr) {
        *applied = written;
    }
    if (skipped != nullptr) {
        *skipped = preserved;
    }
    return ok();
}

}  // namespace cy::serialize
