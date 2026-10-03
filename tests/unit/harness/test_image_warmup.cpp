// SPDX-License-Identifier: MIT
// The process image is paged in before the first case, so that case is not charged for it.
//
// The regression: `unit.animation_runtime_only` has one case, and on the hosted macOS runner it
// spent 1.115 ms of CPU against its 1 ms budget (CI run 37002996092) while the same body costs
// 0.022 ms run a second time in one process on Linux. The budget is the case's own CPU time, the
// kernel charges a page fault to the thread that takes it, and the first case in a binary was the
// one that first executed most of its code. `src/main.cpp` now calls `warm_process_image()` before
// doctest runs anything; see `src/image_warmup.cpp`.

#include <cy/test/test.h>

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#    include <fcntl.h>
#    include <link.h>
#    include <unistd.h>
#endif

#if defined(__linux__)
namespace {

/// A quarter of a megabyte of the image that nothing reads. This binary is otherwise so small that
/// doctest's own start-up and Linux's fault-around (sixteen pages mapped per fault) map all of it,
/// and the check below would pass with the warm-up removed. With it, the check fails without the
/// warm-up: most of these pages are further from any touched page than fault-around reaches.
/// Non-zero so that it is emitted in a file-backed read-only segment rather than in `.bss`; its
/// address is held in a volatile pointer, and its contents never read, so the linker keeps it and
/// no case maps it.
constexpr std::size_t kUntouchedBytes = std::size_t{256} * 1024;
struct Untouched {
    unsigned char bytes[kUntouchedBytes];
};
constexpr Untouched make_untouched() {
    Untouched table{};
    for (std::size_t index = 0; index < kUntouchedBytes; ++index) {
        table.bytes[index] = static_cast<unsigned char>((index * 131U) | 1U);
    }
    return table;
}
const Untouched kUntouched = make_untouched();
/// Holds the table's address where the optimiser cannot see through it, so the table is emitted.
const Untouched* volatile g_untouched = &kUntouched;

struct Residency {
    int pagemap = -1;
    std::size_t page_size = 0;
    std::size_t pages = 0;
    std::size_t absent = 0;
    bool readable = true;
};

/// Bit 63 of a `/proc/self/pagemap` entry: the page is in this process's page tables. Bit 62: it is
/// mapped but swapped out, which is still a page the first case would not have faulted in from the
/// executable. The flags are readable without privilege; only the frame number is withheld.
constexpr std::uint64_t kPresentOrSwapped = (1ULL << 63U) | (1ULL << 62U);

/// Reads the pagemap entry of every page of the main executable's readable segments WITHOUT
/// touching the pages themselves — reading them here would map them and make the check vacuous.
int count_main_object(dl_phdr_info* info, std::size_t /*size*/, void* data) {
    auto* state = static_cast<Residency*>(data);
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) & header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_R) == 0 || header.p_memsz == 0) {
            continue;
        }
        const std::uintptr_t begin = info->dlpi_addr + header.p_vaddr;
        const std::uintptr_t end = begin + header.p_memsz;
        for (std::uintptr_t page = begin / state->page_size; page * state->page_size < end;
             ++page) {
            std::uint64_t entry = 0;
            const auto offset = static_cast<off_t>(page * sizeof(entry));
            if (::pread(state->pagemap, &entry, sizeof(entry), offset) !=
                static_cast<ssize_t>(sizeof(entry))) {
                state->readable = false;
                return 1;
            }
            ++state->pages;
            if ((entry & kPresentOrSwapped) == 0) {
                ++state->absent;
            }
        }
    }
    return 1;  // the main executable is reported first; stop there
}

}  // namespace
#endif

CY_TEST_CASE("harness: every page of the test executable is mapped before the first case runs") {
#if defined(__linux__)
    CY_REQUIRE(cy::test::budget_warms_process_image());
    // The address only — reading the table would map it and make the check below vacuous.
    CY_REQUIRE(g_untouched != nullptr);
    Residency state;
    state.page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    state.pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    CY_REQUIRE(state.pagemap >= 0);
    ::dl_iterate_phdr(count_main_object, &state);
    ::close(state.pagemap);
    CY_REQUIRE(state.readable);
    CY_REQUIRE(state.pages > 0);
    // Zero, not "most": a page left for a case to fault in is a page a case pays for.
    CY_CHECK_EQ(state.absent, std::size_t{0});
#elif defined(__APPLE__) && defined(__LP64__)
    // macOS offers no unprivileged per-process page-table query, so this leg asserts that the walk
    // finds the executable's segments; the CPU it saves is measured by the suites' own budgets.
    CY_CHECK(cy::test::budget_warms_process_image());
    CY_CHECK_GT(cy::test::warm_process_image(), std::size_t{0});
#else
    CY_CHECK_FALSE(cy::test::budget_warms_process_image());
    CY_CHECK_EQ(cy::test::warm_process_image(), std::size_t{0});
#endif
}
