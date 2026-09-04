// Global shader parameters. Task 3.6.
//
// The requirement is about what does *not* happen when one is set: no material is touched and no
// pipeline is invalidated. So the cases here are about the block's layout, its revision, and its
// layout hash — the three things a frame actually consults.

#include <cy/backends/shader/globals.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <cstring>
#include <string_view>

using cy::f32;
using cy::u32;
using cy::usize;
using namespace cy::shader;

CY_TEST_CASE("offsets follow the std140 rules the shading languages agree on") {
    GlobalParameterBlock block(cy::current_allocator());
    auto time = block.declare(cy::Name::intern("time"), GlobalType::Float);
    auto wind = block.declare(cy::Name::intern("wind"), GlobalType::Vec3);
    auto strength = block.declare(cy::Name::intern("strength"), GlobalType::Float);
    auto sun = block.declare(cy::Name::intern("sun"), GlobalType::Vec4);
    CY_REQUIRE(time.has_value());
    CY_REQUIRE(wind.has_value());
    CY_REQUIRE(strength.has_value());
    CY_REQUIRE(sun.has_value());

    // float at 0; a three-component vector aligns as a four-component one, so 16; the next float
    // fills the gap the vec3 left at 28; the vec4 realigns to 32. A block laid out by naive packing
    // would put the vec3 at 4 and the shader would read garbage.
    CY_CHECK_EQ(block.parameter_at(*time)->offset, 0U);
    CY_CHECK_EQ(block.parameter_at(*wind)->offset, 16U);
    CY_CHECK_EQ(block.parameter_at(*strength)->offset, 28U);
    CY_CHECK_EQ(block.parameter_at(*sun)->offset, 32U);
    CY_CHECK_EQ(block.byte_size(), 48U);
}

CY_TEST_CASE("setting a parameter writes its bytes and moves the revision") {
    GlobalParameterBlock block(cy::current_allocator());
    auto exposure = block.declare(cy::Name::intern("exposure"), GlobalType::Float);
    auto tint = block.declare(cy::Name::intern("tint"), GlobalType::Vec4);
    CY_REQUIRE(exposure.has_value());
    CY_REQUIRE(tint.has_value());

    const cy::u64 before = block.revision();
    CY_REQUIRE(block.set_float(*exposure, 2.5F).has_value());
    const f32 components[] = {0.25F, 0.5F, 0.75F, 1.0F};
    CY_REQUIRE(block.set_vec(*tint, {components, 4}).has_value());
    // One comparison per frame decides whether to upload; there is no dirty flag per parameter and
    // nothing per material.
    CY_CHECK(block.revision() > before);

    f32 read = 0.0F;
    std::memcpy(&read, block.data().data() + block.parameter_at(*exposure)->offset, sizeof(read));
    CY_CHECK_EQ(read, 2.5F);
}

CY_TEST_CASE("a parameter set through the wrong type is refused") {
    GlobalParameterBlock block(cy::current_allocator());
    auto count = block.declare(cy::Name::intern("count"), GlobalType::UInt);
    CY_REQUIRE(count.has_value());

    CY_CHECK_FALSE(block.set_float(*count, 1.0F).has_value());
    CY_REQUIRE(block.set_uint(*count, 7U).has_value());

    // A vector set with the wrong component count is the same mistake wearing a different hat.
    auto direction = block.declare(cy::Name::intern("direction"), GlobalType::Vec3);
    CY_REQUIRE(direction.has_value());
    const f32 two[] = {1.0F, 2.0F};
    CY_CHECK_FALSE(block.set_vec(*direction, {two, 2}).has_value());

    CY_CHECK_FALSE(block.set_float(GlobalParameterId{}, 1.0F).has_value());
}

CY_TEST_CASE("a frozen block refuses further declarations") {
    GlobalParameterBlock block(cy::current_allocator());
    CY_REQUIRE(block.declare(cy::Name::intern("time"), GlobalType::Float).has_value());
    block.freeze();
    CY_CHECK(block.frozen());
    // Declaring after a shader has been compiled against the block would change its layout hash,
    // and a mismatch there is a shader reading a differently laid out buffer.
    CY_CHECK_FALSE(block.declare(cy::Name::intern("late"), GlobalType::Float).has_value());
}

CY_TEST_CASE("the layout hash covers the declarations and not the values") {
    GlobalParameterBlock first(cy::current_allocator());
    auto time = first.declare(cy::Name::intern("time"), GlobalType::Float);
    CY_REQUIRE(time.has_value());
    const auto before = first.layout_hash();
    CY_REQUIRE(first.set_float(*time, 12.0F).has_value());
    CY_CHECK(first.layout_hash() == before);

    GlobalParameterBlock second(cy::current_allocator());
    CY_REQUIRE(second.declare(cy::Name::intern("time"), GlobalType::Float).has_value());
    CY_CHECK(second.layout_hash() == before);

    GlobalParameterBlock third(cy::current_allocator());
    CY_REQUIRE(third.declare(cy::Name::intern("time"), GlobalType::Vec4).has_value());
    CY_CHECK(third.layout_hash() != before);
}

CY_TEST_CASE("the block emits its own Slang declaration") {
    GlobalParameterBlock block(cy::current_allocator());
    CY_REQUIRE(block.declare(cy::Name::intern("timeSeconds"), GlobalType::Float).has_value());
    CY_REQUIRE(block.declare(cy::Name::intern("windDirection"), GlobalType::Vec3).has_value());

    cy::Array<char> source(cy::current_allocator());
    CY_REQUIRE(block.emit_slang_declaration(source).has_value());
    const std::string_view text(source.data(), source.size());
    // It is generated Slang like any other generated Slang and goes through the same registry.
    CY_CHECK(text.find("cbuffer CyGlobals") != std::string_view::npos);
    CY_CHECK(text.find("float timeSeconds;") != std::string_view::npos);
    CY_CHECK(text.find("float3 windDirection;") != std::string_view::npos);
    CY_CHECK(text.find("vk::binding(0, 0)") != std::string_view::npos);
}
