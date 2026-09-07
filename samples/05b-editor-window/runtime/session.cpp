// What the runtime holds on the editor's behalf. See session.h.

#include "session.h"

#include <cstring>

namespace cy::sample::editor_window {
namespace {

/// The editor's codec, reading. `cy_editor_core::codec`: little-endian, `f32` by its bits, a `u32`
/// length before a byte string. Bounds-checked at every step, because these bytes came off a
/// socket.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u8_value(u8& out) noexcept {
        if (offset_ + 1 > bytes_.size()) {
            return false;
        }
        out = bytes_[offset_++];
        return true;
    }

    [[nodiscard]] bool u32_value(u32& out) noexcept { return little_endian(out, 4); }
    [[nodiscard]] bool u64_value(u64& out) noexcept { return little_endian(out, 8); }

    /// A 128-bit identity, read as its two halves. Nothing here needs the value as a number, only
    /// as an identity, so the low half is what is kept — see `read_translations` for why that is
    /// safe.
    [[nodiscard]] bool u128_low(u64& out) noexcept {
        u64 high = 0;
        return u64_value(out) && u64_value(high);
    }

    [[nodiscard]] bool f32_value(f32& out) noexcept {
        u32 bits = 0;
        if (!u32_value(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    [[nodiscard]] bool skip(usize count) noexcept {
        if (offset_ + count > bytes_.size()) {
            return false;
        }
        offset_ += count;
        return true;
    }

    [[nodiscard]] bool skip_bytes() noexcept {
        u32 length = 0;
        return u32_value(length) && skip(length);
    }

private:
    template <class T>
    [[nodiscard]] bool little_endian(T& out, usize width) noexcept {
        if (offset_ + width > bytes_.size()) {
            return false;
        }
        T value = 0;
        for (usize index = 0; index < width; ++index) {
            value |= static_cast<T>(static_cast<T>(bytes_[offset_ + index])
                                    << static_cast<T>(index * 8));
        }
        offset_ += width;
        out = value;
        return true;
    }

    Span<const u8> bytes_;
    usize offset_ = 0;
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

/// Read one value, reporting whether it was a `Vec3` and what it held.
[[nodiscard]] bool read_value(Reader& reader, bool& is_vec3, Vec3& lanes) noexcept {
    u8 tag = 0;
    if (!reader.u8_value(tag)) {
        return false;
    }
    is_vec3 = false;
    switch (static_cast<ValueTag>(tag)) {
        case ValueTag::Nil:
            return true;
        case ValueTag::Bool:
            return reader.skip(1);
        case ValueTag::Int:
        case ValueTag::Double:
        case ValueTag::Entity:
            return reader.skip(8);
        case ValueTag::Float:
            return reader.skip(4);
        case ValueTag::Vec2:
            return reader.skip(8);
        case ValueTag::Vec3:
            is_vec3 = true;
            return reader.f32_value(lanes.x) && reader.f32_value(lanes.y) &&
                   reader.f32_value(lanes.z);
        case ValueTag::Vec4:
        case ValueTag::Quat:
            return reader.skip(16);
        case ValueTag::Text:
        case ValueTag::Bytes:
            return reader.skip_bytes();
    }
    // A tag from a newer editor. The whole transaction is abandoned rather than half-applied: the
    // reader cannot know how many bytes to skip, so everything after this point is unreadable.
    return false;
}

/// Skip the operations this runtime has no object for, by their own encoding.
///
/// Every branch is `cy_editor_documents::operation`'s, restated. An operation this function does
/// not know makes the whole transaction unreadable rather than being guessed past — a skip of the
/// wrong length would misread the NEXT operation as something it is not, which is worse than
/// declining.
[[nodiscard]] bool skip_fields(Reader& reader) noexcept {
    u32 count = 0;
    if (!reader.u32_value(count)) {
        return false;
    }
    for (u32 index = 0; index < count; ++index) {
        u64 field = 0;
        bool is_vec3 = false;
        Vec3 lanes{0.0F, 0.0F, 0.0F};
        if (!reader.u64_value(field) || !read_value(reader, is_vec3, lanes)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool skip_node_state(Reader& reader) noexcept {
    // `NodeState::encode`: the name, the parent option, the component list. Its exact shape is more
    // than this file needs, and a partial skip would desynchronise the reader — so a transaction
    // carrying one is declined rather than half-read. Creates and deletes are M8's, when the worlds
    // are shared and the runtime has somewhere to put them.
    (void)reader;
    return false;
}

}  // namespace

u32 EditorSession::object_for(u64 identity, u32 object_count) noexcept {
    if (object_count == 0) {
        return kNoObject;
    }
    for (const Association& association : associations_) {
        if (association.identity == identity) {
            return association.object;
        }
    }
    const u32 next = static_cast<u32>(associations_.size() % object_count);
    if (Status pushed = associations_.push_back(Association{identity, next}); !pushed) {
        return next;
    }
    return next;
}

Expected<u32, Error> read_translations(Span<const u8> transaction,
                                       Array<TranslationDelta>& out) noexcept {
    out.clear();
    Reader reader(transaction);
    u64 ignored = 0;
    u8 actor_tag = 0;
    u8 has_key = 0;
    u32 operations = 0;
    if (!reader.u64_value(ignored) ||  // the transaction's identity
        !reader.u128_low(ignored) ||   // the document's
        !reader.skip_bytes() ||        // its name
        !reader.u8_value(actor_tag)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a transaction ends before its actor"});
    }
    // `encode_actor`: a human is one string, an agent is three, a system is one.
    const u32 actor_strings = (actor_tag == 1) ? 3U : 1U;
    if (actor_tag > 2) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a transaction names an actor kind this build has not"});
    }
    for (u32 index = 0; index < actor_strings; ++index) {
        if (!reader.skip_bytes()) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "a transaction ends inside its actor"});
        }
    }
    if (!reader.u8_value(has_key) || (has_key == 1 && !reader.skip_bytes()) ||
        !reader.u32_value(operations)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a transaction ends before its operations"});
    }

    for (u32 index = 0; index < operations; ++index) {
        u8 tag = 0;
        if (!reader.u8_value(tag)) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "a transaction ends inside its operations"});
        }
        u64 node = 0;
        switch (tag) {
            case 6: {  // SetField
                u64 component = 0;
                u64 field = 0;
                bool before_is_vec3 = false;
                bool after_is_vec3 = false;
                Vec3 before{0.0F, 0.0F, 0.0F};
                Vec3 after{0.0F, 0.0F, 0.0F};
                if (!reader.u128_low(node) || !reader.u64_value(component) ||
                    !reader.u64_value(field) || !read_value(reader, before_is_vec3, before) ||
                    !read_value(reader, after_is_vec3, after)) {
                    return make_unexpected(
                        Error{ErrorCode::InvalidArgument, "a set-field operation is truncated"});
                }
                if (before_is_vec3 && after_is_vec3) {
                    const Vec3 amount{after.x - before.x, after.y - before.y, after.z - before.z};
                    if (Status pushed = out.push_back(TranslationDelta{node, amount}); !pushed) {
                        return make_unexpected(pushed.error());
                    }
                }
                break;
            }
            case 4:    // AddComponent
            case 5: {  // RemoveComponent
                u64 component = 0;
                if (!reader.u128_low(node) || !reader.u64_value(component) ||
                    !skip_fields(reader)) {
                    return make_unexpected(
                        Error{ErrorCode::InvalidArgument, "a component operation is truncated"});
                }
                break;
            }
            case 0: {  // CreateNode: the node, then an optional parent
                u8 has_parent = 0;
                u64 parent = 0;
                if (!reader.u128_low(node) || !reader.u8_value(has_parent) ||
                    (has_parent == 1 && !reader.u128_low(parent))) {
                    return make_unexpected(
                        Error{ErrorCode::InvalidArgument, "a create-node operation is truncated"});
                }
                break;
            }
            case 3: {  // Reparent: the node, then two optional parents
                for (u32 which = 0; which < 3; ++which) {
                    u8 has_parent = 0;
                    if (which == 0) {
                        if (!reader.u128_low(node)) {
                            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                                         "a reparent operation is truncated"});
                        }
                        continue;
                    }
                    u64 parent = 0;
                    if (!reader.u8_value(has_parent) ||
                        (has_parent == 1 && !reader.u128_low(parent))) {
                        return make_unexpected(
                            Error{ErrorCode::InvalidArgument, "a reparent operation is truncated"});
                    }
                }
                break;
            }
            default:
                // A delete, a restore, or something newer. Declined rather than skipped by a
                // guessed length: see `skip_node_state`.
                (void)skip_node_state(reader);
                return make_unexpected(Error{
                    ErrorCode::NotImplemented,
                    "this runtime reads moves and component edits; a create or delete needs the "
                    "shared world M8's live editing brings"});
        }
    }
    return operations;
}

}  // namespace cy::sample::editor_window
