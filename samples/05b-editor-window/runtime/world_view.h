#pragma once
// The authored world this runtime renders, and what the editor's messages mean against it.
// M8.a tasks 1.1, 1.3 and 1.4.
//
// ================================================================================================
// WHAT THIS REPLACES
// ================================================================================================
//
// `session.h`, which is gone, and whose own header said what it was: *"the runtime associates each
// identity the editor names with one of its own objects, IN FIRST-SEEN ORDER, and keeps that
// association for the session … It is a stand-in for a shared world and it is named as one."*
//
// There is no association here, because there is nothing to associate. The runtime opens **the same
// `.cyworld` the editor opened**, `cy::scene::serialization::read_world` derives each node's
// identity the way `cy_editor_core::ids` derives it, and the object the editor names is the node
// with that identity. A world with three nodes and an editor that selects the second one puts the
// gizmo on the second one, not on whichever object the runtime happened to hand out first.
//
// The three consequences, each of which was a defect at M7:
//
//   * an entity CREATED in the editor appears in the frame, because a `CreateNode` is applied to
//     the world and the world is what the frame is built from;
//   * a scale is applied as a scale, because the transaction's field identifiers are the file's and
//     the file says which field is which — see `world_transaction.h`;
//   * a pick resolves against what was drawn, because the same walk that fills the scene's objects
//     fills the `GpuInstance` and `DrawItem` records `cy::render::pick_ray` reads.
//
// ================================================================================================
// WHY THE RENDER OBJECTS ARE SLOTS IN M3'S SCENE
// ================================================================================================
//
// `first_light::Scene` is built once — its vertex and index buffers are uploaded by
// `Renderer::prepare` — and exposes its objects as a span that may be written but not grown. That
// is M3's, this artefact reuses it deliberately (a second renderer would drift from the one
// `render.golden` photographs), and it is not this milestone's to change.
//
// So the scene is built with **capacity**: one ground plane and `kWorldCapacity` box slots. The
// world's live nodes are written into the leading slots and the rest are given an index count of
// zero, which draws nothing. A world with more nodes than the capacity is REPORTED, with the two
// numbers, rather than silently truncated — a viewport quietly missing an object is the shape of
// failure this milestone exists to end.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/registry.h>
#include <cy/scene/serialization/world_transaction.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/render/gpu_scene.h>
#include <cy/servers/render/model.h>
#include <cy/servers/render/sort.h>

#include "scene.h"

namespace cy::sample::editor_window {

/// How many authored nodes one of these runtimes can draw. See the header note.
inline constexpr u32 kWorldCapacity = 64;

/// Which scene object an authored node was written into.
struct Placed {
    u64 identity = 0;
    u32 object = 0;
};

/// The authored world, and the frame's view of it.
class WorldView {
public:
    explicit WorldView(Allocator& allocator) noexcept
        : world_(allocator), schema_(allocator), placed_(allocator), allocator_(&allocator) {}

    WorldView(const WorldView&) = delete;
    WorldView& operator=(const WorldView&) = delete;

    /// Open `directory/asset_path`, deriving identity from `asset_path` alone.
    ///
    /// The two are separate because they are different things: the bytes come from wherever the
    /// project is checked out, and the identity comes from the path the EDITOR names the document
    /// by. Hashing an absolute path would give a world whose nodes the editor has never heard of on
    /// every machine but the one it was authored on.
    [[nodiscard]] Status open(const char* directory, const char* asset_path,
                              const reflect::TypeRegistry& registry) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] scene::serialization::World& world() noexcept { return world_; }
    [[nodiscard]] const scene::serialization::World& world() const noexcept { return world_; }

    /// Apply one encoded editor transaction to the world.
    [[nodiscard]] Status apply(Span<const u8> bytes,
                               scene::serialization::TransactionReport& out) noexcept;

    /// Write the world's live nodes into `scene`'s object slots.
    ///
    /// Returns how many nodes were written. `overflowed()` says how many did not fit.
    [[nodiscard]] u32 present(first_light::Scene& scene) noexcept;

    /// How many nodes the last `present` could not fit into the scene's slots.
    [[nodiscard]] u32 overflowed() const noexcept { return overflowed_; }
    /// How many nodes the last `present` wrote.
    [[nodiscard]] u32 presented() const noexcept { return static_cast<u32>(placed_.size()); }

    /// The scene object an editor identity names, or `kNoObject`.
    [[nodiscard]] u32 object_for(u64 identity) const noexcept;
    /// The editor identity a scene object carries, or zero for one that is not an authored node.
    [[nodiscard]] u64 identity_of(u32 object) const noexcept;

    /// The frame's instance and draw records, camera-relative, for `cy::render::pick_ray`.
    ///
    /// **These are the records of what was drawn**, produced from the same placement `present`
    /// wrote into the scene rather than from a second walk of the world — which is the whole of
    /// `picking.h`'s requirement that "what is picked matches what is rendered".
    [[nodiscard]] Status publish(const first_light::Scene& scene, const first_light::Camera& camera,
                                 Array<render::GpuInstance>& instances,
                                 Array<render::DrawItem>& draws) const noexcept;

    /// A camera that frames the world's nodes, for the one view suggestion the runtime offers.
    [[nodiscard]] first_light::Camera framing(const first_light::Scene& scene) const noexcept;

    static constexpr u32 kNoObject = 0xFFFF'FFFFU;

private:
    scene::serialization::World world_;
    scene::serialization::AuthoringSchema schema_;
    Array<Placed> placed_;
    Allocator* allocator_;
    /// The index range of the unit box in the scene's index buffer, read out of the scene rather
    /// than assumed: an authored node is drawn as that box until a mesh asset says otherwise.
    u32 box_first_ = 0;
    u32 box_count_ = 0;
    u32 overflowed_ = 0;
    bool loaded_ = false;
};

}  // namespace cy::sample::editor_window
