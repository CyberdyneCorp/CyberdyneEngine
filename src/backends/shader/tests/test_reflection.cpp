// Reflection-driven binding, over real compiled SPIR-V. Task 3.3.
//
// The fixtures are the three entry points of fixtures/probe.slang, compiled by Slang and checked in
// (fixtures/spirv_probe.h says how to regenerate them). Real modules rather than hand-assembled
// words, because the thing under test is whether the parser reads what a *compiler* emits — and a
// hand-written module would only prove that it reads what this test's author expected.

#include <cy/backends/shader/reflection.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include "fixtures/spirv_probe.h"

using cy::u32;
using cy::usize;
using namespace cy::shader;

namespace {

Reflection reflect(cy::Span<const u32> words) {
    auto reflection = reflect_spirv(cy::current_allocator(), words);
    CY_REQUIRE(reflection.has_value());
    return std::move(reflection.value());
}

const ReflectedBinding* binding_at(const Reflection& reflection, u32 set, u32 binding) {
    for (const ReflectedBinding& entry : reflection.bindings()) {
        if (entry.set == set && entry.binding == binding) {
            return &entry;
        }
    }
    return nullptr;
}

cy::Span<const u32> probe_vertex() {
    return {test::kProbeVertexSpirv, std::size(test::kProbeVertexSpirv)};
}
cy::Span<const u32> probe_fragment() {
    return {test::kProbeFragmentSpirv, std::size(test::kProbeFragmentSpirv)};
}
cy::Span<const u32> probe_compute() {
    return {test::kProbeComputeSpirv, std::size(test::kProbeComputeSpirv)};
}

}  // namespace

CY_TEST_CASE("a buffer that is not SPIR-V is refused rather than misread") {
    const u32 rubbish[] = {0xDEADBEEFU, 0, 0, 0, 0, 0};
    auto reflection = reflect_spirv(cy::current_allocator(), {rubbish, std::size(rubbish)});
    CY_CHECK_FALSE(reflection.has_value());

    // A module whose header is right and whose instruction stream is cut short is the other way a
    // buffer gets this far, and it is the one a truncated cache entry produces.
    const u32 truncated[] = {0x07230203U, 0x00010500U, 0, 16, 0, 0x000A000FU};
    auto cut = reflect_spirv(cy::current_allocator(), {truncated, std::size(truncated)});
    CY_CHECK_FALSE(cut.has_value());
}

CY_TEST_CASE("the vertex module's inputs come out at the locations the shader declared") {
    const Reflection reflection = reflect(probe_vertex());

    CY_REQUIRE_EQ(reflection.entry_points().size(), usize{1});
    CY_CHECK(reflection.entry_points()[0].stage == cy::rhi::ShaderStage::Vertex);
    CY_CHECK(reflection.stages() == cy::rhi::ShaderStage::Vertex);
    CY_CHECK(reflection.instruction_count() > 0);

    // POSITION, NORMAL, TEXCOORD0 — three inputs, in ascending location order, with the formats the
    // types imply. Nothing in C++ declared these; they were read out of the binary.
    CY_REQUIRE_EQ(reflection.vertex_inputs().size(), usize{3});
    CY_CHECK_EQ(reflection.vertex_inputs()[0].location, 0U);
    CY_CHECK(reflection.vertex_inputs()[0].format == cy::rhi::Format::Rgb32Sfloat);
    CY_CHECK_EQ(reflection.vertex_inputs()[1].location, 1U);
    CY_CHECK(reflection.vertex_inputs()[1].format == cy::rhi::Format::Rgb32Sfloat);
    CY_CHECK_EQ(reflection.vertex_inputs()[2].location, 2U);
    CY_CHECK(reflection.vertex_inputs()[2].format == cy::rhi::Format::Rg32Sfloat);
}

CY_TEST_CASE("descriptor kinds are derived from the SPIR-V type, not from a table in C++") {
    const Reflection reflection = reflect(probe_fragment());

    const ReflectedBinding* frame = binding_at(reflection, kSetGlobal, 0);
    CY_REQUIRE(frame != nullptr);
    CY_CHECK(frame->kind == cy::rhi::DescriptorKind::UniformBuffer);

    // The storage buffer is read from the compute module: Slang emits only the globals an entry
    // point actually uses, and probeFragment does not touch `instanceData`. That is worth knowing
    // rather than working around — a reflection is of a *module*, not of the source file, and a
    // pipeline layout built from the wrong stage's module would be missing a binding.
    const Reflection compute = reflect(probe_compute());
    const ReflectedBinding* instances = binding_at(compute, kSetGlobal, 1);
    CY_REQUIRE(instances != nullptr);
    CY_CHECK(instances->kind == cy::rhi::DescriptorKind::StorageBuffer);

    // The bindless table: a runtime-sized array, which is `count == 0` and `runtime_array`.
    const ReflectedBinding* bindless = binding_at(reflection, kSetGlobal, 2);
    CY_REQUIRE(bindless != nullptr);
    CY_CHECK(bindless->runtime_array);
    CY_CHECK_EQ(bindless->count, 0U);
    CY_CHECK(bindless->kind == cy::rhi::DescriptorKind::SampledTexture);

    const ReflectedBinding* color = binding_at(reflection, kSetPass, 0);
    CY_REQUIRE(color != nullptr);
    CY_CHECK(color->kind == cy::rhi::DescriptorKind::SampledTexture);

    const ReflectedBinding* sampler = binding_at(reflection, kSetPass, 1);
    CY_REQUIRE(sampler != nullptr);
    CY_CHECK(sampler->kind == cy::rhi::DescriptorKind::Sampler);

    // Every binding is stamped with the stage that declared it, which is what a pipeline layout
    // needs and what a hand-maintained table gets wrong first.
    CY_CHECK(frame->stages == cy::rhi::ShaderStage::Fragment);
}

CY_TEST_CASE("the push-constant range and the specialization constant are read out") {
    const Reflection reflection = reflect(probe_fragment());

    CY_REQUIRE_EQ(reflection.push_constants().size(), usize{1});
    // float4 tint plus two uints: twenty-four bytes, laid out by the compiler and not by anybody's
    // arithmetic here.
    CY_CHECK_EQ(reflection.push_constants()[0].offset, 0U);
    CY_CHECK_EQ(reflection.push_constants()[0].size, 24U);
    CY_CHECK(reflection.push_constants()[0].stages == cy::rhi::ShaderStage::Fragment);

    CY_REQUIRE_EQ(reflection.spec_constants().size(), usize{1});
    CY_CHECK_EQ(reflection.spec_constants()[0].default_value, 4U);
}

CY_TEST_CASE("a compute module reports its workgroup size") {
    const Reflection reflection = reflect(probe_compute());

    CY_REQUIRE_EQ(reflection.entry_points().size(), usize{1});
    const ReflectedEntryPoint& entry = reflection.entry_points()[0];
    CY_CHECK(entry.stage == cy::rhi::ShaderStage::Compute);
    // [numthreads(8, 4, 2)]. A dispatch that guessed this would be wrong the day the shader
    // changed.
    CY_CHECK_EQ(entry.workgroup_size[0], 8U);
    CY_CHECK_EQ(entry.workgroup_size[1], 4U);
    CY_CHECK_EQ(entry.workgroup_size[2], 2U);

    const ReflectedBinding* storage = binding_at(reflection, kSetPass, 2);
    CY_REQUIRE(storage != nullptr);
    CY_CHECK(storage->kind == cy::rhi::DescriptorKind::StorageTexture);
}

CY_TEST_CASE("merging two modules unions their stage masks and keeps one binding") {
    Reflection vertex = reflect(probe_vertex());
    const Reflection compute = reflect(probe_compute());

    DiagnosticLog diagnostics(cy::current_allocator());
    CY_REQUIRE(vertex.merge(compute, diagnostics).has_value());
    CY_CHECK_FALSE(diagnostics.has_errors());

    // Both modules name set 0 binding 1; the merged reflection has one binding for it, visible to
    // both stages. That is the pipeline layout, and there is no second declaration to disagree
    // with it.
    const ReflectedBinding* instances = binding_at(vertex, kSetGlobal, 1);
    CY_REQUIRE(instances != nullptr);
    CY_CHECK(cy::rhi::has_stage(instances->stages, cy::rhi::ShaderStage::Vertex));
    CY_CHECK(cy::rhi::has_stage(instances->stages, cy::rhi::ShaderStage::Compute));
    // The view constants came only from the vertex module and are still there.
    CY_REQUIRE(binding_at(vertex, kSetView, 0) != nullptr);
}

CY_TEST_CASE("two stages that disagree about a binding are a reported error, not a merge") {
    Reflection first(cy::current_allocator());
    Reflection second(cy::current_allocator());

    ReflectedBinding uniform;
    uniform.set = kSetPass;
    uniform.binding = 0;
    uniform.kind = cy::rhi::DescriptorKind::UniformBuffer;
    uniform.stages = cy::rhi::ShaderStage::Vertex;
    CY_REQUIRE(first.add_binding(uniform).has_value());

    ReflectedBinding storage = uniform;
    storage.kind = cy::rhi::DescriptorKind::StorageBuffer;
    storage.stages = cy::rhi::ShaderStage::Fragment;
    CY_REQUIRE(second.add_binding(storage).has_value());

    DiagnosticLog diagnostics(cy::current_allocator());
    // A pipeline created from these two would be created against a layout one of its stages does
    // not accept, which is a device error at draw time and a diagnostic here.
    CY_CHECK_FALSE(first.merge(second, diagnostics).has_value());
    CY_CHECK(diagnostics.has_errors());
}

CY_TEST_CASE("the derived descriptor set layout is the reflection, in the RHI's shape") {
    const Reflection reflection = reflect(probe_fragment());

    cy::Array<cy::rhi::DescriptorBinding> global(cy::current_allocator());
    CY_REQUIRE(reflection.set_layout(kSetGlobal, global).has_value());
    // The fragment module names two of set 0's bindings: the frame constants and the bindless
    // table. Ascending binding order, derived from the module.
    CY_REQUIRE_EQ(global.size(), usize{2});
    CY_CHECK_EQ(global[0].binding, 0U);
    CY_CHECK_EQ(global[1].binding, 2U);
    // A runtime-sized array is the bindless table, and a bindless table is the case that needs
    // partial binding — derived, not declared.
    CY_CHECK(global[1].partially_bound);
    CY_CHECK_EQ(global[1].count, 0U);

    cy::Array<cy::rhi::PushConstantRange> ranges(cy::current_allocator());
    CY_REQUIRE(reflection.push_constant_ranges(ranges).has_value());
    CY_REQUIRE_EQ(ranges.size(), usize{1});
    CY_CHECK_EQ(ranges[0].size, 24U);

    CY_CHECK(reflection.uses_set(kSetGlobal));
    CY_CHECK(reflection.uses_set(kSetPass));
    CY_CHECK_FALSE(reflection.uses_set(kSetDraw));
}

CY_TEST_CASE("the descriptor set convention is checked, and names itself when it is broken") {
    const Reflection good = reflect(probe_fragment());
    DiagnosticLog clean(cy::current_allocator());
    CY_CHECK(validate_set_convention(good, clean, "probe"));
    CY_CHECK_FALSE(clean.has_errors());

    Reflection bad(cy::current_allocator());
    ReflectedBinding outside;
    outside.set = 5;  // there is no set 5
    outside.binding = 0;
    CY_REQUIRE(bad.add_binding(outside).has_value());

    ReflectedBinding misplaced_table;
    misplaced_table.set = kSetDraw;
    misplaced_table.binding = 0;
    misplaced_table.count = 0;
    misplaced_table.runtime_array = true;
    CY_REQUIRE(bad.add_binding(misplaced_table).has_value());

    DiagnosticLog diagnostics(cy::current_allocator());
    CY_CHECK_FALSE(validate_set_convention(bad, diagnostics, "probe"));
    CY_CHECK_EQ(diagnostics.error_count(), 2U);
}

CY_TEST_CASE("reflecting the same module twice produces the same answer") {
    // Ordering is by (set, binding) and by location, never by the order ids happened to appear in
    // the binary — a reflection that depended on the emitter's internal ordering would change when
    // the compiler was updated and take every derived cache key with it.
    const Reflection first = reflect(probe_fragment());
    const Reflection second = reflect(probe_fragment());

    CY_REQUIRE_EQ(first.bindings().size(), second.bindings().size());
    for (usize index = 0; index < first.bindings().size(); ++index) {
        CY_CHECK_EQ(first.bindings()[index].set, second.bindings()[index].set);
        CY_CHECK_EQ(first.bindings()[index].binding, second.bindings()[index].binding);
        CY_CHECK(first.bindings()[index].kind == second.bindings()[index].kind);
    }
}
