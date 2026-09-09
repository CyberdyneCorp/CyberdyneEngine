// `.cyworld`, read and written by the engine, and the editor's transactions applied to it.
// M8.a tasks 1.1 and 1.2.
//
// THE CLAIM UNDER TEST is not "this parser parses". It is that **the editor's document and the
// engine's world are one world**: that the file the editor saved is a file the engine reads back
// byte for byte, that a node in it has THE SAME IDENTITY on both sides with nothing transmitted,
// and that a transaction the editor committed moves the engine's object rather than an object
// associated with it in first-seen order.
//
// The identity cases carry literal numbers. They were produced independently — an FNV-1a-128 of the
// same bytes, written from the specification rather than from this code — and the matching Rust
// test `cy_editor_services::worldfile::tests::the_engine_derives_the_same_identities` pins the same
// numbers from the other side. A hash reimplemented and never compared is a hash that agrees until
// somebody changes a constant.

#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/scene/serialization/world_transaction.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using cy::Array;
using cy::f32;
using cy::i64;
using cy::Span;
using cy::Transform;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using namespace cy::scene::serialization;

[[nodiscard]] cy::Allocator& test_allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Assets);
}

/// The world the sample project carries, which is the one the editor writes and opens.
[[nodiscard]] std::string sample_world() {
    std::ifstream file(CY_SAMPLE_WORLD, std::ios::binary);
    CY_REQUIRE(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/// The path the EDITOR opens the sample world by. `cy-editor-app` passes exactly this string to
/// `open_document`, so it is what both sides hash.
constexpr std::string_view kAssetPath = "worlds/city.cyworld";

[[nodiscard]] AuthoringSchema scene_schema(cy::reflect::TypeRegistry& registry) {
    CY_REQUIRE(cy::reflect::register_scene_types(registry));
    AuthoringSchema schema(test_allocator());
    CY_REQUIRE(build_authoring_schema(registry, schema));
    return schema;
}

// --- the editor's codec, writing ---------------------------------------------------------------
//
// A transaction is BUILT here rather than captured, so that a case can name the exact operation it
// is about. The encoding is `cy_editor_core::codec`'s: little-endian, `f32` by its bits, a `u32`
// length before a byte string.

class TransactionBuilder {
public:
    explicit TransactionBuilder(cy::Allocator& allocator) noexcept : bytes_(allocator) {}

    void u8_value(u8 value) { CY_REQUIRE(bytes_.push_back(value)); }

    void u32_value(u32 value) {
        for (u32 index = 0; index < 4; ++index) {
            u8_value(static_cast<u8>(value >> (index * 8U)));
        }
    }

    void u64_value(u64 value) {
        for (u32 index = 0; index < 8; ++index) {
            u8_value(static_cast<u8>(value >> (index * 8U)));
        }
    }

    void identity(const EditorId& value) {
        u64_value(value.low);
        u64_value(value.high);
    }

    void f32_value(f32 value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32_value(bits);
    }

    void text(std::string_view value) {
        u32_value(static_cast<u32>(value.size()));
        for (const char character : value) {
            u8_value(static_cast<u8>(character));
        }
    }

    void vec3(f32 x, f32 y, f32 z) {
        u8_value(6);  // the Vec3 tag
        f32_value(x);
        f32_value(y);
        f32_value(z);
    }

    /// The header: identity, document, name, a human actor, no coalesce key, then a count.
    void header(const EditorId& document, u32 operations) {
        u64_value(7);
        identity(document);
        text("a test");
        u8_value(0);
        text("a person");
        u8_value(0);
        u32_value(operations);
    }

    [[nodiscard]] Span<const u8> span() const noexcept { return bytes_.span(); }

private:
    Array<u8> bytes_;
};

// --- identity ------------------------------------------------------------------------------

CY_TEST_CASE("the document identity is the editor's own") {
    const EditorId document = editor_document_identity(kAssetPath);
    // FNV-1a-128 of "worlds/city.cyworld", as `DocumentId::of_asset` computes it.
    CY_CHECK_EQ(document.high, 0xab40'8f05'37b6'c99aULL);
    CY_CHECK_EQ(document.low, 0x90fd'e4ab'baa2'06e8ULL);
}

CY_TEST_CASE("a node identity is the editor's own") {
    const EditorId document = editor_document_identity(kAssetPath);
    // `NodeId::in_document(document, ordinal)` for the first four ordinals. The low half is what
    // the protocol carries: `cy_editor_services::mirror::engine_identity`.
    CY_CHECK_EQ(editor_node_identity(document, 1).low, 0x539e'e13c'1a82'e51fULL);
    CY_CHECK_EQ(editor_node_identity(document, 2).low, 0x8fda'e6b9'cff7'097cULL);
    CY_CHECK_EQ(editor_node_identity(document, 3).low, 0xd11c'3a3a'937a'fd5dULL);
    CY_CHECK_EQ(editor_node_identity(document, 4).low, 0x0d58'3fb8'48ef'21baULL);
    CY_CHECK_EQ(editor_node_identity(document, 1).high, 0x004c'31f9'0838'c03aULL);
}

CY_TEST_CASE("two documents give one ordinal two identities") {
    const EditorId city = editor_document_identity("worlds/city.cyworld");
    const EditorId forest = editor_document_identity("worlds/forest.cyworld");
    CY_CHECK(!(editor_node_identity(city, 1) == editor_node_identity(forest, 1)));
}

// --- reading and writing ---------------------------------------------------------------------

CY_TEST_CASE("the engine reads the world the editor wrote") {
    const std::string text = sample_world();
    World world(test_allocator());
    WorldReadReport report;
    CY_REQUIRE(read_world(text, kAssetPath, world, &report));
    CY_CHECK_EQ(report.nodes, 3U);
    CY_CHECK_EQ(report.types, 7U);
    CY_CHECK_EQ(world.nodes().size(), usize{3});
    // The identities the editor would give these three nodes, with nothing transmitted.
    CY_CHECK_EQ(world.nodes()[0].identity, 0x539e'e13c'1a82'e51fULL);
    CY_CHECK_EQ(world.nodes()[1].identity, 0x8fda'e6b9'cff7'097cULL);
    CY_CHECK_EQ(world.nodes()[2].identity, 0xd11c'3a3a'937a'fd5dULL);
    CY_CHECK_EQ(world.index_of(0x8fda'e6b9'cff7'097cULL), 1U);
    CY_CHECK_EQ(world.index_of(1ULL), WorldNode::kNoParent);
}

CY_TEST_CASE("a world written back is the same bytes") {
    const std::string text = sample_world();
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    Array<char> written(test_allocator());
    CY_REQUIRE(write_world(world, written));
    const std::string_view produced(written.data(), written.size());
    CY_CHECK_EQ(produced, std::string_view(text));
}

CY_TEST_CASE("a world file this build cannot read is refused by name") {
    World world(test_allocator());
    CY_CHECK(!read_world("cydoc 1\n", kAssetPath, world));
    CY_CHECK(!read_world("cyworld 99\n", kAssetPath, world));
    CY_CHECK(!read_world("", kAssetPath, world));
}

// --- the engine's own types --------------------------------------------------------------------

CY_TEST_CASE("the file's Transform is the engine's LocalTransform") {
    cy::reflect::TypeRegistry registry;
    const AuthoringSchema schema = scene_schema(registry);
    const std::string text = sample_world();
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    const cy::Expected<u32, cy::Error> resolved = resolve_against(world, schema);
    CY_REQUIRE(resolved);
    CY_CHECK_EQ(*resolved, 7U);

    const cy::reflect::TypeInfo* local = registry.find("cy::scene::LocalTransform");
    CY_REQUIRE(local != nullptr);
    const WorldTypeDecl* declaration = world.type_of(local->id);
    CY_REQUIRE(declaration != nullptr);
    CY_CHECK_EQ(world.text(declaration->name), std::string_view("Transform"));

    // The placement, read through the ENGINE's own type rather than guessed from a value's shape.
    Transform placement;
    CY_REQUIRE(transform_of(world, world.nodes()[1], placement));
    CY_CHECK_EQ(placement.translation.x, 3.0F);
    CY_CHECK_EQ(placement.translation.y, 0.0F);
    CY_CHECK_EQ(placement.translation.z, -2.0F);
    CY_CHECK_EQ(placement.scale.x, 1.0F);
}

CY_TEST_CASE("a type this build has never heard of is carried, not dropped") {
    const std::string text =
        "cyworld 1\n"
        "type 1 runtime \"Transform\"\n"
        "  field 1 vec3 \"translation\" \"\"\n"
        "type 2 runtime \"AlienPluginThing\"\n"
        "  field 2 int \"count\" \"\"\n"
        "node 0 - \"default\"\n"
        "  component 2\n"
        "    field 2 41\n";
    cy::reflect::TypeRegistry registry;
    const AuthoringSchema schema = scene_schema(registry);
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    const cy::Expected<u32, cy::Error> resolved = resolve_against(world, schema);
    CY_REQUIRE(resolved);
    CY_CHECK_EQ(*resolved, 1U);  // Transform resolved; the plugin's type did not
    CY_CHECK(!world.types()[1].engine_type.valid());

    Array<char> written(test_allocator());
    CY_REQUIRE(write_world(world, written));
    CY_CHECK_EQ(std::string_view(written.data(), written.size()), std::string_view(text));
}

// --- transactions -----------------------------------------------------------------------------

CY_TEST_CASE("a move the editor committed moves the engine's node") {
    const std::string text = sample_world();
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    cy::reflect::TypeRegistry registry;
    const AuthoringSchema schema = scene_schema(registry);
    CY_REQUIRE(resolve_against(world, schema));

    // The FILE's identifiers, which are the DOCUMENT's: `Transform` is type 3 and `translation` is
    // field 4, straight out of the file's own type section. No mode flag, and no guess.
    TransactionBuilder builder(test_allocator());
    builder.header(world.document(), 1);
    builder.u8_value(6);  // SetField
    builder.identity(world.nodes()[1].full_identity);
    builder.u64_value(3);
    builder.u64_value(4);
    builder.vec3(3.0F, 0.0F, -2.0F);
    builder.vec3(3.0F, 5.0F, -2.0F);

    TransactionReport report;
    CY_REQUIRE(apply_transaction(world, builder.span(), report));
    CY_CHECK_EQ(report.operations, 1U);
    CY_CHECK_EQ(report.applied, 1U);
    CY_CHECK(verify_document_identity(world, report.document));

    Transform placement;
    CY_REQUIRE(transform_of(world, world.nodes()[1], placement));
    CY_CHECK_EQ(placement.translation.y, 5.0F);
    // And the other two are where they were: an edit addresses one node.
    CY_REQUIRE(transform_of(world, world.nodes()[0], placement));
    CY_CHECK_EQ(placement.translation.y, 0.0F);
}

CY_TEST_CASE("a scale the editor committed scales, rather than moving") {
    // The defect this closes, named: M7's runtime treated any changed `Vec3` as a translation when
    // the gizmo mode was Translate, because it could not tell which field it was.
    const std::string text = sample_world();
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    cy::reflect::TypeRegistry registry;
    const AuthoringSchema schema = scene_schema(registry);
    CY_REQUIRE(resolve_against(world, schema));

    TransactionBuilder builder(test_allocator());
    builder.header(world.document(), 1);
    builder.u8_value(6);
    builder.identity(world.nodes()[2].full_identity);
    builder.u64_value(3);
    builder.u64_value(5);  // `scale`, not `translation`
    builder.vec3(1.0F, 1.0F, 1.0F);
    builder.vec3(2.0F, 2.0F, 2.0F);

    TransactionReport report;
    CY_REQUIRE(apply_transaction(world, builder.span(), report));
    Transform placement;
    CY_REQUIRE(transform_of(world, world.nodes()[2], placement));
    CY_CHECK_EQ(placement.scale.x, 2.0F);
    CY_CHECK_EQ(placement.translation.x, -3.0F);
    CY_CHECK_EQ(placement.translation.z, 2.0F);
}

CY_TEST_CASE("an entity created in the editor exists in the engine's world") {
    const std::string text = sample_world();
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    cy::reflect::TypeRegistry registry;
    const AuthoringSchema schema = scene_schema(registry);
    CY_REQUIRE(resolve_against(world, schema));

    // The editor allocates ordinal 4 for the next node it creates, so this is the identity it
    // sends — derived here the same way, which is the whole point.
    const EditorId created = editor_node_identity(world.document(), 4);
    TransactionBuilder builder(test_allocator());
    builder.header(world.document(), 2);
    builder.u8_value(0);  // CreateNode
    builder.identity(created);
    builder.u8_value(0);  // no parent
    builder.u8_value(4);  // AddComponent
    builder.identity(created);
    builder.u64_value(3);  // Transform
    builder.u32_value(3);
    builder.u64_value(3);  // rotation
    builder.u8_value(8);   // the Quat tag
    builder.f32_value(0.0F);
    builder.f32_value(0.0F);
    builder.f32_value(0.0F);
    builder.f32_value(1.0F);
    builder.u64_value(4);  // translation
    builder.vec3(7.0F, 1.0F, -4.0F);
    builder.u64_value(5);  // scale
    builder.vec3(1.0F, 1.0F, 1.0F);

    TransactionReport report;
    CY_REQUIRE(apply_transaction(world, builder.span(), report));
    CY_CHECK_EQ(report.created, 1U);
    CY_CHECK_EQ(report.applied, 2U);
    CY_CHECK_EQ(world.nodes().size(), usize{4});

    const u32 index = world.index_of(created.low);
    CY_REQUIRE(index != WorldNode::kNoParent);
    Transform placement;
    CY_REQUIRE(transform_of(world, world.nodes()[index], placement));
    CY_CHECK_EQ(placement.translation.x, 7.0F);
    CY_CHECK_EQ(placement.translation.z, -4.0F);

    // And it survives a save: the world written now holds four nodes and reads back with them.
    Array<char> written(test_allocator());
    CY_REQUIRE(write_world(world, written));
    World again(test_allocator());
    WorldReadReport second;
    CY_REQUIRE(
        read_world(std::string_view(written.data(), written.size()), kAssetPath, again, &second));
    CY_CHECK_EQ(second.nodes, 4U);
}

CY_TEST_CASE("undoing a create removes the node again") {
    const std::string text = sample_world();
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    const EditorId created = editor_node_identity(world.document(), 4);

    TransactionBuilder create(test_allocator());
    create.header(world.document(), 1);
    create.u8_value(0);
    create.identity(created);
    create.u8_value(0);
    TransactionReport report;
    CY_REQUIRE(apply_transaction(world, create.span(), report));
    CY_CHECK_EQ(world.index_of(created.low) != WorldNode::kNoParent, true);

    // The inverse the editor sends on undo: a delete carrying the state it had.
    TransactionBuilder undo(test_allocator());
    undo.header(world.document(), 1);
    undo.u8_value(1);  // DeleteNode
    undo.identity(created);
    undo.u8_value(0);   // no parent
    undo.u32_value(0);  // no children
    undo.text("");      // the layer
    undo.u8_value(0);   // no prefab
    undo.u32_value(0);  // no components
    undo.u32_value(0);  // no overrides
    CY_REQUIRE(apply_transaction(world, undo.span(), report));
    CY_CHECK_EQ(report.deleted, 1U);
    CY_CHECK_EQ(world.index_of(created.low), WorldNode::kNoParent);

    // And the file written afterwards is the file that was read: undo returns the world exactly.
    Array<char> written(test_allocator());
    CY_REQUIRE(write_world(world, written));
    CY_CHECK_EQ(std::string_view(written.data(), written.size()), std::string_view(text));
}

CY_TEST_CASE("a transaction for another document changes nothing") {
    const std::string text = sample_world();
    World world(test_allocator());
    CY_REQUIRE(read_world(text, kAssetPath, world));
    const EditorId elsewhere = editor_document_identity("worlds/forest.cyworld");

    TransactionBuilder builder(test_allocator());
    builder.header(elsewhere, 1);
    builder.u8_value(6);
    builder.identity(editor_node_identity(elsewhere, 1));
    builder.u64_value(3);
    builder.u64_value(4);
    builder.vec3(0.0F, 0.0F, 0.0F);
    builder.vec3(9.0F, 9.0F, 9.0F);

    TransactionReport report;
    CY_REQUIRE(apply_transaction(world, builder.span(), report));
    CY_CHECK(!verify_document_identity(world, report.document));
    CY_CHECK_EQ(report.unknown_nodes, 1U);
    CY_CHECK_EQ(report.applied, 0U);
}

CY_TEST_CASE("an operation this build does not know refuses the transaction") {
    World world(test_allocator());
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world));
    TransactionBuilder builder(test_allocator());
    builder.header(world.document(), 1);
    builder.u8_value(200);  // a tag from a newer editor
    TransactionReport report;
    CY_CHECK(!apply_transaction(world, builder.span(), report));
}

CY_TEST_CASE("a truncated transaction is refused rather than half read") {
    World world(test_allocator());
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world));
    TransactionBuilder builder(test_allocator());
    builder.header(world.document(), 1);
    builder.u8_value(6);
    builder.identity(world.nodes()[0].full_identity);
    builder.u64_value(3);
    // and nothing after it
    TransactionReport report;
    CY_CHECK(!apply_transaction(world, builder.span(), report));
}

CY_TEST_CASE("a header read alone names the document") {
    World world(test_allocator());
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world));
    TransactionBuilder builder(test_allocator());
    builder.header(world.document(), 0);
    const cy::Expected<TransactionReport, cy::Error> header =
        read_transaction_header(builder.span());
    CY_REQUIRE(header);
    CY_CHECK_EQ(header->transaction, 7ULL);
    CY_CHECK(verify_document_identity(world, header->document));
}

}  // namespace
