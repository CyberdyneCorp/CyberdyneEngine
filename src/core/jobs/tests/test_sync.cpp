// The synchronisation vocabulary, double buffering, and the single-producer queue. Task 3.2.9.
//
// Single-threaded and clock-free on purpose: these cases fix the *semantics* — what an event means
// once set, what a double buffer publishes, when a queue reports full — and a unit test has one
// millisecond, which does not stretch to starting a thread or to waiting for a timeout. A single
// hundred-microsecond `wait_for` costs two to four milliseconds once the scheduler has rounded it
// up, so anything that waits on a clock lives in test_sync_timed.cpp, in the integration suite.
// The concurrent behaviour is exercised under load by the integration suites and under
// ThreadSanitizer in CI.

#include "harness.h"

#include <cy/core/jobs/double_buffer.h>
#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/thread_role.h>

namespace {

using namespace cy;
using namespace cy::jobs;

}  // namespace

CY_TEST_CASE("an atomic reference count reports the last release and only that one") {
    AtomicRefCount count(1);
    CY_CHECK_EQ(count.add_ref(), 2u);
    CY_CHECK_FALSE(count.release());
    CY_CHECK_EQ(count.count(), 1u);
    CY_CHECK(count.release());
    CY_CHECK_EQ(count.count(), 0u);
}

CY_TEST_CASE("a flag reports what it was, so a once-only path needs no second variable") {
    AtomicFlag flag;
    CY_CHECK_FALSE(flag.test());
    CY_CHECK_FALSE(flag.test_and_set());
    CY_CHECK(flag.test_and_set());
    flag.clear();
    CY_CHECK_FALSE(flag.test());
}

CY_TEST_CASE("a spin lock is exclusive and try_lock reports the truth") {
    SpinLock lock;
    CY_CHECK(lock.try_lock());
    CY_CHECK_FALSE(lock.try_lock());
    lock.unlock();
    CY_CHECK(lock.try_lock());
    lock.unlock();

    ScopedLock<SpinLock> held(lock);
    CY_CHECK_FALSE(lock.try_lock());
}

CY_TEST_CASE("a read-write lock admits many readers or one writer") {
    RwLock lock;
    CY_CHECK(lock.try_lock_shared());
    CY_CHECK(lock.try_lock_shared());
    CY_CHECK_FALSE(lock.try_lock());
    lock.unlock_shared();
    lock.unlock_shared();
    CY_CHECK(lock.try_lock());
    lock.unlock();
}

CY_TEST_CASE("a semaphore counts") {
    Semaphore semaphore(2);
    CY_CHECK(semaphore.try_acquire());
    CY_CHECK(semaphore.try_acquire());
    CY_CHECK_FALSE(semaphore.try_acquire());
    semaphore.post();
    CY_CHECK(semaphore.try_acquire());
    CY_CHECK_FALSE(semaphore.try_acquire());

    semaphore.post(3);
    CY_CHECK(semaphore.try_acquire());
    CY_CHECK(semaphore.try_acquire());
    CY_CHECK(semaphore.try_acquire());
    CY_CHECK_FALSE(semaphore.try_acquire());
}

CY_TEST_CASE("a manual-reset event stays set until it is reset") {
    Event event;
    CY_CHECK_FALSE(event.is_set());

    event.set();
    CY_CHECK(event.is_set());
    // Set once, every waiter proceeds — including one that arrives afterwards, which is the
    // difference between this and a semaphore of capacity one.
    CY_CHECK(event.wait_for(0));
    CY_CHECK(event.wait_for(0));

    event.reset();
    CY_CHECK_FALSE(event.is_set());
}

// --- Double buffering ------------------------------------------------------------------------------

CY_TEST_CASE("a double buffer publishes only at the swap") {
    DoubleBuffered<int> buffered(0);
    CY_CHECK_EQ(buffered.read(), 0);

    buffered.write() = 42;
    // The reader still sees the published buffer. This is the property the render thread depends
    // on: it reads last frame's snapshot while simulation writes the next one, with no lock.
    CY_CHECK_EQ(buffered.read(), 0);

    buffered.swap();
    CY_CHECK_EQ(buffered.read(), 42);
    CY_CHECK_EQ(buffered.generation(), 1u);

    // The writer now holds what the readers were reading, which is the buffer to overwrite.
    buffered.write() = 7;
    CY_CHECK_EQ(buffered.read(), 42);
    buffered.swap();
    CY_CHECK_EQ(buffered.read(), 7);
    CY_CHECK_EQ(buffered.generation(), 2u);
}

// --- The single-producer, single-consumer queue -----------------------------------------------------

CY_TEST_CASE("an spsc queue reports full rather than overwriting") {
    SpscQueue<u32> queue;
    CY_REQUIRE(queue.initialize(4).has_value());

    CY_CHECK(queue.push(1));
    CY_CHECK(queue.push(2));
    CY_CHECK(queue.push(3));
    // One slot is always left empty so that full and empty are distinguishable without a counter.
    CY_CHECK_FALSE(queue.push(4));
    CY_CHECK_EQ(queue.size(), 3u);

    u32 value = 0;
    CY_CHECK(queue.pop(value));
    CY_CHECK_EQ(value, 1u);
    CY_CHECK(queue.push(4));

    CY_CHECK(queue.pop(value));
    CY_CHECK_EQ(value, 2u);
    CY_CHECK(queue.pop(value));
    CY_CHECK_EQ(value, 3u);
    CY_CHECK(queue.pop(value));
    CY_CHECK_EQ(value, 4u);
    CY_CHECK_FALSE(queue.pop(value));
    CY_CHECK(queue.empty());
}

CY_TEST_CASE("an spsc queue of fewer than two slots is refused") {
    SpscQueue<u32> queue;
    const auto refused = queue.initialize(1);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

// --- Thread roles ------------------------------------------------------------------------------------

CY_TEST_CASE("a thread role violation is counted in every configuration") {
    reset_thread_role_violations();
    clear_thread_roles();

    set_thread_role(ThreadRole::Main);
    CY_CHECK_EQ(current_thread_role(), ThreadRole::Main);
    CY_CHECK(thread_holds_role(ThreadRole::Main));
    CY_CHECK_FALSE(thread_holds_role(ThreadRole::Render));

    // The specification's scenario: code on the wrong thread attempts to record RHI commands.
    CY_CHECK_FALSE(require_thread_role(ThreadRole::Render, "record_rhi_command"));
    CY_CHECK_EQ(thread_role_violations(), 1u);
    CY_CHECK_EQ(last_required_thread_role(), ThreadRole::Render);

    // The platform-mandated pairing: one OS thread may hold Main and Simulation both.
    set_thread_role(ThreadRole::Simulation);
    CY_CHECK(thread_holds_role(ThreadRole::Main));
    CY_CHECK(thread_holds_role(ThreadRole::Simulation));
    CY_CHECK(require_thread_role(ThreadRole::Simulation, "flush_commands"));
    CY_CHECK_EQ(thread_role_violations(), 1u);
    // The primary role is the first one declared, which is what a diagnostic names.
    CY_CHECK_EQ(current_thread_role(), ThreadRole::Main);

    clear_thread_roles();
    reset_thread_role_violations();
}
