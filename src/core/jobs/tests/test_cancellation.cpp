// Cooperative cancellation: the source, the token, and the tree. Task 3.2.6.
//
// No threads here. What these cases fix is the *semantics* — what a token reports, what a child
// inherits, when a callback fires — and those are what the scheduler and the coroutine machinery
// are written against. The behaviour under a running system (a cancelled task's body never runs,
// an unresponsive task is reported) is in the integration suite, because it needs workers.

#include "harness.h"

#include <cy/core/jobs/cancellation.h>

namespace {

using namespace cy;
using namespace cy::jobs;

struct Observer {
    u32 fired = 0;
};

void note(void* user) noexcept {
    static_cast<Observer*>(user)->fired += 1;
}

}  // namespace

CY_TEST_CASE("the default token is the never-cancelled token and allocates nothing") {
    const u64 before = cancellation_states_live();
    const CancellationToken token;
    CY_CHECK_FALSE(token.can_be_cancelled());
    CY_CHECK_FALSE(token.is_cancelled());
    CY_CHECK_EQ(token.cancelled_at_ns(), 0);
    CY_CHECK_EQ(cancellation_states_live(), before);
}

CY_TEST_CASE("a source cancels its own token and nothing else") {
    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());

    CancellationToken token = source.value().token();
    CY_CHECK(token.can_be_cancelled());
    CY_CHECK_FALSE(token.is_cancelled());

    source.value().cancel();
    CY_CHECK(token.is_cancelled());
    CY_CHECK(source.value().is_cancelled());
    CY_CHECK(token.cancelled_at_ns() != 0);

    // Idempotent: cancelling twice is not two cancellations.
    const i64 first = token.cancelled_at_ns();
    source.value().cancel();
    CY_CHECK_EQ(token.cancelled_at_ns(), first);
}

CY_TEST_CASE("cancellation propagates to children and not to parents") {
    auto parent = CancellationSource::create();
    CY_REQUIRE(parent.has_value());
    auto child = CancellationSource::create_child(parent.value().token());
    CY_REQUIRE(child.has_value());
    auto grandchild = CancellationSource::create_child(child.value().token());
    CY_REQUIRE(grandchild.has_value());

    // The specification's scenario: a parent load is cancelled, and its child decompression and
    // activation tasks observe cancellation too.
    parent.value().cancel();
    CY_CHECK(child.value().is_cancelled());
    CY_CHECK(grandchild.value().is_cancelled());

    auto other = CancellationSource::create();
    CY_REQUIRE(other.has_value());
    auto other_child = CancellationSource::create_child(other.value().token());
    CY_REQUIRE(other_child.has_value());
    other_child.value().cancel();
    // Cancelling a child does not cancel its parent: the child is one part of the operation.
    CY_CHECK_FALSE(other.value().is_cancelled());
}

CY_TEST_CASE("a child of an already-cancelled operation is born cancelled") {
    auto parent = CancellationSource::create();
    CY_REQUIRE(parent.has_value());
    parent.value().cancel();

    auto child = CancellationSource::create_child(parent.value().token());
    CY_REQUIRE(child.has_value());
    CY_CHECK(child.value().is_cancelled());
}

CY_TEST_CASE("a child of the never-cancelled token is an independent source") {
    const CancellationToken none;
    auto child = CancellationSource::create_child(none);
    CY_REQUIRE(child.has_value());
    CY_CHECK(child.value().is_valid());
    CY_CHECK_FALSE(child.value().is_cancelled());
}

CY_TEST_CASE("a callback fires once on cancellation, and inline when already cancelled") {
    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());
    CancellationToken token = source.value().token();

    Observer observer;
    const CancellationRegistration registration = token.on_cancel(&note, &observer);
    CY_CHECK(registration != kNoCancellationRegistration);
    CY_CHECK_EQ(observer.fired, 0u);

    source.value().cancel();
    CY_CHECK_EQ(observer.fired, 1u);
    source.value().cancel();
    CY_CHECK_EQ(observer.fired, 1u);

    // Registering against a token that is already cancelled runs the callback now, on this thread,
    // and returns no registration because there is nothing left to withdraw.
    Observer late;
    CY_CHECK_EQ(token.on_cancel(&note, &late), kNoCancellationRegistration);
    CY_CHECK_EQ(late.fired, 1u);
}

CY_TEST_CASE("a withdrawn callback does not fire") {
    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());
    CancellationToken token = source.value().token();

    Observer observer;
    const CancellationRegistration registration = token.on_cancel(&note, &observer);
    token.withdraw(registration);
    source.value().cancel();
    CY_CHECK_EQ(observer.fired, 0u);

    // Withdrawing nothing is not an error: an awaiter that finished for another reason withdraws
    // unconditionally rather than remembering whether it registered.
    token.withdraw(kNoCancellationRegistration);
}

CY_TEST_CASE("a cancellation tree is released when its last reference goes") {
    const u64 before = cancellation_states_live();
    {
        auto parent = CancellationSource::create();
        CY_REQUIRE(parent.has_value());
        auto child = CancellationSource::create_child(parent.value().token());
        CY_REQUIRE(child.has_value());
        CancellationToken copy = child.value().token();
        CY_CHECK_EQ(cancellation_states_live(), before + 2);
    }
    // A child holds a counted reference to its parent and unlinks itself before it dies, so the
    // whole tree goes rather than the cycle a two-way reference would leave behind.
    CY_CHECK_EQ(cancellation_states_live(), before);
}

CY_TEST_CASE("a moved-from source no longer cancels") {
    auto source = CancellationSource::create();
    CY_REQUIRE(source.has_value());
    CancellationToken token = source.value().token();

    CancellationSource moved = std::move(source.value());
    CY_CHECK(moved.is_valid());
    moved.cancel();
    CY_CHECK(token.is_cancelled());
}
