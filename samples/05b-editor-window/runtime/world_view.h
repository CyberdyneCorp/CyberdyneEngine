#pragma once
// The editor and runtime open the same .cyworld and derive the same stable node identities.
// present_authored tracks live nodes for selection without borrowing first-light box slots.
// The legacy present/publish path remains for the one-world fixture and its tests.

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

#include <string_view>

#include "scene.h"

namespace cy::sample::editor_window {

/// Legacy first-light test fixture capacity. The authored renderer has its own draw capacity.
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

    /// Replace the in-memory world after the editor adds declarations absent from the opened file.
    [[nodiscard]] Status sync(std::string_view text) noexcept;

    /// Write the world's live nodes into `scene`'s object slots.
    ///
    /// Returns how many nodes were written. `overflowed()` says how many did not fit.
    [[nodiscard]] u32 present(first_light::Scene& scene) noexcept;
    /// Track authored node identities without borrowing fixed first-light object slots.
    [[nodiscard]] u32 present_authored() noexcept;

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
