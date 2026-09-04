// The shader library: deduplication, the hot-reload swap, the report, and the round trip.
// Tasks 3.4 and 3.7.

#include <cy/backends/shader/library.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include "fixtures/spirv_probe.h"

#include <utility>

using cy::u32;
using cy::u8;
using cy::usize;
using namespace cy::shader;

namespace {

CompiledShader compiled(cy::Span<const u32> words, const char* entry_point,
                        cy::rhi::ShaderStage stage) {
    cy::Array<u32> code(cy::current_allocator());
    CY_REQUIRE(code.append(words).has_value());
    DiagnosticLog diagnostics(cy::current_allocator());
    CompiledShader shader(cy::current_allocator());
    CY_REQUIRE(shader.adopt(std::move(code), cy::Name::intern(entry_point), stage, diagnostics)
                   .has_value());
    return shader;
}

cy::Span<const u32> vertex_words() {
    return {test::kProbeVertexSpirv, std::size(test::kProbeVertexSpirv)};
}
cy::Span<const u32> fragment_words() {
    return {test::kProbeFragmentSpirv, std::size(test::kProbeFragmentSpirv)};
}

VariantKey key_of(const char* module, const char* entry, cy::rhi::ShaderStage stage,
                  cy::u64 permutation) {
    VariantKey key;
    key.module_name = cy::Name::intern(module);
    key.entry_point = cy::Name::intern(entry);
    key.stage = stage;
    key.permutation = PermutationKey{permutation};
    return key;
}

}  // namespace

CY_TEST_CASE("two variants whose code is identical share one program") {
    // `shader-system`'s "Two materials share a pipeline": two materials differing only in parameter
    // values lower to identical source, and the content hash is what collapses them.
    ShaderLibrary library(cy::current_allocator());
    auto first = library.insert(key_of("material.brick", "main", cy::rhi::ShaderStage::Vertex, 0),
                                compiled(vertex_words(), "main", cy::rhi::ShaderStage::Vertex));
    auto second = library.insert(key_of("material.stone", "main", cy::rhi::ShaderStage::Vertex, 0),
                                 compiled(vertex_words(), "main", cy::rhi::ShaderStage::Vertex));
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    CY_CHECK(*first != *second);
    CY_CHECK(library.program_of(*first) == library.program_of(*second));
    CY_CHECK_EQ(library.variant_count(), usize{2});
    CY_CHECK_EQ(library.program_count(), usize{1});

    const LibraryReport report = library.report();
    CY_CHECK_EQ(report.variant_count, 2U);
    CY_CHECK_EQ(report.program_count, 1U);
    CY_CHECK_EQ(report.deduplicated, 1U);
}

CY_TEST_CASE("different code produces different programs, and the report names the biggest") {
    ShaderLibrary library(cy::current_allocator());
    auto vertex = library.insert(key_of("probe", "vertex", cy::rhi::ShaderStage::Vertex, 0),
                                 compiled(vertex_words(), "vertex", cy::rhi::ShaderStage::Vertex));
    auto fragment =
        library.insert(key_of("probe", "fragment", cy::rhi::ShaderStage::Fragment, 0),
                       compiled(fragment_words(), "fragment", cy::rhi::ShaderStage::Fragment));
    CY_REQUIRE(vertex.has_value());
    CY_REQUIRE(fragment.has_value());
    CY_CHECK_EQ(library.program_count(), usize{2});

    const LibraryReport report = library.report();
    CY_CHECK(report.max_instruction_count > 0);
    CY_CHECK(report.max_instruction_variant.is_valid());
    CY_CHECK(report.code_bytes > 0);
    // One vertex variant and one fragment variant, counted by the stage's bit position.
    CY_CHECK_EQ(report.stage_counts[0], 1U);
    CY_CHECK_EQ(report.stage_counts[1], 1U);
}

CY_TEST_CASE("replacing a variant keeps its id and moves its generation") {
    ShaderLibrary library(cy::current_allocator());
    const VariantKey key = key_of("probe", "main", cy::rhi::ShaderStage::Vertex, 0);
    auto id = library.insert(key, compiled(vertex_words(), "main", cy::rhi::ShaderStage::Vertex));
    CY_REQUIRE(id.has_value());
    CY_CHECK_EQ(library.generation(*id), 0U);

    // Everything downstream holds the id, so a successful rebuild is invisible to it — there is no
    // moment at which a consumer holds a handle to something being rebuilt.
    CY_REQUIRE(
        library.replace(*id, compiled(fragment_words(), "main", cy::rhi::ShaderStage::Fragment))
            .has_value());
    CY_CHECK_EQ(library.generation(*id), 1U);
    CY_CHECK(library.find(key) == *id);
    CY_REQUIRE(library.shader_at(*id) != nullptr);
    CY_CHECK_EQ(library.shader_at(*id)->spirv().size(), fragment_words().size());
}

CY_TEST_CASE("inserting the same key twice replaces rather than duplicates") {
    ShaderLibrary library(cy::current_allocator());
    const VariantKey key = key_of("probe", "main", cy::rhi::ShaderStage::Vertex, 0);
    auto first =
        library.insert(key, compiled(vertex_words(), "main", cy::rhi::ShaderStage::Vertex));
    auto second =
        library.insert(key, compiled(fragment_words(), "main", cy::rhi::ShaderStage::Fragment));
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK(*first == *second);
    CY_CHECK_EQ(library.variant_count(), usize{1});
}
