// A node has a name. `editor-documents-and-transactions`, and M11.b task 4.1.
//
// ================================================================================================
// THE CLAIM UNDER TEST
// ================================================================================================
//
// The requirement is one sentence — *"a node in an authoring document SHALL carry an author-given
// name, distinct from its identity and distinct from every other property it happens to have"* —
// and it outlived four milestones because nothing could check it. The consequence was visible in
// the repository rather than hidden in it: `samples/05b-editor-window/project/worlds/city.cyworld`
// read `node 0 - "Pillar"`, **and the third field of a `node` line is the node's LAYER**, so the
// only authored world committed here put three objects in three one-node layers because a name had
// nowhere else to go. M8.a's own screenshot showed two authored objects as two outliner rows both
// reading `Transform`, which is the node's kind.
//
// So the cases below are about the three separations the requirement asks for, not about a field
// existing:
//
//   * a name is not an IDENTITY — a rename leaves every operation, reference and journal entry
//     addressing what it addressed, which here is `World::index_of` still finding the node by the
//     identity a transaction names;
//   * a name is not a LAYER — the two round-trip as separate values, and two nodes share a layer
//     and differ by name;
//   * a document that encodes a name in the layer field is REPORTED — `layers_used_as_names`, and
//     it is reported rather than refused because the only way to move a name out of the layer field
//     is to open the world it is in.
//
// ================================================================================================
// WHY THIS SUITE IS HERE AND NOT IN `cargo test`
// ================================================================================================
//
// The document model is the editor's, and the editor is a Rust workspace whose own tests cover the
// Rust half — `cy_editor_documents::content`, `cy_editor_services::worldfile` and
// `cy_editor_viewmodels::hierarchy` each carry the matching case. What cannot be written there is
// the half this file is about: the ENGINE reads the same `.cyworld` and applies the same operation
// stream, so a name that existed only in the editor would be a name the runtime drops on the first
// save — which is the exact shape of the defect `worldfile.h`'s header was written to close for
// identity. Two readers of one format need a test on each side or they agree until one changes.
//
// tests/editor/README.md said this directory should be added to `tests/CMakeLists.txt` by whoever
// next edited it. This is that edit.

#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/scene/serialization/world_transaction.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/test/test.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using cy::Array;
using cy::Span;
using cy::u32;
using cy::u64;
using cy::u8;
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

constexpr std::string_view kAssetPath = "worlds/city.cyworld";

/// A world whose nodes carry no name and whose layers are one per node: the shape the sample had
/// before this rung, written out literally so the check has something to recognise.
constexpr std::string_view kNamesInTheLayerField =
    "cyworld 1\n"
    "type 1 runtime \"Transform\"\n"
    "  field 1 vec3 \"translation\" \"\"\n"
    "node 0 - \"Pillar\"\n"
    "node 1 - \"Crate\"\n"
    "node 2 - \"Marker\"\n";

/// The same three nodes, named, in two layers. What the requirement asks for.
constexpr std::string_view kNamedInTwoLayers =
    "cyworld 1\n"
    "type 1 runtime \"Transform\"\n"
    "  field 1 vec3 \"translation\" \"\"\n"
    "node 0 - \"set\" \"Pillar\"\n"
    "node 1 - \"set\" \"Crate\"\n"
    "node 2 - \"props\" \"Marker\"\n";

/// The editor's transaction encoding, enough of it to spell one rename.
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

    void text(std::string_view value) {
        u32_value(static_cast<u32>(value.size()));
        for (const char character : value) {
            u8_value(static_cast<u8>(character));
        }
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

    /// `Operation::SetName`, tag 12.
    void rename(const EditorId& node, std::string_view before, std::string_view after) {
        u8_value(12);
        identity(node);
        text(before);
        text(after);
    }

    [[nodiscard]] Span<const u8> span() const noexcept { return bytes_.span(); }

private:
    Array<u8> bytes_;
};

[[nodiscard]] std::string written(const World& world) {
    Array<char> out(test_allocator());
    CY_REQUIRE(write_world(world, out));
    return {out.data(), out.size()};
}

// --- the name itself ----------------------------------------------------------------------------

CY_TEST_CASE("a node has a name, and it is the name the author gave it") {
    World world(test_allocator());
    WorldReadReport report;
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world, &report));

    CY_CHECK_EQ(report.nodes, 3U);
    CY_CHECK_EQ(report.named, 3U);
    CY_CHECK_EQ(world.text(world.nodes()[0].name), std::string_view("Pillar"));
    CY_CHECK_EQ(world.text(world.nodes()[1].name), std::string_view("Crate"));
    CY_CHECK_EQ(world.text(world.nodes()[2].name), std::string_view("Marker"));
}

CY_TEST_CASE("a node has a name that is not its layer") {
    // The requirement's third scenario: "each node's name and its layer SHALL round-trip as
    // separate values". Two of these three share a layer and differ by name, which is the half a
    // name-in-the-layer-field world cannot express at all.
    World world(test_allocator());
    CY_REQUIRE(read_world(kNamedInTwoLayers, kAssetPath, world));

    CY_CHECK_EQ(world.text(world.nodes()[0].layer), std::string_view("set"));
    CY_CHECK_EQ(world.text(world.nodes()[1].layer), std::string_view("set"));
    CY_CHECK_EQ(world.text(world.nodes()[0].name), std::string_view("Pillar"));
    CY_CHECK_EQ(world.text(world.nodes()[1].name), std::string_view("Crate"));
    CY_CHECK(world.text(world.nodes()[0].name) != world.text(world.nodes()[1].name));
    CY_CHECK_EQ(world.text(world.nodes()[2].layer), std::string_view("props"));
}

CY_TEST_CASE("a node has a name that survives a round trip") {
    World world(test_allocator());
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world));
    CY_CHECK_EQ(written(world), sample_world());
}

CY_TEST_CASE("a node has a name and two nodes of one kind are distinguishable") {
    // "WHEN an author creates two nodes carrying the same first component and names them
    // differently THEN the hierarchy SHALL show two rows bearing the two names". The hierarchy is
    // the editor's; what the engine owes is that the two names reach it distinctly, which before
    // this rung they could not — both nodes carried one `Transform` and nothing else.
    World world(test_allocator());
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world));
    CY_REQUIRE(world.nodes().size() >= 2);
    CY_CHECK_EQ(world.nodes()[0].components().size(), world.nodes()[1].components().size());
    CY_CHECK_EQ(world.nodes()[0].components()[0].file_type,
                world.nodes()[1].components()[0].file_type);
    CY_CHECK(world.text(world.nodes()[0].name) != world.text(world.nodes()[1].name));
}

// --- a name is not an identity -------------------------------------------------------------------

CY_TEST_CASE("a node has a name that a rename changes without moving its identity") {
    World world(test_allocator());
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world));
    const EditorId subject = world.nodes()[1].full_identity;
    const u32 before_index = world.index_of(subject.low);
    CY_REQUIRE(before_index != WorldNode::kNoParent);

    TransactionBuilder rename(test_allocator());
    rename.header(world.document(), 1);
    rename.rename(subject, "Crate", "Barrel");
    TransactionReport report;
    CY_REQUIRE(apply_transaction(world, rename.span(), report));

    CY_CHECK_EQ(report.applied, 1U);
    CY_CHECK_EQ(world.text(world.nodes()[before_index].name), std::string_view("Barrel"));
    // "every operation, reference and journal entry addressing it SHALL still address it".
    CY_CHECK_EQ(world.index_of(subject.low), before_index);
    CY_CHECK_EQ(world.nodes()[before_index].full_identity, subject);
    CY_CHECK_EQ(world.nodes()[before_index].ordinal, 2U);

    // "undo of the rename SHALL restore the previous name rather than recreating the node" — the
    // editor's undo is the inverse operation, so the node index has to be the same one afterwards.
    TransactionBuilder undo(test_allocator());
    undo.header(world.document(), 1);
    undo.rename(subject, "Barrel", "Crate");
    CY_REQUIRE(apply_transaction(world, undo.span(), report));
    CY_CHECK_EQ(world.text(world.nodes()[before_index].name), std::string_view("Crate"));
    CY_CHECK_EQ(world.index_of(subject.low), before_index);
}

CY_TEST_CASE("a node has a name a rename does not give to a node in another document") {
    World world(test_allocator());
    CY_REQUIRE(read_world(sample_world(), kAssetPath, world));
    const EditorId elsewhere = editor_document_identity("worlds/forest.cyworld");

    TransactionBuilder rename(test_allocator());
    rename.header(elsewhere, 1);
    rename.rename(editor_node_identity(elsewhere, 1), "", "Intruder");
    TransactionReport report;
    CY_REQUIRE(apply_transaction(world, rename.span(), report));

    CY_CHECK_EQ(report.unknown_nodes, 1U);
    CY_CHECK_EQ(report.applied, 0U);
    CY_CHECK_EQ(world.text(world.nodes()[0].name), std::string_view("Pillar"));
}

// --- a name in the layer field is reported
// --------------------------------------------------------

CY_TEST_CASE("a node has a name and a world that put one in the layer field is reported") {
    World world(test_allocator());
    WorldReadReport report;
    CY_REQUIRE(read_world(kNamesInTheLayerField, kAssetPath, world, &report));

    // Reported, and read: refusing would make the world unopenable, and opening it is the only way
    // to move the names out of the field they are in.
    CY_CHECK(report.layers_used_as_names);
    CY_CHECK_EQ(report.named, 0U);
    CY_CHECK_EQ(report.nodes, 3U);
    CY_CHECK(layers_used_as_names(world));
}

CY_TEST_CASE("a node has a name and a world that uses the field for a name is not reported") {
    World world(test_allocator());
    WorldReadReport report;
    CY_REQUIRE(read_world(kNamedInTwoLayers, kAssetPath, world, &report));
    CY_CHECK(!report.layers_used_as_names);
    CY_CHECK_EQ(report.named, 3U);

    // And the sample project, which is the world this check was written about.
    World sample(test_allocator());
    WorldReadReport sample_report;
    CY_REQUIRE(read_world(sample_world(), kAssetPath, sample, &sample_report));
    CY_CHECK(!sample_report.layers_used_as_names);
}

CY_TEST_CASE("a node has a name and a world written before names existed still opens") {
    // Four words on a `node` line is what every world in the tree carried until this rung. It reads
    // with every node unnamed rather than being refused, and gains the fifth word when written.
    World world(test_allocator());
    WorldReadReport report;
    CY_REQUIRE(read_world(kNamesInTheLayerField, kAssetPath, world, &report));
    CY_CHECK_EQ(report.named, 0U);
    for (const WorldNode& node : world.nodes()) {
        CY_CHECK_EQ(world.text(node.name), std::string_view(""));
    }

    // And it is written back unchanged: the name word is omitted where there is no name, so the
    // format's "byte-identical on a round trip" contract survives the field being added to it.
    const std::string out = written(world);
    CY_CHECK_EQ(out, std::string(kNamesInTheLayerField));
}

}  // namespace
