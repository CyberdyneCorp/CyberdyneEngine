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

#    include <cstring>
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

/// Counts, into `state`, the pages of one object's readable segments that are not in this
/// process's page tables, reading `/proc/self/pagemap` WITHOUT touching the pages themselves —
/// reading them here would map them and make the check vacuous.
void count_object(const dl_phdr_info* info, Residency& state) {
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) & header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_R) == 0 || header.p_memsz == 0) {
            continue;
        }
        const std::uintptr_t begin = info->dlpi_addr + header.p_vaddr;
        const std::uintptr_t end = begin + header.p_memsz;
        for (std::uintptr_t page = begin / state.page_size; page * state.page_size < end; ++page) {
            std::uint64_t entry = 0;
            const auto offset = static_cast<off_t>(page * sizeof(entry));
            if (::pread(state.pagemap, &entry, sizeof(entry), offset) !=
                static_cast<ssize_t>(sizeof(entry))) {
                state.readable = false;
                return;
            }
            ++state.pages;
            if ((entry & kPresentOrSwapped) == 0) {
                ++state.absent;
            }
        }
    }
}

/// The main executable, which `dl_iterate_phdr` reports first; stop there.
int count_main_object(dl_phdr_info* info, std::size_t /*size*/, void* data) {
    count_object(info, *static_cast<Residency*>(data));
    return 1;
}

/// The C math library, found by name among the shared objects.
int count_math_library(dl_phdr_info* info, std::size_t /*size*/, void* data) {
    const char* name = info->dlpi_name;
    const char* base = name != nullptr ? std::strrchr(name, '/') : nullptr;
    base = base != nullptr ? base + 1 : name;
    if (base != nullptr && std::strncmp(base, "libm.so", 7) == 0) {
        count_object(info, *static_cast<Residency*>(data));
        return 1;
    }
    return 0;
}

/// The residency of whatever `callback` walks.
Residency residency(int (*callback)(dl_phdr_info*, std::size_t, void*)) {
    Residency state;
    state.page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    state.pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (state.pagemap < 0) {
        state.readable = false;
        return state;
    }
    ::dl_iterate_phdr(callback, &state);
    ::close(state.pagemap);
    return state;
}

}  // namespace
#endif

CY_TEST_CASE("harness: every page of the test executable is mapped before the first case runs") {
#if defined(__linux__)
    CY_REQUIRE(cy::test::budget_warms_process_image());
    // The address only — reading the table would map it and make the check below vacuous.
    CY_REQUIRE(g_untouched != nullptr);
    const Residency state = residency(count_main_object);
    CY_REQUIRE(state.readable);
    CY_REQUIRE(state.pages > 0);
    // Zero, not "most": a page left for a case to fault in is a page a case pays for.
    CY_CHECK_EQ(state.absent, std::size_t{0});
#else
    // Apple platforms walk libsystem_m's code too, but offer no unprivileged page-table query to
    // check it with; the case it was added for is `unit.determinism`'s, which runs on that leg.
    CY_TEST_MESSAGE("the residency check reads /proc/self/pagemap, which is Linux's");
#endif
}

CY_TEST_CASE("harness: the C math library's pages are mapped before the first case runs") {
    // `unit.determinism`'s "the twelve replacements agree with <cmath>" is the first case to call
    // most of libm, and on the hosted macOS runner it went over its 1 ms budget at 1.12 to 1.38 ms
    // paying for those first touches. Nothing before this case calls into libm beyond what the
    // loader and doctest's start-up do, so without the warm-up most of its pages are unmapped here.
#if defined(__linux__)
    const Residency state = residency(count_math_library);
    CY_REQUIRE(state.readable);
    CY_REQUIRE(state.pages > 0);  // the binary loads libm, through libstdc++ if nothing else
    CY_CHECK_EQ(state.absent, std::size_t{0});
#else
    // Apple platforms walk libsystem_m's code too, but offer no unprivileged page-table query to
    // check it with; the case it was added for is `unit.determinism`'s, which runs on that leg.
    CY_TEST_MESSAGE("the residency check reads /proc/self/pagemap, which is Linux's");
#endif
}
