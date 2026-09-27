// The IR: identity, canonicalisation, traversal and the round trip. M7 task 6.1.
//
// design.md §1.7: "The IR, its content hash, the builder, the serialiser and the round-trip test —
// before any front-end. The criterion is a property of that layer and of nothing above it." These
// cases are that layer, and each one is a decision from §1.2 stated as a check.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/emit.h>
#include <cy/rendering/material/ir.h>
#include <cy/test/test.h>

#include <utility>

using namespace cy;
using namespace cy::rendering::material;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// `diffuse(base_color * texture(uv))`, built with the operands in a stated order and with
/// `leading` unused constants placed first so that every node id shifts.
[[nodiscard]] Expected<Module, Error> reference(bool parameter_first, u32 leading) noexcept {
    Builder builder(allocator(), Name::intern("identity_case"));
    const ParameterDecl colour{Name::intern("base_color"), ValueType::Vec3,
                               Immediate{1.0F, 1.0F, 1.0F, 0.0F, 0}, false};
    if (!builder.declare_parameter(colour)) {
        return make_unexpected(Error{ErrorCode::Internal, "declare", 0});
    }
    if (!builder.declare_texture(TextureDecl{Name::intern("albedo_map"), Immediate{}, false})) {
        return make_unexpected(Error{ErrorCode::Internal, "declare", 0});
    }
    for (u32 index = 0; index < leading; ++index) {
        if (!builder.constant_float(static_cast<f32>(index) + 7.0F)) {
            return make_unexpected(Error{ErrorCode::Internal, "constant", 0});
        }
    }
    auto uv = builder.attribute(Name::intern("uv0"), ValueType::Vec2);
    auto sample = builder.texture_sample(Name::intern("albedo_map"), uv.value());
    const u8 components[] = {0, 1, 2};
    Immediate mask;
    mask.mask = Builder::swizzle_mask(Span<const u8>(components, 3));
    const NodeId sampled[] = {sample.value()};
    auto rgb =
        builder.make(Op::Swizzle, ValueType::Count, Name{}, mask, Span<const NodeId>(sampled, 1));
    auto parameter = builder.parameter(Name::intern("base_color"));

    const NodeId product[] = {parameter_first ? parameter.value() : rgb.value(),
                              parameter_first ? rgb.value() : parameter.value()};
    auto multiplied = builder.make(Op::Mul, Span<const NodeId>(product, 2));
    const NodeId colour_operand[] = {multiplied.value()};
    auto closure = builder.make(Op::Diffuse, Span<const NodeId>(colour_operand, 1));
    if (!closure) {
        return make_unexpected(closure.error());
    }
    if (!builder.set_surface(closure.value())) {
        return make_unexpected(Error{ErrorCode::Internal, "surface", 0});
    }
    auto one = builder.constant_float(1.0F);
    if (!builder.set_opacity(one.value())) {
        return make_unexpected(Error{ErrorCode::Internal, "opacity", 0});
    }
    return builder.finish();
}

[[nodiscard]] Expected<GeneratedSource, Error> emit(const Module& module) noexcept {
    return emit_program(module, EmitOptions{});
}

}  // namespace

CY_TEST_CASE("material_ir: a commutative operand list is ordered by content, not by wiring") {
    // Decision 3. The two modules wire `base_color * texture` in opposite orders and are one value.
    auto wired_one_way = reference(true, 0);
    auto wired_the_other = reference(false, 0);
    CY_REQUIRE(wired_one_way.has_value());
    CY_REQUIRE(wired_the_other.has_value());
    CY_CHECK_EQ(wired_one_way.value().digest(), wired_the_other.value().digest());
    CY_CHECK_EQ(wired_one_way.value().size(), wired_the_other.value().size());
}

CY_TEST_CASE("material_ir: the program does not depend on node ids") {
    // Decision 6, and the case the spike proved with an id-shifted module. `leading` puts three
    // unused constants in front, so every id after them differs.
    auto plain = reference(true, 0);
    auto shifted = reference(true, 3);
    CY_REQUIRE(plain.has_value());
    CY_REQUIRE(shifted.has_value());
    CY_CHECK_EQ(plain.value().digest(), shifted.value().digest());

    auto plain_source = emit(plain.value());
    auto shifted_source = emit(shifted.value());
    CY_REQUIRE(plain_source.has_value());
    CY_REQUIRE(shifted_source.has_value());
    CY_CHECK_EQ(plain_source.value().digest, shifted_source.value().digest);
}

CY_TEST_CASE("material_ir: identical values are one node") {
    Builder builder(allocator(), Name::intern("interning"));
    auto first = builder.constant_float(0.5F);
    auto second = builder.constant_float(0.5F);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first.value(), second.value());

    // And a hash-consed value is reached from two different expressions built separately.
    auto attribute = builder.attribute(Name::intern("uv0"), ValueType::Vec2);
    const NodeId left[] = {attribute.value(), attribute.value()};
    auto once = builder.make(Op::Mul, Span<const NodeId>(left, 2));
    auto twice = builder.make(Op::Mul, Span<const NodeId>(left, 2));
    CY_CHECK_EQ(once.value(), twice.value());
}

CY_TEST_CASE("material_ir: one spelling per operation") {
    // Decision 7. `1 - x` and a "one minus" node are the same value, because the builder rewrites
    // the first into the second — the only place such a choice can be made once.
    Builder builder(allocator(), Name::intern("spelling"));
    auto metallic = builder.attribute(Name::intern("metallic"), ValueType::Float);
    auto one = builder.constant_float(1.0F);
    const NodeId subtraction[] = {one.value(), metallic.value()};
    auto written = builder.make(Op::Sub, Span<const NodeId>(subtraction, 2));
    const NodeId single[] = {metallic.value()};
    auto dragged = builder.make(Op::OneMinus, Span<const NodeId>(single, 1));
    CY_REQUIRE(written.has_value());
    CY_REQUIRE(dragged.has_value());
    CY_CHECK_EQ(written.value(), dragged.value());
    CY_CHECK_EQ(builder.node(written.value()).op, Op::OneMinus);

    // `float3(1,1,1) - float` is a widening and stays a subtraction: rewriting it would change the
    // value's type.
    auto splat = builder.constant_vec3(1.0F, 1.0F, 1.0F);
    const NodeId widening[] = {splat.value(), metallic.value()};
    auto kept = builder.make(Op::Sub, Span<const NodeId>(widening, 2));
    CY_REQUIRE(kept.has_value());
    CY_CHECK_EQ(builder.node(kept.value()).op, Op::Sub);
}

CY_TEST_CASE("material_ir: the builder refuses what it cannot type") {
    Builder builder(allocator(), Name::intern("typing"));
    auto two = builder.attribute(Name::intern("uv0"), ValueType::Vec2);
    auto three = builder.attribute(Name::intern("normal"), ValueType::Vec3);
    const NodeId mismatched[] = {two.value(), three.value()};
    CY_CHECK_FALSE(builder.make(Op::Mul, Span<const NodeId>(mismatched, 2)).has_value());

    const NodeId colour[] = {three.value()};
    auto closure = builder.make(Op::Diffuse, Span<const NodeId>(colour, 1));
    const NodeId arithmetic[] = {closure.value(), three.value()};
    CY_CHECK_FALSE(builder.make(Op::Add, Span<const NodeId>(arithmetic, 2)).has_value());

    // A root of the wrong kind is refused rather than recorded.
    CY_CHECK_FALSE(builder.set_surface(three.value()).has_value());
    CY_CHECK_FALSE(builder.set_opacity(closure.value()).has_value());

    // An undeclared parameter or texture is a diagnostic, not a silent zero.
    CY_CHECK_FALSE(builder.parameter(Name::intern("nothing")).has_value());
    CY_CHECK_FALSE(builder.texture_sample(Name::intern("nothing"), two.value()).has_value());
}

CY_TEST_CASE("material_ir: a module round-trips through its serialised form") {
    auto original = reference(true, 2);
    CY_REQUIRE(original.has_value());

    Array<u8> bytes(allocator());
    CY_REQUIRE(encode_module(original.value(), bytes).has_value());
    CY_CHECK_GT(bytes.size(), 0U);

    auto decoded = decode_module(bytes.span(), allocator());
    CY_REQUIRE(decoded.has_value());
    CY_CHECK_EQ(decoded.value().digest(), original.value().digest());
    CY_CHECK_EQ(decoded.value().size(), original.value().size());
    CY_CHECK_EQ(decoded.value().parameters().size(), original.value().parameters().size());
    CY_CHECK_EQ(decoded.value().textures().size(), original.value().textures().size());

    // The strongest statement available: the two emit the same program, byte for byte.
    auto before = emit(original.value());
    auto after = emit(decoded.value());
    CY_REQUIRE(before.has_value());
    CY_REQUIRE(after.has_value());
    CY_CHECK_EQ(before.value().digest, after.value().digest);

    // Encoding is deterministic, which is what makes a digest over these bytes a cook key.
    Array<u8> again(allocator());
    CY_REQUIRE(encode_module(original.value(), again).has_value());
    CY_REQUIRE_EQ(again.size(), bytes.size());
    bool identical = true;
    for (usize index = 0; index < bytes.size(); ++index) {
        identical = identical && bytes[index] == again[index];
    }
    CY_CHECK(identical);
}

CY_TEST_CASE("material_ir: a malformed module is refused rather than half-read") {
    auto original = reference(true, 0);
    CY_REQUIRE(original.has_value());
    Array<u8> bytes(allocator());
    CY_REQUIRE(encode_module(original.value(), bytes).has_value());

    // Truncated at every length: none of them decodes, and none of them crashes.
    for (usize length = 0; length < bytes.size(); length += 7) {
        auto decoded = decode_module(Span<const u8>(bytes.data(), length), allocator());
        CY_CHECK_FALSE(decoded.has_value());
    }
    Array<u8> corrupted(allocator());
    CY_REQUIRE(corrupted.append(bytes.span()).has_value());
    corrupted[0] = static_cast<u8>(corrupted[0] + 1U);
    CY_CHECK_FALSE(decode_module(corrupted.span(), allocator()).has_value());
}

CY_TEST_CASE("material_ir: the canonical order visits every operand before its consumer") {
    auto module = reference(true, 1);
    CY_REQUIRE(module.has_value());
    const NodeId roots[] = {module.value().surface(), module.value().opacity()};
    Array<NodeId> order(allocator());
    CY_REQUIRE(canonical_order(module.value(), Span<const NodeId>(roots, 2), order).has_value());

    Array<u8> seen(allocator());
    CY_REQUIRE(seen.resize(module.value().size()).has_value());
    for (cy::u8& mark : seen) {
        mark = 0;
    }
    for (const NodeId id : order) {
        for (const NodeId operand : module.value().operands(id)) {
            CY_CHECK_EQ(seen[operand], 1U);
        }
        CY_CHECK_EQ(seen[id], 0U);
        seen[id] = 1;
    }
    // The unused constant placed in front is not reachable from the roots and is not visited.
    CY_CHECK_LT(order.size(), module.value().size());
}
