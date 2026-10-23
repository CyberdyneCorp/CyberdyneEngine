// SPDX-License-Identifier: MIT
#pragma once
// One compute pipeline of this module: its shader, its one set layout and its layout. Each pass of
// the module is one of these; `create` and `destroy` are the only places the four handles are made
// and released, so a pass cannot leak one on a failed creation.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::lights_gpu::detail {

struct ComputeProgram {
    const char* name = "";
    /// The committed modules: SPIR-V words and MSL text. The entry point is SPIR-V's `main` and the
    /// MSL's own name.
    Span<const u32> spirv;
    Span<const u8> msl;
    const char* msl_entry = "main";
    /// The set's bindings, in the `ParameterBlock`'s field order, which is also Metal's argument
    /// order.
    Span<const rhi::DescriptorKind> bindings;
    u32 push_bytes = 0;
    u32 workgroup[3] = {1, 1, 1};
};

class ComputePipeline {
public:
    [[nodiscard]] Status create(rhi::Device& device, const ComputeProgram& program) noexcept;
    void destroy(rhi::Device& device) noexcept;

    /// A descriptor set of this pipeline's layout for this frame, written with `writes`.
    [[nodiscard]] Expected<rhi::DescriptorSetHandle, Error> set(
        rhi::Device& device, Span<const rhi::DescriptorWrite> writes) const noexcept;

    [[nodiscard]] rhi::ComputePipelineHandle pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] rhi::PipelineLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] bool ready() const noexcept { return !pipeline_.is_null(); }

private:
    rhi::ShaderModuleHandle shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle layout_;
    rhi::ComputePipelineHandle pipeline_;
};

/// Whether the device has a compute stage and a native format this module ships.
[[nodiscard]] bool compute_supported(const rhi::Device& device) noexcept;

/// A host-visible buffer, created and checked mapped.
[[nodiscard]] Expected<rhi::BufferHandle, Error> host_buffer(rhi::Device& device, const char* name,
                                                             u64 size,
                                                             rhi::BufferUsage usage) noexcept;

/// A device-local buffer.
[[nodiscard]] Expected<rhi::BufferHandle, Error> device_buffer(rhi::Device& device,
                                                               const char* name, u64 size,
                                                               rhi::BufferUsage usage) noexcept;

/// A storage-buffer write for binding `binding`.
[[nodiscard]] rhi::DescriptorWrite storage_write(u32 binding, rhi::BufferHandle buffer) noexcept;
/// A sampled-texture or storage-texture write for binding `binding`.
[[nodiscard]] rhi::DescriptorWrite texture_write(u32 binding, rhi::TextureViewHandle view,
                                                 bool storage) noexcept;

}  // namespace cy::rendering::lights_gpu::detail
