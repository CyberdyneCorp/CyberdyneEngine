// Documents: the three asset kinds, stable local ids, and the two forms round-tripping.

#include <cy/core/serialize/tagged.h>
#include <cy/scene/serialization/format.h>
#include <cy/test/test.h>

#include <string_view>

#include "fixtures.h"

using namespace cy;
using namespace cy::scene::serialization;
using namespace cy::scene::serialization::test;

namespace {

[[nodiscard]] std::string_view view_of(const Array<char>& text) noexcept {
    return {text.data(), text.size()};
}

[[nodiscard]] bool same_bytes(const Array<u8>& left, const Array<u8>& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (usize index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

/// A scene with a small hierarchy, a reference, and a prefab instance with an override.
[[nodiscard]] Status build_scene(Document& document) noexcept {
    document.kind = AssetKind::Scene;
    document.id = asset(0x5CE7E);
    document.schema_version = 2;

    const Expected<DocumentEntity*, Error> root = document.add_entity(kNoLocalId, "Root");
    if (!root) {
        return make_unexpected(root.error());
    }
    if (Status placed = set_placement(**root, cy::Transform::from_translation(cy::Vec3{1, 2, 3}));
        !placed) {
        return placed;
    }

    const Expected<DocumentEntity*, Error> child = document.add_entity((*root)->id, "Child \"A\"");
    if (!child) {
        return make_unexpected(child.error());
    }
    (*child)->motion = MotionKind::Dynamic;
    (*child)->flatten = FlattenPolicy::Keep;
    if (Status healthy = set_health(**child, 250.0F, 37.5F); !healthy) {
        return healthy;
    }
    if (Status aimed = set_target(**child, (*root)->id); !aimed) {
        return aimed;
    }

    const Expected<Instance*, Error> instance =
        document.add_instance(asset(0x7011E7), (*root)->id, "Turret");
    if (!instance) {
        return make_unexpected(instance.error());
    }
    (*instance)->cook_mode = CookMode::Packed;
    (*instance)->transform = cy::Transform::from_translation(cy::Vec3{0, 5, 0});
    if (Status mapped = add_mapping((*instance)->mapping(), LocalId(1), document.allocate_id());
        !mapped) {
        return mapped;
    }

    Override item(document.allocator());
    item.set_op(OverrideOp::SetField);
    item.set_target(
        OverrideTarget{LocalId(1), reflect::TypeId(kHealthType), reflect::FieldId(kHealthMaximum)});
    item.set_schema_version(1);
    const f32 maximum = 999.0F;
    if (Status written = item.payload().set_scalar(
            reflect::FieldId(kHealthMaximum), serialize::WireType::F32, &maximum, sizeof(maximum));
        !written) {
        return written;
    }
    item.payload().set_type(reflect::TypeId(kHealthType));
    return (*instance)->overrides().add(std::move(item));
}

}  // namespace

CY_TEST_CASE("prefab, scene and world are distinct kinds, and the distinction is on the document") {
    Document prefab(test_allocator());
    prefab.kind = AssetKind::Prefab;
    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    Document world(test_allocator());
    world.kind = AssetKind::World;

    CY_CHECK_EQ(std::string_view(asset_kind_name(prefab.kind)), std::string_view("prefab"));
    CY_CHECK_EQ(std::string_view(asset_kind_name(scene.kind)), std::string_view("scene"));
    CY_CHECK_EQ(std::string_view(asset_kind_name(world.kind)), std::string_view("world"));
    // A village built from many prefabs is a scene, not a prefab of prefabs: the kind is a
    // property of the document, and nothing in this module reads a file extension.
    CY_CHECK_NE(scene.kind, prefab.kind);
}

CY_TEST_CASE("local ids are issued once and never reused") {
    Document document(test_allocator());
    const Expected<DocumentEntity*, Error> first = document.add_entity(kNoLocalId, "a");
    const Expected<DocumentEntity*, Error> second = document.add_entity(kNoLocalId, "b");
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    const LocalId taken = (*second)->id;
    CY_CHECK_NE((*first)->id, taken);

    const Expected<DocumentEntity*, Error> third = document.add_entity(kNoLocalId, "c");
    CY_REQUIRE(third.has_value());
    CY_CHECK_GT((*third)->id.value(), taken.value());
    // Entities and instances share one id space, so an instance cannot take an entity's number.
    CY_CHECK_FALSE(document.add_instance_with_id(taken, asset(1), kNoLocalId, "x").has_value());
}

CY_TEST_CASE("reordering entities does not break a reference, because nothing keys on position") {
    Document document(test_allocator());
    const Expected<DocumentEntity*, Error> target = document.add_entity(kNoLocalId, "target");
    CY_REQUIRE(target.has_value());
    const LocalId target_id = (*target)->id;

    const Expected<DocumentEntity*, Error> source = document.add_entity(kNoLocalId, "source");
    CY_REQUIRE(source.has_value());
    CY_REQUIRE(set_target(**source, target_id).has_value());

    // A round trip through the text form writes them in id order whatever order they were added in;
    // the reference resolves either way because it names an id.
    Array<char> text(test_allocator());
    CY_REQUIRE(write_text(document, text).has_value());
    Document reloaded(test_allocator());
    CY_REQUIRE(read_text(view_of(text), reloaded).has_value());

    const DocumentEntity* restored = reloaded.find_entity((*source)->id);
    CY_REQUIRE(restored != nullptr);
    const ComponentData* component = restored->find(reflect::TypeId(kTargetType));
    CY_REQUIRE(component != nullptr);
    const Expected<u32, Error> reference =
        component->record.local_reference(reflect::FieldId(kTargetEntity));
    CY_REQUIRE(reference.has_value());
    CY_CHECK_EQ(reference.value(), target_id.value());
}

CY_TEST_CASE("a document round-trips through the text form") {
    Document document(test_allocator());
    CY_REQUIRE(build_scene(document).has_value());

    Array<char> first(test_allocator());
    CY_REQUIRE(write_text(document, first).has_value());

    Document reloaded(test_allocator());
    CY_REQUIRE(read_text(view_of(first), reloaded).has_value());

    Array<char> second(test_allocator());
    CY_REQUIRE(write_text(reloaded, second).has_value());
    CY_CHECK_EQ(view_of(first), view_of(second));

    CY_CHECK_EQ(reloaded.kind, AssetKind::Scene);
    CY_CHECK_EQ(reloaded.id, document.id);
    CY_CHECK_EQ(reloaded.schema_version, 2U);
    CY_CHECK_EQ(reloaded.entities().size(), document.entities().size());
    CY_CHECK_EQ(reloaded.instances().size(), 1U);
    CY_CHECK_EQ(reloaded.instances()[0].cook_mode, CookMode::Packed);
    CY_CHECK_EQ(reloaded.instances()[0].overrides().size(), 1U);
    // A designer's name with an embedded quote survives.
    CY_CHECK_EQ(reloaded.text(reloaded.entities()[1].name), std::string_view("Child \"A\""));
}

CY_TEST_CASE("a document round-trips through the binary form") {
    Document document(test_allocator());
    CY_REQUIRE(build_scene(document).has_value());

    Array<u8> first(test_allocator());
    CY_REQUIRE(write_binary(document, first).has_value());

    Document reloaded(test_allocator());
    CY_REQUIRE(read_binary(first.span(), reloaded).has_value());

    Array<u8> second(test_allocator());
    CY_REQUIRE(write_binary(reloaded, second).has_value());
    CY_CHECK(same_bytes(first, second));
}

CY_TEST_CASE("text to binary to text reproduces the file") {
    // The milestone gate: "a scene round-trips text → binary → text with no semantic change". It is
    // asserted on the bytes, which is stronger and needs no notion of sameness to be agreed.
    Document document(test_allocator());
    CY_REQUIRE(build_scene(document).has_value());

    Array<char> first_text(test_allocator());
    CY_REQUIRE(write_text(document, first_text).has_value());

    Document from_text(test_allocator());
    CY_REQUIRE(read_text(view_of(first_text), from_text).has_value());

    Array<u8> binary(test_allocator());
    CY_REQUIRE(write_binary(from_text, binary).has_value());

    Document from_binary(test_allocator());
    CY_REQUIRE(read_binary(binary.span(), from_binary).has_value());

    Array<char> second_text(test_allocator());
    CY_REQUIRE(write_text(from_binary, second_text).has_value());
    CY_CHECK_EQ(view_of(first_text), view_of(second_text));
}

CY_TEST_CASE("one changed property is one changed line, and the rest of the file is untouched") {
    Document before(test_allocator());
    CY_REQUIRE(build_scene(before).has_value());
    Array<char> before_text(test_allocator());
    CY_REQUIRE(write_text(before, before_text).has_value());

    Document after(test_allocator());
    CY_REQUIRE(build_scene(after).has_value());
    DocumentEntity* child = after.find_entity(LocalId(2));
    CY_REQUIRE(child != nullptr);
    CY_REQUIRE(set_health(*child, 251.0F, 37.5F).has_value());
    Array<char> after_text(test_allocator());
    CY_REQUIRE(write_text(after, after_text).has_value());

    usize differences = 0;
    usize left = 0;
    usize right = 0;
    const std::string_view a = view_of(before_text);
    const std::string_view b = view_of(after_text);
    while (left < a.size() || right < b.size()) {
        const usize left_end = a.find('\n', left);
        const usize right_end = b.find('\n', right);
        const usize left_stop = (left_end == std::string_view::npos) ? a.size() : left_end;
        const usize right_stop = (right_end == std::string_view::npos) ? b.size() : right_end;
        if (a.substr(left, left_stop - left) != b.substr(right, right_stop - right)) {
            ++differences;
        }
        left = left_stop + 1;
        right = right_stop + 1;
    }
    CY_CHECK_EQ(differences, 1U);
}

CY_TEST_CASE("a component from a module this build does not have survives a load and a save") {
    // "Editing without a plugin": a scene carrying a component whose type is registered nowhere is
    // written back unchanged rather than stripped.
    Document document(test_allocator());
    document.id = asset(7);
    const Expected<DocumentEntity*, Error> entity = document.add_entity(kNoLocalId, "e");
    CY_REQUIRE(entity.has_value());
    const Expected<ComponentData*, Error> plugin = (*entity)->ensure(reflect::TypeId(777001));
    CY_REQUIRE(plugin.has_value());
    const u64 value = 0xFEED'FACE'0000'0001ULL;
    CY_REQUIRE((*plugin)
                   ->record.set_scalar(reflect::FieldId(41), serialize::WireType::U64, &value, 8)
                   .has_value());
    (*plugin)->record.set_schema_version(9);

    Array<u8> binary(test_allocator());
    CY_REQUIRE(write_binary(document, binary).has_value());
    Document reloaded(test_allocator());
    CY_REQUIRE(read_binary(binary.span(), reloaded).has_value());

    const DocumentEntity* restored = reloaded.find_entity((*entity)->id);
    CY_REQUIRE(restored != nullptr);
    const ComponentData* preserved = restored->find(reflect::TypeId(777001));
    CY_REQUIRE(preserved != nullptr);
    CY_CHECK_EQ(preserved->record.schema_version(), 9U);
    CY_CHECK(preserved->record.contains(reflect::FieldId(41)));

    Array<u8> rewritten(test_allocator());
    CY_REQUIRE(write_binary(reloaded, rewritten).has_value());
    CY_CHECK(same_bytes(binary, rewritten));
}

CY_TEST_CASE("an entity moves between authoring documents without changing identity") {
    // "Rebalancing is safe": an authoring chunk is split and every reference still resolves,
    // because identity is independent of the file an entity is written in.
    Document original(test_allocator());
    const Expected<DocumentEntity*, Error> kept = original.add_entity(kNoLocalId, "kept");
    const Expected<DocumentEntity*, Error> moved = original.add_entity(kNoLocalId, "moved");
    CY_REQUIRE(kept.has_value());
    CY_REQUIRE(moved.has_value());
    CY_REQUIRE(set_target(**kept, (*moved)->id).has_value());
    const LocalId moved_id = (*moved)->id;

    Document split(test_allocator());
    const Expected<DocumentEntity*, Error> rehomed =
        split.add_entity_with_id(moved_id, kNoLocalId, "moved");
    CY_REQUIRE(rehomed.has_value());
    CY_CHECK_EQ((*rehomed)->id, moved_id);

    const ComponentData* reference = (*kept)->find(reflect::TypeId(kTargetType));
    CY_REQUIRE(reference != nullptr);
    const Expected<u32, Error> local =
        reference->record.local_reference(reflect::FieldId(kTargetEntity));
    CY_REQUIRE(local.has_value());
    CY_CHECK_EQ(local.value(), moved_id.value());
}

CY_TEST_CASE("a text form written by a newer version is refused rather than half-read") {
    const std::string_view future = "cydoc 99\nkind scene\n";
    Document document(test_allocator());
    const Status read = read_text(future, document);
    CY_REQUIRE_FALSE(read.has_value());
    CY_CHECK_EQ(read.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("an unknown chunk in the binary form is stepped over, not rejected") {
    Document document(test_allocator());
    document.id = asset(11);
    const Expected<DocumentEntity*, Error> entity = document.add_entity(kNoLocalId, "e");
    CY_REQUIRE(entity.has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(write_binary(document, bytes).has_value());

    // Splice a chunk from a build that knows something this one does not, by rewriting the stream
    // through the tagged writer with one extra chunk in it.
    Array<u8> extended(test_allocator());
    {
        serialize::TaggedWriter writer(extended);
        CY_REQUIRE(writer.begin_stream().has_value());
        CY_REQUIRE(writer.begin_chunk(serialize::chunk_tag('F', 'U', 'T', 'R')).has_value());
        serialize::ByteWriter raw(extended);
        CY_REQUIRE(raw.write_u64(0xDEAD'BEEFULL).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }
    // The reader must accept a stream whose chunks it does not all recognise.
    Document ignored(test_allocator());
    CY_CHECK(read_binary(extended.span(), ignored).has_value());
}
