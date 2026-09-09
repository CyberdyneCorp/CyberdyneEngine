// The editor's operation stream, applied to the engine's world. See world_transaction.h.

#include <cy/scene/serialization/world_transaction.h>

#include <cstring>

namespace cy::scene::serialization {
namespace {

/// `cy_editor_core::codec`, reading: little-endian throughout, `f32` by its bits, a `u32` length
/// before a byte string. Bounds-checked at every step, because these bytes came off a socket.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }

    [[nodiscard]] u8 u8_value() noexcept {
        if (failed_ || offset_ + 1 > bytes_.size()) {
            failed_ = true;
            return 0;
        }
        return bytes_[offset_++];
    }

    [[nodiscard]] u32 u32_value() noexcept { return static_cast<u32>(little_endian(4)); }
    [[nodiscard]] u64 u64_value() noexcept { return little_endian(8); }
    [[nodiscard]] i64 i64_value() noexcept { return static_cast<i64>(little_endian(8)); }

    /// A 128-bit identity.
    [[nodiscard]] EditorId u128_value() noexcept {
        EditorId value;
        value.low = u64_value();
        value.high = u64_value();
        return value;
    }

    [[nodiscard]] f32 f32_value() noexcept {
        const u32 bits = u32_value();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] f64 f64_value() noexcept {
        const u64 bits = u64_value();
        f64 value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    /// A length-prefixed byte string, as a view into the buffer.
    [[nodiscard]] Span<const u8> bytes_value() noexcept {
        const u32 length = u32_value();
        if (failed_ || offset_ + length > bytes_.size()) {
            failed_ = true;
            return {};
        }
        const Span<const u8> view(bytes_.data() + offset_, length);
        offset_ += length;
        return view;
    }

    [[nodiscard]] std::string_view text_value() noexcept {
        const Span<const u8> raw = bytes_value();
        return {reinterpret_cast<const char*>(raw.data()), raw.size()};
    }

    /// An `Option<NodeId>`: a tag, then the identity when there is one.
    [[nodiscard]] bool option_identity(EditorId& out) noexcept {
        const bool present = u8_value() == 1;
        if (present) {
            out = u128_value();
        }
        return present;
    }

private:
    [[nodiscard]] u64 little_endian(usize width) noexcept {
        if (failed_ || offset_ + width > bytes_.size()) {
            failed_ = true;
            return 0;
        }
        u64 value = 0;
        for (usize index = 0; index < width; ++index) {
            value |= static_cast<u64>(bytes_[offset_ + index]) << (index * 8U);
        }
        offset_ += width;
        return value;
    }

    Span<const u8> bytes_;
    usize offset_ = 0;
    bool failed_ = false;
};

/// The value tags `cy_editor_core::codec::Writer::value` writes.
enum class ValueTag : u8 {
    Nil = 0,
    Bool = 1,
    Int = 2,
    Float = 3,
    Double = 4,
    Vec2 = 5,
    Vec3 = 6,
    Vec4 = 7,
    Quat = 8,
    Text = 9,
    Bytes = 10,
    Entity = 11,
};

/// Read one value in the editor's vocabulary, interning any bytes into the world.
[[nodiscard]] bool read_value(Reader& reader, World& world, WorldValue& out) noexcept {
    out = WorldValue{};
    const u8 tag = reader.u8_value();
    switch (static_cast<ValueTag>(tag)) {
        case ValueTag::Nil:
            out.kind = WorldValueKind::Nil;
            return reader.ok();
        case ValueTag::Bool:
            out.kind = WorldValueKind::Bool;
            out.integer = (reader.u8_value() != 0) ? 1 : 0;
            return reader.ok();
        case ValueTag::Int:
            out.kind = WorldValueKind::Int;
            out.integer = reader.i64_value();
            return reader.ok();
        case ValueTag::Float:
            out.kind = WorldValueKind::Float;
            out.lanes[0] = reader.f32_value();
            return reader.ok();
        case ValueTag::Double:
            out.kind = WorldValueKind::Double;
            out.real = reader.f64_value();
            return reader.ok();
        case ValueTag::Vec2:
        case ValueTag::Vec3:
        case ValueTag::Vec4:
        case ValueTag::Quat: {
            static constexpr u32 kLanes[] = {2, 3, 4, 4};
            static constexpr WorldValueKind kKinds[] = {WorldValueKind::Vec2, WorldValueKind::Vec3,
                                                        WorldValueKind::Vec4, WorldValueKind::Quat};
            const u32 which = tag - static_cast<u8>(ValueTag::Vec2);
            out.kind = kKinds[which];
            for (u32 lane = 0; lane < kLanes[which]; ++lane) {
                out.lanes[lane] = reader.f32_value();
            }
            return reader.ok();
        }
        case ValueTag::Text:
        case ValueTag::Bytes: {
            out.kind = (static_cast<ValueTag>(tag) == ValueTag::Text) ? WorldValueKind::Text
                                                                      : WorldValueKind::Bytes;
            const Span<const u8> raw = reader.bytes_value();
            if (!reader.ok()) {
                return false;
            }
            return static_cast<bool>(world.intern_blob(raw, out));
        }
        case ValueTag::Entity:
            out.kind = WorldValueKind::Entity;
            out.integer = static_cast<i64>(reader.u64_value());
            return reader.ok();
    }
    // A tag from a newer editor. Nothing after this point can be located.
    return false;
}

/// A `(FieldId, Value)` list, as `encode_fields` writes it, into a component.
[[nodiscard]] bool read_fields(Reader& reader, World& world, WorldComponent& into) noexcept {
    const u32 count = reader.u32_value();
    for (u32 index = 0; index < count && reader.ok(); ++index) {
        WorldField field;
        field.file_field = reader.u64_value();
        if (!read_value(reader, world, field.value)) {
            return false;
        }
        if (Status pushed = into.fields().push_back(field); !pushed) {
            return false;
        }
    }
    return reader.ok();
}

/// The same list, read and discarded. What an operation carries as its "before" half.
[[nodiscard]] bool skip_fields(Reader& reader, World& world) noexcept {
    WorldComponent scratch(world.allocator());
    return read_fields(reader, world, scratch);
}

/// The node an identity names, creating nothing. `WorldNode::kNoParent` when it is not here.
[[nodiscard]] u32 locate(const World& world, const EditorId& identity) noexcept {
    return world.index_of(identity.low);
}

/// Attach a node to the world with the identity the EDITOR allocated.
///
/// The ordinal is not recoverable from a `NodeId` — it is a hash — so the node is created with the
/// identity itself and an ordinal of zero, which `World::reidentify` treats as "already identified"
/// and leaves alone. On the next save the node takes a position like any other and, on the next
/// load, an ordinal derived from it; that is what the editor does too.
[[nodiscard]] Expected<u32, Error> adopt(World& world, const EditorId& identity,
                                         u32 parent) noexcept {
    WorldNode node(world.allocator());
    node.ordinal = 0;
    node.full_identity = identity;
    node.identity = identity.low;
    node.parent = parent;
    const u32 index = static_cast<u32>(world.nodes().size());
    if (Status pushed = world.nodes().push_back(std::move(node)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

/// `NodeState::encode`, read into a node that already exists.
///
/// The children list is READ AND DROPPED on purpose: a child edge is the child's own `parent`, and
/// restoring it from both ends is two sources of truth for one relationship. A restore is always
/// accompanied by the children's own restores, because the editor records a subtree delete as one
/// operation per node.
[[nodiscard]] bool read_node_state(Reader& reader, World& world, u32 index) noexcept {
    EditorId parent;
    const bool has_parent = reader.option_identity(parent);
    const u32 child_count = reader.u32_value();
    for (u32 child = 0; child < child_count && reader.ok(); ++child) {
        (void)reader.u128_value();
    }
    const std::string_view layer = reader.text_value();
    if (reader.u8_value() == 1) {
        (void)reader.text_value();  // the prefab this node instantiates
    }
    const u32 components = reader.u32_value();
    if (!reader.ok()) {
        return false;
    }
    if (index != WorldNode::kNoParent) {
        WorldNode& node = world.nodes()[index];
        node.parent = has_parent ? locate(world, parent) : WorldNode::kNoParent;
        node.components().clear();
        const Expected<WorldText, Error> interned = world.intern(layer);
        if (!interned) {
            return false;
        }
        node.layer = *interned;
    }
    for (u32 which = 0; which < components && reader.ok(); ++which) {
        WorldComponent component(world.allocator());
        component.file_type = reader.u64_value();
        if (!read_fields(reader, world, component)) {
            return false;
        }
        if (index == WorldNode::kNoParent) {
            continue;
        }
        if (Status pushed = world.nodes()[index].components().push_back(std::move(component));
            !pushed) {
            return false;
        }
        resolve_component(world, world.nodes()[index].components().back());
    }
    const u32 overrides = reader.u32_value();
    for (u32 which = 0; which < overrides && reader.ok(); ++which) {
        (void)reader.u64_value();  // the component
        (void)reader.u64_value();  // the field
        WorldValue ignored;
        if (!read_value(reader, world, ignored)) {
            return false;
        }
    }
    return reader.ok();
}

/// Set one field of one component, adding the component when it is not there.
void set_field(World& world, u32 index, u64 file_type, u64 file_field, const WorldValue& value,
               TransactionReport& report) noexcept {
    WorldNode& node = world.nodes()[index];
    WorldComponent* component = node.find(file_type);
    if (component == nullptr) {
        WorldComponent created(world.allocator());
        created.file_type = file_type;
        if (Status pushed = node.components().push_back(std::move(created)); !pushed) {
            return;
        }
        component = &node.components().back();
    }
    if (WorldField* field = component->find(file_field); field != nullptr) {
        field->value = value;
    } else if (Status pushed = component->fields().push_back(WorldField{file_field, {}, value});
               !pushed) {
        return;
    }
    // A component the session created has no engine identifiers until they are filled in, and a
    // `Transform` the engine cannot recognise as its own is an object nothing draws.
    resolve_component(world, *component);
    report.applied += 1;
}

// --- one operation --------------------------------------------------------------------------

/// Every tag `cy_editor_documents::Operation::encode` writes.
enum class OperationTag : u8 {
    CreateNode = 0,
    DeleteNode = 1,
    RestoreNode = 2,
    Reparent = 3,
    AddComponent = 4,
    RemoveComponent = 5,
    SetField = 6,
    InstantiatePrefab = 7,
    SetOverride = 8,
    SetLayer = 9,
    SetAssetReference = 10,
    Domain = 11,
};

[[nodiscard]] bool apply_create(Reader& reader, World& world, TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    EditorId parent;
    const bool has_parent = reader.option_identity(parent);
    if (!reader.ok()) {
        return false;
    }
    if (locate(world, node) != WorldNode::kNoParent) {
        report.ignored += 1;  // already here: a redo of a create the runtime never lost
        return true;
    }
    const u32 owner = has_parent ? locate(world, parent) : WorldNode::kNoParent;
    if (!adopt(world, node, owner)) {
        return false;
    }
    report.created += 1;
    report.applied += 1;
    return true;
}

[[nodiscard]] bool apply_delete(Reader& reader, World& world, TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    const u32 index = locate(world, node);
    // The state is read whether or not the node is here: its bytes are in the stream either way.
    if (!read_node_state(reader, world, WorldNode::kNoParent)) {
        return false;
    }
    if (index == WorldNode::kNoParent) {
        report.unknown_nodes += 1;
        return true;
    }
    world.nodes()[index].live = false;
    report.deleted += 1;
    report.applied += 1;
    return true;
}

[[nodiscard]] bool apply_restore(Reader& reader, World& world, TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    u32 index = locate(world, node);
    if (index == WorldNode::kNoParent) {
        // A restore of a node this world dropped. Put it back rather than declining: undo is what
        // sends these, and an undo that left the viewport short an object would be undo lying.
        const Expected<u32, Error> created = adopt(world, node, WorldNode::kNoParent);
        if (!created) {
            return false;
        }
        index = *created;
    }
    world.nodes()[index].live = true;
    if (!read_node_state(reader, world, index)) {
        return false;
    }
    report.restored += 1;
    report.applied += 1;
    return true;
}

[[nodiscard]] bool apply_reparent(Reader& reader, World& world,
                                  TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    EditorId before;
    (void)reader.option_identity(before);
    EditorId after;
    const bool has_after = reader.option_identity(after);
    if (!reader.ok()) {
        return false;
    }
    const u32 index = locate(world, node);
    if (index == WorldNode::kNoParent) {
        report.unknown_nodes += 1;
        return true;
    }
    world.nodes()[index].parent = has_after ? locate(world, after) : WorldNode::kNoParent;
    report.applied += 1;
    return true;
}

[[nodiscard]] bool apply_add_component(Reader& reader, World& world,
                                       TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    const u64 file_type = reader.u64_value();
    const u32 index = locate(world, node);
    WorldComponent component(world.allocator());
    component.file_type = file_type;
    if (!read_fields(reader, world, component)) {
        return false;
    }
    if (index == WorldNode::kNoParent) {
        report.unknown_nodes += 1;
        return true;
    }
    WorldNode& owner = world.nodes()[index];
    if (WorldComponent* existing = owner.find(file_type); existing != nullptr) {
        existing->fields().clear();
        if (Status appended = existing->fields().append(component.fields().span()); !appended) {
            return false;
        }
        resolve_component(world, *existing);
    } else {
        if (Status pushed = owner.components().push_back(std::move(component)); !pushed) {
            return false;
        }
        resolve_component(world, owner.components().back());
    }
    report.applied += 1;
    return true;
}

[[nodiscard]] bool apply_remove_component(Reader& reader, World& world,
                                          TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    const u64 file_type = reader.u64_value();
    const u32 index = locate(world, node);
    if (!skip_fields(reader, world)) {
        return false;
    }
    if (index == WorldNode::kNoParent) {
        report.unknown_nodes += 1;
        return true;
    }
    Array<WorldComponent>& components = world.nodes()[index].components();
    // REBUILT IN ORDER rather than compacted in place: `WorldComponent` owns an `Array` and is
    // therefore not default-constructible, so there is no `resize` to shrink with, and the order
    // has to survive because it is the order the file is written in.
    Array<WorldComponent> kept(world.allocator());
    bool removed = false;
    for (WorldComponent& component : components) {
        if (component.file_type == file_type) {
            removed = true;
            continue;
        }
        if (Status pushed = kept.push_back(std::move(component)); !pushed) {
            return false;
        }
    }
    components = std::move(kept);
    if (removed) {
        report.applied += 1;
    } else {
        report.ignored += 1;
    }
    return true;
}

[[nodiscard]] bool apply_set_field(Reader& reader, World& world,
                                   TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    const u64 file_type = reader.u64_value();
    const u64 file_field = reader.u64_value();
    WorldValue before;
    WorldValue after;
    if (!read_value(reader, world, before) || !read_value(reader, world, after)) {
        return false;
    }
    const u32 index = locate(world, node);
    if (index == WorldNode::kNoParent) {
        report.unknown_nodes += 1;
        return true;
    }
    set_field(world, index, file_type, file_field, after, report);
    return true;
}

[[nodiscard]] bool apply_set_layer(Reader& reader, World& world,
                                   TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    (void)reader.text_value();
    const std::string_view after = reader.text_value();
    if (!reader.ok()) {
        return false;
    }
    const u32 index = locate(world, node);
    if (index == WorldNode::kNoParent) {
        report.unknown_nodes += 1;
        return true;
    }
    const Expected<WorldText, Error> interned = world.intern(after);
    if (!interned) {
        return false;
    }
    world.nodes()[index].layer = *interned;
    report.applied += 1;
    return true;
}

[[nodiscard]] bool apply_set_reference(Reader& reader, World& world,
                                       TransactionReport& report) noexcept {
    const EditorId node = reader.u128_value();
    const u64 file_type = reader.u64_value();
    const u64 file_field = reader.u64_value();
    (void)reader.text_value();
    const std::string_view after = reader.text_value();
    if (!reader.ok()) {
        return false;
    }
    const u32 index = locate(world, node);
    if (index == WorldNode::kNoParent) {
        report.unknown_nodes += 1;
        return true;
    }
    WorldValue value;
    value.kind = WorldValueKind::Text;
    if (Status interned = world.intern_blob(
            Span<const u8>(reinterpret_cast<const u8*>(after.data()), after.size()), value);
        !interned) {
        return false;
    }
    set_field(world, index, file_type, file_field, value, report);
    return true;
}

[[nodiscard]] bool apply_operation(Reader& reader, World& world, TransactionReport& report,
                                   bool& understood) noexcept {
    understood = true;
    const u8 tag = reader.u8_value();
    if (!reader.ok()) {
        return false;
    }
    switch (static_cast<OperationTag>(tag)) {
        case OperationTag::CreateNode:
            return apply_create(reader, world, report);
        case OperationTag::DeleteNode:
            return apply_delete(reader, world, report);
        case OperationTag::RestoreNode:
            return apply_restore(reader, world, report);
        case OperationTag::Reparent:
            return apply_reparent(reader, world, report);
        case OperationTag::AddComponent:
            return apply_add_component(reader, world, report);
        case OperationTag::RemoveComponent:
            return apply_remove_component(reader, world, report);
        case OperationTag::SetField:
            return apply_set_field(reader, world, report);
        case OperationTag::InstantiatePrefab: {
            // Decoded so the stream stays aligned; a prefab is not something this world resolves,
            // because resolution needs the prefab document and that is `resolve.h`'s.
            (void)reader.u128_value();
            (void)reader.text_value();
            EditorId parent;
            (void)reader.option_identity(parent);
            report.ignored += 1;
            return reader.ok();
        }
        case OperationTag::SetOverride: {
            const EditorId node = reader.u128_value();
            const u64 file_type = reader.u64_value();
            const u64 file_field = reader.u64_value();
            WorldValue before;
            if (reader.u8_value() == 1 && !read_value(reader, world, before)) {
                return false;
            }
            WorldValue after;
            const bool has_after = reader.u8_value() == 1;
            if (has_after && !read_value(reader, world, after)) {
                return false;
            }
            const u32 index = locate(world, node);
            if (index == WorldNode::kNoParent) {
                report.unknown_nodes += 1;
                return reader.ok();
            }
            // An override on a resolved world is a field value: the inherited half is already in
            // the node, because a world file carries flattened content rather than a reference.
            if (has_after) {
                set_field(world, index, file_type, file_field, after, report);
            } else {
                report.ignored += 1;
            }
            return reader.ok();
        }
        case OperationTag::SetLayer:
            return apply_set_layer(reader, world, report);
        case OperationTag::SetAssetReference:
            return apply_set_reference(reader, world, report);
        case OperationTag::Domain: {
            EditorId node;
            (void)reader.option_identity(node);
            (void)reader.text_value();
            (void)reader.bytes_value();
            (void)reader.bytes_value();
            report.ignored += 1;
            return reader.ok();
        }
    }
    understood = false;
    return false;
}

/// The header every transaction begins with: identity, document, name, actor, coalesce key, count.
[[nodiscard]] bool read_header(Reader& reader, TransactionReport& out) noexcept {
    out.transaction = reader.u64_value();
    out.document = reader.u128_value();
    (void)reader.text_value();  // the transaction's name
    const u8 actor = reader.u8_value();
    if (actor > 2) {
        return false;
    }
    // `encode_actor`: a human is one string, an agent is three, a system is one.
    const u32 strings = (actor == 1) ? 3U : 1U;
    for (u32 index = 0; index < strings; ++index) {
        (void)reader.text_value();
    }
    if (reader.u8_value() == 1) {
        (void)reader.text_value();  // the coalesce key
    }
    out.operations = reader.u32_value();
    return reader.ok();
}

}  // namespace

Status apply_transaction(World& world, Span<const u8> bytes, TransactionReport& out) noexcept {
    out = TransactionReport{};
    Reader reader(bytes);
    if (!read_header(reader, out)) {
        return fail(ErrorCode::InvalidArgument,
                    "a transaction ends inside its header, or names an actor kind this build has "
                    "not");
    }
    for (u32 index = 0; index < out.operations; ++index) {
        bool understood = false;
        if (apply_operation(reader, world, out, understood)) {
            continue;
        }
        return fail(ErrorCode::InvalidArgument,
                    understood ? "a transaction's operation is truncated"
                               : "a transaction carries an operation this build does not know, and "
                                 "its length cannot be guessed");
    }
    return ok();
}

Expected<TransactionReport, Error> read_transaction_header(Span<const u8> bytes) noexcept {
    TransactionReport report;
    Reader reader(bytes);
    if (!read_header(reader, report)) {
        return fail(ErrorCode::InvalidArgument, "a transaction ends inside its header");
    }
    return report;
}

}  // namespace cy::scene::serialization
