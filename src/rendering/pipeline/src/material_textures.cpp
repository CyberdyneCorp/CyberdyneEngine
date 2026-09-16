#include <cy/rendering/pipeline/material_textures.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/servers/render/types.h>

#include <cstring>

namespace cy::rendering::pipeline {
namespace {

/// What one pass records: the copies, and the staging buffer they read from.
struct UploadRecording {
    const Array<rhi::BufferTextureCopy>* regions = nullptr;
    const Array<rhi::TextureHandle>* destinations = nullptr;
    /// `destinations[owner[i]]` is what `regions[i]` copies into. Kept beside the regions rather
    /// than inside them because `BufferTextureCopy` is the RHI's struct and has no room for it.
    const Array<u32>* owner = nullptr;
    rhi::BufferHandle staging;
};

void record_uploads(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const UploadRecording*>(user);
    for (usize index = 0; index < recording->regions->size(); ++index) {
        const rhi::BufferTextureCopy& region = (*recording->regions)[index];
        context.commands->copy_buffer_to_texture((*recording->staging_or(recording)).staging,
                                                 (*recording->destinations)[(*recording->owner)
                                                                                [index]],
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }
}

}  // namespace
}  // namespace cy::rendering::pipeline
