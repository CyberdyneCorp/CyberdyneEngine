#pragma once
// The skinning compute pipeline, shared by `SkinPass` and `SkinnedScene`. Private to the module.
//
// ONE SHADER, ONE LAYOUT, TWO OWNERS. `SkinPass` skins one mesh and `SkinnedScene` skins a scene's
// worth of instances; both bind the same nine storage buffers of `shaders/skin.slang` and push the
// same `GpuSkinConstants`. Creating the pipeline is therefore one function, so the binding list a
// descriptor set is written against cannot differ between the two.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/skinning/skin_pass.h>

namespace cy::rendering::skinning::detail {

/// `skin_vertices`'s `[numthreads(64, 1, 1)]`.
inline constexpr u32 kSkinGroupSize = 64;

/// The nine bindings the shader declares, in the order it declares them.
enum SkinBinding : u32 {
    kBindingBones = 0,
    kBindingInPositions = 1,
    kBindingInFrames = 2,
    kBindingInfluences = 3,
    kBindingOutPositions = 4,
    kBindingOutFrames = 5,
    kBindingBoneDualQuaternions = 6,
    kBindingBlendShapeDeltas = 7,
    kBindingActiveBlendShapes = 8,
    kBindingCount = 9,
};

// `SkinPipeline` itself — the device objects the dispatch needs, created and destroyed together —
// is declared in skin_pass.h, because both public classes hold one by value.

/// Whether the device can run the dispatch: compute, and a native format the module ships.
[[nodiscard]] bool skin_supported(const rhi::Device& device) noexcept;

/// The refusal `create` gives on a device `skin_supported` rejects.
[[nodiscard]] Status skin_unsupported(const rhi::Device& device) noexcept;

/// Write the nine bindings of `set`, in `SkinBinding` order.
[[nodiscard]] Status write_skin_set(rhi::Device& device, rhi::DescriptorSetHandle set,
                                    const rhi::BufferHandle (&buffers)[kBindingCount]) noexcept;

/// Vulkan has no zero-length buffer, and a descriptor must name something even when the stream
/// behind it is empty.
[[nodiscard]] constexpr u64 at_least_one(u64 count, u64 stride) noexcept {
    return (count == 0 ? 1 : count) * stride;
}

/// Thread groups covering `vertices`, never zero.
[[nodiscard]] constexpr u32 skin_groups(u32 vertices) noexcept {
    const u32 groups = (vertices + kSkinGroupSize - 1U) / kSkinGroupSize;
    return groups == 0 ? 1U : groups;
}

}  // namespace cy::rendering::skinning::detail
