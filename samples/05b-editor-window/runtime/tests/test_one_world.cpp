// One world, end to end, without a device. M8.a tasks 1.1 and 1.4.
//
// THE CLAIM UNDER TEST is M7's gate's sentence, inverted: *"the entities the editor creates change
// nothing in that frame"*. Every case here opens the world the editor opens, applies the bytes the
// editor sends, and then asks what the frame holds — the scene's object slots, and the
// `GpuInstance` and `DrawItem` records a pick resolves against. Nothing is associated, nothing is
// mirrored, and nothing is transmitted but the transaction itself.
//
// It needs no Vulkan because none of that needs a device: `first_light::Scene` is CPU geometry and
// `cy::render::pick_ray` is arithmetic over records. What a device would add is the picture, which
// is `smoke.editor_window`'s and `window.py`'s.

#include <cy/core/memory/system_allocator.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/render/picking.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include "world_view.h"

#include <cstring>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using namespace cy::sample::editor_window;
namespace ser = cy::scene::serialization;

namespace {

constexpr const char* kAssetPath = "worlds/city.cyworld";

[[nodiscard]] cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Gpu);
}

/// The engine's own scene components, which is what makes the file's `Transform` this build's
/// `cy::scene::LocalTransform`.
struct Registry {
    cy::reflect::TypeRegistry registry;
    Registry() { CY_REQUIRE(cy::reflect::register_scene_types(registry)); }
};

/// The world and the scene it is presented into, as one fixture.
struct Opened {
    Registry types;
    WorldView view{allocator()};
    cy::sample::first_light::Scene scene{allocator()};

    Opened() {
        cy::sample::first_light::SceneDescription description;
        description.box_count = kWorldCapacity;
        CY_REQUIRE(scene.build(description));
        CY_REQUIRE(view.open(CY_SAMPLE_PROJECT, kAssetPath, types.registry));
    }

    [[nodiscard]] static cy::sample::first_light::Camera camera() {
        cy::sample::first_light::Camera looking;
        // Straight down −Z from twelve metres out, so a pixel in the middle of a 640x360 frame
        // points at the origin and the arithmetic in a test is checkable by hand.
        looking.position[0] = 0.0;
        looking.position[1] = 0.0;
        looking.position[2] = 12.0;
        looking.forward = cy::Vec3{0.0F, 0.0F, -1.0F};
        looking.up = cy::Vec3{0.0F, 1.0F, 0.0F};
        looking.fov_y_radians = 0.9F;
        looking.near_plane = 0.1F;
        return looking;
    }

    [[nodiscard]] static cy::render::View view_of(u32 width, u32 height) {
        cy::render::View out;
        out.desc.purpose = cy::render::ViewPurpose::EditorViewport;
        out.desc.viewport = cy::render::ViewportRect{0, 0, width, height};
        out.desc.projection.kind = cy::render::ProjectionKind::Perspective;
        out.desc.projection.fov_y_radians = 0.9F;
        out.desc.projection.near_plane = 0.1F;
        out.desc.projection.far_plane = 0.0F;
        out.desc.camera.rotation = cy::Quat::identity();
        out.desc.camera.translation = cy::Vec3{0.0F, 0.0F, 0.0F};
        out.refresh();
        return out;
    }
};

// --- the editor's codec, writing ---------------------------------------------------------------

class Bytes {
public:
    void byte(u8 value) { CY_REQUIRE(out_.push_back(value)); }

    void number(u64 value, usize width) {
        for (usize index = 0; index < width; ++index) {
            byte(static_cast<u8>(value >> (index * 8U)));
        }
    }

    void identity(const ser::EditorId& value) {
        number(value.low, 8);
        number(value.high, 8);
    }

    void real(f32 value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        number(bits, 4);
    }

    void text(const char* value) {
        const usize length = std::strlen(value);
        number(length, 4);
        for (usize index = 0; index < length; ++index) {
            byte(static_cast<u8>(value[index]));
        }
    }

    void vec3(f32 x, f32 y, f32 z) {
        byte(6);
        real(x);
        real(y);
        real(z);
    }

    void quat(f32 x, f32 y, f32 z, f32 w) {
        byte(8);
        real(x);
        real(y);
        real(z);
        real(w);
    }

    void header(const ser::EditorId& document, u32 operations) {
        number(1, 8);
        identity(document);
        text("an edit");
        byte(0);
        text("designer");
        byte(0);
        number(operations, 4);
    }

    [[nodiscard]] cy::Span<const u8> span() const noexcept { return out_.span(); }

private:
    cy::Array<u8> out_{allocator()};
};

/// The transaction the editor commits when a person creates an object and places it.
[[nodiscard]] Bytes a_created_box(const ser::EditorId& document, u64 ordinal, f32 x, f32 y, f32 z) {
    Bytes bytes;
    const ser::EditorId node = ser::editor_node_identity(document, ordinal);
    bytes.header(document, 2);
    bytes.byte(0);  // CreateNode
    bytes.identity(node);
    bytes.byte(0);  // no parent
    bytes.byte(4);  // AddComponent
    bytes.identity(node);
    bytes.number(3, 8);  // Transform, as the file's own type section numbers it
    bytes.number(3, 4);  // three fields
    bytes.number(3, 8);  // rotation
    bytes.quat(0.0F, 0.0F, 0.0F, 1.0F);
    bytes.number(4, 8);  // translation
    bytes.vec3(x, y, z);
    bytes.number(5, 8);  // scale
    bytes.vec3(1.0F, 1.0F, 1.0F);
    return bytes;
}

}  // namespace

CY_TEST_CASE("the world the editor opened is the world this runtime draws") {
    Opened opened;
    CY_REQUIRE(opened.view.loaded());
    CY_CHECK_EQ(opened.view.world().nodes().size(), usize{3});

    const u32 presented = opened.view.present(opened.scene);
    CY_CHECK_EQ(presented, 3U);
    CY_CHECK_EQ(opened.view.overflowed(), 0U);

    // The object the editor's second node means is the one holding that node's placement — by
    // identity, not by the order the editor happened to name things in.
    const u64 crate = opened.view.world().nodes()[1].identity;
    const u32 object = opened.view.object_for(crate);
    CY_REQUIRE(object != WorldView::kNoObject);
    CY_CHECK_EQ(opened.scene.objects()[object].world_position[0], 3.0);
    CY_CHECK_EQ(opened.scene.objects()[object].world_position[2], -2.0);
    CY_CHECK_EQ(opened.view.identity_of(object), crate);

    // An identity from nowhere names nothing, rather than the next unused object. That refusal is
    // the whole difference from M7's association.
    CY_CHECK_EQ(opened.view.object_for(0xDEAD'BEEFULL), WorldView::kNoObject);
}

CY_TEST_CASE("an entity created in the editor is in the next frame") {
    Opened opened;
    CY_CHECK_EQ(opened.view.present(opened.scene), 3U);

    const ser::EditorId document = opened.view.world().document();
    const Bytes created = a_created_box(document, 4, 7.0F, 1.0F, -4.0F);
    ser::TransactionReport report;
    CY_REQUIRE(opened.view.apply(created.span(), report));
    CY_CHECK_EQ(report.created, 1U);

    // THE NEXT FRAME. In the runtime this is the `present` at the top of `publish_frame`, one loop
    // iteration after `serve_editor` took the message — which is what makes the change visible in
    // the frame the transaction commits rather than the one after it.
    CY_CHECK_EQ(opened.view.present(opened.scene), 4U);
    const u64 identity = ser::editor_node_identity(document, 4).low;
    const u32 object = opened.view.object_for(identity);
    CY_REQUIRE(object != WorldView::kNoObject);
    CY_CHECK_EQ(opened.scene.objects()[object].world_position[0], 7.0);
    CY_CHECK_EQ(opened.scene.objects()[object].world_position[1], 1.0);
    CY_CHECK_EQ(opened.scene.objects()[object].world_position[2], -4.0);
    CY_CHECK(opened.scene.objects()[object].index_count > 0U);
}

CY_TEST_CASE("a node the editor deleted leaves the frame") {
    Opened opened;
    CY_CHECK_EQ(opened.view.present(opened.scene), 3U);
    const u64 marker = opened.view.world().nodes()[2].identity;

    Bytes deleted;
    deleted.header(opened.view.world().document(), 1);
    deleted.byte(1);  // DeleteNode
    deleted.identity(opened.view.world().nodes()[2].full_identity);
    deleted.byte(0);       // no parent
    deleted.number(0, 4);  // no children
    deleted.text("");      // the layer
    deleted.byte(0);       // no prefab
    deleted.number(0, 4);  // no components
    deleted.number(0, 4);  // no overrides
    ser::TransactionReport report;
    CY_REQUIRE(opened.view.apply(deleted.span(), report));
    CY_CHECK_EQ(report.deleted, 1U);

    CY_CHECK_EQ(opened.view.present(opened.scene), 2U);
    CY_CHECK_EQ(opened.view.object_for(marker), WorldView::kNoObject);
    // And the slot it used draws nothing, rather than keeping the box that was there.
    CY_CHECK_EQ(opened.scene.objects()[3].index_count, 0U);
}

CY_TEST_CASE("a world larger than the scene's slots is reported, not truncated silently") {
    Opened opened;
    const ser::EditorId document = opened.view.world().document();
    for (u64 ordinal = 4; ordinal < 4 + kWorldCapacity; ++ordinal) {
        const Bytes created = a_created_box(document, ordinal, 0.0F, 0.0F, 0.0F);
        ser::TransactionReport report;
        CY_REQUIRE(opened.view.apply(created.span(), report));
    }
    const u32 presented = opened.view.present(opened.scene);
    CY_CHECK(opened.view.overflowed() > 0U);
    CY_CHECK_EQ(presented + opened.view.overflowed(), 3U + kWorldCapacity);
}

// --- picking, M8.a task 1.4 -------------------------------------------------------------------

CY_TEST_CASE("a pick resolves against what was drawn") {
    // M7 REFUSED THIS BY NAME: "this runtime renders through M3's sample renderer, which publishes
    // no draw list for cy::render::pick_ray to resolve against". This is that draw list.
    Opened opened;
    CY_CHECK_EQ(opened.view.present(opened.scene), 3U);
    cy::Array<cy::render::GpuInstance> instances(allocator());
    cy::Array<cy::render::DrawItem> draws(allocator());
    CY_REQUIRE(opened.view.publish(opened.scene, opened.camera(), instances, draws));
    CY_CHECK_EQ(instances.size(), usize{3});
    CY_CHECK_EQ(draws.size(), usize{3});
    CY_CHECK(cy::render::draws_are_ordered(draws.span()));

    // Every record carries the identity the editor knows the node by, which is what the answer is
    // expressed in and what makes a selection survive a runtime restart.
    for (const cy::render::DrawItem& item : draws.span()) {
        CY_CHECK(opened.view.object_for(item.stable_id) != WorldView::kNoObject);
    }

    // The centre of the frame, looking down −Z from twelve metres: the pillar at the origin.
    const cy::render::View view = opened.view_of(640, 360);
    const cy::Ray ray = cy::render::ray_through_pixel(view, 320.0F, 180.0F);
    cy::Array<cy::render::PickCandidate> candidates(allocator());
    CY_REQUIRE(cy::render::pick_ray(instances.span(), draws.span(), ray, {}, candidates));
    CY_REQUIRE(!candidates.empty());
    CY_CHECK_EQ(candidates[0].stable_id, opened.view.world().nodes()[0].identity);
}

CY_TEST_CASE("a pick answers with the entity the editor just created") {
    // The two halves together: an object that did not exist when the runtime started is pickable,
    // by the identity the editor allocated for it.
    Opened opened;
    const ser::EditorId document = opened.view.world().document();
    const Bytes created = a_created_box(document, 4, 0.0F, 4.0F, 0.0F);
    ser::TransactionReport report;
    CY_REQUIRE(opened.view.apply(created.span(), report));
    CY_CHECK_EQ(opened.view.present(opened.scene), 4U);

    cy::Array<cy::render::GpuInstance> instances(allocator());
    cy::Array<cy::render::DrawItem> draws(allocator());
    CY_REQUIRE(opened.view.publish(opened.scene, opened.camera(), instances, draws));
    CY_CHECK_EQ(draws.size(), usize{4});

    // Four metres above the origin, from a camera at (0, 0, 12) looking down −Z: above the centre
    // of the frame by `4 / tan(fov/2) / 12` of a half-height.
    const cy::render::View view = opened.view_of(640, 360);
    cy::Vec2 pixel{0.0F, 0.0F};
    CY_REQUIRE(cy::render::project_to_pixel(view, cy::Vec3{0.0F, 4.0F, -12.0F}, pixel));
    const cy::Ray ray = cy::render::ray_through_pixel(view, pixel.x, pixel.y);
    cy::Array<cy::render::PickCandidate> candidates(allocator());
    CY_REQUIRE(cy::render::pick_ray(instances.span(), draws.span(), ray, {}, candidates));
    CY_REQUIRE(!candidates.empty());
    CY_CHECK_EQ(candidates[0].stable_id, ser::editor_node_identity(document, 4).low);
}

CY_TEST_CASE("an object the editor locked is not a candidate") {
    // Locking is authoring state, so the editor supplies the excluded identities and the engine
    // resolves without them. `picking.h` calls that the capability's division of labour.
    Opened opened;
    CY_CHECK_EQ(opened.view.present(opened.scene), 3U);
    cy::Array<cy::render::GpuInstance> instances(allocator());
    cy::Array<cy::render::DrawItem> draws(allocator());
    CY_REQUIRE(opened.view.publish(opened.scene, opened.camera(), instances, draws));

    const u64 pillar = opened.view.world().nodes()[0].identity;
    const u64 excluded[] = {pillar};
    cy::render::PickFilter filter;
    filter.excluded = cy::Span<const u64>(excluded, 1);
    const cy::render::View view = opened.view_of(640, 360);
    const cy::Ray ray = cy::render::ray_through_pixel(view, 320.0F, 180.0F);
    cy::Array<cy::render::PickCandidate> candidates(allocator());
    CY_REQUIRE(cy::render::pick_ray(instances.span(), draws.span(), ray, filter, candidates));
    for (const cy::render::PickCandidate& candidate : candidates.span()) {
        CY_CHECK_NE(candidate.stable_id, pillar);
    }
}

CY_TEST_CASE("a node that is not drawn cannot be picked") {
    // `picking.h`'s first rule, held here rather than assumed: a pick resolves against the draw
    // list, so a node the frame did not draw is not in it.
    Opened opened;
    CY_CHECK_EQ(opened.view.present(opened.scene), 3U);
    cy::Array<cy::render::GpuInstance> instances(allocator());
    cy::Array<cy::render::DrawItem> draws(allocator());
    CY_REQUIRE(opened.view.publish(opened.scene, opened.camera(), instances, draws));
    const usize before = draws.size();

    // The ground plane is object 0 and is not an authored node, so it is not in the list — a click
    // on the floor selects nothing rather than an object the document has never heard of.
    for (const cy::render::DrawItem& item : draws.span()) {
        CY_CHECK_NE(item.stable_id, 0ULL);
    }
    CY_CHECK_EQ(before, usize{3});
}
