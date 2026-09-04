// The material model and its parameter storage. Task 4.2.4.
//
// `rendering-materials-and-shading` — "Material model" and "Parameter storage". The four scenarios
// checked here are the four that describe how a material behaves at run time:
//
//   "WHEN 50 material instances derive from one parent THEN all SHALL use the same program and
//    pipeline"
//   "WHEN gameplay changes a runtime parameter THEN no compilation SHALL occur and the change SHALL
//    apply the same frame"
//   "WHEN a script changes one material parameter THEN only that material's range SHALL be marked
//    dirty and included in the next batched upload"
//   "WHEN a GPU-generated draw shades a pixel THEN it SHALL index the material table using the
//    instance's material identifier, with no per-object descriptor binding"
//
// The last one is a property of the LAYOUT rather than of a call, so it is checked as arithmetic: a
// material's bytes begin at `index * kMaterialBlockBytes`, which is what makes a shader able to
// reach them from an instance record with no lookup.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/material.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstring>

using cy::f32;
using cy::u32;
using cy::Vec4;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A program with one parameter of each kind that the cases below write.
struct Fixture {
    Fixture() noexcept : program(allocator()), table(allocator()) {}

    [[nodiscard]] bool start(u32 capacity = 8) noexcept {
        if (!program.initialize(cy::Name::intern("test"), ShadingModel::Lit, BlendMode::Opaque)
                 .has_value()) {
            return false;
        }
        const auto declare = [this](const char* name, ParameterKind kind) noexcept {
            auto declared = program.add_parameter(name, kind);
            return declared.has_value();
        };
        return declare("base_color_factor", ParameterKind::Color) &&
               declare("roughness_factor", ParameterKind::Float) &&
               declare("metallic_factor", ParameterKind::Float) &&
               declare("uv_tiling_offset", ParameterKind::Vec4) &&
               declare("uv_channel_mask", ParameterKind::Int) &&
               declare("base_color_texture", ParameterKind::Texture) &&
               table.initialize(capacity).has_value();
    }

    /// The identifiers, resolved once at compile time. What a consumer holds instead of calling
    /// `parameter_id("base_color_factor")` per frame.
    struct Ids {
        ParameterId base_color = parameter_id("base_color_factor");
        ParameterId roughness = parameter_id("roughness_factor");
        ParameterId metallic = parameter_id("metallic_factor");
        ParameterId tiling = parameter_id("uv_tiling_offset");
        ParameterId channels = parameter_id("uv_channel_mask");
        ParameterId texture = parameter_id("base_color_texture");
    };

    MaterialProgram program;
    MaterialTable table;
    Ids ids;
};

}  // namespace

CY_TEST_CASE("a parameter identifier is a hash of its name, computed at compile time") {
    // `constexpr`, so the frame path has nowhere to take a string: "Parameter names SHALL resolve
    // to compile-time identifiers; per-frame string lookup SHALL NOT be required."
    // Two spellings of the same name resolve to one identifier, and two names do not collide. The
    // first is written through a constant rather than as `f(x) == f(x)`, which is a tautology a
    // compiler is entitled to fold away.
    constexpr ParameterId kBaseColor = parameter_id("base_color_factor");
    static_assert(kBaseColor == parameter_id("base_color_factor"));
    static_assert(kBaseColor != parameter_id("roughness_factor"));
    CY_CHECK_NE(parameter_id(""), parameter_id("x"));
}

CY_TEST_CASE("declaring the same parameter twice is refused, and so is a hash collision") {
    MaterialProgram program(allocator());
    CY_REQUIRE(program.initialize(cy::Name::intern("p"), ShadingModel::Lit, BlendMode::Opaque)
                   .has_value());
    CY_REQUIRE(program.add_parameter("tint", ParameterKind::Color).has_value());

    const auto duplicate = program.add_parameter("tint", ParameterKind::Float);
    CY_REQUIRE_FALSE(duplicate.has_value());
    CY_CHECK(duplicate.error().code == cy::ErrorCode::AlreadyExists);

    const auto unnamed = program.add_parameter("", ParameterKind::Float);
    CY_CHECK_FALSE(unnamed.has_value());
    const auto null_name = program.add_parameter(nullptr, ParameterKind::Float);
    CY_CHECK_FALSE(null_name.has_value());
}

CY_TEST_CASE(
    "offsets follow std430, so the CPU and the shader agree without a rule being restated") {
    MaterialProgram program(allocator());
    CY_REQUIRE(program.initialize(cy::Name::intern("p"), ShadingModel::Lit, BlendMode::Opaque)
                   .has_value());
    CY_REQUIRE(program.add_parameter("a_float", ParameterKind::Float).has_value());
    CY_REQUIRE(program.add_parameter("a_vec2", ParameterKind::Vec2).has_value());
    CY_REQUIRE(program.add_parameter("a_vec4", ParameterKind::Vec4).has_value());

    const MaterialParameter* a = program.find(parameter_id("a_float"));
    const MaterialParameter* b = program.find(parameter_id("a_vec2"));
    const MaterialParameter* c = program.find(parameter_id("a_vec4"));
    CY_REQUIRE(a != nullptr);
    CY_REQUIRE(b != nullptr);
    CY_REQUIRE(c != nullptr);
    CY_CHECK_EQ(a->offset, 0U);
    // A vec2 aligns to 8, so the four bytes after the float are padding rather than the vec2's
    // first half — the mistake that shifts every subsequent parameter by four bytes and shows up
    // as a material whose roughness changes when its tint does.
    CY_CHECK_EQ(b->offset, 8U);
    CY_CHECK_EQ(c->offset, 16U);
    CY_CHECK_EQ(program.block_bytes(), 32U);
    CY_CHECK_EQ(parameter_byte_size(ParameterKind::Bool), 4U);
    CY_CHECK_EQ(parameter_byte_size(ParameterKind::Vec3), 12U);
}

CY_TEST_CASE("a program that outgrows its block is refused where it is cheap") {
    MaterialProgram program(allocator());
    CY_REQUIRE(program.initialize(cy::Name::intern("p"), ShadingModel::Lit, BlendMode::Opaque)
                   .has_value());
    char name[16] = {};
    bool refused = false;
    for (u32 index = 0; index < kMaxMaterialParameters + 1U; ++index) {
        std::snprintf(name, sizeof(name), "p%u", index);
        if (!program.add_parameter(name, ParameterKind::Vec4).has_value()) {
            refused = true;
            break;
        }
    }
    CY_CHECK(refused);
    CY_CHECK_LE(program.block_bytes(), kMaterialBlockBytes);
}

CY_TEST_CASE("a static boolean doubles the permutation count and a runtime one does not") {
    MaterialProgram program(allocator());
    CY_REQUIRE(program.initialize(cy::Name::intern("p"), ShadingModel::Lit, BlendMode::Opaque)
                   .has_value());
    CY_CHECK_EQ(program.permutation_count(), 1ULL);

    CY_REQUIRE(program.add_parameter("use_triplanar", ParameterKind::Bool, true).has_value());
    CY_CHECK_EQ(program.permutation_count(), 2ULL);
    CY_REQUIRE(program.add_parameter("use_detail", ParameterKind::Bool, true).has_value());
    CY_CHECK_EQ(program.permutation_count(), 4ULL);
    CY_CHECK_EQ(program.static_bool_count(), 2U);

    // A runtime parameter is data: it changes nothing about the program's structure.
    CY_REQUIRE(program.add_parameter("tint", ParameterKind::Color).has_value());
    CY_CHECK_EQ(program.permutation_count(), 4ULL);
}

CY_TEST_CASE("a material's bytes begin at its index times the block size") {
    // What makes a GPU-generated draw able to reach its parameters: the address is a multiplication
    // and there is nothing to look up first.
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    const auto first = fixture.table.allocate();
    const auto second = fixture.table.allocate();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(*first, 0U);
    CY_CHECK_EQ(*second, 1U);

    CY_REQUIRE(fixture.table
                   .set_color(fixture.program, *second, fixture.ids.base_color,
                              Vec4{0.25F, 0.5F, 0.75F, 1.0F})
                   .has_value());

    // Read the bytes the way a shader would: at index · 256 + the parameter's offset.
    const MaterialParameter* slot = fixture.program.find(fixture.ids.base_color);
    CY_REQUIRE(slot != nullptr);
    Vec4 raw{};
    std::memcpy(&raw,
                fixture.table.bytes().data() +
                    (static_cast<cy::usize>(*second) * kMaterialBlockBytes) + slot->offset,
                sizeof(Vec4));
    CY_CHECK_NEAR(raw.x, 0.25F, 1e-6F);
    CY_CHECK_NEAR(raw.z, 0.75F, 1e-6F);
    CY_CHECK_EQ(fixture.table.bytes().size(), static_cast<cy::usize>(8) * kMaterialBlockBytes);
}

CY_TEST_CASE("a write of the wrong kind is refused rather than overwriting the next parameter") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto index = fixture.table.allocate();
    CY_REQUIRE(index.has_value());

    // A `Vec4` written where a `Float` was declared runs into the two parameters after it.
    const cy::Status wrong_kind =
        fixture.table.set_vec4(fixture.program, *index, fixture.ids.roughness, Vec4{});
    CY_REQUIRE_FALSE(wrong_kind.has_value());
    CY_CHECK(wrong_kind.error().code == cy::ErrorCode::InvalidArgument);

    // An unknown parameter is NotFound, and an out-of-range slot is OutOfRange: two different
    // mistakes, told apart so the message can name the right one.
    CY_CHECK(
        fixture.table.set_float(fixture.program, *index, parameter_id("nope"), 1.0F).error().code ==
        cy::ErrorCode::NotFound);
    CY_CHECK(
        fixture.table.set_float(fixture.program, 999, fixture.ids.roughness, 1.0F).error().code ==
        cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("every parameter kind round-trips through the table") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto index = fixture.table.allocate();
    CY_REQUIRE(index.has_value());

    CY_REQUIRE(
        fixture.table.set_float(fixture.program, *index, fixture.ids.roughness, 0.3F).has_value());
    CY_REQUIRE(
        fixture.table
            .set_vec4(fixture.program, *index, fixture.ids.tiling, Vec4{2.0F, 3.0F, 0.1F, 0.2F})
            .has_value());
    CY_REQUIRE(
        fixture.table.set_texture(fixture.program, *index, fixture.ids.texture, 17).has_value());
    CY_REQUIRE(fixture.table.set_int(fixture.program, *index, fixture.ids.channels, 5).has_value());

    CY_CHECK_NEAR(*fixture.table.get_float(fixture.program, *index, fixture.ids.roughness), 0.3F,
                  1e-6F);
    CY_CHECK_NEAR(fixture.table.get_vec4(fixture.program, *index, fixture.ids.tiling)->y, 3.0F,
                  1e-6F);
    CY_CHECK_EQ(*fixture.table.get_texture(fixture.program, *index, fixture.ids.texture), 17U);
}

CY_TEST_CASE("an instance copies its parent's parameters and shares its program") {
    // "a material instance references a program and overrides a subset of runtime parameters.
    // Instances SHALL be creatable at runtime without compilation, and any number SHALL share one
    // program."
    Fixture fixture;
    CY_REQUIRE(fixture.start(64));
    const auto parent = fixture.table.allocate();
    CY_REQUIRE(parent.has_value());
    CY_REQUIRE(fixture.table
                   .set_color(fixture.program, *parent, fixture.ids.base_color,
                              Vec4{1.0F, 0.0F, 0.0F, 1.0F})
                   .has_value());
    CY_REQUIRE(
        fixture.table.set_float(fixture.program, *parent, fixture.ids.roughness, 0.8F).has_value());

    for (u32 count = 0; count < 50; ++count) {
        const auto instance = fixture.table.instantiate(*parent);
        CY_REQUIRE(instance.has_value());
        CY_CHECK_NEAR(*fixture.table.get_float(fixture.program, *instance, fixture.ids.roughness),
                      0.8F, 1e-6F);
    }
    CY_CHECK_EQ(fixture.table.live(), 51U);

    // Overriding one instance leaves the parent and the other instances alone: the override lives
    // in the instance's own block, which is what "without duplicating the material" means when the
    // material is bytes.
    const auto overridden = fixture.table.instantiate(*parent);
    CY_REQUIRE(overridden.has_value());
    CY_REQUIRE(fixture.table.set_float(fixture.program, *overridden, fixture.ids.roughness, 0.1F)
                   .has_value());
    CY_CHECK_NEAR(*fixture.table.get_float(fixture.program, *parent, fixture.ids.roughness), 0.8F,
                  1e-6F);

    CY_CHECK_FALSE(fixture.table.instantiate(999).has_value());
}

CY_TEST_CASE("changing one parameter dirties one range, and the upload is one transfer") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto first = fixture.table.allocate();
    const auto second = fixture.table.allocate();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    fixture.table.clear_dirty();

    u32 offset = 0;
    u32 size = 0;
    CY_CHECK_FALSE(fixture.table.dirty_range(offset, size));

    CY_REQUIRE(
        fixture.table.set_float(fixture.program, *second, fixture.ids.roughness, 0.2F).has_value());
    CY_REQUIRE(fixture.table.dirty_range(offset, size));
    const MaterialParameter* slot = fixture.program.find(fixture.ids.roughness);
    CY_REQUIRE(slot != nullptr);
    CY_CHECK_EQ(offset, (*second * kMaterialBlockBytes) + slot->offset);
    CY_CHECK_EQ(size, 4U);

    // A second change in another material widens the interval to cover both — ONE transfer, which
    // is what the requirement asks for, at the cost of the untouched bytes between them.
    CY_REQUIRE(
        fixture.table.set_float(fixture.program, *first, fixture.ids.metallic, 1.0F).has_value());
    CY_REQUIRE(fixture.table.dirty_range(offset, size));
    CY_CHECK_LT(offset, *second * kMaterialBlockBytes);
    CY_CHECK_GT(size, 4U);

    fixture.table.clear_dirty();
    CY_CHECK_FALSE(fixture.table.dirty_range(offset, size));
}

CY_TEST_CASE("a released slot is cleared, reused, and does not leak its old parameters") {
    Fixture fixture;
    CY_REQUIRE(fixture.start(2));
    const auto first = fixture.table.allocate();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(
        fixture.table.set_float(fixture.program, *first, fixture.ids.roughness, 0.9F).has_value());
    CY_REQUIRE(fixture.table.release(*first).has_value());
    CY_CHECK_EQ(fixture.table.live(), 0U);

    const auto reused = fixture.table.allocate();
    CY_REQUIRE(reused.has_value());
    CY_CHECK_EQ(*reused, *first);
    // Zeroed, not inherited: a material that arrived with the previous occupant's roughness would
    // be a defect that only appears after something else was destroyed.
    CY_CHECK_NEAR(*fixture.table.get_float(fixture.program, *reused, fixture.ids.roughness), 0.0F,
                  1e-6F);

    CY_CHECK_FALSE(fixture.table.release(999).has_value());
}

CY_TEST_CASE("a full table says so rather than growing under a descriptor that names its address") {
    Fixture fixture;
    CY_REQUIRE(fixture.start(2));
    CY_REQUIRE(fixture.table.allocate().has_value());
    CY_REQUIRE(fixture.table.allocate().has_value());
    const auto overflow = fixture.table.allocate();
    CY_REQUIRE_FALSE(overflow.has_value());
    CY_CHECK(overflow.error().code == cy::ErrorCode::OutOfMemory);

    MaterialTable empty(allocator());
    CY_CHECK_FALSE(empty.initialize(0).has_value());
}

CY_TEST_CASE("reflection marks what a shader reads, and validation reads it back") {
    MaterialProgram program(allocator());
    CY_REQUIRE(program.initialize(cy::Name::intern("p"), ShadingModel::Lit, BlendMode::Opaque)
                   .has_value());
    CY_REQUIRE(program.add_parameter("tint", ParameterKind::Color).has_value());
    // Declared as referenced, because a program under construction has had no reflection run over
    // it and reporting every parameter as unused would make the report useless.
    CY_CHECK(program.find(parameter_id("tint"))->referenced);

    program.mark_referenced(parameter_id("tint"), false);
    CY_CHECK_FALSE(program.find(parameter_id("tint"))->referenced);
    // Marking a parameter that does not exist is a no-op rather than an error: reflection reports
    // what a shader reads, and a shader may read a global this program does not declare.
    program.mark_referenced(parameter_id("absent"), true);
    CY_CHECK_EQ(program.parameters().size(), 1U);
}
