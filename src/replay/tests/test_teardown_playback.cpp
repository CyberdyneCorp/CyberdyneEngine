// M9 SECTION 3 — TEARDOWN UNDER LOAD, FOR THE STRUCTURES SECTION 3 ADDED.
//
// `tests/test_teardown.cpp` covers section 1's bounded structures. This file covers section 3's,
// and there is exactly one that owns heap: `PresentationTrackSet`, which hand-places each track and
// must destroy each one. `CrashArtefact` and `LockstepSession` own `Array`s, which is less
// interesting but is driven past a bound here anyway, because "less interesting" is what every leak
// was called before it was found.
//
// The rule this file follows is the one that file established: **drive it past its bound, destroy
// everything, and ask the allocator what is left.** The totals are asserted as well as the
// outstanding counts, so a case that allocated nothing cannot report the same zeroes as a case that
// allocated and freed.
//
// THE MUTATION THAT PROVES IT, RUN RATHER THAN ASSERTED: delete the destructor and the
// `deallocate` from `PresentationTrackSet::clear()`.
//
//     test_teardown_playback.cpp:130: ERROR: CHECK_EQ( counting.live_blocks(), u64{0} )
//       values: CHECK_EQ( 122, 0 )
//     test_teardown_playback.cpp:131: ERROR: CHECK_EQ( counting.live_bytes(), u64{0} )
//       values: CHECK_EQ( 341552, 0 )

#include "fixture.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/replay/crash.h>
#include <cy/replay/lockstep.h>
#include <cy/replay/playback.h>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::usize;

namespace {

/// Counts blocks and bytes outstanding. Forwards everything; remembers nothing about addresses.
///
/// The same instrument `tests/test_teardown.cpp` uses, and for the reason its header records at
/// length: `cy::TrackingAllocator` scores a false double free whenever the upstream hands back an
/// address it recently returned, which is what an allocator does constantly.
class CountingAllocator final : public cy::Allocator {
public:
    CountingAllocator() noexcept
        : cy::Allocator(cy::MemoryDomain::World, "replay-section3-teardown"),
          upstream_(cy::system_allocator(cy::MemoryDomain::World)) {}

    [[nodiscard]] u64 live_blocks() const noexcept { return live_blocks_; }
    [[nodiscard]] u64 live_bytes() const noexcept { return live_bytes_; }
    [[nodiscard]] u64 total_blocks() const noexcept { return total_blocks_; }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override {
        void* block = upstream_.allocate(size, alignment);
        if (block != nullptr) {
            ++live_blocks_;
            ++total_blocks_;
            live_bytes_ += size;
        }
        return block;
    }

    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override {
        void* block = upstream_.reallocate(pointer, old_size, new_size, alignment);
        if (block != nullptr) {
            live_bytes_ = live_bytes_ + new_size - old_size;
            ++total_blocks_;
        }
        return block;
    }

    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override {
        if (pointer != nullptr) {
            if (live_blocks_ == 0 || size > live_bytes_) {
                ++oversized_frees_;
            } else {
                --live_blocks_;
                live_bytes_ -= size;
            }
        }
        upstream_.deallocate(pointer, size, alignment);
    }

private:
    cy::Allocator& upstream_;
    u64 live_blocks_ = 0;
    u64 live_bytes_ = 0;
    u64 total_blocks_ = 0;
    u64 oversized_frees_ = 0;
};

constexpr u32 kTracks = 60;
constexpr u32 kSamplesPerTrack = 40;

}  // namespace

CY_TEST_CASE("replay: a track set frees every track it placed, and every sample in it") {
    CountingAllocator counting;
    {
        cy::replay::PresentationTrackSet tracks(counting);
        // ONE BUFFER PER TRACK. `add()` stores the pointer rather than copying the text — the same
        // convention `determinism::HashNode::name` follows — so a single reused stack buffer would
        // give every track the same name and the second `add()` would refuse it as a duplicate.
        // Found by writing this case, which is what a teardown suite is for.
        char names[kTracks][8] = {};
        for (u32 index = 0; index < kTracks; ++index) {
            names[index][0] = 't';
            names[index][1] = static_cast<char>('a' + (index / 26));
            names[index][2] = static_cast<char>('a' + (index % 26));
            auto track =
                tracks.add(index % 2 == 0 ? cy::replay::PresentationTrackKind::CameraDirection
                                          : cy::replay::PresentationTrackKind::Annotation,
                           names[index]);
            CY_REQUIRE(track.has_value());
            for (u32 sample = 0; sample < kSamplesPerTrack; ++sample) {
                cy::replay::PresentationSample value;
                value.tick = sample * 3ULL;
                value.subject = index;
                value.payload_size = cy::replay::kMaxTrackPayload;
                CY_REQUIRE((*track)->append(value).has_value());
            }
        }
        CY_CHECK_EQ(tracks.size(), kTracks);
        CY_CHECK(counting.live_blocks() > 0);

        // Cleared and refilled once, because a `clear()` that leaked would otherwise be hidden by
        // the destructor doing the same work correctly.
        tracks.clear();
        CY_CHECK_EQ(tracks.size(), 0U);
        auto again = tracks.add(cy::replay::PresentationTrackKind::Marker, "chapters");
        CY_REQUIRE(again.has_value());
        CY_REQUIRE((*again)->append(cy::replay::PresentationSample{1, 1, 0, {}}).has_value());
    }
    // The track names are string literals the set does not own; the tracks and their samples are
    // everything it does.
    CY_CHECK_EQ(counting.live_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_bytes(), u64{0});
    CY_CHECK(counting.total_blocks() >= kTracks);
}

CY_TEST_CASE("replay: a crash artefact and a lockstep session leave nothing behind") {
    CountingAllocator counting;
    {
        RecordLog log(counting, manifest());
        cy::replay::CrashReplayBuffer ring(counting);
        CY_REQUIRE(ring.reserve(128).has_value());

        // Past the bound: 2 000 records through a ring of 128, so the ring wraps fifteen times and
        // the artefact is assembled over the survivors.
        for (u64 tick = 0; tick < 500; ++tick) {
            ring.push(command_record(tick, 0, 1));
            ring.push(command_record(tick, 1, 2));
            ring.push(hash_record(tick, tick * 7));
            ring.push(checkpoint_record(tick, tick));
        }
        CY_CHECK(ring.overwritten() > 0);

        cy::replay::CrashArtefact artefact(counting);
        CY_REQUIRE(artefact
                       .assemble(manifest(), ring, cy::replay::CrashTrigger::Assertion,
                                 cy::determinism::SimulationPoint{cy::determinism::Epoch{}, 499})
                       .has_value());
        cy::Array<cy::u8> bytes(counting);
        CY_REQUIRE(artefact.write(bytes).has_value());

        cy::replay::CrashArtefact reloaded(counting);
        cy::replay::RejectReason why = cy::replay::RejectReason::None;
        CY_REQUIRE(reloaded.read(bytes.span(), why).has_value());
        // Assembled twice over, so a second `assemble()` that forgot to clear would show up.
        CY_REQUIRE(artefact
                       .assemble(manifest(), ring, cy::replay::CrashTrigger::Crash,
                                 cy::determinism::SimulationPoint{cy::determinism::Epoch{}, 499})
                       .has_value());

        cy::replay::LockstepConfiguration configuration;
        cy::replay::LockstepSession session(counting, log, configuration);
        for (u64 participant = 1; participant <= 32; ++participant) {
            CY_REQUIRE(session.add_participant(participant * 0x1001).has_value());
        }
        // Past the hash history's bound (64), so its shift-and-drop path runs many times.
        for (u64 tick = 0; tick < 300; ++tick) {
            CY_REQUIRE(session.publish_hash(tick, tick * 31, cy::determinism::Epoch{}).has_value());
        }
    }
    CY_CHECK_EQ(counting.live_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_bytes(), u64{0});
    CY_CHECK(counting.total_blocks() > 0);
}
