// Shapes, tensors, allocation reuse and specification matching. M8.c task 4.1.

#include <cy/core/memory/system_allocator.h>
#include <cy/ml/tensor.h>
#include <cy/test/test.h>

namespace {

using namespace cy;
using namespace cy::ml;

[[nodiscard]] TensorShape shape_of(std::initializer_list<i64> dimensions) {
    return TensorShape::of(Span<const i64>(dimensions.begin(), dimensions.size())).value();
}

CY_TEST_CASE("ml.tensor: a shape rejects a rank it cannot hold and a dimension that is not one") {
    const i64 too_many[kMaxTensorRank + 1] = {};
    CY_CHECK_FALSE(TensorShape::of(Span<const i64>(too_many, kMaxTensorRank + 1)).has_value());

    const i64 negative[] = {2, -3};
    CY_CHECK_FALSE(TensorShape::of(Span<const i64>(negative, 2)).has_value());

    const i64 dynamic[] = {TensorShape::kDynamic, 4};
    Expected<TensorShape, Error> shape = TensorShape::of(Span<const i64>(dynamic, 2));
    CY_REQUIRE(shape.has_value());
    CY_CHECK(shape.value().has_dynamic());
    // A dynamic shape has no element count, and that is a value rather than an assertion: a caller
    // that multiplies the dimensions of a specification would otherwise get -4.
    CY_CHECK_EQ(shape.value().element_count(), 0U);
}

CY_TEST_CASE("ml.tensor: a specification accepts the concrete shapes it declared and no others") {
    const TensorShape declared = shape_of({TensorShape::kDynamic, 4});
    CY_CHECK(declared.accepts(shape_of({1, 4})));
    CY_CHECK(declared.accepts(shape_of({64, 4})));
    CY_CHECK_FALSE(declared.accepts(shape_of({64, 5})));
    CY_CHECK_FALSE(declared.accepts(shape_of({64})));
}

CY_TEST_CASE("ml.tensor: allocation is refused for a dynamic shape and an unknown type") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    CY_CHECK_FALSE(
        Tensor::allocate(allocator, ElementType::F32, shape_of({TensorShape::kDynamic, 4}))
            .has_value());
    CY_CHECK_FALSE(Tensor::allocate(allocator, ElementType::Unknown, shape_of({4})).has_value());
}

CY_TEST_CASE("ml.tensor: a const view of a mutable tensor is a view, not an empty span") {
    // THE REGRESSION TEST for a defect this suite did not catch and `integration.ml_runtime` did,
    // by segfaulting: `as<const f32>()` compared `const f32` against `f32`, did not match, and
    // returned an EMPTY span. A read that silently yields nothing is worse than one that fails.
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<Tensor, Error> tensor =
        Tensor::allocate(allocator, ElementType::F32, shape_of({2, 3}));
    CY_REQUIRE(tensor.has_value());
    CY_CHECK_EQ(tensor.value().as<const f32>().size(), 6U);
    CY_CHECK_EQ(tensor.value().as<volatile f32>().size(), 6U);
    const Tensor& immutable = tensor.value();
    CY_CHECK_EQ(immutable.as<f32>().size(), 6U);
    CY_CHECK_EQ(immutable.as<const f32>().size(), 6U);
    // And a genuinely wrong type is still empty, cv or not.
    CY_CHECK(tensor.value().as<const i32>().empty());
}

CY_TEST_CASE("ml.tensor: a typed view is empty when the caller guessed the element type wrong") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<Tensor, Error> tensor =
        Tensor::allocate(allocator, ElementType::F32, shape_of({2, 3}));
    CY_REQUIRE(tensor.has_value());
    CY_CHECK_EQ(tensor.value().element_count(), 6U);
    CY_CHECK_EQ(tensor.value().byte_size(), 24U);
    CY_CHECK_EQ(tensor.value().as<f32>().size(), 6U);
    // The whole point: reading f32 memory as i32 hands back nothing rather than reinterpreting it.
    CY_CHECK(tensor.value().as<i32>().empty());
}

CY_TEST_CASE("ml.tensor: reshape reuses the allocation and refuses to grow it") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<Tensor, Error> tensor =
        Tensor::allocate(allocator, ElementType::F32, shape_of({8, 4}));
    CY_REQUIRE(tensor.has_value());
    const void* address = tensor.value().data();
    CY_CHECK_EQ(tensor.value().allocations(), 1U);

    CY_CHECK(tensor.value().reshape(shape_of({3, 4})).has_value());
    CY_CHECK_EQ(tensor.value().data(), address);         // the same memory
    CY_CHECK_EQ(tensor.value().byte_size(), 48U);        // a smaller view of it
    CY_CHECK_EQ(tensor.value().capacity_bytes(), 128U);  // and the allocation is unchanged
    CY_CHECK_EQ(tensor.value().allocations(), 1U);       // "reuse without reallocation", measured

    // Growing is refused rather than silently reallocating in the middle of a frame.
    Status grown = tensor.value().reshape(shape_of({9, 4}));
    CY_REQUIRE_FALSE(grown.has_value());
    CY_CHECK_EQ(grown.error().code, ErrorCode::BufferTooSmall);
    CY_CHECK_EQ(tensor.value().allocations(), 1U);
}

CY_TEST_CASE("ml.tensor: a borrowed tensor frees nothing and covers its shape") {
    f32 storage[12] = {};
    Expected<Tensor, Error> tensor =
        Tensor::borrow(storage, sizeof(storage), ElementType::F32, shape_of({3, 4}));
    CY_REQUIRE(tensor.has_value());
    CY_CHECK_FALSE(tensor.value().owns_memory());
    CY_CHECK_EQ(tensor.value().data(), static_cast<void*>(storage));

    const Span<f32> view = tensor.value().as<f32>();
    CY_REQUIRE_EQ(view.size(), 12U);
    view[5] = 2.5F;
    CY_CHECK_EQ(storage[5], 2.5F);

    // Too small for the shape: refused rather than trusted.
    CY_CHECK_FALSE(Tensor::borrow(storage, 8, ElementType::F32, shape_of({3, 4})).has_value());
}

CY_TEST_CASE("ml.tensor: a mismatch against a specification names which kind it was") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    TensorSpec spec;
    spec.name = Name::intern("features");
    spec.type = ElementType::F32;
    spec.shape = shape_of({TensorShape::kDynamic, 4});

    Expected<Tensor, Error> right = Tensor::allocate(allocator, ElementType::F32, shape_of({2, 4}));
    CY_REQUIRE(right.has_value());
    CY_CHECK_EQ(match_spec(spec, right.value()), SpecMismatch::None);

    Expected<Tensor, Error> wrong_type =
        Tensor::allocate(allocator, ElementType::I32, shape_of({2, 4}));
    CY_REQUIRE(wrong_type.has_value());
    CY_CHECK_EQ(match_spec(spec, wrong_type.value()), SpecMismatch::ElementType);

    Expected<Tensor, Error> wrong_rank =
        Tensor::allocate(allocator, ElementType::F32, shape_of({8}));
    CY_REQUIRE(wrong_rank.has_value());
    CY_CHECK_EQ(match_spec(spec, wrong_rank.value()), SpecMismatch::Rank);

    Expected<Tensor, Error> wrong_width =
        Tensor::allocate(allocator, ElementType::F32, shape_of({2, 5}));
    CY_REQUIRE(wrong_width.has_value());
    u32 dimension = 0;
    CY_CHECK_EQ(match_spec(spec, wrong_width.value(), &dimension), SpecMismatch::Dimension);
    CY_CHECK_EQ(dimension, 1U);  // the requirement's "with both shapes named", as an index
}

CY_TEST_CASE("ml.tensor: moving a tensor moves its memory and leaves nothing to free twice") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<Tensor, Error> tensor = Tensor::allocate(allocator, ElementType::F32, shape_of({4}));
    CY_REQUIRE(tensor.has_value());
    const void* address = tensor.value().data();

    Tensor moved(std::move(tensor).value());
    CY_CHECK_EQ(moved.data(), address);
    CY_CHECK(moved.owns_memory());

    Tensor assigned;
    assigned = std::move(moved);
    CY_CHECK_EQ(assigned.data(), address);
}

}  // namespace
