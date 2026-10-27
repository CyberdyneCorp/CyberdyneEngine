// SPDX-License-Identifier: MIT
// The hero's skinned mesh. See hero_mesh.h.

#include "hero_mesh.h"

#include <cmath>

#include "hero_rig.h"

namespace sample::tps {
namespace {

using cy::f32;
using cy::u32;
using cy::u8;
using cy::Vec2;
using cy::Vec3;

constexpr f32 kPi = 3.14159265F;

/// Builds the mesh a part at a time: each part's triangles in one material, each vertex on one
/// joint.
class Builder {
public:
    explicit Builder(HeroMesh& out) noexcept : out_(&out) {}

    void begin(const char* material, u8 joint) {
        joint_ = joint;
        if (out_->parts.empty() || out_->parts.back().material != material) {
            HeroPart part;
            part.first_index = static_cast<u32>(out_->indices.size());
            part.material = material;
            out_->parts.push_back(part);
        }
    }

    u32 vertex(Vec3 position, Vec3 normal, Vec2 uv) {
        out_->positions.push_back(position);
        out_->normals.push_back(normal);
        out_->uvs.push_back(uv);
        out_->joints.push_back(joint_);
        return static_cast<u32>(out_->positions.size() - 1U);
    }

    void triangle(u32 a, u32 b, u32 c) {
        out_->indices.insert(out_->indices.end(), {a, b, c});
        out_->parts.back().index_count += 3U;
    }

    /// A box between `low` and `high` whose top face is `taper` times as wide on x and z.
    void box(Vec3 low, Vec3 high, f32 taper = 1.0F) {
        const Vec3 centre = (low + high) * 0.5F;
        const auto corner = [&](u32 bits) {
            const bool top = (bits & 2U) != 0U;
            const f32 scale = top ? taper : 1.0F;
            const f32 x = (bits & 1U) != 0U ? high.x : low.x;
            const f32 z = (bits & 4U) != 0U ? high.z : low.z;
            return Vec3{centre.x + ((x - centre.x) * scale), top ? high.y : low.y,
                        centre.z + ((z - centre.z) * scale)};
        };
        // Each face's four corners, wound counter-clockwise seen from outside.
        constexpr u32 kFaces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
                                      {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
        for (const auto& face : kFaces) {
            const Vec3 a = corner(face[0]);
            const Vec3 b = corner(face[1]);
            const Vec3 c = corner(face[2]);
            const Vec3 d = corner(face[3]);
            const Vec3 normal = cy::normalize(cy::cross(b - a, c - a));
            const u32 first = vertex(a, normal, Vec2{0.0F, 0.0F});
            (void)vertex(b, normal, Vec2{1.0F, 0.0F});
            (void)vertex(c, normal, Vec2{1.0F, 1.0F});
            (void)vertex(d, normal, Vec2{0.0F, 1.0F});
            triangle(first, first + 1U, first + 2U);
            triangle(first, first + 2U, first + 3U);
        }
    }

    /// A capped cylinder along y from `bottom` up to `top`, `sides` round, its radius at the top
    /// `taper` times the bottom's.
    void cylinder(Vec3 bottom, f32 top, f32 radius, u32 sides = 8U, f32 taper = 1.0F) {
        const f32 height = top - bottom.y;
        const u32 ring = static_cast<u32>(out_->positions.size());
        for (u32 side = 0; side <= sides; ++side) {
            const f32 angle = 2.0F * kPi * static_cast<f32>(side) / static_cast<f32>(sides);
            const Vec3 out{std::cos(angle), 0.0F, std::sin(angle)};
            const f32 slope = (radius - (radius * taper)) / std::fmax(height, 1e-3F);
            const Vec3 normal = cy::normalize(Vec3{out.x, slope, out.z});
            const f32 u = static_cast<f32>(side) / static_cast<f32>(sides);
            (void)vertex(bottom + (out * radius), normal, Vec2{u, 0.0F});
            (void)vertex(Vec3{bottom.x, top, bottom.z} + (out * (radius * taper)), normal,
                         Vec2{u, 1.0F});
        }
        for (u32 side = 0; side < sides; ++side) {
            const u32 a = ring + (side * 2U);
            triangle(a, a + 1U, a + 3U);
            triangle(a, a + 3U, a + 2U);
        }
        for (const bool upper : {false, true}) {
            const f32 y = upper ? top : bottom.y;
            const f32 r = upper ? radius * taper : radius;
            const Vec3 normal{0.0F, upper ? 1.0F : -1.0F, 0.0F};
            const u32 centre = vertex(Vec3{bottom.x, y, bottom.z}, normal, Vec2{0.5F, 0.5F});
            for (u32 side = 0; side < sides; ++side) {
                const f32 angle = 2.0F * kPi * static_cast<f32>(side) / static_cast<f32>(sides);
                (void)vertex(
                    Vec3{bottom.x + (std::cos(angle) * r), y, bottom.z + (std::sin(angle) * r)},
                    normal, Vec2{0.5F + (0.5F * std::cos(angle)), 0.5F + (0.5F * std::sin(angle))});
            }
            for (u32 side = 0; side < sides; ++side) {
                const u32 a = centre + 1U + side;
                const u32 b = centre + 1U + ((side + 1U) % sides);
                if (upper) {
                    triangle(centre, b, a);
                } else {
                    triangle(centre, a, b);
                }
            }
        }
    }

    /// An ellipsoid of radii `radii` round `centre`, `rings` by `segments`.
    void sphere(Vec3 centre, Vec3 radii, u32 rings = 7U, u32 segments = 10U) {
        const u32 first = static_cast<u32>(out_->positions.size());
        for (u32 ring = 0; ring <= rings; ++ring) {
            const f32 polar = kPi * static_cast<f32>(ring) / static_cast<f32>(rings);
            for (u32 segment = 0; segment <= segments; ++segment) {
                const f32 azimuth =
                    2.0F * kPi * static_cast<f32>(segment) / static_cast<f32>(segments);
                const Vec3 unit{std::sin(polar) * std::cos(azimuth), std::cos(polar),
                                std::sin(polar) * std::sin(azimuth)};
                const Vec3 normal =
                    cy::normalize(Vec3{unit.x / radii.x, unit.y / radii.y, unit.z / radii.z});
                (void)vertex(centre + Vec3{unit.x * radii.x, unit.y * radii.y, unit.z * radii.z},
                             normal,
                             Vec2{static_cast<f32>(segment) / static_cast<f32>(segments),
                                  static_cast<f32>(ring) / static_cast<f32>(rings)});
            }
        }
        const u32 stride = segments + 1U;
        for (u32 ring = 0; ring < rings; ++ring) {
            for (u32 segment = 0; segment < segments; ++segment) {
                const u32 a = first + (ring * stride) + segment;
                const u32 b = a + stride;
                triangle(a, a + 1U, b + 1U);
                triangle(a, b + 1U, b);
            }
        }
    }

private:
    HeroMesh* out_;
    u8 joint_ = 0;
};

[[nodiscard]] Vec3 at(cy::u16 joint) noexcept {
    return hero_bind_position(joint);
}

void build_head(Builder& b) {
    const Vec3 head = at(kHead);
    b.begin("hero_skin", kHead);
    b.sphere(head + Vec3{0.0F, 0.10F, 0.0F}, Vec3{0.105F, 0.125F, 0.115F});
    // A nose, and the ears.
    b.box(head + Vec3{-0.015F, 0.07F, -0.135F}, head + Vec3{0.015F, 0.11F, -0.105F});
    b.box(head + Vec3{-0.118F, 0.07F, -0.02F}, head + Vec3{-0.098F, 0.13F, 0.02F});
    b.box(head + Vec3{0.098F, 0.07F, -0.02F}, head + Vec3{0.118F, 0.13F, 0.02F});
    b.begin("hero_hair", kHead);
    b.sphere(head + Vec3{0.0F, 0.135F, 0.022F}, Vec3{0.112F, 0.11F, 0.116F});
    // The eyes, the brows over them, and the mouth: dark, on the face's front.
    for (const f32 side : {-1.0F, 1.0F}) {
        b.box(head + Vec3{(side * 0.038F) - 0.011F, 0.112F, -0.121F},
              head + Vec3{(side * 0.038F) + 0.011F, 0.128F, -0.104F});
        b.box(head + Vec3{(side * 0.040F) - 0.018F, 0.142F, -0.118F},
              head + Vec3{(side * 0.040F) + 0.018F, 0.150F, -0.100F});
    }
    b.box(head + Vec3{-0.024F, 0.050F, -0.116F}, head + Vec3{0.024F, 0.057F, -0.100F});
    b.begin("hero_skin", kNeck);
    const Vec3 neck = at(kNeck);
    b.cylinder(neck + Vec3{0.0F, -0.04F, 0.0F}, neck.y + 0.11F, 0.05F);
}

void build_torso(Builder& b) {
    const Vec3 chest = at(kChest);
    const Vec3 spine = at(kSpine);
    const Vec3 pelvis = at(kPelvis);
    b.begin("hero_shirt", kChest);
    b.box(Vec3{-0.17F, chest.y - 0.06F, -0.105F}, Vec3{0.17F, chest.y + 0.24F, 0.10F}, 1.12F);
    // The shoulders' yoke, so the sleeves meet the body.
    b.box(Vec3{-0.23F, chest.y + 0.15F, -0.09F}, Vec3{0.23F, chest.y + 0.24F, 0.085F});
    b.begin("hero_shirt", kSpine);
    b.box(Vec3{-0.15F, spine.y - 0.07F, -0.095F}, Vec3{0.15F, spine.y + 0.18F, 0.09F}, 1.08F);
    b.begin("hero_leather", kChest);
    // The satchel on the back, and its strap across the chest.
    b.box(Vec3{-0.12F, chest.y - 0.12F, 0.10F}, Vec3{0.12F, chest.y + 0.16F, 0.21F}, 0.92F);
    b.box(Vec3{-0.17F, chest.y + 0.10F, -0.112F}, Vec3{0.17F, chest.y + 0.14F, -0.10F});
    b.begin("hero_trousers", kPelvis);
    b.box(Vec3{-0.165F, pelvis.y - 0.11F, -0.10F}, Vec3{0.165F, pelvis.y + 0.08F, 0.10F}, 0.95F);
    b.begin("hero_leather", kPelvis);
    b.box(Vec3{-0.168F, pelvis.y + 0.04F, -0.104F}, Vec3{0.168F, pelvis.y + 0.09F, 0.104F});
}

void build_arm(Builder& b, cy::u16 shoulder, cy::u16 elbow, cy::u16 wrist) {
    const Vec3 s = at(shoulder);
    const Vec3 e = at(elbow);
    const Vec3 w = at(wrist);
    b.begin("hero_shirt", static_cast<u8>(shoulder));
    b.sphere(s, Vec3{0.065F, 0.06F, 0.065F}, 5U, 8U);
    b.cylinder(Vec3{s.x, e.y - 0.01F, s.z}, s.y, 0.052F, 8U, 1.12F);
    b.begin("hero_skin", static_cast<u8>(elbow));
    b.cylinder(Vec3{e.x, w.y + 0.01F, e.z}, e.y + 0.02F, 0.038F, 8U, 1.2F);
    b.begin("hero_skin", static_cast<u8>(wrist));
    b.box(Vec3{w.x - 0.025F, w.y - 0.10F, w.z - 0.04F},
          Vec3{w.x + 0.025F, w.y + 0.01F, w.z + 0.03F});
}

void build_leg(Builder& b, cy::u16 hip, cy::u16 knee, cy::u16 ankle, cy::u16 toe) {
    const Vec3 h = at(hip);
    const Vec3 k = at(knee);
    const Vec3 a = at(ankle);
    const Vec3 t = at(toe);
    b.begin("hero_trousers", static_cast<u8>(hip));
    b.cylinder(Vec3{h.x, k.y + 0.02F, k.z * 0.5F}, h.y + 0.06F, 0.064F, 8U, 1.25F);
    b.begin("hero_trousers", static_cast<u8>(knee));
    b.cylinder(Vec3{k.x, a.y + 0.14F, k.z * 0.5F}, k.y + 0.03F, 0.056F, 8U, 1.12F);
    b.begin("hero_boots", static_cast<u8>(knee));
    b.cylinder(Vec3{a.x, a.y - 0.02F, a.z}, a.y + 0.17F, 0.062F, 8U, 1.05F);
    b.begin("hero_boots", static_cast<u8>(ankle));
    b.box(Vec3{a.x - 0.052F, 0.0F, t.z + 0.03F}, Vec3{a.x + 0.052F, a.y + 0.04F, a.z + 0.06F});
    b.begin("hero_boots", static_cast<u8>(toe));
    b.box(Vec3{t.x - 0.05F, 0.0F, t.z - 0.05F}, Vec3{t.x + 0.05F, t.y + 0.045F, t.z + 0.03F},
          0.85F);
}

}  // namespace

HeroMesh build_hero_mesh() {
    HeroMesh mesh;
    Builder builder(mesh);
    build_head(builder);
    build_torso(builder);
    build_arm(builder, kShoulderL, kElbowL, kWristL);
    build_arm(builder, kShoulderR, kElbowR, kWristR);
    build_leg(builder, kHipL, kKneeL, kAnkleL, kToeL);
    build_leg(builder, kHipR, kKneeR, kAnkleR, kToeR);
    return mesh;
}

}  // namespace sample::tps
