// State classification and the determinism firewall. Task 4.2.5.
//
// THE CENTRAL CLAIM OF THIS FILE IS A NEGATIVE ONE, AND IT IS CHECKED AT COMPILE TIME. design.md §5
// asks for an illegal read to be *unspellable*, not refused; the way to test that without a build
// failure is a concept that asks whether the expression is well formed, and a `static_assert` that
// it is not. `CanRead` and `CanWrite` below are that question, and every assertion using them is
// evaluated by the compiler rather than by doctest — a regression that made the read legal again
// would fail the build, which is exactly the right failure mode.

#include <cy/core/determinism/classification.h>
#include <cy/test/test.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::determinism;

/// Is `field.read(context)` a well-formed expression?
template <class Context, class Field>
concept CanRead = requires(const Field& field, Context context) { field.read(context); };

/// Is `field.write(context, value)`?
template <class Context, class Field>
concept CanWrite = requires(Field& field, Context context, const Field::value_type& value) {
    field.write(context, value);
};

}  // namespace

// --- The firewall, as a compile-time fact -------------------------------------------------------

static_assert(CanRead<AuthoritativeContext, Authoritative<f32>>,
              "authority reads authority; if this fails nothing else in the model works");
static_assert(CanRead<AuthoritativeContext, Persistent<f32>>);
static_assert(CanRead<AuthoritativeContext, Derived<f32>>);

static_assert(!CanRead<AuthoritativeContext, Presentation<f32>>,
              "THE FIREWALL. An authoritative system must not be able to spell a read of "
              "presentation state — not be refused one at run time.");
static_assert(!CanRead<AuthoritativeContext, Predicted<f32>>,
              "authority that reads a prediction has adopted a guess");
static_assert(!CanRead<PersistentContext, Presentation<f32>>);
static_assert(
    !CanRead<DerivedContext, Presentation<f32>>,
    "a cache computed from presentation data is a presentation cache; allowing this would "
    "be a two-hop route from presentation into authority");
static_assert(!CanRead<PredictedContext, Presentation<f32>>);

static_assert(CanRead<PresentationContext, Authoritative<f32>>,
              "appearance may look at everything; what it may not do is write back");
static_assert(CanRead<PresentationContext, Predicted<f32>>);
static_assert(CanRead<PredictedContext, Authoritative<f32>>);

// The other direction. "Presentation-only systems SHALL NOT feed back into authoritative state."
static_assert(!CanWrite<PresentationContext, Authoritative<f32>>,
              "THE FEEDBACK DIRECTION. Reading authority from a presentation system is legal; "
              "writing it is not, and the two are different predicates for that reason.");
static_assert(!CanWrite<PresentationContext, Persistent<f32>>);
static_assert(!CanWrite<DerivedContext, Authoritative<f32>>);
static_assert(CanWrite<AuthoritativeContext, Authoritative<f32>>);
static_assert(CanWrite<AuthoritativeContext, Presentation<f32>>,
              "gameplay setting a flash intensity is ordinary; `may_write` is not derived from "
              "`may_read`, precisely so that this stays spellable while the read does not");
static_assert(CanWrite<PresentationContext, Presentation<f32>>);
static_assert(CanWrite<AuthoritativeContext, Predicted<f32>>);
static_assert(!CanWrite<PredictedContext, Authoritative<f32>>);

// A value cannot be laundered by copying it: the wrappers are distinct types with no converting
// constructor, so `Authoritative<f32> a = presentation_value;` does not compile either.
static_assert(!std::is_convertible_v<Presentation<f32>, Authoritative<f32>>);
static_assert(!std::is_constructible_v<Authoritative<f32>, Presentation<f32>>);

CY_TEST_CASE("a classified field costs nothing in layout") {
    // The ECS refuses a component that is not trivially relocatable, and computes a chunk layout
    // from the struct's size. A wrapper that changed either would be found as a registration
    // failure in an unrelated module rather than here.
    struct Plain {
        f32 x;
        u32 y;
    };
    struct Wrapped {
        Authoritative<f32> x;
        Presentation<u32> y;
    };
    CY_CHECK_EQ(sizeof(Wrapped), sizeof(Plain));
    CY_CHECK_EQ(alignof(Wrapped), alignof(Plain));
    CY_CHECK(std::is_trivially_copyable_v<Wrapped>);
}

CY_TEST_CASE("one declaration drives five behaviours") {
    // `simulation-and-determinism`: "WHEN a field is declared authoritative and persistent THEN it
    // SHALL be hashed, snapshotted, saved, and replicated according to that declaration with no
    // further configuration."
    const Participation persistent = participation_of(SimulationClass::Persistent);
    CY_CHECK(persistent.hashed);
    CY_CHECK(persistent.rollback);
    CY_CHECK(persistent.checkpoint);
    CY_CHECK(persistent.saved);
    CY_CHECK(persistent.replicable);

    const Participation authoritative = participation_of(SimulationClass::Authoritative);
    CY_CHECK(authoritative.hashed);
    CY_CHECK_FALSE(authoritative.saved);

    // "WHEN a spatial index or a cached transform exists THEN it SHALL be classified derived and
    // reconstructed rather than captured."
    CY_CHECK(participation_of(SimulationClass::Derived) == Participation{});
    CY_CHECK(participation_of(SimulationClass::Presentation) == Participation{});

    // A prediction is rolled back but never hashed: two peers legitimately disagree about it, and
    // hashing it would make correct prediction look like divergence.
    const Participation predicted = participation_of(SimulationClass::Predicted);
    CY_CHECK_FALSE(predicted.hashed);
    CY_CHECK(predicted.rollback);
}

CY_TEST_CASE("a classification is derived from the serialization declaration") {
    CY_CHECK(class_of(reflect::PersistenceKind::Authoring) == SimulationClass::Authoritative);
    CY_CHECK(class_of(reflect::PersistenceKind::RuntimeState) == SimulationClass::Authoritative);
    CY_CHECK(class_of(reflect::PersistenceKind::PersistentState) == SimulationClass::Persistent);
    CY_CHECK(class_of(reflect::PersistenceKind::Derived) == SimulationClass::Derived);
}

CY_TEST_CASE("a presentation outcome reaches gameplay only as a recorded external result") {
    // "Where a presentation system's outcome must influence gameplay, the outcome SHALL be captured
    // as an authoritative event or an external result rather than read directly."
    const Presentation<f32> measured_from_the_render_thread{0.75F};
    const SimulationPoint at{Epoch{2}, 4100};

    const ExternalResult<f32> captured =
        record_external(PresentationContext{}, at, measured_from_the_render_thread,
                        "hit-test resolved on the render thread");

    CY_CHECK_EQ(captured.value(), 0.75F);
    CY_CHECK(captured.captured_at() == at);
    CY_CHECK_EQ(captured.captured_at().tick, 4100ULL);

    // And now — and only now — authority may read it.
    const Authoritative<f32> laundered = captured.as_authoritative();
    CY_CHECK_EQ(laundered.read(AuthoritativeContext{}), 0.75F);
}

CY_TEST_CASE("the machinery's door is spelled to be found") {
    // Reflection-driven consumers — serialization, the hasher, the inspector — must see every field
    // regardless of class and cannot carry a witness, because they do not know the class until they
    // have read it. `bypass_classification()` is that door, and its name is the audit:
    // `grep -rn bypass_classification src/` finds every use.
    Presentation<f32> value{1.5F};
    CY_CHECK_EQ(value.bypass_classification(), 1.5F);
    value.bypass_classification() = 2.5F;
    CY_CHECK_EQ(value.read(PresentationContext{}), 2.5F);
}

CY_TEST_CASE("every class has a name") {
    CY_CHECK(std::string_view(simulation_class_name(SimulationClass::Authoritative)) ==
             "authoritative");
    CY_CHECK(std::string_view(simulation_class_name(SimulationClass::Presentation)) ==
             "presentation");
}
