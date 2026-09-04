// The two concurrency properties the value layer claims. Integration, because it starts threads.
//
// `core-type-system` — "Interning is thread-safe": two threads interning the same string both
// receive the same `Name` and only one entry is stored. And "Cross-thread handle validity": a
// handle resolved concurrently with an unrelated allocation in the same pool resolves safely,
// because pool growth is published atomically.
//
// Neither of these is a test that proves the absence of a race by itself — a race that does not
// happen to occur is a green run. What makes them worth their second is that they are the suite
// TSan is pointed at (`core-jobs-and-concurrency` task 3.2.12 brings the CI job): under the
// detector, an unsynchronised read of a chunk pointer or of the intern table is reported whether or
// not the timing happened to expose it.

#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/core/values/var.h>

#include <cy/test/test.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

CY_HANDLE_TAG(Concurrent);

constexpr int kThreads = 8;

}  // namespace

CY_TEST_CASE("Name: concurrent interning of the same text yields one entry") {
    // A text this process has certainly not interned yet, so the entry count is a measurement.
    const std::string text = "values.threads.contended.name";
    const cy::NameTableStats before = cy::name_table_stats();

    std::vector<cy::Name> results(kThreads);
    std::vector<std::thread> threads;
    std::atomic<int> ready{0};

    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            // Spin until every thread is at the same point, so the interning calls actually
            // overlap rather than being serialised by thread startup.
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < kThreads) {
            }
            results[static_cast<cy::usize>(i)] = cy::Name::intern(text);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const cy::NameTableStats after = cy::name_table_stats();
    for (const cy::Name name : results) {
        CY_CHECK(name == results[0]);
        CY_CHECK_FALSE(name.is_empty());
    }
    CY_CHECK_EQ(after.entries, before.entries + 1);
    CY_CHECK_EQ(after.insertions, before.insertions + 1);
    CY_CHECK_EQ(results[0].text(), std::string_view(text));
}

CY_TEST_CASE("GenerationTable: resolving while the pool grows is safe") {
    cy::GenerationTable table(64);

    // One handle every reader will hold across the growth. Its slot is in the first chunk, which
    // stays put; what moves is the set of chunks, and that is what publication has to get right.
    const cy::Expected<cy::Handle<ConcurrentTag>, cy::Error> stable =
        table.allocate_handle<ConcurrentTag>();
    CY_REQUIRE(stable.has_value());

    std::atomic<bool> stop{false};
    std::atomic<cy::u64> resolutions{0};
    std::atomic<int> mismatches{0};

    std::vector<std::thread> readers;
    readers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        readers.emplace_back([&]() {
            // A floor of a thousand resolutions before the stop flag is consulted. Without it, a
            // loaded machine can finish the allocation loop below before a reader's first
            // iteration, and the test then passes having measured nothing — which is worse than
            // failing, because it is a green run that proves nothing about concurrent resolution.
            cy::u64 local = 0;
            do {
                if (!table.is_live(*stable)) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                ++local;
            } while (local < 1000 || !stop.load(std::memory_order_acquire));
            resolutions.fetch_add(local, std::memory_order_relaxed);
        });
    }

    // Meanwhile, grow the pool well past its first chunk.
    std::vector<cy::Handle<ConcurrentTag>> allocated;
    allocated.reserve(4096);
    for (int i = 0; i < 4096; ++i) {
        const cy::Expected<cy::Handle<ConcurrentTag>, cy::Error> handle =
            table.allocate_handle<ConcurrentTag>();
        CY_REQUIRE(handle.has_value());
        allocated.push_back(*handle);
    }

    stop.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }

    CY_CHECK_EQ(mismatches.load(std::memory_order_relaxed), 0);
    CY_CHECK_GE(resolutions.load(std::memory_order_relaxed),
                static_cast<cy::u64>(kThreads) * 1000u);
    for (const cy::Handle<ConcurrentTag> handle : allocated) {
        CY_CHECK(table.is_live(handle));
    }
}

CY_TEST_CASE("Var: heap blocks are shared across threads without a race on the count") {
    // A single value copied and destroyed on many threads at once. The reference count is the only
    // shared mutable state, and this is the shape that exposes a non-atomic one.
    const cy::Var shared = cy::Var::from_string("values.threads.shared.block");

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&shared]() {
            for (int n = 0; n < 2000; ++n) {
                // The copy is the subject: retaining and releasing the block from many threads at
                // once is the race this test exists to expose.
                // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
                const cy::Var copy = shared;
                (void)copy.as_string();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CY_CHECK_FALSE(shared.is_shared());
    CY_CHECK_EQ(*shared.as_string(), std::string_view("values.threads.shared.block"));
}
