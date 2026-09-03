// The command queue for single-threaded servers. Task 3.2.9.
//
// The scenarios: a fire-and-forget render call returns immediately and is applied at the next drain;
// a synchronous cross-thread call is flagged in a development build. The third case is this
// module's own: a synchronous call from a job worker is refused outright, because a worker waiting
// for another thread is the same defect as a worker waiting for a disk.

#include "harness.h"

#include <cy/core/jobs/command_queue.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/sync.h>

#include <atomic>
#include <cstring>

namespace {

using namespace cy;
using namespace cy::jobs;
using cy::jobs::test::ScopedJobSystem;

constexpr CommandId kSetTransform = 1;
constexpr CommandId kQueryCount = 2;
constexpr CommandId kLargePayload = 3;

struct RenderState {
    std::atomic<u32> transforms{0};
    u64 last_entity = 0;
    u64 checksum = 0;
};

struct SetTransform {
    u64 entity;
    f32 position[3];
};

void set_transform(const void* arguments, usize bytes, void*, usize, void* user) noexcept {
    auto* state = static_cast<RenderState*>(user);
    if (bytes != sizeof(SetTransform)) {
        return;
    }
    SetTransform payload{};
    std::memcpy(&payload, arguments, sizeof(payload));
    state->last_entity = payload.entity;
    state->transforms.fetch_add(1);
}

void query_count(const void*, usize, void* result, usize result_bytes, void* user) noexcept {
    auto* state = static_cast<RenderState*>(user);
    if (result == nullptr || result_bytes < sizeof(u32)) {
        return;
    }
    const u32 count = state->transforms.load();
    std::memcpy(result, &count, sizeof(count));
}

void checksum_payload(const void* arguments, usize bytes, void*, usize, void* user) noexcept {
    auto* state = static_cast<RenderState*>(user);
    const auto* data = static_cast<const u8*>(arguments);
    u64 sum = 0;
    for (usize i = 0; i < bytes; ++i) {
        sum += data[i];
    }
    state->checksum = sum;
}

CommandQueue::Config small_config() {
    CommandQueue::Config config;
    config.capacity = 8;
    config.arena_bytes = 1024;
    config.max_commands = 8;
    return config;
}

}  // namespace

CY_TEST_CASE("a fire-and-forget call returns immediately and is applied at the drain") {
    RenderState state;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(small_config()).has_value());
    CY_REQUIRE(queue.register_command(kSetTransform, &set_transform, &state, "set_transform")
                   .has_value());

    const SetTransform payload{42, {1.0F, 2.0F, 3.0F}};
    CY_REQUIRE(queue.submit(kSetTransform, &payload, sizeof(payload)).has_value());

    // Nothing has run: the owner has not drained. This is the property the render thread depends
    // on — the caller was not made to wait for it.
    CY_CHECK_EQ(state.transforms.load(), 0u);
    CY_CHECK_EQ(queue.pending(), 1u);

    CY_CHECK_EQ(queue.drain(), 1u);
    CY_CHECK_EQ(state.transforms.load(), 1u);
    CY_CHECK_EQ(state.last_entity, 42u);
    CY_CHECK_EQ(queue.pending(), 0u);
}

CY_TEST_CASE("commands are replayed in submission order") {
    RenderState state;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(small_config()).has_value());
    CY_REQUIRE(queue.register_command(kSetTransform, &set_transform, &state, "set_transform")
                   .has_value());

    for (u64 entity = 1; entity <= 5; ++entity) {
        const SetTransform payload{entity, {0.0F, 0.0F, 0.0F}};
        CY_REQUIRE(queue.submit(kSetTransform, &payload, sizeof(payload)).has_value());
    }
    CY_CHECK_EQ(queue.drain(), 5u);
    CY_CHECK_EQ(state.last_entity, 5u);
}

CY_TEST_CASE("a payload larger than the inline limit goes to the arena, never the heap") {
    RenderState state;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(small_config()).has_value());
    CY_REQUIRE(
        queue.register_command(kLargePayload, &checksum_payload, &state, "large").has_value());

    u8 payload[200];
    for (usize i = 0; i < sizeof(payload); ++i) {
        payload[i] = 1;
    }
    CY_REQUIRE(sizeof(payload) > kInlineArgumentBytes);
    CY_REQUIRE(queue.submit(kLargePayload, payload, sizeof(payload)).has_value());
    CY_CHECK_EQ(queue.drain(), 1u);
    CY_CHECK_EQ(state.checksum, 200u);
}

CY_TEST_CASE("an oversized payload is refused rather than reaching for the heap") {
    RenderState state;
    CommandQueue::Config config = small_config();
    config.arena_bytes = 128;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(config).has_value());
    CY_REQUIRE(
        queue.register_command(kLargePayload, &checksum_payload, &state, "large").has_value());

    u8 payload[256] = {};
    const auto refused = queue.submit(kLargePayload, payload, sizeof(payload));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::OutOfRange);
    CY_CHECK_EQ(queue.refused(), 1u);
}

CY_TEST_CASE("a full queue reports rather than dropping") {
    RenderState state;
    CommandQueue::Config config = small_config();
    config.capacity = 2;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(config).has_value());
    CY_REQUIRE(queue.register_command(kSetTransform, &set_transform, &state, "set_transform")
                   .has_value());

    const SetTransform payload{1, {0.0F, 0.0F, 0.0F}};
    CY_CHECK(queue.submit(kSetTransform, &payload, sizeof(payload)).has_value());
    CY_CHECK(queue.submit(kSetTransform, &payload, sizeof(payload)).has_value());
    CY_CHECK_FALSE(queue.submit(kSetTransform, &payload, sizeof(payload)).has_value());
    CY_CHECK_EQ(queue.refused(), 1u);
}

CY_TEST_CASE("an unregistered command is refused at submission") {
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(small_config()).has_value());
    const auto refused = queue.submit(kSetTransform, nullptr, 0);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::NotFound);
}

CY_TEST_CASE("a synchronous call from the owning thread drains inline rather than deadlocking") {
    RenderState state;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(small_config()).has_value());
    CY_REQUIRE(queue.register_command(kSetTransform, &set_transform, &state, "set_transform")
                   .has_value());
    CY_REQUIRE(queue.register_command(kQueryCount, &query_count, &state, "query_count").has_value());
    queue.claim_owner();

    const SetTransform payload{7, {0.0F, 0.0F, 0.0F}};
    CY_REQUIRE(queue.submit(kSetTransform, &payload, sizeof(payload)).has_value());

    u32 count = 0;
    CY_REQUIRE(queue.submit_sync(kQueryCount, nullptr, 0, &count, sizeof(count)).has_value());
    CY_CHECK_EQ(count, 1u);
    CY_CHECK_EQ(queue.synchronous_calls(), 1u);
}

CY_TEST_CASE("a synchronous call blocks until the owner has run it and written the result back") {
    RenderState state;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(small_config()).has_value());
    CY_REQUIRE(queue.register_command(kQueryCount, &query_count, &state, "query_count").has_value());
    state.transforms.store(11);

    // A stand-in for the owning thread: it drains until it has served the call.
    Event served;
    std::atomic<bool> stop{false};
    Thread owner("test.owner", [&queue, &stop] {
        queue.claim_owner();
        while (!stop.load()) {
            if (queue.drain() > 0) {
                continue;
            }
            Thread::sleep_for_ns(100'000);
        }
    });
    // Give the owner thread time to claim ownership before the caller decides it is not the owner.
    Thread::sleep_for_ns(2'000'000);

    u32 count = 0;
    CY_REQUIRE(queue.submit_sync(kQueryCount, nullptr, 0, &count, sizeof(count)).has_value());
    CY_CHECK_EQ(count, 11u);

    stop.store(true);
    owner.join();
    served.set();
}

CY_TEST_CASE("a synchronous call from a job worker is refused, not merely warned about") {
    // This module's own strengthening of the specification's rule, and it follows from it directly:
    // a worker that waits for another thread's drain has left the pool for as long as that takes,
    // which is the defect `begin_blocking_region` exists to report.
    ScopedJobSystem system(2);
    CY_REQUIRE(system.started());
    reset_blocking_violations();

    RenderState state;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(small_config()).has_value());
    CY_REQUIRE(queue.register_command(kQueryCount, &query_count, &state, "query_count").has_value());

    struct Attempt {
        CommandQueue* queue;
        std::atomic<u32> code{0};
    };
    Attempt attempt{&queue, {}};

    auto handle = system->submit(
        [](const TaskContext&, void* user) noexcept {
            auto* state_of_attempt = static_cast<Attempt*>(user);
            u32 count = 0;
            const Status result =
                state_of_attempt->queue->submit_sync(kQueryCount, nullptr, 0, &count,
                                                     sizeof(count));
            state_of_attempt->code.store(result.has_value()
                                             ? 0u
                                             : static_cast<u32>(result.error().code));
        },
        &attempt, "sync-from-worker");
    CY_REQUIRE(handle.has_value());
    system->wait(handle.value());

    CY_CHECK_EQ(attempt.code.load(), static_cast<u32>(ErrorCode::Unsupported));
    CY_CHECK_EQ(blocking_violations(), 1u);
    reset_blocking_violations();
}

CY_TEST_CASE("repeated synchronous calls in one frame are counted so they can be identified") {
    RenderState state;
    CommandQueue::Config config = small_config();
    config.synchronous_warning_threshold = 1;
    CommandQueue queue;
    CY_REQUIRE(queue.initialize(config).has_value());
    CY_REQUIRE(queue.register_command(kQueryCount, &query_count, &state, "query_count").has_value());
    queue.claim_owner();

    queue.begin_frame();
    u32 count = 0;
    for (u32 i = 0; i < 3; ++i) {
        CY_REQUIRE(queue.submit_sync(kQueryCount, nullptr, 0, &count, sizeof(count)).has_value());
    }
    CY_CHECK_EQ(queue.synchronous_calls_this_frame(), 3u);
    CY_CHECK_EQ(queue.synchronous_calls(), 3u);

    queue.begin_frame();
    CY_CHECK_EQ(queue.synchronous_calls_this_frame(), 0u);
    CY_CHECK_EQ(queue.synchronous_calls(), 3u);
}
