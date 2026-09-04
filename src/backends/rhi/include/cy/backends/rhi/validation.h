#pragma once
// Creation-time validation of the RHI's hard limits. Task 2.1.1.
//
// `rhi-and-render-graph`, "Descriptor exceeds a limit": creation SHALL fail "with a diagnostic
// naming the limit, at creation time rather than at draw time". The rule is the same for every
// limit and every backend, so the checks live here and each backend calls them before it touches
// its own API. A backend that wrote its own copy would eventually have a different opinion about
// what kMaxPushConstantBytes means, and the difference would show up as a pipeline that works on
// one platform.
//
// Every function returns a cy::Status whose message names the limit, the requested value and the
// ceiling — because "too many descriptor sets" is a diagnostic the reader has to go and measure,
// and "9 descriptor sets; the engine's limit is 8" is one they can act on.
//
// The messages are built into a caller-supplied buffer rather than allocated: cy::Error carries a
// `const char*` that it does not own, and an engine with -fno-exceptions and no allocator on the
// failure path has nowhere else to put the text.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/resources.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

namespace cy::rhi {

/// Storage for one diagnostic. Lives on the caller's stack for the duration of the create call,
/// which is exactly as long as the Error it fills is looked at.
struct ValidationMessage {
    char text[256] = {};

    /// `printf`-shaped. Returns `text`, so a caller writes
    /// `return fail(ErrorCode::InvalidArgument, message.format("..."));`
    // NOLINTNEXTLINE(cert-dcl50-cpp) — a diagnostic sink is variadic by nature; see .clang-tidy.
    const char* format(const char* pattern, ...) noexcept;
};

/// Every limit check a backend must make before creating the thing. Each is independent so that a
/// backend can report the first failure rather than a list, which is what a caller can act on.
[[nodiscard]] Status validate_buffer(const BufferDescription& desc,
                                     ValidationMessage& message) noexcept;
[[nodiscard]] Status validate_texture(const TextureDescription& desc, const DeviceLimits& limits,
                                      ValidationMessage& message) noexcept;
[[nodiscard]] Status validate_texture_view(const TextureViewDescription& desc,
                                           const TextureDescription& texture,
                                           ValidationMessage& message) noexcept;
[[nodiscard]] Status validate_sampler(const SamplerDescription& desc, const DeviceLimits& limits,
                                      ValidationMessage& message) noexcept;
[[nodiscard]] Status validate_pipeline_layout(const PipelineLayoutDescription& desc,
                                              ValidationMessage& message) noexcept;
[[nodiscard]] Status validate_graphics_pipeline(const GraphicsPipelineDescription& desc,
                                                ValidationMessage& message) noexcept;
[[nodiscard]] Status validate_device_limits(const DeviceLimits& limits,
                                            ValidationMessage& message) noexcept;

/// Resolve a "count of 0 means all remaining" range against an image. Every caller that tracks or
/// records a subresource range goes through this, so the sentinel exists in exactly one place.
[[nodiscard]] SubresourceRange resolve_range(const SubresourceRange& range, u16 mip_levels,
                                             u16 array_layers) noexcept;

/// Whether an intent is legal against a resource of this kind. Checked when a pass declares a use,
/// so that "you cannot sample a buffer" names the pass rather than surfacing as a barrier with an
/// undefined layout.
[[nodiscard]] bool access_valid_for(Access access, bool is_image) noexcept;

}  // namespace cy::rhi
