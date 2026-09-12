// Typed spatial datasets. See include/cy/pcg/dataset.h.

#include <cy/pcg/dataset.h>

#include <cstring>

namespace cy::pcg {

namespace {

/// Two names are the same name. `std::strcmp` without the header, because the whole of this
/// module's string handling is comparing two literals a graph author wrote.
[[nodiscard]] bool same_name(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

}  // namespace

const char* attribute_type_name(AttributeType type) noexcept {
    switch (type) {
        case AttributeType::F32:
            return "f32";
        case AttributeType::F32x3:
            return "f32x3";
        case AttributeType::I32:
            return "i32";
        case AttributeType::U64:
            return "u64";
        case AttributeType::Bool:
            return "bool";
    }
    return "unknown";
}

const char* dataset_kind_name(DatasetKind kind) noexcept {
    switch (kind) {
        case DatasetKind::PointSet:
            return "point-set";
        case DatasetKind::Volume:
            return "volume";
        case DatasetKind::Surface:
            return "surface";
        case DatasetKind::Spline:
            return "spline";
        case DatasetKind::Field:
            return "field";
        case DatasetKind::Geometry:
            return "geometry";
        case DatasetKind::AttributeTable:
            return "attribute-table";
        case DatasetKind::EntitySet:
            return "entity-set";
        case DatasetKind::Regions:
            return "regions";
        case DatasetKind::Raster:
            return "raster";
        case DatasetKind::kCount:
            break;
    }
    return "unknown";
}

// --- AttributeTable ---------------------------------------------------------------------------

Expected<AttributeId, Error> AttributeTable::intern(const char* name, AttributeType type) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "pcg: an attribute must have a name"});
    }
    if (const AttributeDecl* existing = find_by_name(name); existing != nullptr) {
        if (existing->type != type) {
            // Two nodes disagreeing about a column's shape is a graph defect. Resolving it by
            // last-writer-wins would make the compiled column's meaning depend on node order, which
            // is the same class of bug as an identity assigned by traversal.
            return make_unexpected(Error{ErrorCode::AlreadyExists,
                                         "pcg: an attribute is already interned with a different "
                                         "type"});
        }
        return existing->id;
    }
    // Zero is the null identifier, so the first entry is 1.
    if (entries_.size() >= 0xFFFEU) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "pcg: too many attributes in one program"});
    }
    AttributeDecl declaration;
    declaration.name = name;
    declaration.type = type;
    declaration.id = AttributeId{static_cast<u16>(entries_.size() + 1)};
    if (Status pushed = entries_.push_back(declaration); !pushed) {
        return make_unexpected(pushed.error());
    }
    return declaration.id;
}

const AttributeDecl* AttributeTable::find_by_name(const char* name) const noexcept {
    for (const AttributeDecl& entry : entries_) {
        if (same_name(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

const AttributeDecl* AttributeTable::find(AttributeId id) const noexcept {
    if (!id.is_valid() || id.value > entries_.size()) {
        return nullptr;
    }
    return &entries_[id.value - 1];
}

const char* AttributeTable::name_of(AttributeId id) const noexcept {
    const AttributeDecl* entry = find(id);
    return entry != nullptr ? entry->name : "";
}

Expected<AttributeTable, Error> AttributeTable::clone() const noexcept {
    AttributeTable copy(entries_.allocator());
    if (Status reserved = copy.entries_.reserve(entries_.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (const AttributeDecl& entry : entries_) {
        if (Status pushed = copy.entries_.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return copy;
}

// --- PointSet ---------------------------------------------------------------------------------

Status PointSet::reserve(usize points, Span<const AttributeDecl> columns) noexcept {
    clear();
    columns_.clear();
    if (Status reserved = x_.reserve(points); !reserved) {
        return reserved;
    }
    if (Status reserved = y_.reserve(points); !reserved) {
        return reserved;
    }
    if (Status reserved = z_.reserve(points); !reserved) {
        return reserved;
    }
    if (Status reserved = slots_.reserve(points); !reserved) {
        return reserved;
    }
    if (Status reserved = identities_.reserve(points); !reserved) {
        return reserved;
    }
    if (Status reserved = columns_.reserve(columns.size()); !reserved) {
        return reserved;
    }
    for (const AttributeDecl& declaration : columns) {
        Expected<Column*, Error> slot = columns_.emplace_back(*allocator_);
        if (!slot) {
            return Status{make_unexpected(slot.error())};
        }
        (*slot)->id = declaration.id;
        (*slot)->type = declaration.type;
        // The column's whole allocation, once. `add()` writes into the tail of it rather than
        // pushing, which is what makes a million points zero further allocations.
        if (Status sized = (*slot)->data.resize(points * attribute_bytes(declaration.type));
            !sized) {
            return sized;
        }
    }
    capacity_ = points;
    return ok();
}

Expected<u32, Error> PointSet::add(f32 x, f32 y, f32 z, u32 slot) noexcept {
    if (x_.size() >= capacity_) {
        // Refused, never grown. `procedural-content-generation` — "PCG performance": "WHEN a region
        // generates millions of candidates THEN no per-point heap allocation SHALL occur." A
        // container that grew here would allocate, and the refusal is what makes the property
        // checkable rather than merely likely.
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "pcg: point set is at its reserved capacity; a generator must "
                                     "reserve for its declared candidate count"});
    }
    const u32 index = static_cast<u32>(x_.size());
    if (Status pushed = x_.push_back(x); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = y_.push_back(y); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = z_.push_back(z); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = slots_.push_back(slot); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = identities_.push_back(0); !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

Status PointSet::retain(Span<const u8> keep) noexcept {
    if (keep.size() < x_.size()) {
        return Status{make_unexpected(Error{
            ErrorCode::InvalidArgument, "pcg: retain() needs one keep byte per point in the set"})};
    }
    usize out = 0;
    for (usize index = 0; index < x_.size(); ++index) {
        if (keep[index] == 0) {
            continue;
        }
        if (out != index) {
            x_[out] = x_[index];
            y_[out] = y_[index];
            z_[out] = z_[index];
            slots_[out] = slots_[index];
            identities_[out] = identities_[index];
            for (Column& column : columns_) {
                const u32 width = attribute_bytes(column.type);
                std::memcpy(column.data.data() + out * width, column.data.data() + index * width,
                            width);
            }
        }
        ++out;
    }
    while (x_.size() > out) {
        x_.pop_back();
        y_.pop_back();
        z_.pop_back();
        slots_.pop_back();
        identities_.pop_back();
    }
    return ok();
}

void PointSet::clear() noexcept {
    x_.clear();
    y_.clear();
    z_.clear();
    slots_.clear();
    identities_.clear();
    // The columns keep their storage: clearing a point set between regenerations of one region must
    // not give the allocation back only to take it again.
}

PointSet::Column* PointSet::find_column(AttributeId id) noexcept {
    for (Column& column : columns_) {
        if (column.id == id) {
            return &column;
        }
    }
    return nullptr;
}

const PointSet::Column* PointSet::find_column(AttributeId id) const noexcept {
    for (const Column& column : columns_) {
        if (column.id == id) {
            return &column;
        }
    }
    return nullptr;
}

f32 PointSet::get_f32(AttributeId id, usize index) const noexcept {
    const Column* column = find_column(id);
    if (column == nullptr || index >= capacity_) {
        return 0.0F;
    }
    f32 value = 0.0F;
    std::memcpy(&value, column->data.data() + index * sizeof(f32), sizeof(f32));
    return value;
}

void PointSet::set_f32(AttributeId id, usize index, f32 value) noexcept {
    Column* column = find_column(id);
    if (column == nullptr || index >= capacity_) {
        return;
    }
    std::memcpy(column->data.data() + index * sizeof(f32), &value, sizeof(f32));
}

i32 PointSet::get_i32(AttributeId id, usize index) const noexcept {
    const Column* column = find_column(id);
    if (column == nullptr || index >= capacity_) {
        return 0;
    }
    i32 value = 0;
    std::memcpy(&value, column->data.data() + index * sizeof(i32), sizeof(i32));
    return value;
}

void PointSet::set_i32(AttributeId id, usize index, i32 value) noexcept {
    Column* column = find_column(id);
    if (column == nullptr || index >= capacity_) {
        return;
    }
    std::memcpy(column->data.data() + index * sizeof(i32), &value, sizeof(i32));
}

u64 PointSet::get_u64(AttributeId id, usize index) const noexcept {
    const Column* column = find_column(id);
    if (column == nullptr || index >= capacity_) {
        return 0;
    }
    u64 value = 0;
    std::memcpy(&value, column->data.data() + index * sizeof(u64), sizeof(u64));
    return value;
}

void PointSet::set_u64(AttributeId id, usize index, u64 value) noexcept {
    Column* column = find_column(id);
    if (column == nullptr || index >= capacity_) {
        return;
    }
    std::memcpy(column->data.data() + index * sizeof(u64), &value, sizeof(u64));
}

u64 PointSet::bytes() const noexcept {
    u64 total = static_cast<u64>(capacity_) * (3 * sizeof(f32) + sizeof(u32) + sizeof(u64));
    for (const Column& column : columns_) {
        total += column.data.size();
    }
    return total;
}

u64 PointSet::digest() const noexcept {
    Digest digest;
    digest.u64_value(x_.size());
    for (usize index = 0; index < x_.size(); ++index) {
        digest.f32_value(x_[index]);
        digest.f32_value(y_[index]);
        digest.f32_value(z_[index]);
        digest.u32_value(slots_[index]);
        digest.u64_value(identities_[index]);
    }
    // Columns in declaration order, which is the order `reserve()` was given and therefore the
    // program's own order rather than an insertion order.
    for (const Column& column : columns_) {
        digest.u32_value(column.id.value);
        digest.bytes(column.data.data(), x_.size() * attribute_bytes(column.type));
    }
    return digest.value();
}

Expected<PointSet, Error> PointSet::clone() const noexcept {
    PointSet copy(*allocator_);
    if (Status reserved = copy.x_.reserve(capacity_); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status reserved = copy.y_.reserve(capacity_); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status reserved = copy.z_.reserve(capacity_); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status reserved = copy.slots_.reserve(capacity_); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status reserved = copy.identities_.reserve(capacity_); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status reserved = copy.columns_.reserve(columns_.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    copy.capacity_ = capacity_;
    for (const Column& column : columns_) {
        Expected<Column*, Error> slot = copy.columns_.emplace_back(*allocator_);
        if (!slot) {
            return make_unexpected(slot.error());
        }
        (*slot)->id = column.id;
        (*slot)->type = column.type;
        if (Status sized = (*slot)->data.resize(column.data.size()); !sized) {
            return make_unexpected(sized.error());
        }
        if (column.data.size() != 0) {
            std::memcpy((*slot)->data.data(), column.data.data(), column.data.size());
        }
    }
    for (usize index = 0; index < x_.size(); ++index) {
        Expected<u32, Error> added = copy.add(x_[index], y_[index], z_[index], slots_[index]);
        if (!added) {
            return make_unexpected(added.error());
        }
        copy.set_identity(index, identities_[index]);
    }
    return copy;
}

// --- Raster -----------------------------------------------------------------------------------

Status Raster::reset(u32 edge, Span<const AttributeId> channels) noexcept {
    edge_ = edge;
    const usize cells = static_cast<usize>(edge) * static_cast<usize>(edge);
    // Existing channels are reused where the declaration still names them, so a regeneration of one
    // region is zero allocations after the first.
    for (Channel& channel : channels_) {
        channel.id = AttributeId{};
    }
    usize next = 0;
    for (AttributeId id : channels) {
        if (next < channels_.size()) {
            channels_[next].id = id;
            if (Status sized = channels_[next].values.resize(cells); !sized) {
                return sized;
            }
        } else {
            Expected<Channel*, Error> slot = channels_.emplace_back(channels_.allocator());
            if (!slot) {
                return Status{make_unexpected(slot.error())};
            }
            (*slot)->id = id;
            if (Status sized = (*slot)->values.resize(cells); !sized) {
                return sized;
            }
        }
        ++next;
    }
    while (channels_.size() > next) {
        channels_.pop_back();
    }
    for (Channel& channel : channels_) {
        for (f32& value : channel.values) {
            value = 0.0F;
        }
    }
    return ok();
}

Raster::Channel* Raster::find(AttributeId id) noexcept {
    for (Channel& channel : channels_) {
        if (channel.id == id) {
            return &channel;
        }
    }
    return nullptr;
}

const Raster::Channel* Raster::find(AttributeId id) const noexcept {
    for (const Channel& channel : channels_) {
        if (channel.id == id) {
            return &channel;
        }
    }
    return nullptr;
}

f32 Raster::at(AttributeId id, u32 x, u32 z) const noexcept {
    const Channel* channel = find(id);
    if (channel == nullptr || x >= edge_ || z >= edge_) {
        return 0.0F;
    }
    return channel->values[static_cast<usize>(z) * edge_ + x];
}

void Raster::set(AttributeId id, u32 x, u32 z, f32 value) noexcept {
    Channel* channel = find(id);
    if (channel == nullptr || x >= edge_ || z >= edge_) {
        return;
    }
    channel->values[static_cast<usize>(z) * edge_ + x] = value;
}

Span<const f32> Raster::values(AttributeId id) const noexcept {
    const Channel* channel = find(id);
    return channel != nullptr ? channel->values.span() : Span<const f32>();
}

Span<f32> Raster::values_mutable(AttributeId id) noexcept {
    Channel* channel = find(id);
    return channel != nullptr ? channel->values.span() : Span<f32>();
}

u64 Raster::digest() const noexcept {
    Digest digest;
    digest.u32_value(edge_);
    for (const Channel& channel : channels_) {
        digest.u32_value(channel.id.value);
        for (f32 value : channel.values) {
            digest.f32_value(value);
        }
    }
    return digest.value();
}

u64 Raster::digest_of(AttributeId id) const noexcept {
    const Channel* channel = find(id);
    if (channel == nullptr) {
        return 0;
    }
    Digest digest;
    digest.u32_value(id.value);
    for (f32 value : channel->values) {
        digest.f32_value(value);
    }
    return digest.value();
}

u64 Raster::bytes() const noexcept {
    u64 total = 0;
    for (const Channel& channel : channels_) {
        total += channel.values.size() * sizeof(f32);
    }
    return total;
}

Expected<Raster, Error> Raster::clone() const noexcept {
    Raster copy(channels_.allocator());
    copy.edge_ = edge_;
    if (Status reserved = copy.channels_.reserve(channels_.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (const Channel& channel : channels_) {
        Expected<Channel*, Error> slot = copy.channels_.emplace_back(channels_.allocator());
        if (!slot) {
            return make_unexpected(slot.error());
        }
        (*slot)->id = channel.id;
        if (Status sized = (*slot)->values.resize(channel.values.size()); !sized) {
            return make_unexpected(sized.error());
        }
        for (usize index = 0; index < channel.values.size(); ++index) {
            (*slot)->values[index] = channel.values[index];
        }
    }
    return copy;
}

// --- Digest -----------------------------------------------------------------------------------

void Digest::f32_value(f32 value) noexcept {
    // The BIT PATTERN, not the number, and a canonical zero: -0.0 and +0.0 compare equal and must
    // digest equal, or a region that computed one and a region that computed the other would look
    // like a change to the fixed point and would re-dirty the world forever.
    if (value == 0.0F) {
        u64_value(0);
        return;
    }
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    u64_value(static_cast<u64>(bits));
}

void Digest::bytes(const void* data, usize size) noexcept {
    const auto* cursor = static_cast<const u8*>(data);
    u64_value(size);
    usize index = 0;
    // Eight at a time where they are there, which is what makes a digest over a million-point
    // column a memory-bandwidth operation rather than a byte loop.
    for (; index + sizeof(u64) <= size; index += sizeof(u64)) {
        u64 chunk = 0;
        std::memcpy(&chunk, cursor + index, sizeof(chunk));
        u64_value(chunk);
    }
    u64 tail = 0;
    for (usize byte = 0; index + byte < size; ++byte) {
        tail |= static_cast<u64>(cursor[index + byte]) << (byte * 8U);
    }
    u64_value(tail);
}

}  // namespace cy::pcg
