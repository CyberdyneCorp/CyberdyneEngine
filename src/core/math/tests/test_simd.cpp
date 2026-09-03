// EVERY SIMD PATH AGAINST THE SCALAR REFERENCE. Task 3.1.3, design.md §5.
//
// "One scalar reference implementation, always compiled and always tested, plus SIMD paths selected
// at build time. Every SIMD path is tested against the scalar reference for bit-identical results
// where the operation is exact, and within a stated tolerance where it is not."
//
// The tolerance, stated: **zero**. Every operation the abstraction exposes — add, subtract,
// multiply, divide, min, max, multiply-add, comparison, horizontal sum — is a single IEEE 754
// operation or a fixed sequence of them, so the two paths must agree to the bit. There is no
// approximate reciprocal and no fused multiply-add in either path, precisely so that this can be an
// equality rather than an epsilon; see the comment on `reference::madd`.
//
// The batch functions are compared with `std::memcmp`, not componentwise: a componentwise
// floating-point comparison would treat -0.0 and 0.0 as equal, and "the two paths produce different
// bits" is exactly the kind of divergence M9's cross-platform work will need to see.
//
// This is an *integration* suite. It sweeps thousands of values and transforms ten thousand points,
// which is the specification's own "bulk transform" scenario, and neither fits a unit test's
// millisecond.

#include <cy/core/math/batch.h>
#include <cy/core/math/math.h>
#include <cy/core/math/simd.h>

#include <cy/test/test.h>

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace simd = cy::math::simd;

/// A spread of values chosen to exercise the cases that differ between a scalar loop and a vector
/// unit: zero and negative zero, denormals, values that round differently, and the two infinities.
/// NaN is deliberately absent — `min` and `max` disagree about it between the reference and the
/// hardware instruction, which simd.h documents, and no bounding volume contains one.
const std::vector<cy::f32>& probe_values() {
    static const std::vector<cy::f32> values = {
        0.0f,    -0.0f,       1.0f,         -1.0f,         0.5f,        -0.5f,
        3.0f,    -3.0f,       1e-30f,       -1e-30f,       1e30f,       -1e30f,
        0.1f,    -0.1f,       16.25f,       -16.25f,       123456.789f, -123456.789f,
        1.0e-7f, 3.14159265f, 2.718281828f, -2.718281828f,
    };
    return values;
}

/// Compare two backends over every pair of probe values, for every primitive operation.
///
/// Templated on both tags rather than written twice, so that adding a backend is one call.
template <class A, class B>
void compare_primitives() {
    const std::vector<cy::f32>& values = probe_values();
    for (const cy::f32 x : values) {
        for (const cy::f32 y : values) {
            const cy::f32 lhs[4] = {x, y, -x, y * 0.5f};
            const cy::f32 rhs[4] = {y, x, y, -x};

            const typename A::Vec a_lhs = A::load(lhs);
            const typename A::Vec a_rhs = A::load(rhs);
            const typename B::Vec b_lhs = B::load(lhs);
            const typename B::Vec b_rhs = B::load(rhs);

            cy::f32 a_out[4];
            cy::f32 b_out[4];

            // The operation's name is passed in only so that a failure prints which one it was:
            // doctest reports the expression, and `same("madd")` is a far better failure line than
            // `std::memcmp(...) == 0`.
            const auto same = [&](const char*) {
                return std::memcmp(a_out, b_out, sizeof(a_out)) == 0;
            };

            A::store(a_out, A::add(a_lhs, a_rhs));
            B::store(b_out, B::add(b_lhs, b_rhs));
            CY_CHECK(same("add"));

            A::store(a_out, A::sub(a_lhs, a_rhs));
            B::store(b_out, B::sub(b_lhs, b_rhs));
            CY_CHECK(same("sub"));

            A::store(a_out, A::mul(a_lhs, a_rhs));
            B::store(b_out, B::mul(b_lhs, b_rhs));
            CY_CHECK(same("mul"));

            A::store(a_out, A::madd(a_lhs, a_rhs, a_lhs));
            B::store(b_out, B::madd(b_lhs, b_rhs, b_lhs));
            CY_CHECK(same("madd"));

            A::store(a_out, A::min(a_lhs, a_rhs));
            B::store(b_out, B::min(b_lhs, b_rhs));
            CY_CHECK(same("min"));

            A::store(a_out, A::max(a_lhs, a_rhs));
            B::store(b_out, B::max(b_lhs, b_rhs));
            CY_CHECK(same("max"));

            CY_CHECK_EQ(A::mask_ge(a_lhs, a_rhs), B::mask_ge(b_lhs, b_rhs));
            CY_CHECK_EQ(A::hsum(a_lhs), B::hsum(b_lhs));
            for (cy::u32 lane = 0; lane < 4; ++lane) {
                CY_CHECK_EQ(A::lane(a_lhs, lane), B::lane(b_lhs, lane));
            }
        }
    }
}

/// A deterministic spread of points, so a failure is reproducible from the test name alone.
std::vector<cy::Vec3> make_points(cy::usize count) {
    cy::Random random(0xC0FFEEull, 7ull);
    std::vector<cy::Vec3> points;
    points.reserve(count);
    for (cy::usize i = 0; i < count; ++i) {
        points.push_back(cy::Vec3{random.next_float_in(-1000.0f, 1000.0f),
                                  random.next_float_in(-1000.0f, 1000.0f),
                                  random.next_float_in(-1000.0f, 1000.0f)});
    }
    return points;
}

}  // namespace

CY_TEST_CASE("SIMD: the scalar reference is compiled into every build") {
    // design.md §5's first clause, asserted rather than assumed. If this ever fails, the comparison
    // every other test in this file makes has no baseline.
    CY_CHECK(simd::backend_compiled(simd::Backend::Scalar));

    simd::Backend backends[static_cast<cy::usize>(simd::Backend::kCount)];
    const cy::usize count = simd::compiled_backends(backends, std::size(backends));
    CY_REQUIRE(count >= 1);
    CY_CHECK(backends[0] == simd::Backend::Scalar);

    // The active backend is always one that is compiled in.
    CY_CHECK(simd::backend_compiled(simd::active_backend()));
    // Built into a local first: the message macro streams its argument, and `<<` binds tighter
    // than `+`, so a concatenation written inline would try to append to the stream builder.
    const std::string active =
        std::string("active SIMD backend: ") + simd::backend_name(simd::active_backend());
    CY_TEST_MESSAGE(active);

    // AVX2 is named by `core-math` and is not implemented; the honest answer is what is asserted,
    // so that a benchmark cannot claim a width the engine does not have.
    CY_CHECK_FALSE(simd::backend_compiled(simd::Backend::Avx2));
}

CY_TEST_CASE("SIMD: every compiled backend is bit-identical to the reference on the primitives") {
    // The reference against itself first: if this fails, the harness is wrong and nothing below it
    // means anything.
    compare_primitives<simd::ReferenceOps, simd::ReferenceOps>();

#if defined(CY_MATH_HAS_SSE)
    compare_primitives<simd::ReferenceOps, simd::SseOps>();
#endif
#if defined(CY_MATH_HAS_NEON)
    compare_primitives<simd::ReferenceOps, simd::NeonOps>();
#endif
}

CY_TEST_CASE("SIMD: the batch point transform is bit-identical to the reference") {
    const std::vector<cy::Vec3> points = make_points(4096);
    const cy::Mat4 m = cy::Mat4::from_trs(cy::Vec3{12.5f, -3.25f, 0.125f},
                                          cy::Quat::from_axis_angle(cy::kAxisY, 0.77f),
                                          cy::Vec3{1.5f, 2.0f, 0.75f});

    std::vector<cy::Vec3> reference(points.size());
    std::vector<cy::Vec3> vectorised(points.size());
    cy::math::transform_points_reference(m, points.data(), reference.data(), points.size());
    cy::math::transform_points_simd(m, points.data(), vectorised.data(), points.size());

    // memcmp, not a componentwise comparison: the requirement is bit-identity, and a float compare
    // would call -0.0 and 0.0 the same.
    CY_CHECK_EQ(std::memcmp(reference.data(), vectorised.data(), points.size() * sizeof(cy::Vec3)),
                0);

    // Directions too — a different code path, since it drops the translation column.
    cy::math::transform_directions_reference(m, points.data(), reference.data(), points.size());
    cy::math::transform_directions_simd(m, points.data(), vectorised.data(), points.size());
    CY_CHECK_EQ(std::memcmp(reference.data(), vectorised.data(), points.size() * sizeof(cy::Vec3)),
                0);
}

CY_TEST_CASE("SIMD: the batch transform agrees with the per-point transform") {
    // The batch and the scalar `transform_point` accumulate in different orders, so this is the one
    // comparison in the file with a real tolerance rather than a bit-identity: they are two correct
    // answers to the same question, differing only in rounding.
    const std::vector<cy::Vec3> points = make_points(512);
    const cy::Mat4 m =
        cy::Mat4::from_trs(cy::Vec3{1.0f, 2.0f, 3.0f}, cy::Quat::from_axis_angle(cy::kAxisX, -1.3f),
                           cy::Vec3{1.0f, 1.0f, 1.0f});

    std::vector<cy::Vec3> batched(points.size());
    cy::math::transform_points(m, points.data(), batched.data(), points.size());
    for (cy::usize i = 0; i < points.size(); ++i) {
        // Relative: the inputs span ±1000 metres, so an absolute epsilon would be meaningless at
        // one end of that range and useless at the other.
        CY_CHECK(nearly_equal(batched[i], cy::transform_point(m, points[i]),
                              1e-3f * (1.0f + max_component(cwise_abs(points[i])))));
    }
}

CY_TEST_CASE("SIMD: transforming in place gives the same answer as transforming into a buffer") {
    // `in == out` is explicitly allowed, and it is the case a loop that reads a point after writing
    // it would get wrong.
    std::vector<cy::Vec3> points = make_points(256);
    const std::vector<cy::Vec3> original = points;
    const cy::Mat4 m = cy::Mat4::from_translation(cy::Vec3{1.0f, -2.0f, 0.5f});

    std::vector<cy::Vec3> separate(points.size());
    cy::math::transform_points(m, original.data(), separate.data(), original.size());
    cy::math::transform_points(m, points.data(), points.data(), points.size());
    CY_CHECK_EQ(std::memcmp(points.data(), separate.data(), points.size() * sizeof(cy::Vec3)), 0);
}

CY_TEST_CASE("SIMD: the bulk transform scenario — ten thousand points by one matrix") {
    // `core-math` — "Bulk transform". The assertion is about the *API* being the array-wide one:
    // that is what makes the vectorisation deliberate rather than hoped for.
    constexpr cy::usize kCount = 10000;
    const std::vector<cy::Vec3> points = make_points(kCount);
    std::vector<cy::Vec3> out(kCount);

    const cy::Mat4 m = cy::Mat4::from_quat(cy::Quat::from_axis_angle(cy::kAxisZ, 0.25f));
    cy::math::transform_points(m, points.data(), out.data(), kCount);

    // A rotation preserves length, for all ten thousand of them.
    for (cy::usize i = 0; i < kCount; ++i) {
        CY_REQUIRE(cy::math::nearly_equal(length(out[i]), length(points[i]),
                                          1e-2f * (1.0f + length(points[i]))));
    }
}

CY_TEST_CASE("SIMD: the batch frustum cull is bit-identical and agrees with the scalar test") {
    const cy::Mat4 view = cy::look_at(cy::Vec3{0.0f, 20.0f, 60.0f}, cy::Vec3{0.0f, 0.0f, 0.0f});
    const cy::Mat4 projection =
        cy::perspective_reversed_z(cy::math::radians(55.0f), 16.0f / 9.0f, 0.5f, 500.0f);
    const cy::Frustum frustum = cy::Frustum::from_view_projection(projection * view);

    // Boxes spread well beyond the frustum so that both answers occur many times; a cull test where
    // everything is visible asserts nothing.
    cy::Random random(4242ull);
    std::vector<cy::Aabb> boxes;
    boxes.reserve(2048);
    for (cy::usize i = 0; i < 2048; ++i) {
        const cy::Vec3 center{random.next_float_in(-300.0f, 300.0f),
                              random.next_float_in(-300.0f, 300.0f),
                              random.next_float_in(-300.0f, 300.0f)};
        boxes.push_back(cy::Aabb::from_center_extents(center, cy::Vec3{2.0f, 2.0f, 2.0f}));
    }

    std::vector<cy::u8> reference(boxes.size());
    std::vector<cy::u8> vectorised(boxes.size());
    cy::math::cull_aabbs_reference(frustum, boxes.data(), reference.data(), boxes.size());
    cy::math::cull_aabbs_simd(frustum, boxes.data(), vectorised.data(), boxes.size());
    CY_CHECK_EQ(std::memcmp(reference.data(), vectorised.data(), boxes.size()), 0);

    // And both agree with the scalar `Frustum::intersects`. The two accumulate the signed distance
    // in different orders, so a box lying exactly on a plane could in principle disagree; none of
    // these does, and a mismatch here would be a real difference rather than a rounding one.
    cy::usize visible = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE_EQ(reference[i] != 0, frustum.intersects(boxes[i]));
        visible += reference[i] != 0 ? 1u : 0u;
    }
    CY_CHECK(visible > 0);
    CY_CHECK(visible < boxes.size());

    // Compaction turns the byte array into the surviving indices.
    std::vector<cy::u32> indices(boxes.size());
    const cy::usize compacted =
        cy::math::compact_visible(reference.data(), boxes.size(), indices.data());
    CY_CHECK_EQ(compacted, visible);
    for (cy::usize i = 0; i < compacted; ++i) {
        CY_REQUIRE(reference[indices[i]] != 0);
    }
}

CY_TEST_CASE("SIMD: a zero-length batch touches nothing") {
    const cy::Mat4 m = cy::Mat4::identity();
    cy::math::transform_points(m, nullptr, nullptr, 0);
    cy::math::transform_directions(m, nullptr, nullptr, 0);
    cy::math::cull_aabbs(cy::Frustum{}, nullptr, nullptr, 0);
    CY_CHECK_EQ(cy::math::compact_visible(nullptr, 0, nullptr), 0u);
}
