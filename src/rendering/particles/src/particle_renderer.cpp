// The first consumer of the pipeline layer, and the proof of it. M8.c task 1b.4.

#include <cy/rendering/particles/particle_renderer.h>

#include "particle_msl.h"
#include "particle_spirv.h"

#include <cstring>

namespace cy::rendering::particles {
namespace {

inline constexpr u32 kVerticesPerParticle = 6;

/// The extension's callback. Everything it binds is either the frame's (sets 0 and 1, already
/// bound and still valid because the layouts are identical) or its own (set 2 and the pipeline).
void record_particles(const ExtensionContext& context, void* user) noexcept {
    auto* renderer = static_cast<ParticleRenderer*>(user);
    if (renderer == nullptr || !renderer->ready() || renderer->live() == 0) {
        return;
    }
    if (!context.inside_rendering || context.commands == nullptr) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    renderer->draw().bind(commands);
    // ONE DRAW for the whole effect. Six vertices a particle, expanded from `SV_VertexID`; there is
    // no vertex buffer, no index buffer and no per-particle call. The first vertex is ZERO and must
    // stay so: SPIR-V's index is `VertexIndex - BaseVertex` and Metal's counts from the first
    // vertex, and zero is the base on which they agree (`cy/fullscreen.slang`).
    commands.draw(renderer->live() * kVerticesPerParticle, 1, 0, 0);
    ++renderer->mutable_report().draws;
}

}  // namespace

ParticleRenderer::~ParticleRenderer() {
    shutdown();
}

Status ParticleRenderer::initialize(rhi::Device& device, const FramePipelines& pipelines,
                                    u32 capacity) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "particle renderer: already initialized");
    }
    if (capacity == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "particle renderer: a capacity of zero draws "
                    "nothing and hides the reason");
    }
    detail::TransparentDrawDescription description;
    description.name = "particles";
    description.vertex.name = "cy particle vertex";
    description.vertex.spirv =
        Span<const u32>(kParticleVertexSpirv, sizeof(kParticleVertexSpirv) / sizeof(u32));
    description.vertex.msl = Span<const u8>(reinterpret_cast<const u8*>(kParticleVertexMsl),
                                            sizeof(kParticleVertexMsl) - 1);
    description.vertex.msl_entry = "cyParticleVertex";
    description.fragment.name = "cy particle fragment";
    description.fragment.spirv =
        Span<const u32>(kParticleFragmentSpirv, sizeof(kParticleFragmentSpirv) / sizeof(u32));
    description.fragment.msl = Span<const u8>(reinterpret_cast<const u8*>(kParticleFragmentMsl),
                                              sizeof(kParticleFragmentMsl) - 1);
    description.fragment.msl_entry = "cyParticleFragment";
    description.record_size = sizeof(ParticleInstance);
    description.capacity = capacity;
    if (Status made = draw_.initialize(device, pipelines, description); !made) {
        return made;
    }
    pipelines_ = &pipelines;
    capacity_ = capacity;
    ready_ = true;
    return ok();
}

Status ParticleRenderer::upload(u32 frame_slot, Span<const ParticleInstance> particles) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "particle renderer: not initialized");
    }
    live_ = static_cast<u32>(particles.size() > capacity_ ? capacity_ : particles.size());
    report_.particles = live_;
    report_.dropped = static_cast<u32>(particles.size()) - live_;
    return draw_.upload(frame_slot, particles.data(), live_);
}

PassExtension ParticleRenderer::extension() noexcept {
    PassExtension extension;
    extension.kind = FramePassKind::Transparent;
    extension.record = &record_particles;
    extension.user = this;
    return extension;
}

void ParticleRenderer::shutdown() noexcept {
    draw_.shutdown();
    pipelines_ = nullptr;
    capacity_ = 0;
    live_ = 0;
    ready_ = false;
}

}  // namespace cy::rendering::particles
