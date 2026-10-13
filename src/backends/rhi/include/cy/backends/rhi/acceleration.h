// SPDX-License-Identifier: MIT
#pragma once
// Acceleration structures: what a device builds for ray queries. Issue #35, stage 3.
//
// `ray-tracing-infrastructure` — "Ray tracing infrastructure is a renderer service": the SERVICE
// (`cy::rendering-raytracing`'s `AccelerationService`) decides what is built, refit, kept and
// evicted, and under which budget. This file is the half below it that a device needs: a bottom
// level over one triangle list, a top level over instances of bottom levels, a build or an
// in-place update recorded into a command buffer, and a descriptor a shader's ray query reads.
//
// ================================================================================================
// WHAT A DEVICE WITHOUT RAY QUERIES DOES
// ================================================================================================
//
// It refuses, by name, at creation: `Device::create_acceleration_structure` returns
// `ErrorCode::Unsupported` on every backend whose device does not report `Capability::RayTracing`,
// and that is the base class's own answer — so the null, D3D12 and Metal backends refuse without a
// line of their own, and a consumer selects its software tier once from the refusal (or from the
// capability) rather than per ray. Vulkan is the first backend that builds one; DXR and Metal's
// `MTLAccelerationStructure` are the two later ones, behind this same vocabulary.
//
// ================================================================================================
// THREE OWNERSHIP DECISIONS, SAID ONCE
// ================================================================================================
//
//   * THE STRUCTURE OWNS ITS STORAGE AND ITS SCRATCH. A build needs scratch memory whose size the
//     driver states and whose alignment it constrains; one scratch per structure, allocated with it
//     and sized for the larger of a build and an update, means builds of different structures never
//     share a scratch region and a caller never sizes one.
//   * A TOP LEVEL OWNS ITS INSTANCE RECORDS. The record format is the backend's (Vulkan's and
//     DXR's happen to agree; Metal's does not), so the caller hands `AccelerationStructureInstance`
//     values and the backend writes its own form into memory it owns at record time.
//   * SYNCHRONISATION STAYS THE RENDER GRAPH'S. `acceleration_structure_info().storage` is a
//     buffer handle like any other: a pass that builds declares `AccelerationStructureBuildWrite`
//     on it, a top-level build declares `AccelerationStructureBuildRead` on each bottom level's,
//     and a pass that traces declares `ComputeAccelerationStructureRead`. The graph derives the
//     barriers from those rows of access.h exactly as it does for every other resource.
//
// THE INPUTS ARE THE CALLER'S. A bottom level's vertices and indices live in ordinary buffers
// created with `BufferUsage::AccelerationStructureInput`; the build reads them where they are, and
// a refit is the same build with `AccelerationStructureBuildMode::Update` over moved vertices.

#include <cy/backends/rhi/handles.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rhi {

enum class AccelerationStructureKind : u8 {
    /// Triangles in one object's own space.
    BottomLevel = 0,
    /// Instances of bottom levels, each with a transform.
    TopLevel,
};

enum class AccelerationStructureBuildMode : u8 {
    /// From nothing. The structure's previous contents are discarded.
    Build = 0,
    /// In place, from its previous build: the topology (triangle or instance count) is unchanged
    /// and only positions or transforms moved. Cheaper than a build and of lower traversal quality,
    /// which is why the service decides when one is good enough. Requires `allow_update`.
    Update,
};

/// What a structure is sized for. Fixed at creation: a build that carries more is refused.
struct AccelerationStructureDescription {
    const char* name = "acceleration-structure";
    AccelerationStructureKind kind = AccelerationStructureKind::BottomLevel;
    /// Bottom level: the most triangles and vertices any build of it will carry.
    u32 max_triangles = 0;
    u32 max_vertices = 0;
    /// Top level: the most instances any build of it will carry.
    u32 max_instances = 0;
    /// Builds of it may later be updated in place (`AccelerationStructureBuildMode::Update`).
    bool allow_update = false;
};

/// What a created structure turned out to be.
struct AccelerationStructureInfo {
    /// The buffer the structure lives in. What a render graph pass declares its access against.
    BufferHandle storage;
    u64 structure_bytes = 0;
    u64 scratch_bytes = 0;
    /// The top level's own instance records, in the backend's format. Zero for a bottom level.
    u64 instance_bytes = 0;
    AccelerationStructureKind kind = AccelerationStructureKind::BottomLevel;
    bool allow_update = false;
};

/// One bottom level's input: an indexed triangle list in buffers the caller owns.
///
/// Positions are three `f32`s at `vertex_stride` bytes apart; indices are `u32`. Every triangle is
/// opaque: a ray query over it commits the closest hit without an any-hit test, which is what a
/// GI ray wants — a hit resolves through the surface cache, never through a material's alpha.
struct AccelerationStructureTriangles {
    BufferHandle vertices;
    u64 vertex_offset = 0;
    u32 vertex_count = 0;
    u32 vertex_stride = 12;
    BufferHandle indices;
    u64 index_offset = 0;
    u32 triangle_count = 0;
};

/// One instance of a top level.
struct AccelerationStructureInstance {
    /// Object to world: the first three ROWS of the matrix, row-major — `transform[4 * r + c]`.
    f32 transform[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    /// Reported by a ray query's hit unchanged (`CommittedInstanceID`). 24 bits.
    u32 custom_index = 0;
    /// A ray query's cull mask is ANDed with this; zero makes the instance invisible.
    u8 mask = 0xFF;
    AccelerationStructureHandle bottom;
};

/// One build or update, recorded by `CommandBuffer::build_acceleration_structures`.
struct AccelerationStructureBuild {
    AccelerationStructureHandle target;
    AccelerationStructureBuildMode mode = AccelerationStructureBuildMode::Build;
    /// A bottom level's triangles. Ignored for a top level.
    AccelerationStructureTriangles triangles{};
    /// A top level's instances, written by the backend into the structure's own instance records.
    /// Ignored for a bottom level. Not owned; read during the call only.
    Span<const AccelerationStructureInstance> instances;
};

}  // namespace cy::rhi
