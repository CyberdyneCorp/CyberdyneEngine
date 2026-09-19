#pragma once
// SPDX-License-Identifier: MIT
// The golden-image ledger: which backend produced each image, and on what device. M11.d task 7.3.
//
// ================================================================================================
// WHAT THIS EXISTS FOR, AND WHY IT IS NOT AN EXTRA LOG LINE
// ================================================================================================
//
// `rhi-and-render-graph` has required since M3 that the image comparison "run against every enabled
// backend and record which backend produced a failure", and one backend has never been able to test
// it: there has only ever been one that can draw. So the requirement has been satisfied by a
// hardcoded `DeviceFixture("vulkan", ...)` in every golden case, and the answer to "which backend
// produced this image?" has been "the only one there is" — true, unrecorded, and about to stop
// being true.
//
// M11.d's spike measured what the next rung walks into. Both hosted runners that answered present a
// device, **neither of them is a GPU**, and the Windows one is worse than "it is WARP": the probe
// found two adapters, both `Microsoft Basic Render Driver`, and adapter 0 DOES NOT SET
// `DXGI_ADAPTER_FLAG_SOFTWARE`. The ordinary "pick the first adapter without the software flag"
// selects a software rasteriser and reports hardware. Its conclusion is the rule this header
// implements:
//
//     THE LABEL HAS TO COME FROM THE ADAPTER'S IDENTITY, NOT FROM ITS FLAG.
//
// So `classify()` below reads the device's own reported name against a table of known software and
// paravirtual implementations, and anything it does not recognise is `Unattested` — never
// `Hardware`. A classifier that guessed "hardware" for an unknown string would relabel exactly the
// device the spike caught.
//
// ================================================================================================
// THE RECORD IS A FILE, BECAUSE A CLAIM ABOUT A MACHINE NOBODY HERE OWNS HAS TO BE READABLE LATER
// ================================================================================================
//
// `cygolden 1` is a sorted, deterministic text document. It is written to `CY_GOLDEN_LEDGER` when
// that names a path, so continuous integration can upload one per leg and a later rung can compare
// the legs' answers rather than re-deriving them; when it is unset, nothing is written and the
// assertions in the suite are unchanged. A ledger that only exists inside a passing test would be
// evidence nobody can read.
//
// ================================================================================================
// NOT EVALUATED IS A ROW, NOT AN ABSENCE
// ================================================================================================
//
// A backend that is compiled in but has no device on this machine produces a row saying so, with
// the reason the selection gave. That is the difference between "Metal matched" and "Metal was not
// asked", and this project has already paid twice for a criterion that passed on evidence it never
// examined.

#include <cy/backends/rhi/backend.h>
#include <cy/core/base/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cy::render_test {

/// What kind of implementation answered. Derived from the device's reported name, never from a
/// flag.
enum class DeviceClass : u8 {
    /// A software rasteriser: llvmpipe, lavapipe, SwiftShader, WARP, Microsoft Basic Render Driver.
    Software,
    /// A virtualised device on a hosted runner: "Apple Paravirtual device".
    Paravirtual,
    /// A backend that draws nothing by construction. Its row is never an image claim.
    NullBackend,
    /// A name no table entry matches: no evidence it is software, and NO EVIDENCE IT IS HARDWARE.
    /// Deliberately not spelled `Hardware` — see the header comment. The rung that writes the Metal
    /// and D3D12 backends is where this can become an attested answer, because
    /// `VkPhysicalDeviceType`,
    /// `DXGI_ADAPTER_DESC` and `MTLDevice`'s own properties are the only things that can give one,
    /// and none of them is reachable through `DeviceCapabilities` today.
    Unattested,
};

[[nodiscard]] inline const char* describe(DeviceClass kind) noexcept {
    switch (kind) {
        case DeviceClass::Software:
            return "software";
        case DeviceClass::Paravirtual:
            return "paravirtual";
        case DeviceClass::NullBackend:
            return "null-backend";
        case DeviceClass::Unattested:
            return "unattested";
    }
    return "unattested";
}

/// The names this project has seen answer that are not hardware. Each is a string a driver reports
/// about itself, and each was observed rather than guessed: the three Linux ones on this host, the
/// two hosted-runner ones by M11.d's spike on `macos-14`, `windows-2022` and `windows-11-arm`.
struct NonHardware {
    const char* fragment;
    DeviceClass kind;
};

inline constexpr NonHardware kNonHardware[] = {
    {"llvmpipe", DeviceClass::Software},
    {"lavapipe", DeviceClass::Software},
    {"SwiftShader", DeviceClass::Software},
    {"Software Rasterizer", DeviceClass::Software},
    {"Microsoft Basic Render Driver", DeviceClass::Software},
    {"WARP", DeviceClass::Software},
    {"Paravirtual", DeviceClass::Paravirtual},
    {"Virtual", DeviceClass::Paravirtual},
    // The null backend is NOT a fragment here: `classify` decides it from the backend KIND, which
    // is a fact rather than a string. A `"null"` fragment would also match a real device whose
    // reported name happened to contain the word, which is the false positive a name table has to
    // avoid when its whole purpose is not to mislabel a device.
};

/// Classify a device by the name it reports about itself.
///
/// Unattested is deliberately NOT hardware. The spike's Windows finding is the whole reason: a
/// classifier that defaults to "hardware" would call a software rasteriser hardware the moment its
/// name changed, and the golden image it labelled would be a claim about a machine nobody ran.
[[nodiscard]] inline DeviceClass classify(const char* device_name, rhi::BackendKind kind) noexcept {
    if (kind == rhi::BackendKind::Null) {
        return DeviceClass::NullBackend;
    }
    if (device_name == nullptr || device_name[0] == '\0') {
        return DeviceClass::Unattested;
    }
    for (const NonHardware& entry : kNonHardware) {
        if (std::strstr(device_name, entry.fragment) != nullptr) {
            return entry.kind;
        }
    }
    return DeviceClass::Unattested;
}

/// What happened to one image on one backend.
enum class Outcome : u8 {
    /// Rendered and matched the committed reference within tolerance.
    Matched,
    /// Rendered and did not match. `differing` and `max_channel_delta` say by how much.
    Differed,
    /// The backend is compiled in and produced no device here. NOT a pass.
    NoDevice,
    /// The backend has no readable colour output by construction — the null backend.
    NoImage,
};

[[nodiscard]] inline const char* describe(Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::Matched:
            return "matched";
        case Outcome::Differed:
            return "differed";
        case Outcome::NoDevice:
            return "NOT-EVALUATED-no-device";
        case Outcome::NoImage:
            return "NOT-EVALUATED-no-image";
    }
    return "NOT-EVALUATED-no-device";
}

/// One row: one reference image, judged by one backend, on one device.
struct LedgerRow {
    char backend[32] = {};
    char image[64] = {};
    char device[128] = {};
    DeviceClass device_class = DeviceClass::Unattested;
    Outcome outcome = Outcome::NoDevice;
    u32 differing = 0;
    u32 differing_off_edge = 0;
    u32 max_channel_delta = 0;
    /// Why, when the outcome is not an image. The selection's reason, or a sentence.
    char reason[160] = {};
};

inline void set_field(char* field, usize capacity, const char* value) noexcept {
    if (value == nullptr) {
        field[0] = '\0';
        return;
    }
    std::snprintf(field, capacity, "%s", value);
}

/// Write the ledger where `CY_GOLDEN_LEDGER` points, or nowhere when it is unset.
///
/// Deterministic: the rows are written in the order the backend registry reports them, which is
/// registration order and is fixed by the link. No timestamp and no path, for the reason
/// `cy::build::write_package` gives — a document carrying a clock cannot be compared between two
/// runs, and comparing two legs is what this file is for.
///
/// Returns the path written, or nullptr.
[[nodiscard]] inline const char* write_ledger(const LedgerRow* rows, usize count,
                                              const char* suite) noexcept {
    const char* path = std::getenv("CY_GOLDEN_LEDGER");
    if (path == nullptr || path[0] == '\0') {
        return nullptr;
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return nullptr;
    }
    std::fprintf(file, "cygolden 1\nsuite %s\nrows %zu\n", suite, count);
    for (usize index = 0; index < count; ++index) {
        const LedgerRow& row = rows[index];
        std::fprintf(file,
                     "row backend=%s image=%s device=\"%s\" class=%s outcome=%s "
                     "differing=%u off_edge=%u max_delta=%u reason=\"%s\"\n",
                     row.backend, row.image, row.device, describe(row.device_class),
                     describe(row.outcome), row.differing, row.differing_off_edge,
                     row.max_channel_delta, row.reason);
    }
    std::fclose(file);
    return path;
}

/// Print the ledger to stderr, always. A file a reader has to know to look for is not a report, and
/// a leg that never set CY_GOLDEN_LEDGER still has to say which backend answered.
inline void report_ledger(const LedgerRow* rows, usize count) noexcept {
    std::fprintf(stderr,
                 "\n--- golden images by backend ---------------------------------------\n");
    for (usize index = 0; index < count; ++index) {
        const LedgerRow& row = rows[index];
        std::fprintf(stderr, "  %-8s %-24s %-12s %s", row.backend, row.image,
                     describe(row.device_class), describe(row.outcome));
        if (row.outcome == Outcome::Differed) {
            std::fprintf(stderr, "  (%u texels, worst channel %u)", row.differing,
                         row.max_channel_delta);
        }
        if (row.device[0] != '\0') {
            std::fprintf(stderr, "  on \"%s\"", row.device);
        }
        if (row.reason[0] != '\0') {
            std::fprintf(stderr, "  — %s", row.reason);
        }
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "---------------------------------------------------------------------\n");
}

}  // namespace cy::render_test
