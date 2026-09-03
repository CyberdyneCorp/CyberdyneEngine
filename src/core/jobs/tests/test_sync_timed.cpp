// The synchronisation primitives' timed waits. Task 3.2.9.
//
// Separated from test_sync.cpp for one reason, and it is the taxonomy's: a unit test has one
// millisecond, and a hundred-microsecond `wait_for` costs two to four milliseconds once the
// operating system has rounded the sleep up and rescheduled the thread. That is the tool working
// correctly, not the test being slow, so these cases belong in the suite whose budget is a second.
//
// What they fix is the half of each primitive's contract that a clock-free test cannot reach: that
// a timeout is reported as a timeout rather than as a failure, and that a real handover between two
// threads completes.

#include "harness.h"

#include <cy/core/jobs/sync.h>

#include <atomic>

namespace {

using namespace cy;
using namespace cy::jobs;

}  // namespace

CY_TEST_CASE("a timed acquire reports the timeout rather than failing") {
    Semaphore semaphore(1);
    CY_CHECK(semaphore.try_acquire_for(1'000'000));
    // Nothing will post: the wait must end by the clock and say so.
    const i64 started = monotonic_now_ns();
    CY_CHECK_FALSE(semaphore.try_acquire_for(2'000'000));
    CY_CHECK(monotonic_now_ns() - started >= 1'000'000);
}

CY_TEST_CASE("an unset event times out and a set one does not wait") {
    Event event;
    CY_CHECK_FALSE(event.wait_for(2'000'000));
    event.set();
    CY_CHECK(event.wait_for(0));
    CY_CHECK(event.is_set());
}

CY_TEST_CASE("a semaphore hands over between two threads") {
    Semaphore ready;
    Semaphore done;
    std::atomic<u32> value{0};

    Thread producer("test.producer", [&ready, &done, &value] {
        value.store(42);
        ready.post();
        done.acquire();
        value.store(7);
    });

    ready.acquire();
    CY_CHECK_EQ(value.load(), 42u);
    done.post();
    producer.join();
    CY_CHECK_EQ(value.load(), 7u);
}

CY_TEST_CASE("an event releases every waiter that is already blocked") {
    Event event;
    std::atomic<u32> woken{0};

    Thread first("test.waiter.1", [&event, &woken] {
        event.wait();
        woken.fetch_add(1);
    });
    Thread second("test.waiter.2", [&event, &woken] {
        event.wait();
        woken.fetch_add(1);
    });

    // Long enough that both waiters are inside wait() rather than about to enter it. The
    // assertion below does not depend on that — set() is manual-reset, so a late arrival does not
    // block — but waking two blocked threads is the case worth exercising.
    Thread::sleep_for_ns(5'000'000);
    event.set();
    first.join();
    second.join();
    CY_CHECK_EQ(woken.load(), 2u);
}

CY_TEST_CASE("a condition variable wakes its waiter") {
    Mutex mutex;
    ConditionVariable condition;
    bool ready = false;

    Thread signaller("test.signaller", [&mutex, &condition, &ready] {
        Thread::sleep_for_ns(2'000'000);
        {
            ScopedLock<Mutex> held(mutex);
            ready = true;
        }
        condition.notify_one();
    });

    {
        ScopedLock<Mutex> held(mutex);
        condition.wait(mutex, [&ready] { return ready; });
        CY_CHECK(ready);
    }
    signaller.join();
}
