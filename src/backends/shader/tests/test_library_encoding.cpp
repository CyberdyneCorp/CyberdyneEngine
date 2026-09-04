// The shader library's own encoding: the round trip, and what a damaged artefact does. Task 3.4.
//
// Separate from test_library.cpp and classified as integration for one reason: cost. Each case
// builds several compiled shaders, and building one reflects a real SPIR-V module — at -O0 that is
// comfortably past the unit suite's millisecond. The taxonomy in `testing-and-quality` places a
// test this expensive in the next suite up, so that is where it is.

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

CY_TEST_CASE("a library round-trips through its own encoding") {
    ShaderLibrary library(cy::current_allocator());
    CY_REQUIRE(library
                   .insert(key_of("probe", "vertex", cy::rhi::ShaderStage::Vertex, 3),
                           compiled(vertex_words(), "vertex", cy::rhi::ShaderStage::Vertex))
                   .has_value());
    CY_REQUIRE(library
                   .insert(key_of("probe", "fragment", cy::rhi::ShaderStage::Fragment, 5),
                           compiled(fragment_words(), "fragment", cy::rhi::ShaderStage::Fragment))
                   .has_value());
    CY_REQUIRE(library
                   .insert(key_of("other", "vertex", cy::rhi::ShaderStage::Vertex, 0),
                           compiled(vertex_words(), "vertex", cy::rhi::ShaderStage::Vertex))
                   .has_value());

    cy::Array<u8> bytes(cy::current_allocator());
    CY_REQUIRE(library.serialize(bytes).has_value());
    CY_CHECK(bytes.size() > 0);

    auto parsed = ShaderLibrary::parse(cy::current_allocator(), {bytes.data(), bytes.size()});
    CY_REQUIRE(parsed.has_value());
    CY_CHECK_EQ(parsed->variant_count(), library.variant_count());
    // Deduplication survives the encoding: three variants, two programs, both sides.
    CY_CHECK_EQ(parsed->program_count(), library.program_count());

    const ShaderVariantId id =
        parsed->find(key_of("probe", "fragment", cy::rhi::ShaderStage::Fragment, 5));
    CY_REQUIRE(id.is_valid());
    const CompiledShader* shader = parsed->shader_at(id);
    CY_REQUIRE(shader != nullptr);
    CY_CHECK_EQ(shader->spirv().size(), fragment_words().size());
    // The reflection came out of the artefact, not out of a second parse of the SPIR-V.
    CY_CHECK_EQ(
        shader->reflection().bindings().size(),
        library
            .shader_at(library.find(key_of("probe", "fragment", cy::rhi::ShaderStage::Fragment, 5)))
            ->reflection()
            .bindings()
            .size());
    CY_CHECK(shader->hash() == library.shader_at(id)->hash());

    // Serialising twice produces the same bytes, which is what lets a library be content-addressed.
    cy::Array<u8> again(cy::current_allocator());
    CY_REQUIRE(library.serialize(again).has_value());
    CY_REQUIRE_EQ(again.size(), bytes.size());
    for (usize index = 0; index < bytes.size(); ++index) {
        CY_REQUIRE_EQ(again[index], bytes[index]);
    }
}

CY_TEST_CASE("a truncated or foreign library is refused") {
    cy::Array<u8> rubbish(cy::current_allocator());
    CY_REQUIRE(rubbish.resize(64).has_value());
    CY_CHECK_FALSE(ShaderLibrary::parse(cy::current_allocator(), {rubbish.data(), rubbish.size()})
                       .has_value());

    ShaderLibrary library(cy::current_allocator());
    CY_REQUIRE(library
                   .insert(key_of("probe", "vertex", cy::rhi::ShaderStage::Vertex, 0),
                           compiled(vertex_words(), "vertex", cy::rhi::ShaderStage::Vertex))
                   .has_value());
    cy::Array<u8> bytes(cy::current_allocator());
    CY_REQUIRE(library.serialize(bytes).has_value());
    CY_CHECK_FALSE(ShaderLibrary::parse(cy::current_allocator(), {bytes.data(), bytes.size() / 2})
                       .has_value());
}
