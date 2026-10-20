// SPDX-License-Identifier: MIT
#pragma once
// The editor GI suites' world: a floor, a crate, a small glowing lamp hung over the floor and a
// cart, under a sun, as the host would read it from a document. Shared by
// `integration.editor_backend_illumination` and `smoke.editor_gi_cost`.

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/illumination_preview.h>

#include <iterator>
#include <vector>

namespace cy::illumination_test {

namespace gi = cy::rendering::gi;
using editor::IlluminationEmission;
using editor::IlluminationMaterial;
using editor::IlluminationMesh;
using editor::IlluminationObject;
using editor::IlluminationScene;
using editor::IlluminationViewCamera;

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A unit cube centred on the origin, outward winding.
struct Cube {
    std::vector<Vec3> positions;
    std::vector<u32> indices;

    Cube() {
        const Vec3 corners[8] = {{-0.5F, -0.5F, -0.5F}, {0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, -0.5F},
                                 {-0.5F, 0.5F, -0.5F},  {-0.5F, -0.5F, 0.5F}, {0.5F, -0.5F, 0.5F},
                                 {0.5F, 0.5F, 0.5F},    {-0.5F, 0.5F, 0.5F}};
        positions.assign(std::begin(corners), std::end(corners));
        indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                   3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    }
};

constexpr u64 kFloor = 0x10;
constexpr u64 kCrate = 0x11;
constexpr u64 kLamp = 0x12;
constexpr u64 kCart = 0x13;

/// A floor, a crate, a small glowing lamp hung over the floor and a cart, under a sun.
struct World {
    Cube cube;
    std::vector<IlluminationMesh> meshes;
    std::vector<IlluminationMaterial> materials;
    std::vector<IlluminationObject> objects;
    std::vector<gi::GiLight> lights;
    std::vector<gi::GiVolume> volumes;
    std::vector<IlluminationEmission> emission;
    gi::GiRuntimeSettings settings{};
    gi::SkyTerm sky{};

    World() {
        meshes.push_back(IlluminationMesh{"cube",
                                          {cube.positions.data(), cube.positions.size()},
                                          {cube.indices.data(), cube.indices.size()}});
        materials.push_back(IlluminationMaterial{"materials/stone", Vec3{0.6F, 0.6F, 0.6F}});
        materials.push_back(IlluminationMaterial{"materials/red", Vec3{0.8F, 0.1F, 0.1F}});
        IlluminationMaterial glass{"materials/lamp-glass", Vec3{0.8F, 0.8F, 0.8F}};
        glass.emission = Vec3{60.0F, 55.0F, 50.0F};
        materials.push_back(glass);
        IlluminationObject floor;
        floor.node = kFloor;
        floor.transform = Mat4::from_translation(Vec3{0.0F, -0.25F, 0.0F}) *
                          Mat4::from_scale(Vec3{8.0F, 0.5F, 8.0F});
        objects.push_back(floor);
        IlluminationObject crate;
        crate.node = kCrate;
        crate.material = 1;
        crate.transform = Mat4::from_translation(Vec3{1.5F, 0.5F, 0.0F});
        objects.push_back(crate);
        IlluminationObject lamp;
        lamp.node = kLamp;
        lamp.material = 2;
        lamp.transform = Mat4::from_translation(Vec3{-1.5F, 2.0F, -1.0F}) *
                         Mat4::from_scale(Vec3{0.3F, 0.1F, 0.3F});
        objects.push_back(lamp);
        IlluminationObject cart;
        cart.node = kCart;
        cart.movable = true;
        cart.transform = Mat4::from_translation(Vec3{-1.0F, 0.4F, 2.0F}) *
                         Mat4::from_scale(Vec3{0.8F, 0.8F, 0.8F});
        objects.push_back(cart);
        gi::GiLight sun;
        sun.directional = true;
        sun.direction = normalize(Vec3{0.4F, -1.0F, 0.3F});
        sun.intensity = 2.0F;
        sun.id = 1;
        lights.push_back(sun);
    }

    [[nodiscard]] IlluminationScene scene() const noexcept {
        IlluminationScene out;
        out.meshes = {meshes.data(), meshes.size()};
        out.materials = {materials.data(), materials.size()};
        out.objects = {objects.data(), objects.size()};
        out.lights = {lights.data(), lights.size()};
        out.volumes = {volumes.data(), volumes.size()};
        out.emission = {emission.data(), emission.size()};
        out.settings = settings;
        out.sky = sky;
        return out;
    }
};

[[nodiscard]] inline IlluminationViewCamera camera(u32 width, u32 height) noexcept {
    IlluminationViewCamera out;
    out.width = width;
    out.height = height;
    out.eye = Vec3{0.0F, 4.0F, 8.0F};
    const Mat4 view = look_at(out.eye, Vec3{0.0F, 0.0F, 0.0F});
    const Mat4 projection = perspective_reversed_z(
        1.0F, static_cast<f32>(width) / static_cast<f32>(height), 0.1F, 100.0F);
    out.world_to_clip = projection * view;
    out.clip_to_world = inverse(out.world_to_clip).value();
    return out;
}

}  // namespace cy::illumination_test
