// SPDX-License-Identifier: MIT
// MSAA through the attachment model, and the multi-view declaration. M11.d tasks 5.1 and 5.2.
//
// Every case here compiles a plan with no device. What they hold is the half of MSAA a picture
// cannot: that the twin is the graph's, that the resolve lands where the pass said, that a resolve
// which is missing or in the wrong place is REFUSED rather than drawn, and that a frame which never
// asks for MSAA compiles to exactly the plan it compiled to before any of this existed. The half a
// picture can — that 4x edges are smoother than 1x — is `render.msaa_multiview`'s, on a device.

#include <cy/test/test.h>

#include "fixtures.h"

#include <cstring>

using cy::rendering::CompiledGraph;
using cy::rendering::kInvalidResource;
using cy::rendering::PassId;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::QueueKind;
using namespace cy::rendering::test;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

bool message_contains(const cy::Error& error, const char* needle) noexcept {
    return error.message != nullptr && std::strstr(error.message, needle) != nullptr;
}

/// Where a pass sits in the schedule, or ~0 when it was culled.
cy::u32 schedule_position(const CompiledGraph& plan, PassId pass) noexcept {
    cy::u32 position = 0;
    for (const cy::rendering::Submit& submit : plan.submits) {
        for (const cy::rendering::ScheduledPass& scheduled : submit.passes) {
            if (scheduled.pass == pass) {
                return position;
            }
            ++position;
        }
    }
    return ~0U;
}

/// A readback of `colour`: the consumer every case below needs so nothing is culled.
void declare_readback(RenderGraph& graph, ResourceId colour) noexcept {
    const ResourceId out = graph.create_buffer(storage_buffer("readback", 16ULL * 16 * 4));
    graph.add_pass("readback", QueueKind::Graphics)
        .read(colour, Access::TransferRead)
        .write(out, Access::TransferWrite)
        .side_effect();
}

}  // namespace

CY_TEST_CASE("a multisampled pass renders into a graph-owned twin, resolved where it declares") {
    RenderGraph graph(allocator());
    const ResourceId colour = graph.create_texture(colour_target("colour"));
    const PassId draw = graph.add_pass("draw", QueueKind::Graphics)
                            .multisample(4)
                            .write(colour, Access::ColorAttachmentWrite)
                            .resolve(colour)
                            .id();
    declare_readback(graph, colour);
    CY_REQUIRE(graph.status().has_value());

    // THE TWIN: the same texture at four samples, graph-owned, linked both ways.
    const ResourceId twin = graph.resource(colour).multisampled;
    CY_REQUIRE_NE(twin, kInvalidResource);
    CY_CHECK_EQ(graph.resource(twin).texture.sample_count, 4U);
    CY_CHECK_EQ(graph.resource(twin).texture.format, graph.resource(colour).texture.format);
    CY_CHECK_EQ(graph.resource(twin).resolves_into, colour);
    CY_CHECK(graph.resource(twin).transient);
    CY_CHECK_EQ(graph.pass_sample_count(draw), 4U);

    // The pass's attachment landed on the twin, not on the texture it named.
    CY_REQUIRE_EQ(graph.pass_uses(draw).size(), 1U);
    CY_CHECK_EQ(graph.pass_uses(draw)[0].resource, twin);

    // THE RESOLVE: a graph-owned pass declared directly after the one that asked for it.
    const PassId resolve = draw + 1;
    CY_CHECK_EQ(graph.pass_resolve_source(resolve), twin);
    CY_CHECK_EQ(graph.pass_resolve_target(resolve), colour);
    CY_CHECK_EQ(graph.pass_resolve_source(draw), kInvalidResource);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK_EQ(plan->stats.passes_declared, 3U);
    CY_CHECK_EQ(plan->stats.passes_culled, 0U);
    // Draw, then resolve, then the readback — the placement the declaration named.
    CY_CHECK_LT(schedule_position(*plan, draw), schedule_position(*plan, resolve));
    CY_CHECK_LT(schedule_position(*plan, resolve), schedule_position(*plan, resolve + 1));
}

CY_TEST_CASE("a missing resolve is refused rather than drawn") {
    RenderGraph graph(allocator());
    const ResourceId colour = graph.create_texture(colour_target("colour"));
    graph.add_pass("draw", QueueKind::Graphics)
        .multisample(4)
        .write(colour, Access::ColorAttachmentWrite);
    declare_readback(graph, colour);
    CY_REQUIRE(graph.status().has_value());

    // Without the refusal this compiles, runs, and the readback returns whatever `colour` held —
    // black on a fresh transient, another pass's pixels on an aliased one.
    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE_FALSE(plan.has_value());
    CY_CHECK(message_contains(plan.error(), "no resolve has reached"));
}

CY_TEST_CASE("a resolve declared before the last multisampled write is refused") {
    RenderGraph graph(allocator());
    const ResourceId colour = graph.create_texture(colour_target("colour"));
    // Opaque resolves, then transparency keeps rendering into the SAME twin and resolves nothing:
    // the resolved colour would be missing the transparent pass.
    graph.add_pass("opaque", QueueKind::Graphics)
        .multisample(4)
        .write(colour, Access::ColorAttachmentWrite)
        .resolve(colour);
    graph.add_pass("transparent", QueueKind::Graphics)
        .multisample(4)
        .use(colour, Access::ColorAttachmentReadWrite);
    declare_readback(graph, colour);
    CY_REQUIRE(graph.status().has_value());
    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE_FALSE(plan.has_value());
    CY_CHECK(message_contains(plan.error(), "before the last multisampled write"));

    // The same frame with the resolve on the LAST multisampled pass compiles.
    RenderGraph fixed(allocator());
    const ResourceId fixed_colour = fixed.create_texture(colour_target("colour"));
    fixed.add_pass("opaque", QueueKind::Graphics)
        .multisample(4)
        .write(fixed_colour, Access::ColorAttachmentWrite);
    fixed.add_pass("transparent", QueueKind::Graphics)
        .multisample(4)
        .use(fixed_colour, Access::ColorAttachmentReadWrite)
        .resolve(fixed_colour);
    declare_readback(fixed, fixed_colour);
    CY_CHECK(fixed.compile(single_queue_options()).has_value());
}

CY_TEST_CASE("a resolve on a pass that renders no multisampled attachment of it is refused") {
    RenderGraph graph(allocator());
    const ResourceId colour = graph.create_texture(colour_target("colour"));
    graph.add_pass("single-sample draw", QueueKind::Graphics)
        .write(colour, Access::ColorAttachmentWrite)
        .resolve(colour);
    declare_readback(graph, colour);
    CY_REQUIRE_FALSE(graph.compile(single_queue_options()).has_value());
    CY_CHECK(message_contains(graph.status().error(), "renders no multisampled attachment"));
}

CY_TEST_CASE("1x is the graph it was: multisample(1) creates nothing and changes no decision") {
    // "1x is byte-identical to before" on the plan. The pixel half is `render.msaa_multiview`'s.
    const auto build = [](RenderGraph& graph, bool say_one) noexcept {
        const ResourceId colour = graph.create_texture(colour_target("colour"));
        const ResourceId depth = graph.create_texture(colour_target("depth"));
        cy::rendering::PassBuilder pass = graph.add_pass("draw", QueueKind::Graphics);
        if (say_one) {
            pass.multisample(1);
        }
        pass.write(colour, Access::ColorAttachmentWrite).write(depth, Access::ColorAttachmentWrite);
        declare_readback(graph, colour);
        return colour;
    };
    RenderGraph before(allocator());
    RenderGraph after(allocator());
    const ResourceId colour = build(before, false);
    (void)build(after, true);

    CY_CHECK_EQ(before.resource_count(), after.resource_count());
    CY_CHECK_EQ(before.pass_count(), after.pass_count());
    CY_CHECK_EQ(after.resource(colour).multisampled, kInvalidResource);
    cy::Expected<CompiledGraph, cy::Error> first = before.compile(single_queue_options());
    cy::Expected<CompiledGraph, cy::Error> second = after.compile(single_queue_options());
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first->plan_hash, second->plan_hash);
}

CY_TEST_CASE("a pass that cannot take MSAA refuses it with its own reason") {
    constexpr const char* kReason = "visibility payload: one surface a pixel";
    // Declared refusal first, then the request.
    RenderGraph graph(allocator());
    graph.add_pass("virtual geometry", QueueKind::Graphics).single_sample(kReason).multisample(4);
    CY_REQUIRE_FALSE(graph.status().has_value());
    CY_CHECK_EQ(graph.status().error().code, cy::ErrorCode::Unsupported);
    CY_CHECK(message_contains(graph.status().error(), kReason));

    // The other order names the same reason.
    RenderGraph reversed(allocator());
    reversed.add_pass("virtual geometry", QueueKind::Graphics)
        .multisample(4)
        .single_sample(kReason);
    CY_REQUIRE_FALSE(reversed.status().has_value());
    CY_CHECK(message_contains(reversed.status().error(), kReason));

    // And single-sample is no refusal at all.
    RenderGraph plain(allocator());
    plain.add_pass("virtual geometry", QueueKind::Graphics).single_sample(kReason).multisample(1);
    CY_CHECK(plain.status().has_value());
}

CY_TEST_CASE(
    "multisample() is refused after an attachment, at a count that is not 1/2/4/8, and "
    "on a depth resolve") {
    RenderGraph late(allocator());
    const ResourceId colour = late.create_texture(colour_target("colour"));
    late.add_pass("draw", QueueKind::Graphics)
        .write(colour, Access::ColorAttachmentWrite)
        .multisample(4);
    CY_CHECK_FALSE(late.status().has_value());

    RenderGraph odd(allocator());
    odd.add_pass("draw", QueueKind::Graphics).multisample(3);
    CY_CHECK_FALSE(odd.status().has_value());

    RenderGraph depth(allocator());
    cy::rendering::TextureRequest request = colour_target("depth");
    request.format = cy::rhi::Format::D32Sfloat;
    const ResourceId target = depth.create_texture(request);
    depth.add_pass("prepass", QueueKind::Graphics)
        .multisample(4)
        .write(target, Access::DepthStencilAttachmentWrite)
        .resolve(target);
    CY_REQUIRE_FALSE(depth.status().has_value());
    CY_CHECK(message_contains(depth.status().error(), "colour only"));
}

CY_TEST_CASE("a multi-view pass needs a layer for every view") {
    RenderGraph graph(allocator());
    const ResourceId single = graph.create_texture(colour_target("one layer"));
    graph.add_pass("stereo", QueueKind::Graphics)
        .views(2)
        .write(single, Access::ColorAttachmentWrite)
        .side_effect();
    cy::Expected<CompiledGraph, cy::Error> refused = graph.compile(single_queue_options());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(message_contains(refused.error(), "fewer layers than it has views"));

    RenderGraph layered(allocator());
    cy::rendering::TextureRequest request = colour_target("two layers");
    request.array_layers = 2;
    const ResourceId pair = layered.create_texture(request);
    const PassId stereo = layered.add_pass("stereo", QueueKind::Graphics)
                              .views(2)
                              .write(pair, Access::ColorAttachmentWrite)
                              .side_effect()
                              .id();
    CY_CHECK_EQ(layered.pass_view_count(stereo), 2U);
    CY_CHECK(layered.compile(single_queue_options()).has_value());

    RenderGraph none(allocator());
    none.add_pass("no views", QueueKind::Graphics).views(0);
    CY_CHECK_FALSE(none.status().has_value());
}
