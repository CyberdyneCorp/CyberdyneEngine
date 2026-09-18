#include "shade.h"

#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/hash_map.h>
#include <cy/rendering/shadows/bias.h>
#include <cy/rendering/virtual_geometry/asset.h>
#include <cy/rendering/virtual_geometry/visbuffer.h>

#include <algorithm>
#include <cmath>

namespace cy::sample::fidelity {
namespace {

using rendering::ClipmapConfig;
using rendering::ClipmapLevel;
using rendering::DerivedBias;
using rendering::FallbackOptions;
using rendering::FallbackResult;
using rendering::PageEntry;
using rendering::PageLookup;
using rendering::PageState;
using rendering::ShadowAddress;
using rendering::ShadowAddressSpace;
using rendering::ShadowBasis;
using rendering::ShadowCacheConfig;
using rendering::ShadowPageCache;
using rendering::ShadowSubstitution;
using rendering::UpdateClass;
using rendering::VirtualPage;

/// Linear albedo per material bin. The scene sets `material_offset` to the shape index, so bin `n`
/// is shape `n`: hall, column, statue, facade, terrain. Five stated colours rather than a hash of
/// the index, because a hashed palette makes every regression look like a lighting change.
constexpr Vec3 kAlbedo[8] = {
    {0.52F, 0.48F, 0.43F},  // Hall — warm plaster
    {0.72F, 0.69F, 0.63F},  // Column — pale stone
    {0.38F, 0.40F, 0.44F},  // Statue — cold granite
    {0.46F, 0.42F, 0.38F},  // Facade — brick
    {0.30F, 0.33F, 0.27F},  // Terrain — moss over rock
    {0.60F, 0.60F, 0.60F}, {0.60F, 0.60F, 0.60F}, {0.60F, 0.60F, 0.60F},
};

/// Sky and ground for the hemisphere ambient, and how much of it there is. The ambient exists so
/// that a shadowed pixel is still a picture of something; it is deliberately far below the sun so
/// that losing the shadow term is unmistakable rather than subtle.
constexpr Vec3 kSkyColour{0.36F, 0.44F, 0.58F};
constexpr Vec3 kGroundColour{0.16F, 0.14F, 0.12F};
constexpr f32 kAmbientScale = 0.18F;

/// Uncovered pixels. The same value the cluster and triangle views use, so the three captures of
/// one frame share a background.
constexpr u32 kBackground = 0xFF141414U;

/// Depth of an empty shadow texel: no caster, so nothing is in front of any receiver.
constexpr f32 kNoCaster = 1.0e30F;

/// "No page": a receiver outside every clip level, or one the cache had no slot for. Not
/// `VirtualPage{}.pack()`, which is zero and is a perfectly valid page of level 0.
constexpr u64 kNoPage = ~static_cast<u64>(0);

[[nodiscard]] Vec3 multiply(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}

/// ACES's fitted curve, then the sRGB-ish transfer the golden references are stored in. The same
/// last two operations `samples/03-first-light` ends its shader with, so two references in this
/// repository quantise the same way.
[[nodiscard]] u32 encode(Vec3 linear) noexcept {
    const auto channel = [](f32 x) {
        const f32 mapped = (x * ((2.51F * x) + 0.03F)) / ((x * ((2.43F * x) + 0.59F)) + 0.14F);
        const f32 clamped = math::clamp(mapped, 0.0F, 1.0F);
        const f32 encoded = std::pow(clamped, 1.0F / 2.2F);
        return static_cast<u32>((encoded * 255.0F) + 0.5F) & 0xFFU;
    };
    return 0xFF000000U | (channel(linear.z) << 16U) | (channel(linear.y) << 8U) | channel(linear.x);
}

/// One pixel's surface: what the device identified, reconstructed into world space.
struct Surface {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    u32 material = 0;
    bool covered = false;
};

/// One caster, reduced to what a directional page raster needs: its extent across the light and how
/// far along the light its FAR face is. Level-independent — a clip level changes only the origin
/// and the scale — so this is computed once per cluster and reused by every level.
struct Caster {
    f32 x_min = 0.0F;
    f32 x_max = 0.0F;
    f32 y_min = 0.0F;
    f32 y_max = 0.0F;
    f32 depth_far = 0.0F;
};

/// The visible cluster record and the decoded geometry for one surface identity, decoded once.
struct ClusterEntry {
    explicit ClusterEntry(Allocator& allocator) noexcept : geometry(allocator) {}
    ClusterEntry(ClusterEntry&&) noexcept = default;
    ClusterEntry& operator=(ClusterEntry&&) noexcept = default;

    rendering::vg::DecodedCluster geometry;
    rendering::vg::VisibleCluster record;
};

[[nodiscard]] const rendering::gi::GiLight* find_sun(const Scene& scene) noexcept {
    for (const rendering::gi::GiLight& light : scene.lights) {
        if (light.directional) {
            return &light;
        }
    }
    return nullptr;
}

/// The world bounding box of one cluster of one instance.
[[nodiscard]] Aabb world_bounds(const Aabb& local,
                                const rendering::vg::GeometryInstance& instance) noexcept {
    Aabb out = Aabb::empty();
    for (u32 corner = 0; corner < 8U; ++corner) {
        const Vec3 point{(corner & 1U) != 0U ? local.max.x : local.min.x,
                         (corner & 2U) != 0U ? local.max.y : local.min.y,
                         (corner & 4U) != 0U ? local.max.z : local.min.z};
        const Vec3 world = ((instance.rotation * point) * instance.scale) + instance.translation;
        out.min = Vec3{math::min(out.min.x, world.x), math::min(out.min.y, world.y),
                       math::min(out.min.z, world.z)};
        out.max = Vec3{math::max(out.max.x, world.x), math::max(out.max.y, world.y),
                       math::max(out.max.z, world.z)};
    }
    return out;
}

}  // namespace

// ================================================================================================
// THE VIRTUAL SHADOW MAP
// ================================================================================================

namespace {

/// One frame of the sun's virtual shadow map: the clip levels, the page cache, and the physical
/// texels the rendered pages live in.
///
/// Three phases, in the order `src/rendering/shadows/README.md` describes them, and they are three
/// methods rather than one because the middle one cannot start until the last receiver has marked:
/// `ShadowPageCache::request` evicts, so a slot a receiver was handed early can belong to another
/// page by the time the marking ends. Every lookup is therefore resolved after the cache settles.
class VirtualShadowMap {
public:
    explicit VirtualShadowMap(Allocator& allocator) noexcept
        : allocator_(allocator),
          levels_(allocator),
          spaces_(allocator),
          atlas_(allocator),
          slot_of_page_(allocator),
          casters_(allocator),
          cache_(allocator) {}

    [[nodiscard]] Status prepare(Vec3 sun_direction, Vec3 camera,
                                 const ShadeOptions& options) noexcept;
    /// Mark the page a receiver at `position` needs, and hand back its packed id — `kNoPage` when
    /// the receiver is outside every level or the cache could not seat it.
    [[nodiscard]] u64 mark(Vec3 position, f32 receiver_texel_world_size) noexcept;
    /// Rasterise every page the cache says is dirty, from the scene's level-0 clusters.
    [[nodiscard]] Status render(const Scene& scene, ShadeReport& report) noexcept;
    /// The sun's visibility at a receiver, in [0,1], through the fallback chain.
    [[nodiscard]] f32 sample(u64 requested, Vec3 position, Vec3 normal, f32 n_dot_l,
                             ShadeReport& report) const noexcept;

    [[nodiscard]] const ShadowPageCache& cache() const noexcept { return cache_; }
    [[nodiscard]] f32 texel_world_size(u8 level) const noexcept {
        return level < levels_.size() ? levels_[level].texel_world_size : 0.0F;
    }

private:
    [[nodiscard]] Status gather_casters(const Scene& scene) noexcept;
    void rasterise_level(u8 level, ShadeReport& report) noexcept;
    void rasterise_caster(const ShadowAddressSpace& space, const Caster& caster,
                          ShadeReport& report) noexcept;
    [[nodiscard]] f32* texel(u32 slot, u32 x, u32 y) noexcept {
        return &atlas_[(static_cast<usize>(slot) * page_area_) +
                       (static_cast<usize>(y) * config_.geometry.page_texels) + x];
    }
    [[nodiscard]] f32 texel(u32 slot, u32 x, u32 y) const noexcept {
        return atlas_[(static_cast<usize>(slot) * page_area_) +
                      (static_cast<usize>(y) * config_.geometry.page_texels) + x];
    }

    Allocator& allocator_;
    ClipmapConfig config_;
    ShadowBasis basis_;
    Array<ClipmapLevel> levels_;
    Array<ShadowAddressSpace> spaces_;
    Array<f32> atlas_;
    HashMap<u64, u32> slot_of_page_;
    Array<Caster> casters_;
    ShadowPageCache cache_;
    usize page_area_ = 0;
    u8 tail_level_ = 0;
};

Status VirtualShadowMap::prepare(Vec3 sun_direction, Vec3 camera,
                                 const ShadeOptions& options) noexcept {
    config_.level_count = options.clipmap_levels;
    config_.first_level_extent = options.first_level_extent;
    config_.geometry.page_texels = options.page_texels;
    config_.geometry.virtual_texels = options.virtual_texels;
    if (!config_.valid()) {
        return fail(ErrorCode::InvalidArgument, "shade: the clipmap configuration is not valid");
    }
    basis_ = rendering::shadow_basis(sun_direction);
    tail_level_ = static_cast<u8>(config_.level_count - 1U);

    for (u32 level = 0; level < config_.level_count; ++level) {
        const ClipmapLevel resolved = rendering::clipmap_level(config_, basis_, camera, level);
        if (Status added = levels_.push_back(resolved); !added) {
            return added;
        }
        if (Status added =
                spaces_.push_back(rendering::clipmap_address_space(config_, basis_, resolved, 0U));
            !added) {
            return added;
        }
    }

    ShadowCacheConfig cache_config;
    cache_config.slots = options.shadow_slots;
    if (Status ready = cache_.initialize(cache_config); !ready) {
        return ready;
    }
    cache_.begin_frame(1U);

    page_area_ = static_cast<usize>(config_.geometry.page_texels) * config_.geometry.page_texels;
    if (Status sized = atlas_.resize(page_area_ * cache_config.slots); !sized) {
        return sized;
    }
    for (f32& depth : atlas_) {
        depth = kNoCaster;
    }
    return ok();
}

u64 VirtualShadowMap::mark(Vec3 position, f32 receiver_texel_world_size) noexcept {
    const u32 level = rendering::clipmap_level_for(config_, receiver_texel_world_size);
    const ShadowAddress address = rendering::address_of(spaces_[level], position);
    if (!address.inside) {
        return kNoPage;
    }
    // `Normal` because the set is static: `update_class_of` in `frame_assembly.cpp` gives a
    // directional light `Dynamic` when it moves, and this sun does not.
    const PageLookup lookup = cache_.request(address.page, UpdateClass::Normal);
    return lookup.starved ? kNoPage : address.page.pack();
}

Status VirtualShadowMap::gather_casters(const Scene& scene) noexcept {
    for (const rendering::vg::GeometryInstance& instance : scene.instances) {
        if (instance.asset >= scene.decoded.size()) {
            continue;
        }
        const rendering::vg::DecodedAsset& asset = scene.decoded[instance.asset];
        for (const rendering::vg::Cluster& cluster : asset.clusters) {
            // Level 0 is the source geometry's own clustering — the finest caster the hierarchy
            // holds. A coarser level would be cheaper and would shadow with geometry the camera
            // never sees, which is a trade a device shadow pass gets to make and this does not.
            if (cluster.level != 0U || cluster.bounds.is_empty()) {
                continue;
            }
            const Aabb bounds = world_bounds(cluster.bounds, instance);
            Caster caster;
            caster.x_min = math::kInfinity;
            caster.y_min = math::kInfinity;
            caster.x_max = -math::kInfinity;
            caster.y_max = -math::kInfinity;
            caster.depth_far = -math::kInfinity;
            for (u32 corner = 0; corner < 8U; ++corner) {
                const Vec3 point{(corner & 1U) != 0U ? bounds.max.x : bounds.min.x,
                                 (corner & 2U) != 0U ? bounds.max.y : bounds.min.y,
                                 (corner & 4U) != 0U ? bounds.max.z : bounds.min.z};
                caster.x_min = math::min(caster.x_min, dot(point, basis_.right));
                caster.x_max = math::max(caster.x_max, dot(point, basis_.right));
                caster.y_min = math::min(caster.y_min, dot(point, basis_.up));
                caster.y_max = math::max(caster.y_max, dot(point, basis_.up));
                caster.depth_far = math::max(caster.depth_far, dot(point, basis_.forward));
            }
            if (Status added = casters_.push_back(caster); !added) {
                return added;
            }
        }
    }
    return ok();
}

void VirtualShadowMap::rasterise_caster(const ShadowAddressSpace& space, const Caster& caster,
                                        ShadeReport& report) noexcept {
    const f32 extent = math::max(space.extent, math::kSmallLength);
    const auto texels = static_cast<f32>(space.geometry.virtual_texels);
    const f32 fx0 = ((caster.x_min - space.origin_light_space.x) / extent) * texels;
    const f32 fx1 = ((caster.x_max - space.origin_light_space.x) / extent) * texels;
    const f32 fy0 = ((caster.y_min - space.origin_light_space.y) / extent) * texels;
    const f32 fy1 = ((caster.y_max - space.origin_light_space.y) / extent) * texels;
    if (fx1 < 0.0F || fy1 < 0.0F || fx0 >= texels || fy0 >= texels) {
        return;
    }
    const u32 last = space.geometry.virtual_texels - 1U;
    const u32 x0 = static_cast<u32>(math::max(fx0, 0.0F));
    const u32 x1 = math::min(static_cast<u32>(math::max(fx1, 0.0F)), last);
    const u32 y0 = static_cast<u32>(math::max(fy0, 0.0F));
    const u32 y1 = math::min(static_cast<u32>(math::max(fy1, 0.0F)), last);

    const u32 page_texels = space.geometry.page_texels;
    for (u32 page_y = y0 / page_texels; page_y <= y1 / page_texels; ++page_y) {
        for (u32 page_x = x0 / page_texels; page_x <= x1 / page_texels; ++page_x) {
            VirtualPage page;
            page.light_slot = space.light_slot;
            page.level = space.level;
            page.x = static_cast<u16>(page_x);
            page.y = static_cast<u16>(page_y);
            const u32* slot = slot_of_page_.find(page.pack());
            if (slot == nullptr) {
                continue;
            }
            const u32 base_x = page_x * page_texels;
            const u32 base_y = page_y * page_texels;
            for (u32 y = math::max(y0, base_y); y <= math::min(y1, base_y + page_texels - 1U);
                 ++y) {
                for (u32 x = math::max(x0, base_x); x <= math::min(x1, base_x + page_texels - 1U);
                     ++x) {
                    f32* depth = texel(*slot, x - base_x, y - base_y);
                    if (caster.depth_far < *depth) {
                        report.shadow_texels_written += *depth >= kNoCaster ? 1U : 0U;
                        *depth = caster.depth_far;
                    }
                }
            }
        }
    }
}

void VirtualShadowMap::rasterise_level(u8 level, ShadeReport& report) noexcept {
    for (const Caster& caster : casters_) {
        rasterise_caster(spaces_[level], caster, report);
    }
}

Status VirtualShadowMap::render(const Scene& scene, ShadeReport& report) noexcept {
    if (Status gathered = gather_casters(scene); !gathered) {
        return gathered;
    }
    // THE RENDER LIST IS READ OFF THE CACHE, IN SLOT ORDER, AFTER THE MARKING ENDED. A list built
    // while receivers were still marking would name slots that had since been evicted — and slot
    // order is what makes the same frame produce the same picture on two runs.
    Array<VirtualPage> dirty(allocator_);
    for (const PageEntry& entry : cache_.entries()) {
        // An entry EXISTS because a receiver asked for it, and it carries a physical slot from that
        // moment; `request()` leaves its state Absent until something renders into it, so the test
        // is dirtiness and not residency. Filtering on `state != Absent` here rendered nothing at
        // all and every lookup answered "lit" — the shape of defect this whole criterion is about.
        if (!entry.dirty) {
            continue;
        }
        if (Expected<u32*, Error> added =
                slot_of_page_.insert(entry.page.pack(), entry.physical_slot);
            !added) {
            return Status{make_unexpected(added.error())};
        }
        if (Status added = dirty.push_back(entry.page); !added) {
            return added;
        }
    }
    for (u32 level = 0; level < spaces_.size(); ++level) {
        rasterise_level(static_cast<u8>(level), report);
    }
    for (const VirtualPage& page : dirty) {
        cache_.record_render(page, 0.0F);
    }
    report.pages_rendered = static_cast<u32>(dirty.size());
    return ok();
}

f32 VirtualShadowMap::sample(u64 requested, Vec3 position, Vec3 normal, f32 n_dot_l,
                             ShadeReport& report) const noexcept {
    if (requested == kNoPage) {
        // No page was ever asked for, so no rung of the chain was reached. Counted as the loudest
        // rung rather than as a silent success: `virtual-shadows` requires an unshadowed answer to
        // be counted loudly, and a receiver outside every clip level is exactly that.
        report.substitutions.record(ShadowSubstitution::Unshadowed);
        return 1.0F;
    }
    FallbackOptions options;
    options.coarser_levels = 4;
    options.tail_level = tail_level_;
    options.approximation_available = false;
    options.max_stale_frames = 0;
    const FallbackResult resolved =
        rendering::resolve_shadow_lookup(cache_, 1U, VirtualPage::unpack(requested), options);
    report.substitutions.record(resolved.substitution);
    if (resolved.substitution == ShadowSubstitution::Unshadowed ||
        resolved.substitution == ShadowSubstitution::Approximation ||
        resolved.physical_slot >= (atlas_.size() / math::max<usize>(page_area_, 1U))) {
        return 1.0F;
    }

    // The bias is the module's, derived from the texel the answer actually came from — which is a
    // coarser one when the chain climbed, and getting that wrong is what makes a substituted lookup
    // look like acne.
    rendering::BiasInputs inputs;
    inputs.texel_world_size = texel_world_size(resolved.page.level);
    inputs.n_dot_l = n_dot_l;
    inputs.geometric_error = 0.0F;
    inputs.receiver_plane_available = false;
    const DerivedBias bias = rendering::derive_shadow_bias(inputs);

    const Vec3 offset = position + (normal * bias.normal_offset);
    const ShadowAddress address = rendering::address_of(spaces_[resolved.page.level], offset);
    if (!address.inside || !(address.page == resolved.page)) {
        return 1.0F;
    }
    const u32 page_texels = config_.geometry.page_texels;
    const u32 x = math::min(static_cast<u32>(address.page_uv.x * static_cast<f32>(page_texels)),
                            page_texels - 1U);
    const u32 y = math::min(static_cast<u32>(address.page_uv.y * static_cast<f32>(page_texels)),
                            page_texels - 1U);
    const f32 caster_depth = texel(resolved.physical_slot, x, y);
    if (caster_depth >= kNoCaster) {
        return 1.0F;
    }
    const f32 tangent = math::min(
        std::sqrt(math::max(1.0F - (n_dot_l * n_dot_l), 0.0F)) / math::max(n_dot_l, 0.01F), 8.0F);
    const f32 receiver_depth = dot(offset, basis_.forward);
    return receiver_depth > (caster_depth + bias.constant + (bias.slope_scale * tangent)) ? 0.0F
                                                                                          : 1.0F;
}

}  // namespace

// ================================================================================================
// THE FRAME
// ================================================================================================

namespace {

/// Decode every cluster the device left in the visibility buffer, once each, and answer a pixel's
/// identity with the decoded geometry and the visible record.
class VisibleClusters {
public:
    explicit VisibleClusters(Allocator& allocator) noexcept
        : allocator_(allocator), entries_(allocator), index_(allocator) {}

    [[nodiscard]] Status build(const Scene& scene, const Capture& capture,
                               u32 cluster_stride) noexcept {
        for (const rendering::vg::VisibleCluster& record : capture.visible) {
            const u32 identity =
                rendering::vg::surface_identity(record.instance, record.cluster, cluster_stride);
            if (index_.contains(identity) || record.instance >= scene.instances.size()) {
                continue;
            }
            const u32 asset = scene.instances[record.instance].asset;
            if (asset >= scene.decoded.size()) {
                continue;
            }
            ClusterEntry entry(allocator_);
            Expected<rendering::vg::DecodedCluster, Error> decoded =
                rendering::vg::decode_cluster(scene.decoded[asset], record.cluster, allocator_);
            if (!decoded) {
                continue;
            }
            entry.geometry = std::move(*decoded);
            entry.record = record;
            if (Expected<u32*, Error> added =
                    index_.insert(identity, static_cast<u32>(entries_.size()));
                !added) {
                return Status{make_unexpected(added.error())};
            }
            if (Status added = entries_.push_back(std::move(entry)); !added) {
                return added;
            }
        }
        return ok();
    }

    [[nodiscard]] const ClusterEntry* find(u32 identity) const noexcept {
        const u32* slot = index_.find(identity);
        return slot != nullptr ? &entries_[*slot] : nullptr;
    }

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entries_.size()); }

private:
    Allocator& allocator_;
    Array<ClusterEntry> entries_;
    HashMap<u32, u32> index_;
};

/// Reconstruct every covered pixel's world surface from the identity the device wrote.
[[nodiscard]] Status reconstruct(const Scene& scene, const Capture& capture,
                                 const VisibleClusters& clusters, Array<Surface>& out,
                                 ShadeReport& report) noexcept {
    const u64 count = static_cast<u64>(capture.width) * capture.height;
    if (Status sized = out.resize(static_cast<usize>(count)); !sized) {
        return sized;
    }
    for (u64 pixel = 0; pixel < count && pixel < capture.samples.size(); ++pixel) {
        const rendering::vg::VisibilitySample& sample = capture.samples[pixel];
        if (!sample.covered()) {
            continue;
        }
        ++report.covered;
        const ClusterEntry* entry = clusters.find(sample.surface);
        if (entry == nullptr) {
            ++report.unreconstructed;
            continue;
        }
        const Vec2 centre{static_cast<f32>(pixel % capture.width) + 0.5F,
                          static_cast<f32>(pixel / capture.width) + 0.5F};
        Expected<rendering::vg::SurfaceAttributes, Error> attributes =
            rendering::vg::reconstruct_surface(
                entry->geometry, scene.instances[entry->record.instance], entry->record,
                sample.triangle, capture.world_to_clip, centre, capture.width, capture.height);
        if (!attributes) {
            ++report.unreconstructed;
            continue;
        }
        Surface& surface = out[static_cast<usize>(pixel)];
        surface.position = attributes->position;
        // THE NORMAL IS THE DEVICE'S, not the reconstruction's. The resolve packs the world normal
        // into [0,1]; unpacking it here is what makes the picture a photograph of what the device
        // resolved rather than of what the host could have computed without it.
        const Vec4 resolved = pixel < capture.resolved.size()
                                  ? capture.resolved[static_cast<usize>(pixel)]
                                  : Vec4{0.5F, 0.5F, 1.0F, 0.0F};
        const Vec3 unpacked{(resolved.x * 2.0F) - 1.0F, (resolved.y * 2.0F) - 1.0F,
                            (resolved.z * 2.0F) - 1.0F};
        const f32 magnitude = length(unpacked);
        surface.normal = magnitude > 1.0e-6F ? unpacked * (1.0F / magnitude) : attributes->normal;
        surface.material = attributes->material;
        surface.covered = true;
    }
    return ok();
}

/// The point lights' contribution at one surface. No shadow: `virtual-shadows`' paged path is the
/// sun's here, and claiming a shadow for the two interior lights would be claiming a cube-mapped
/// address space this shot never builds.
[[nodiscard]] Vec3 punctual(const Scene& scene, const Surface& surface) noexcept {
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (const rendering::gi::GiLight& light : scene.lights) {
        if (light.directional) {
            continue;
        }
        const Vec3 to_light = light.position - surface.position;
        const f32 distance = length(to_light);
        if (distance <= 1.0e-3F || distance >= light.range) {
            continue;
        }
        const f32 n_dot_l = math::max(dot(surface.normal, to_light * (1.0F / distance)), 0.0F);
        if (n_dot_l <= 0.0F) {
            continue;
        }
        // Inverse square, windowed to the declared range so a light stops exactly where the scene
        // says it does rather than at whatever the tone map hides.
        const f32 ratio = distance / light.range;
        const f32 window = math::max(1.0F - (ratio * ratio * ratio * ratio), 0.0F);
        const f32 attenuation =
            light.intensity * window * window / (12.566371F * distance * distance);
        total = total + (light.colour * (attenuation * n_dot_l));
    }
    return total;
}

}  // namespace

Status shade_frame(const Scene& scene, const Capture& capture, const ShadeOptions& options,
                   ShadedFrame& out, ShadeReport& report) noexcept {
    if (capture.width == 0U || capture.height == 0U || capture.samples.empty()) {
        return fail(ErrorCode::InvalidArgument, "shade: the capture holds no frame");
    }
    const rendering::gi::GiLight* sun = find_sun(scene);
    if (sun == nullptr) {
        return fail(ErrorCode::InvalidArgument, "shade: the scene declares no directional light");
    }
    Allocator& allocator = scene.allocator;

    u32 cluster_stride = 0;
    for (const rendering::vg::DecodedAsset& asset : scene.decoded) {
        cluster_stride = math::max(cluster_stride, static_cast<u32>(asset.clusters.size()));
    }

    VisibleClusters clusters(allocator);
    if (Status built = clusters.build(scene, capture, cluster_stride); !built) {
        return built;
    }
    report.clusters_decoded = clusters.size();

    Array<Surface> surfaces(allocator);
    if (Status done = reconstruct(scene, capture, clusters, surfaces, report); !done) {
        return done;
    }

    // ONE SHADOW TEXEL PER SCREEN PIXEL AT THE RECEIVER is what picks the clip level: the projected
    // footprint of a pixel at that distance, which is `clipmap_level_for`'s own argument.
    const f32 pixel_angle =
        2.0F * std::tan(kShotFovY * 0.5F) / static_cast<f32>(math::max(capture.height, 1U));

    VirtualShadowMap shadows(allocator);
    if (Status ready = shadows.prepare(sun->direction, capture.camera, options); !ready) {
        return ready;
    }
    Array<u64> requested(allocator);
    if (Status sized = requested.resize(surfaces.size()); !sized) {
        return sized;
    }
    for (usize pixel = 0; pixel < surfaces.size(); ++pixel) {
        requested[pixel] = kNoPage;
        if (!surfaces[pixel].covered) {
            continue;
        }
        const f32 distance = length(surfaces[pixel].position - capture.camera);
        requested[pixel] = shadows.mark(surfaces[pixel].position, distance * pixel_angle);
    }
    report.pages_requested = shadows.cache().statistics().requested;
    report.pages_starved = shadows.cache().statistics().starved;
    if (Status rendered = shadows.render(scene, report); !rendered) {
        return rendered;
    }

    out.width = capture.width;
    out.height = capture.height;
    if (Status sized = out.texels.resize(surfaces.size()); !sized) {
        return sized;
    }
    const Vec3 sun_direction = normalize(sun->direction);
    for (usize pixel = 0; pixel < surfaces.size(); ++pixel) {
        const Surface& surface = surfaces[pixel];
        if (!surface.covered) {
            out.texels[pixel] = kBackground;
            continue;
        }
        const Vec3 albedo = kAlbedo[surface.material < 8U ? surface.material : 7U];
        const f32 n_dot_l = math::max(dot(surface.normal, sun_direction * -1.0F), 0.0F);
        f32 visibility = 0.0F;
        if (n_dot_l <= 0.0F) {
            ++report.facing_away;
        } else {
            visibility =
                shadows.sample(requested[pixel], surface.position, surface.normal, n_dot_l, report);
            (visibility > 0.0F ? report.lit_by_sun : report.shadowed) += 1U;
        }

        const f32 hemisphere = (surface.normal.y * 0.5F) + 0.5F;
        const Vec3 ambient =
            ((kSkyColour * hemisphere) + (kGroundColour * (1.0F - hemisphere))) * kAmbientScale;
        // THE SUN'S ILLUMINANCE IS NORMALISED TO ONE. `scene.cpp` declares it in lux (92,000) and
        // this shot has no photometric pipeline to convert that with, so the directional term is
        // unit-scaled and the two point lights are in the same normalised units through their own
        // inverse square. Stated here because an exposure nobody can read is an exposure nobody can
        // reproduce.
        const Vec3 direct = sun->colour * (n_dot_l * visibility);
        const Vec3 linear = multiply(albedo, direct + punctual(scene, surface) + ambient);
        out.texels[pixel] = encode(linear * options.exposure);
    }
    report.cache = shadows.cache().statistics();
    return ok();
}

}  // namespace cy::sample::fidelity
