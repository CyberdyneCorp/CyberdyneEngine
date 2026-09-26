// SPDX-License-Identifier: MIT
#pragma once
// A scene of axis-aligned proxy boxes that illumination can trace and look up.
//
// Proxy geometry is how a probe capture sees a level that has no cooked GI representation yet: a
// designer's blockout, or the bounding boxes of what the frame draws. It implements the same three
// seams every GI consumer holds — `SceneTracer` for where a ray lands, `Occluder` for the direct
// term's shadow, `RadianceLookup` for what leaves the surface there — so an `IrradianceVolume`, the
// radiance cache or the path tracer capture from it without knowing it is boxes.
//
// A surface's outgoing radiance is Lambertian: `albedo / pi` times the shadowed direct irradiance
// from `lights`, plus `albedo` times whatever `indirect` returns (the volume itself, for a capture
// that accumulates bounces), plus emission. There is no specular lobe. That is the approximation a
// diffuse probe capture makes anyway, and the frame's own specular is not what a probe stores.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>

namespace cy::rendering::gi {

struct ProxyBox {
    Aabb bounds{};
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    /// Outgoing radiance the surface emits on its own.
    Vec3 emission{0.0F, 0.0F, 0.0F};
};

class BoxProxyScene final : public SceneTracer, public Occluder, public RadianceLookup {
public:
    BoxProxyScene() noexcept = default;
    ~BoxProxyScene() override = default;

    BoxProxyScene(const BoxProxyScene&) = delete;
    BoxProxyScene(BoxProxyScene&&) = delete;
    BoxProxyScene& operator=(const BoxProxyScene&) = delete;
    BoxProxyScene& operator=(BoxProxyScene&&) = delete;

    [[nodiscard]] Status add(const ProxyBox& box) noexcept { return boxes_.push_back(box); }
    void clear() noexcept { boxes_.clear(); }
    [[nodiscard]] Span<const ProxyBox> boxes() const noexcept { return boxes_.span(); }

    /// The lights the direct term is shaded with. The span must outlive the scene's queries.
    void set_lights(Span<const GiLight> lights) noexcept { lights_ = lights; }
    /// Indirect light a surface receives, as a radiance, or null for direct light only.
    void set_indirect(const IndirectSource* indirect) noexcept { indirect_ = indirect; }

    /// The first box surface along the ray. A ray that starts INSIDE a box reports that box's exit
    /// face with its outward normal, which is how a capture tells a probe is inside geometry.
    [[nodiscard]] bool trace(Vec3 origin, Vec3 direction, f32 max_distance,
                             SceneHit& hit) const noexcept override;
    [[nodiscard]] bool occluded(Vec3 from, Vec3 to) const noexcept override;

    [[nodiscard]] bool radiance_at(Vec3 position, Vec3 normal, Vec3& radiance,
                                   u32& age_frames) const noexcept override;

private:
    /// The box whose surface `position` lies on, or null.
    [[nodiscard]] const ProxyBox* surface_at(Vec3 position) const noexcept;

    Array<ProxyBox> boxes_;
    Span<const GiLight> lights_;
    const IndirectSource* indirect_ = nullptr;
};

}  // namespace cy::rendering::gi
