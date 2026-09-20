// SPDX-License-Identifier: MIT
// The golden image, run against EVERY ENABLED RHI BACKEND, with the answer recorded. M11.d
// task 7.3.
//
// ================================================================================================
// WHAT WAS WRONG WITH THE SUITE THIS ONE JOINS
// ================================================================================================
//
// `rhi-and-render-graph` has required since M3 that the image comparison "run against every enabled
// backend and record which backend produced a failure". Every golden case in this directory opens
// with `DeviceFixture("vulkan", ...)` and skips unless `fixture.is(BackendKind::Vulkan)`. That is
// not a defect — there has only ever been ONE backend in this engine that can draw — but it means
// the requirement has been answered by a coincidence rather than by a mechanism, and the answer to
// "which backend produced this image?" has been the only one there is.
//
// M11.d's own scope change is what makes that stop being good enough. Sections 2 and 3 — Metal and
// D3D12 — moved to a rung of their own because neither can be compiled on Linux, and
// `m11d:golden-images-across-three-backends` moved with them **so that rung cannot close on "it
// compiles somewhere"**. The mechanism that rung needs has to exist before it, on a tree where it
// can be watched working, or it will be written at the same time as the thing it is meant to judge.
//
// So this suite is that mechanism, with the backends that exist today: it ENUMERATES the registry
// rather than naming a backend, judges every entry, and records what answered.
//
// ================================================================================================
// THREE THINGS IT ASSERTS, AND WHY EACH IS NOT THE OTHER TWO
// ================================================================================================
//
//   1. THE ENUMERATION IS NOT EMPTY. A suite that iterated an empty registry would pass in silence,
//      which is the defect class `tools/roadmap/falsify.py` enumerates nine instances of. At least
//      one backend must be registered and at least one row must be written.
//   2. EVERY BACKEND THAT PRODUCED AN IMAGE MATCHED THE ONE COMMITTED REFERENCE. design.md §3 fixes
//      this: Vulkan's references stay THE references, and a second reference per backend would make
//      every backend its own truth so that no comparison could ever fail.
//   3. A BACKEND THAT PRODUCED NO IMAGE IS A ROW, NOT AN ABSENCE. `NOT-EVALUATED-no-device` and
//      `NOT-EVALUATED-no-image` are recorded with their reasons and are never counted as passes.
//
// ================================================================================================
// WHY THE NULL BACKEND IS IN THE LOOP AT ALL
// ================================================================================================
//
// Because leaving it out would make the enumeration a list of backends somebody decided could draw,
// which is the hardcoding this file exists to remove. It is enumerated, asked, and recorded as
// `NOT-EVALUATED-no-image` with the reason — and `render.null_frame` is where what it CAN answer is
// asserted, structure for structure. The row is what stops "two backends were checked" from being
// read off a suite that checked one.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/system_allocator.h>
#include <cy_features.h>

#if defined(CY_RENDERER_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#if defined(CY_RENDERER_METAL)
#    include <cy/backends/rhi-metal/backend.h>
#endif
#if defined(CY_RENDERER_D3D12)
#    include <cy/backends/rhi-d3d12/backend.h>
#endif

#include "golden.h"
#include "golden_ledger.h"
#include "renderer.h"
#include "scene.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using cy::render_test::DeviceClass;
using cy::render_test::LedgerRow;
using cy::render_test::Outcome;
using cy::sample::first_light::Camera;
using cy::sample::first_light::FrameReport;
using cy::sample::first_light::Renderer;
using cy::sample::first_light::RendererOptions;
using cy::sample::first_light::Scene;
using cy::sample::first_light::SceneDescription;

/// The same viewport the committed reference was photographed at. Not a constant of its own: if
/// these diverged the comparison would fail for a reason that is not a rendering difference.
constexpr cy::u32 kWidth = 192;
constexpr cy::u32 kHeight = 108;

/// The one committed reference every backend is judged against. design.md §3: "Vulkan's references
/// stay the references. A second committed reference per backend would make every backend its own
/// truth and no comparison would ever fail."
constexpr const char* kImage = "first_light";

const char* reference_path() noexcept {
    static char storage[1024];
    std::snprintf(storage, sizeof(storage), "%s/references/%s.png", CY_RENDER_TEST_DIR, kImage);
    return storage;
}

/// Register every backend this build links. Nothing below branches on WHICH backend a row is for:
/// `Backend capability model` requires the renderer to branch on capabilities and never on backend
/// identity, and a suite that tested identity would teach the next backend the wrong pattern.
void register_everything() noexcept {
#if defined(CY_RENDERER_VULKAN)
    (void)cy::rhi::vulkan::register_vulkan_backend();
#endif
#if defined(CY_RENDERER_METAL)
    (void)cy::rhi::metal::register_metal_backend();
#endif
#if defined(CY_RENDERER_D3D12)
    (void)cy::rhi::d3d12::register_d3d12_backend();
#endif
    (void)cy::rhi::null::register_null_backend();
}

/// Render the sample's frame at phase 0 — the phase the reference was taken at — on one device.
cy::Status render_once(cy::Allocator& allocator, cy::rhi::Device& device,
                       cy::render_test::Image& out) {
    Scene scene(allocator);
    if (cy::Status built = scene.build(SceneDescription{}); !built) {
        return built;
    }
    RendererOptions options;
    options.width = kWidth;
    options.height = kHeight;

    Renderer renderer(allocator, device);
    if (cy::Status prepared = renderer.prepare(scene, options); !prepared) {
        return prepared;
    }
    cy::Expected<FrameReport, cy::Error> frame = renderer.render(scene, scene.camera_at(0.0F));
    if (!frame.has_value()) {
        return cy::make_unexpected(frame.error());
    }
    return cy::render_test::adopt(out, renderer.color_texels(), kWidth, kHeight);
}

/// Ask one backend for a device, render, compare, and fill in its row. Never throws and never
/// asserts: the assertions are made once, over the whole ledger, by the case below.
LedgerRow judge(const cy::rhi::BackendRegistration& registration,
                const cy::render_test::Image& reference) {
    LedgerRow row;
    cy::render_test::set_field(row.backend, sizeof(row.backend), registration.name);
    cy::render_test::set_field(row.image, sizeof(row.image), kImage);

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::rhi::DeviceDescription description;
    description.application_name = "cy_test_render_golden_backends";
    description.enable_validation = true;
    description.request_async_compute = false;

    cy::rhi::BackendSelection selection{};
    cy::Expected<cy::rhi::Device*, cy::Error> device =
        cy::rhi::create_device(allocator, registration.name, description, selection);

    if (!device.has_value()) {
        row.outcome = Outcome::NoDevice;
        cy::render_test::set_field(row.reason, sizeof(row.reason), device.error().message);
        return row;
    }

    // `create_device` falls back to the null backend rather than failing. A fallback answering for
    // the backend that was asked for would attribute one backend's image to another, which is the
    // single thing this ledger exists to prevent.
    const bool answered_itself =
        selection.selected != nullptr && std::strcmp(selection.selected, registration.name) == 0;

    cy::render_test::set_field(row.device, sizeof(row.device),
                               device.value()->capabilities().device_name());
    const cy::rhi::DeviceIdentity identity =
        cy::rhi::classify_device_identity(row.device, device.value()->capabilities().vendor_id(),
                                          device.value()->capabilities().backend());
    row.vendor_id = identity.vendor_id;
    cy::render_test::set_field(row.vendor, sizeof(row.vendor), identity.vendor);
    row.device_class = identity.classification;

    if (!answered_itself) {
        row.outcome = Outcome::NoDevice;
        cy::render_test::set_field(
            row.reason, sizeof(row.reason),
            selection.reason[0] != '\0' ? selection.reason : "the selection fell back");
        (void)device.value()->wait_idle();
        cy::rhi::destroy_device(allocator, device.value());
        return row;
    }

    if (row.device_class == DeviceClass::NullBackend) {
        row.outcome = Outcome::NoImage;
        cy::render_test::set_field(
            row.reason, sizeof(row.reason),
            "the null backend produces no readable colour; render.null_frame judges its structure");
        (void)device.value()->wait_idle();
        cy::rhi::destroy_device(allocator, device.value());
        return row;
    }

    cy::render_test::Image rendered(allocator);
    const cy::Status drawn = render_once(allocator, *device.value(), rendered);
    const cy::Status idle = drawn ? device.value()->wait_idle() : cy::ok();
    if (!drawn) {
        row.outcome = Outcome::NoDevice;
        cy::render_test::set_field(row.reason, sizeof(row.reason), drawn.error().message);
    } else if (!idle) {
        row.outcome = Outcome::Differed;
        cy::render_test::set_field(row.reason, sizeof(row.reason), idle.error().message);
    } else {
        if (const char* directory = std::getenv("CY_GOLDEN_CAPTURE_DIR");
            directory != nullptr && directory[0] != '\0') {
            char path[1024] = {};
            std::snprintf(path, sizeof(path), "%s/m11d5-three-backends-%s.png", directory,
                          registration.name);
            if (const cy::Status written = cy::render_test::write_png(path, rendered); !written) {
                row.outcome = Outcome::Differed;
                cy::render_test::set_field(row.reason, sizeof(row.reason), written.error().message);
                return row;
            }
            std::fprintf(stderr, "golden_backends: captured %s on device \"%s\"\n", path,
                         row.device);
        }
        const cy::render_test::Comparison comparison =
            cy::render_test::compare(reference, rendered);
        row.differing = comparison.differing;
        row.differing_off_edge = comparison.differing_off_edge;
        row.max_channel_delta = comparison.max_channel_delta;
        const bool within = comparison.comparable && comparison.differing_off_edge == 0 &&
                            comparison.differing <= comparison.edge_texels;
        row.outcome = within ? Outcome::Matched : Outcome::Differed;
        if (!comparison.comparable) {
            cy::render_test::set_field(row.reason, sizeof(row.reason),
                                       "the rendered image and the reference are different sizes");
        }
    }

    (void)device.value()->wait_idle();
    cy::rhi::destroy_device(allocator, device.value());
    return row;
}

}  // namespace

CY_TEST_CASE("render.golden_backends: every enabled backend is judged, and the answer names it") {
    register_everything();

    const cy::Span<const cy::rhi::BackendRegistration> backends = cy::rhi::registered_backends();
    // 1. The enumeration is not empty. A suite that iterated nothing would pass in silence.
    CY_REQUIRE(!backends.empty());

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::render_test::Image reference(allocator);
    const cy::Status read = cy::render_test::read_png(reference_path(), reference);
    if (!read) {
        std::fprintf(stderr, "golden_backends: %s: %s\n", reference_path(), read.error().message);
    }
    CY_REQUIRE(read.has_value());
    CY_REQUIRE(reference.width == kWidth);
    CY_REQUIRE(reference.height == kHeight);

    cy::Array<LedgerRow> rows(allocator);
    for (const cy::rhi::BackendRegistration& registration : backends) {
        CY_REQUIRE(rows.push_back(judge(registration, reference)).has_value());
    }

    CY_REQUIRE_EQ(rows.size(), backends.size());
    cy::render_test::report_ledger(rows.data(), rows.size());
    if (const char* written =
            cy::render_test::write_ledger(rows.data(), rows.size(), "render.golden_backends")) {
        std::fprintf(stderr, "golden_backends: ledger written to %s\n", written);
    }

    // 2. Every backend that produced an image matched the one committed reference, and the failure
    //    NAMES THE BACKEND — which is the half of the M3 requirement that has never been testable.
    cy::u32 judged = 0;
    for (const LedgerRow& row : rows) {
        if (row.outcome == Outcome::Matched || row.outcome == Outcome::Differed) {
            ++judged;
        }
        if (row.outcome == Outcome::Differed) {
            std::fprintf(stderr,
                         "golden_backends: BACKEND '%s' on device \"%s\" (%s) does not match %s: "
                         "%u texels over tolerance, %u of them away from any high-contrast edge, "
                         "worst channel delta %u.\n",
                         row.backend, row.device, cy::render_test::describe(row.device_class),
                         row.image, row.differing, row.differing_off_edge, row.max_channel_delta);
        }
        CY_CHECK(row.outcome != Outcome::Differed);
    }

    // 3. Every enumerated backend produced a row, and a backend with no device is a row saying so
    //    rather than an absence. `judged` may legitimately be 0 on a machine with no GPU — that is
    //    the NOT EVALUATED outcome, and the rows above say which backends and why.
    std::fprintf(stderr, "golden_backends: %u of %zu enabled backend(s) produced an image\n",
                 judged, backends.size());
}

// ==================================================================================================
// THE CLASSIFIER, ASSERTED SEPARATELY, BECAUSE IT IS THE PART A HOSTED RUNNER GETS WRONG
// ==================================================================================================
//
// M11.d's spike found two adapters on every hosted Windows image, both `Microsoft Basic Render
// Driver`, and **adapter 0 does not set `DXGI_ADAPTER_FLAG_SOFTWARE`**. A backend that trusts that
// flag reports hardware it does not have and so do its golden images. The rule the spike wrote down
// is that the label must come from the adapter's IDENTITY, and this case is that rule with a test
// under it — written now, on a tree where it can be watched working, rather than at the same time
// as the D3D12 backend it will judge.
CY_TEST_CASE(
    "render.golden_backends: a device is never called hardware because nothing said it was not") {
    using cy::rhi::BackendKind;
    using cy::rhi::classify_device_identity;

    // Every string here was OBSERVED — the three Linux ones on the machine M11.d was worked on, the
    // two hosted-runner ones by the spike's probe workflow on macos-14 and windows-2022.
    CY_CHECK(classify_device_identity("llvmpipe (LLVM 17.0.6, 256 bits)", 0, BackendKind::Vulkan)
                 .classification == DeviceClass::Software);
    CY_CHECK(classify_device_identity("Microsoft Basic Render Driver", 0, BackendKind::D3D12)
                 .classification == DeviceClass::Software);
    CY_CHECK(classify_device_identity("Apple Paravirtual device", 0, BackendKind::Metal)
                 .classification == DeviceClass::Paravirtual);
    CY_CHECK(classify_device_identity("", 0, BackendKind::Null).classification ==
             DeviceClass::NullBackend);

    // THE CASE THIS EXISTS FOR. An unknown name is `Unknown`, never `Hardware`. A classifier
    // that defaulted to hardware would relabel the exact device the spike caught the moment its
    // reported name changed by one word. A known vendor ID does attest the real adapter.
    CY_CHECK(classify_device_identity("NVIDIA GeForce RTX 5060", 0x10DEU, BackendKind::Vulkan)
                 .classification == DeviceClass::Hardware);
    CY_CHECK(classify_device_identity("Some Future Adapter", 0xFFFFU, BackendKind::D3D12)
                 .classification == DeviceClass::Unknown);
    CY_CHECK(classify_device_identity(nullptr, 0, BackendKind::Vulkan).classification ==
             DeviceClass::Unknown);

    // And the row a reader sees says one of five words, never an empty string.
    for (const DeviceClass kind :
         {DeviceClass::Hardware, DeviceClass::Software, DeviceClass::Paravirtual,
          DeviceClass::NullBackend, DeviceClass::Unknown}) {
        CY_CHECK(cy::render_test::describe(kind)[0] != '\0');
    }
    for (const Outcome outcome :
         {Outcome::Matched, Outcome::Differed, Outcome::NoDevice, Outcome::NoImage}) {
        CY_CHECK(cy::render_test::describe(outcome)[0] != '\0');
    }
}
