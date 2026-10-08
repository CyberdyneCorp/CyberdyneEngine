// SPDX-License-Identifier: MIT
// Paging in the test executable's own image before the first case runs. See cy/test/test.h,
// `warm_process_image`.
//
// WHY. A case's budget is the CPU its own thread spends, and the kernel charges a page fault to the
// thread that takes it. The first case in a binary is the first to execute most of the binary's
// code, so it took every fault on that code — work that belongs to loading the process, not to the
// case, and that the same body does not repeat in a second case. CTest runs each binary once,
// freshly built, so on a CI leg that first run is the only run.
//
// On the hosted macOS runner, where a first touch of a code page is a 16 KiB fault that also checks
// the page's code signature, `unit.animation_runtime_only`'s one case spent 1.115 ms of CPU against
// its 1 ms budget (CI run 37002996092). Measured on that runner while idle, the first run of the
// freshly built binary cost 0.177 ms, later runs 0.035 to 0.058 ms, and with this warm-up the first
// run cost 0.025 ms. The CI figure was that same first run, on a runner busy with the rest of the
// suite. In the sixty CI runs before 37002996092, five of the thirteen distinct unit cases
// that went over budget on that leg were the first case of their binary. tests/harness/README.md
// has the table.
//
// WHAT. One read per page of every mapped segment of the main executable — every test binary links
// the engine statically, so that is all of the code a case runs apart from the C and C++ runtimes —
// and of the C MATH LIBRARY's code. A read is enough: whichever access first touches a page takes
// the fault. Nothing is written.
//
// WHY THE MATH LIBRARY TOO. The other system libraries are paged in by the dynamic loader and by
// doctest's own start-up before this runs; libm is not, because nothing calls `acosh` or `expm1`
// until a case does. `unit.determinism`'s "the twelve replacements agree with <cmath>" is the first
// case to call most of libm, and it costs 0.03 to 0.12 ms on Linux and went over its 1 ms budget at
// 1.12 to 1.38 ms on the hosted macOS runner (CI runs 36943266067, 36970436016 and 37091129342),
// where each first touch is a 16 KiB fault. Only libm is walked, by name, rather than every
// library: on Apple platforms the system libraries live in one shared cache of several hundred
// megabytes.
//
// WHAT IT DOES NOT DO. It does not warm data a case allocates, caches, branch predictors or the
// governor's clock — those are the case's, or the calibration's (budget.cpp). Windows is not
// walked: its CPU clock is cycle-counted (budget.cpp), and no first-case overrun has been seen on
// that leg.

#include <cy/test/test.h>

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#    include <link.h>
#    include <unistd.h>

#    include <cstring>
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <mach-o/loader.h>
#    include <unistd.h>
#    include <cstring>
#endif

namespace cy::test {

/// `volatile` so the reads are not elided: a warm-up the optimiser removes reports pages it never
/// touched.
///
/// Not instrumented by AddressSanitizer: a page's first byte can sit in the redzone ASan places
/// after a global (a string literal in `unit.jobs`, CI run 37076205289), and the read is of the
/// mapped segment, not of any object.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_sanitize_address))
#elif defined(_MSC_VER)
__declspec(no_sanitize_address)
#endif
std::size_t touch_pages(std::uintptr_t begin, std::size_t size, std::size_t page_size) noexcept {
    if (size == 0 || page_size == 0) {
        return 0;
    }
    const std::uintptr_t first = begin & ~(static_cast<std::uintptr_t>(page_size) - 1U);
    const std::uintptr_t end = begin + size;
    std::size_t pages = 0;
    unsigned char sink = 0;
    for (std::uintptr_t page = first; page < end; page += page_size) {
        // The loader reports segments as integers (`dlpi_addr + p_vaddr`, a slid `vmaddr`); this
        // is the one place they become an address, and only to read from a mapped segment.
        const std::uintptr_t address = page < begin ? begin : page;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto* byte = reinterpret_cast<const volatile unsigned char*>(address);
        sink = static_cast<unsigned char>(sink ^ *byte);
        ++pages;
    }
    // Stored and read back, as `reference_workload_ns` in budget.cpp does: the store keeps the
    // reads observable, the read keeps `-Wunused-but-set-variable` satisfied rather than silenced.
    static volatile unsigned char observed = 0;
    observed = sink;
    return pages + (observed & 0U);
}

namespace {

#if defined(__linux__)

struct WalkState {
    std::size_t page_size = 0;
    std::size_t pages = 0;
    bool main_seen = false;
};

/// Whether `name` is the C math library: `/lib/x86_64-linux-gnu/libm.so.6` and its kin.
[[nodiscard]] bool is_math_library(const char* name) noexcept {
    if (name == nullptr) {
        return false;
    }
    const char* base = std::strrchr(name, '/');
    base = base != nullptr ? base + 1 : name;
    return std::strncmp(base, "libm.so", 7) == 0 || std::strncmp(base, "libm-", 5) == 0;
}

/// `dl_iterate_phdr` reports the main executable first, then every shared object. The first is
/// walked whole; of the rest, only the C math library's readable segments are.
int warm_object(dl_phdr_info* info, std::size_t /*size*/, void* data) noexcept {
    auto* state = static_cast<WalkState*>(data);
    const bool main_object = !state->main_seen;
    state->main_seen = true;
    if (!main_object && !is_math_library(info->dlpi_name)) {
        return 0;
    }
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) & header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_R) == 0) {
            continue;
        }
        state->pages += touch_pages(static_cast<std::uintptr_t>(info->dlpi_addr + header.p_vaddr),
                                    static_cast<std::size_t>(header.p_memsz), state->page_size);
    }
    return 0;
}

std::size_t warm_image() noexcept {
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }
    WalkState state;
    state.page_size = static_cast<std::size_t>(page_size);
    ::dl_iterate_phdr(warm_object, &state);
    return state.pages;
}

#elif defined(__APPLE__) && defined(__LP64__)

/// The readable segments of loaded image `image`, walked at their slid addresses. `__PAGEZERO` (no
/// access) and `__LINKEDIT` (the loader's metadata, never executed — and in the shared cache, one
/// region every system library shares) are not; `code_only` keeps the executable segments alone,
/// which is what a system library in the shared cache needs warming. Each load command is copied
/// out rather than cast in place: the commands are only four-byte aligned, and a cast to a 64-bit
/// segment command would raise the alignment `-Wcast-align` checks.
std::size_t warm_loaded_image(std::uint32_t image, bool code_only) noexcept {
    const mach_header* raw = ::_dyld_get_image_header(image);
    if (raw == nullptr || raw->magic != MH_MAGIC_64) {
        return 0;
    }
    const auto* cursor = reinterpret_cast<const unsigned char*>(raw);
    mach_header_64 header{};
    std::memcpy(&header, cursor, sizeof(header));
    cursor += sizeof(header);
    const std::intptr_t slide = ::_dyld_get_image_vmaddr_slide(image);
    const auto page_size = static_cast<std::size_t>(::getpagesize());
    std::size_t pages = 0;
    for (std::uint32_t index = 0; index < header.ncmds; ++index) {
        load_command command{};
        std::memcpy(&command, cursor, sizeof(command));
        if (command.cmd == LC_SEGMENT_64) {
            segment_command_64 segment{};
            std::memcpy(&segment, cursor, sizeof(segment));
            const bool wanted = code_only ? (segment.initprot & VM_PROT_EXECUTE) != 0
                                          : (segment.initprot & VM_PROT_READ) != 0;
            if (wanted &&
                std::strncmp(segment.segname, SEG_LINKEDIT, sizeof(segment.segname)) != 0) {
                const auto address = static_cast<std::intptr_t>(segment.vmaddr) + slide;
                pages += touch_pages(static_cast<std::uintptr_t>(address),
                                     static_cast<std::size_t>(segment.vmsize), page_size);
            }
        }
        cursor += command.cmdsize;
    }
    return pages;
}

/// Whether `name` is the C math library, `/usr/lib/system/libsystem_m.dylib`.
[[nodiscard]] bool is_math_library(const char* name) noexcept {
    if (name == nullptr) {
        return false;
    }
    const char* base = std::strrchr(name, '/');
    base = base != nullptr ? base + 1 : name;
    return std::strcmp(base, "libsystem_m.dylib") == 0;
}

/// Image 0 is the main executable, walked whole; the math library's code is walked after it.
std::size_t warm_image() noexcept {
    std::size_t pages = warm_loaded_image(0, false);
    const std::uint32_t count = ::_dyld_image_count();
    for (std::uint32_t image = 1; image < count; ++image) {
        if (is_math_library(::_dyld_get_image_name(image))) {
            pages += warm_loaded_image(image, true);
        }
    }
    return pages;
}

#else

std::size_t warm_image() noexcept {
    return 0;
}

#endif

}  // namespace

bool budget_warms_process_image() noexcept {
#if defined(__linux__) || (defined(__APPLE__) && defined(__LP64__))
    return true;
#else
    return false;
#endif
}

std::size_t warm_process_image() noexcept {
    return warm_image();
}

}  // namespace cy::test
