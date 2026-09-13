// The determinism firewall, over the first component that adopted it. Task 1.3.
//
// `Classified<>` arrived at M2 correct and unused. Its illegal crossings genuinely do not compile —
// src/core/determinism/tests/test_classification.cpp proves that over `Classified<f32>` — but no
// engine field had adopted it, so the guarantee covered nothing the engine actually holds. This
// file is the difference: the same proof, over `scene::InterpolatedTransform`, which is where M2
// keeps state on both sides of the firewall.
//
// MOST OF THIS FILE IS `static_assert`, AND THAT IS THE POINT. A crossing that the firewall
// forbids has no run-time behaviour to observe: there is no expression that yields the value, so
// there is nothing for a test to call and check. A constrained concept asks the compiler whether
// the expression is well formed, which turns "does not compile" into something a green test run
// can assert.

#include <cy/test/test.h>

#include <cy/core/determinism/classification.h>
#include <cy/core/math/transform.h>
#include <cy/scene/components.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

namespace {

using cy::determinism::AuthoritativeContext;
using cy::determinism::DerivedContext;
using cy::determinism::PredictedContext;
using cy::determinism::PresentationContext;
using cy::scene::InterpolatedTransform;
using cy::scene::test::Fixture;
using cy::scene::test::make_child;

template <class Context, class Field>
concept CanRead = requires(const Field& field, Context context) { field.read(context); };

template <class Context, class Field>
concept CanWrite = requires(Field& field, Context context, const Field::value_type& value) {
    field.write(context, value);
};

using History = decltype(InterpolatedTransform::previous);
using Teleport = decltype(InterpolatedTransform::teleport);

// The interpolation history is presentation state: only presentation may read it. This is the
// crossing `simulation-and-determinism` is about — an authoritative system reading a value produced
// for appearance, whose content depends on when a frame was drawn — and it is now unspellable.
static_assert(CanRead<PresentationContext, History>,
              "a renderer must be able to read the history it blends from");
static_assert(!CanRead<AuthoritativeContext, History>,
              "authoritative simulation must not be able to name the interpolation history");
static_assert(!CanRead<PredictedContext, History>);
static_assert(!CanRead<DerivedContext, History>,
              "a derived value computed from presentation state is presentation state; allowing "
              "this read would make Derived a hole the firewall leaks through in two hops");

// And the other direction: authority may write it — propagation does, every tick — while a
// presentation system may not write back into anything authoritative.
static_assert(CanWrite<AuthoritativeContext, History>,
              "propagation writes the history on the simulation side of the frame");
static_assert(CanWrite<PresentationContext, History>);

// The teleport flag is authoritative: a gameplay decision that presentation consumes.
static_assert(CanRead<AuthoritativeContext, Teleport>,
              "propagation clears the flag on the authoritative side and has to read it first");
static_assert(CanRead<PresentationContext, Teleport>,
              "presentation may read authority; that direction is not the firewall");
static_assert(CanWrite<AuthoritativeContext, Teleport>);
static_assert(!CanWrite<PresentationContext, Teleport>,
              "a presentation system that could set the teleport flag would be feeding back into "
              "authoritative state, which is the second half of the firewall");

}  // namespace

CY_TEST_CASE("the firewall does not change what the component costs") {
    // Layout transparency is the property that makes adoption free, and the one whose loss would be
    // discovered as an ECS registration failure rather than as a compile error here.
    CY_CHECK_EQ(sizeof(InterpolatedTransform), sizeof(cy::Transform) + alignof(cy::Transform));
    CY_CHECK(std::is_trivially_copyable_v<InterpolatedTransform>);
}

CY_TEST_CASE("interpolation still behaves, with every access carrying a witness") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Mover");
    CY_REQUIRE(node.valid());
    CY_REQUIRE(node.set_local_transform(cy::Transform{}).has_value());
    CY_REQUIRE(node.set_interpolated(true).has_value());

    // A node that has not moved blends to where it is, whatever the alpha.
    CY_REQUIRE(fixture.tree.propagate().has_value());
    const cy::Transform half = node.render_transform(0.5F);
    CY_CHECK_NEAR(half.translation.x, node.world_transform().translation.x, 1e-6);

    // A node that has moved blends between the two ticks: the history is presentation state and
    // this is the only path in the engine that may read it.
    cy::Transform moved;
    moved.translation = cy::Vec3{10.0F, 0.0F, 0.0F};
    CY_REQUIRE(node.set_local_transform(moved).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK_NEAR(node.render_transform(0.5F).translation.x, 5.0F, 1e-6);

    // Teleporting collapses the history onto the new placement, so the blend is between two equal
    // values — the "teleport flag SHALL suppress interpolation for that frame" scenario. The flag
    // is authoritative state written by gameplay and cleared on the simulation side, and every one
    // of those accesses now carries a witness.
    cy::Transform jumped;
    jumped.translation = cy::Vec3{20.0F, 0.0F, 0.0F};
    CY_REQUIRE(node.set_local_transform(jumped).has_value());
    CY_REQUIRE(node.teleport().has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK_NEAR(node.render_transform(0.5F).translation.x, 20.0F, 1e-6);
}
