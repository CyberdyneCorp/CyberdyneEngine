// A save killed with SIGKILL at every phase of the write, over real files. Task 6.4.
//
// This is M6's exit criterion in its exacting form: not "an interrupted save is handled" but "the
// process is killed at each step of the sequence, and what is on disk afterwards is either the
// previous generation whole or the new one whole". Simulating the failure inside the process — the
// write-budget case in test_archive.cpp — proves the sequence; it cannot prove that a process
// vanishing mid-syscall leaves the same thing behind, because the kernel is the half being tested.
//
// HOW IT WORKS. The parent commits generation 1. It then forks; the child opens the same store,
// installs a phase observer, and commits generation 2. When the child reaches the phase under test
// it writes one byte down a pipe and then blocks forever. The parent, woken by that byte, sends
// SIGKILL — which cannot be caught, blocked or deferred — reaps the child, and reads the store
// back. The child never runs a destructor, never flushes a buffer, and never unlinks a temporary.
//
// WHY THE KILL LANDS BETWEEN PHASES RATHER THAN INSIDE ONE. The one write whose interruption would
// be interesting is the pointer, and it is interruption-proof by construction: `write_atomic`
// writes a temporary, flushes it, and renames it over the pointer, and rename is atomic on every
// filesystem the engine targets. There is no instant at which `current` holds half a generation
// number. The phases are where a partial SEQUENCE is observable, and that is what this case walks.

#include <cy/save/archive.h>
#include <cy/save/storage.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <cstdio>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#    define CY_SAVE_HAS_FORK 1
#    include <sys/wait.h>
#    include <unistd.h>
#    include <csignal>
#else
#    define CY_SAVE_HAS_FORK 0
#endif

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kVillage{0x0A00'0000'0000'0001ULL};
constexpr RegionKey kQuarry{0x0A00'0000'0000'0002ULL};

SaveIdentity identity() noexcept {
    SaveIdentity id;
    id.build_id = "6.0.0+test";
    id.project = AssetId(1, 1);
    return id;
}

void record_health(Overlay& overlay, RegionKey region, PersistentId id, u32 revives) {
    Health health;
    health.revives = revives;
    CY_REQUIRE(overlay.record_component(region, id, health_type(), &health, 1).has_value());
}

u32 revives_of(const Overlay& overlay, RegionKey region, PersistentId id) noexcept {
    const ComponentDelta* delta =
        overlay.find_component(region, id, reflect::TypeId(kHealthTypeId));
    if (delta == nullptr) {
        return 0xFFFF'FFFFU;
    }
    u32 value = 0;
    const Span<const u8> bytes = delta->record.bytes(reflect::FieldId(kHealthRevives));
    if (!serialize::decode_scalar(serialize::WireType::U32, bytes.data(),
                                  static_cast<u32>(bytes.size()), &value)) {
        return 0xFFFF'FFFEU;
    }
    return value;
}

#if CY_SAVE_HAS_FORK

/// What the child's phase observer is given. Plain data: it is read after a fork, on a path where
/// almost nothing else is safe to touch.
struct KillPoint {
    WritePhase phase;
    int pipe_write;
};

void stop_at_phase(void* user, WritePhase phase) noexcept {
    const KillPoint& point = *static_cast<const KillPoint*>(user);
    if (phase != point.phase) {
        return;
    }
    const char ready = 'k';
    const ssize_t written = ::write(point.pipe_write, &ready, 1);
    (void)written;
    // Block until the signal arrives. `pause()` returns when a signal is delivered; SIGKILL never
    // is delivered to a handler, so the loop is a formality that keeps this from ever proceeding
    // into the next phase if a stray signal arrives first.
    for (;;) {
        ::pause();
    }
}

/// Commit generation 2 in a child process, and stop dead at `phase`. Never returns in the child.
[[noreturn]] void child_commit(const char* root, WritePhase phase, int pipe_write) noexcept {
    FilesystemBackend store;
    if (!store.open(root)) {
        ::_exit(2);
    }
    SaveArchive archive(test_allocator());
    if (!archive.open(store)) {
        ::_exit(3);
    }
    KillPoint point{phase, pipe_write};
    archive.set_phase_observer(stop_at_phase, &point);

    Overlay overlay(test_allocator());
    Health health;
    health.revives = 99;
    if (!overlay.record_component(kVillage, entity(1), health_type(), &health, 1)) {
        ::_exit(4);
    }
    health.revives = 5;
    if (!overlay.record_component(kQuarry, entity(10), health_type(), &health, 1)) {
        ::_exit(5);
    }
    if (!archive.commit(overlay, identity())) {
        ::_exit(6);
    }
    // Reached only if the observer never fired, which is a defect in the archive rather than in the
    // test: `_exit` rather than `exit`, so no destructor of the parent's state runs in the child.
    ::_exit(7);
}

#endif  // CY_SAVE_HAS_FORK

}  // namespace

#if CY_SAVE_HAS_FORK

CY_TEST_CASE("a save killed with SIGKILL at any phase leaves a whole generation behind") {
    const WritePhase phases[] = {WritePhase::ChunksWritten, WritePhase::ChunksVerified,
                                 WritePhase::ManifestWritten, WritePhase::PointerSwitched};
    for (const WritePhase phase : phases) {
        cy::test::TempDir directory("save_kill_nine");
        CY_REQUIRE(directory.valid());

        // Generation 1: the save that must survive.
        {
            FilesystemBackend store;
            CY_REQUIRE(store.open(directory.path().c_str()).has_value());
            SaveArchive archive(test_allocator());
            CY_REQUIRE(archive.open(store).has_value());
            Overlay overlay(test_allocator());
            record_health(overlay, kVillage, entity(1), 3);
            record_health(overlay, kQuarry, entity(10), 5);
            CY_REQUIRE(archive.commit(overlay, identity()).has_value());
        }

        int ready[2] = {-1, -1};
        CY_REQUIRE_EQ(::pipe(ready), 0);
        const pid_t child = ::fork();
        CY_REQUIRE(child >= 0);
        if (child == 0) {
            ::close(ready[0]);
            child_commit(directory.path().c_str(), phase, ready[1]);
        }

        ::close(ready[1]);
        char signal_byte = 0;
        const ssize_t read_bytes = ::read(ready[0], &signal_byte, 1);
        ::close(ready[0]);
        CY_REQUIRE_EQ(read_bytes, 1);  // zero means the child died before reaching the phase

        CY_REQUIRE_EQ(::kill(child, SIGKILL), 0);
        int status = 0;
        CY_REQUIRE_EQ(::waitpid(child, &status, 0), child);
        CY_CHECK(WIFSIGNALED(status));
        CY_CHECK_EQ(WTERMSIG(status), SIGKILL);

        // What is on disk now. Nothing tidies up first: this is the state a player's machine is in
        // after the power went out.
        FilesystemBackend store;
        CY_REQUIRE(store.open(directory.path().c_str()).has_value());
        SaveArchive archive(test_allocator());
        CY_REQUIRE(archive.open(store).has_value());

        Overlay read(test_allocator());
        LoadPolicy policy;
        LoadReport report;
        CY_REQUIRE(archive.load(policy, read, report).has_value());
        CY_CHECK_FALSE(report.failed());
        CY_CHECK_EQ(report.generations_skipped, 0U);

        // Before the switch, the previous generation; after it, the new one. Never a mixture, and
        // never a save that fails to load.
        const u32 expected = phase == WritePhase::PointerSwitched ? 99U : 3U;
        CY_CHECK_EQ(revives_of(read, kVillage, entity(1)), expected);
        // The region the interrupted save did not change is intact either way.
        CY_CHECK_EQ(revives_of(read, kQuarry, entity(10)), 5U);

        // And the store is still writable: a save after a kill commits, and the debris the killed
        // process left — orphan chunks, an unreferenced manifest, a temporary — is collected rather
        // than mistaken for content.
        Overlay next(test_allocator());
        record_health(next, kVillage, entity(1), 123);
        CY_REQUIRE(archive.commit(next, identity()).has_value());
        Overlay after(test_allocator());
        LoadReport second_report;
        CY_REQUIRE(archive.load(policy, after, second_report).has_value());
        CY_CHECK_EQ(revives_of(after, kVillage, entity(1)), 123U);
    }
}

#else

CY_TEST_CASE("a save killed with SIGKILL at any phase leaves a whole generation behind") {
    // The criterion is a property of the platform's process model as much as of this code, and this
    // build has no fork. Reported as a skip rather than a pass: an untested guarantee that reads as
    // green is worse than one that says it was not tested.
    CY_TEST_MESSAGE("skipped: this platform has no fork(), so SIGKILL cannot be staged here");
}

#endif  // CY_SAVE_HAS_FORK
