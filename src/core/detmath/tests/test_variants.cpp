// SPDX-License-Identifier: MIT
// ONE KERNEL, THREE BUILDS, ONE PROCESS. Tasks 1.6 and 4.4; design §7.3 and §10.4.
//
// The kernel's translation units are linked into this binary three times, each under its own
// `CY_DETMATH_VARIANT` (config.h):
//
//   v1     the ordinary build, from `cy::core-detmath`
//   gro    compiled with `-mgeneral-regs-only`: no floating-point or vector register may be used,
//   so
//          a float that reached the kernel failed the BUILD before this test could run
//   avx2   compiled with `-mavx2`: integer code at a second vector width
//
// and each function's digest is compared across them. This is the case `core-math`'s "Precision"
// exemption asks for: "one authoritative computation across two vector widths with identical
// results". Integer code is held to bit identity, which is stricter than core-math's "within
// documented tolerance" and which integer code can meet.
//
// A variant the toolchain cannot build is REPORTED by this suite, by name, rather than skipped in
// silence: src/core/detmath/tests/CMakeLists.txt prints the probe's answer at configure, and the
// first case here prints which variants this binary actually compared.

#include <cy/core/detmath/digest.h>
#include <cy/core/detmath/version.h>
#include <cy/test/test.h>

#include <string>

namespace {

using cy::u32;
using cy::u64;
using cy::detmath::KernelFunction;

/// The digest function of one variant, taking the function's index so that the enumerations of the
/// three builds — distinct types — never have to be converted into one another.
using DigestOf = u64 (*)(u32 index);

}  // namespace

// The other builds' entry points. Declared here rather than through the headers, because including
// the headers again would declare them in this build's own variant namespace.
#if defined(CY_DETMATH_TEST_GRO)
namespace cy::detmath::gro {
enum class KernelFunction : u8;
u64 function_digest(KernelFunction function, u32 count) noexcept;
}  // namespace cy::detmath::gro
#endif

#if defined(CY_DETMATH_TEST_AVX2)
namespace cy::detmath::avx2 {
enum class KernelFunction : u8;
u64 function_digest(KernelFunction function, u32 count) noexcept;
}  // namespace cy::detmath::avx2
#endif

namespace {

struct Variant {
    const char* name;
    DigestOf digest;
};

// clang-format off
constexpr Variant kVariants[] = {
#if defined(CY_DETMATH_TEST_GRO)
    {"general-regs-only", [](u32 index) {
         return cy::detmath::gro::function_digest(
             static_cast<cy::detmath::gro::KernelFunction>(index), cy::detmath::kSweepCount); }},
#endif
#if defined(CY_DETMATH_TEST_AVX2)
    {"avx2", [](u32 index) {
         return cy::detmath::avx2::function_digest(
             static_cast<cy::detmath::avx2::KernelFunction>(index), cy::detmath::kSweepCount); }},
#endif
    {"", nullptr},
};
// clang-format on

}  // namespace

CY_TEST_CASE("detmath variants: say which builds of the kernel this binary compares") {
    std::string compared = "baseline";
    for (const Variant& variant : kVariants) {
        if (variant.digest != nullptr) {
            compared += std::string(", ") + variant.name;
        }
    }
    CY_TEST_MESSAGE("kernel builds compared in this process: " << compared);
#if defined(__GNUC__) && !defined(_MSC_VER) && (defined(__x86_64__) || defined(__aarch64__))
    // GCC and Clang accept -mgeneral-regs-only on both CI architectures, so on those legs the
    // float-free build is an obligation, not an option: its absence would be a check that quietly
    // stopped running.
    CY_CHECK_MESSAGE(compared.find("general-regs-only") != std::string::npos,
                     "the -mgeneral-regs-only build of the kernel is missing on a toolchain that "
                     "accepts the flag");
#endif
}

CY_TEST_CASE("detmath variants: every build of the kernel computes the same digests") {
    for (u32 index = 0; index < static_cast<u32>(KernelFunction::Count); ++index) {
        const auto function = static_cast<KernelFunction>(index);
        const u64 baseline = cy::detmath::function_digest(function);
        for (const Variant& variant : kVariants) {
            if (variant.digest == nullptr) {
                continue;
            }
            const u64 other = variant.digest(index);
            CY_CHECK_MESSAGE(other == baseline,
                             std::string(cy::detmath::kernel_function_name(function))
                                 << ": the " << std::string(variant.name) << " build " << other
                                 << ", the baseline " << baseline);
        }
    }
}
