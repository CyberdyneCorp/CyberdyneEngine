// THE ENVIRONMENT FIELD, SAMPLED ON A DEVICE, AGAINST THE PROCESSOR THAT SAMPLES THE SAME BYTES.
//
// `environment-fields` — "CPU and GPU access": fields "SHALL be samplable from both CPU code and
// GPU shaders, through interfaces that produce the same value for the same position and
// resolution", and GPU access "SHALL be through bindless resources reachable from the GPU scene, so
// a shader can sample a field without per-draw binding".
//
// `m10:fields-sampled-on-a-device` measured the number of `.slang` modules in this tree that sample
// an environment field at ZERO, and `src/environment/README.md` said the same thing in its own
// words: the requirement was "half-discharged, and the half that is missing is the device".
// `integration.environment_gpu` is the other half and is not this one — it compares two
// implementations that share no code, both on the processor, which is evidence about a LAYOUT. This
// suite runs the third implementation, `cy/field.slang`, on a graphics device.
//
// ================================================================================================
// THE COMPARISON IS BIT-EXACT, AND THAT IS MEASURED RATHER THAN FASTIDIOUS
// ================================================================================================
//
// M11.a's spike mutated `sample_field_image()`'s `UNorm16` decode from `/65535` to `/65536` — a
// plausible transcription slip — and every value in its report changed while EVERY FIELD STAYED
// INSIDE ITS DECLARED PRECISION. A criterion written as "within the field's declared precision"
// could not have failed on it. So the numbers below are bit-identical counts and ULP distances, and
// the declared-precision bound is reported beside them as the specification's own sentence rather
// than as the check.
//
// The ULP bound is 4. The spike measured the same shader against the same processor sampler over
// 1 843 968 comparisons and saw a worst of 3, while the STORE and `sample_field_image()` already
// differ from each other by 77 ULP on the processor — for the reason `test_gpu.cpp`'s header gives,
// that one blends in f64 world coordinates and the other in f32 image-local ones. A device that
// drifted past 4 would therefore be drifting inside a band the two processor-side samplers do not
// use, which is a real finding and not a tolerance to widen.
//
// ================================================================================================
// WHAT THIS SUITE REFUSES TO REPORT
// ================================================================================================
//
// Three ways a device comparison passes while measuring nothing, each of them a shape this project
// has shipped before and each asserted here before any value is compared:
//
//   * THE DISPATCH NEVER RAN. Every answer word is prefilled with a signalling-NaN sentinel the
//     arithmetic cannot produce. A surviving sentinel is a dispatch that did not write, and it is
//     counted rather than inferred from a plausible-looking number.
//   * THE POSITIONS FELL OUTSIDE THE WRITTEN TILES. Both samplers then answer the declared default,
//     agree perfectly and prove nothing — which is `m10:sky-field-round-trip` exactly, one level
//     up. `CyFieldSample::resolved` comes back from the device and every position must resolve.
//   * THE CONTENT DOES NOT VARY. A uniformly filled field agrees under a sampler whose lattice
//     arithmetic is completely wrong. The distinct-answer count is asserted PER FIELD, because the
//     spike's own version of this check counted variety across four fields together and a single
//     live field carried three dead ones past it.
//
// ================================================================================================
// THE VOLUMETRIC CASE IS HERE ON PURPOSE
// ================================================================================================
//
// design.md §1.4: every field the spike measured was planar, so `cyFieldVerticalWeight` returned
// exactly 1.0 and reassociating the bilinear weight produced a byte-identical report — the mutation
// could not fail. `wind_like()` below is Vec3/F32 with four vertical cells, and it is the case that
// discriminates. Its positions are sampled at several altitudes inside the column for that reason.

// ================================================================================================
// THE MUTATIONS THIS SUITE WAS PROVED AGAINST, AND THE ONE THAT DID NOT GO RED
// ================================================================================================
//
// Run against the finished suite, each restored and md5-verified afterwards. A check that has never
// failed proves nothing, and this project has shipped six that could not.
//
//   `cy/field.slang` UNorm8 decode `/255` -> `/256`     RED. Sweep 1165 -> 0 bit-identical, worst
//       3 -> 65 536 ULP; lattice centres 514 -> 4, worst 65 279 ULP. **AND THE WORST ABSOLUTE WAS
//       0.00383735 AGAINST A DECLARED PRECISION OF 0.00392157 — INSIDE IT.** The specification's
//       own bound cannot fail on this mutation; only the ULP columns catch it, which is the whole
//       reason this file is written in ULPs.
//   the half-cell offset deleted                        RED on both fields, every count to zero.
//   `commands.dispatch` removed                         RED. 33 280 of 33 280 sentinel words alive.
//   the comparison moved 1 000 km away                  RED. 2 304 of 2 304 bit-identical, ONE
//       distinct answer, 2 304 unresolved — agreement produced by absence, refused by the resolved
//       and variety counts rather than by the values.
//   `wx * wz * wy` -> `wx * (wz * wy)`                   **DID NOT GO RED, AND IS LISTED ANYWAY.**
//       The volumetric case MOVED — 3 528 of 6 912 bit-identical became 3 300 — where M11.a's spike
//       saw a byte-identical report because all four of its fields were planar. So the volumetric
//       case does discriminate, and nothing asserts on it: the sweep's bit-identical FRACTION is
//       the only signal, and a floor on it would be a golden number that moves with a driver. The
//       association claim in `cy/field.slang`'s header is therefore still untested, one step less
//       untestable than it was.

#include <cy/test/test.h>

#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/environment/gpu.h>
#include <cy/environment/store.h>
#include <cy/world/coordinates.h>

#include "device.h"
#include "shaders/field_probe_spirv.h"

#include <cmath>
#include <cstring>

namespace {

using cy::f32;
using cy::i32;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::environment::build_field_image;
using cy::environment::FieldDeclaration;
using cy::environment::FieldEncoding;
using cy::environment::FieldGpuImage;
using cy::environment::FieldImageLocal;
using cy::environment::FieldInterpolation;
using cy::environment::FieldLevel;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldStore;
using cy::environment::FieldType;
using cy::environment::FieldValue;
using cy::environment::image_local;
using cy::environment::ProducerKind;
using cy::environment::ProducerToken;
using cy::environment::sample_field_image;
using cy::environment::TileAddress;
using cy::world::WorldVec3d;
namespace rhi = cy::rhi;

[[nodiscard]] cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] cy::world::PartitionConfig partition() noexcept {
    cy::world::PartitionConfig config;
    config.partition = 1;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

/// A quantised scalar at three resolutions — the shape moisture and wetness have. The same
/// declaration `src/environment/tests/fixtures.h` uses, spelled here rather than included: that
/// header is CyberField's own fixture set and a render suite reaching across the tree for it would
/// couple two directories' test data for four lines.
[[nodiscard]] FieldDeclaration moisture_like() noexcept {
    FieldDeclaration declaration;
    declaration.name = "test.moisture";
    declaration.unit = "fraction";
    declaration.semantics = "water held in the top soil layer, 0 bone dry to 1 saturated";
    declaration.type = FieldType::Scalar;
    declaration.encoding = FieldEncoding::UNorm8;
    declaration.interpolation = FieldInterpolation::Linear;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = FieldValue::scalar(0.25F);
    declaration.levels[0] = FieldLevel{2.0F, false};
    declaration.levels[2] = FieldLevel{64.0F, true};
    declaration.classification = cy::determinism::SimulationClass::Presentation;
    return declaration;
}

/// THE VOLUMETRIC ONE: three f32 components over a four-cell column, which is the shape `wind` has.
[[nodiscard]] FieldDeclaration wind_like() noexcept {
    FieldDeclaration declaration;
    declaration.name = "test.wind";
    declaration.unit = "m/s";
    declaration.semantics = "air velocity, world axes";
    declaration.type = FieldType::Vec3;
    declaration.encoding = FieldEncoding::F32;
    declaration.interpolation = FieldInterpolation::Linear;
    declaration.range_min = -40.0F;
    declaration.range_max = 40.0F;
    declaration.default_value = FieldValue::vec3(1.0F, 0.0F, 0.0F);
    declaration.levels[0] = FieldLevel{4.0F, false};
    declaration.levels[2] = FieldLevel{32.0F, true};
    declaration.vertical_cells = 4;
    declaration.vertical_metres = 25.0F;
    declaration.vertical_origin_metres = 0.0F;
    declaration.classification = cy::determinism::SimulationClass::Presentation;
    return declaration;
}

/// A value that differs at every lattice point and is a function of the point's place in the world
/// rather than of the tile it lives in — so a sampler that confused two tiles produces a value that
/// is WRONG rather than one that is merely from next door. `test_gpu.cpp`'s pattern, kept identical
/// so a disagreement between the two suites is about the device and not about the data.
[[nodiscard]] f32 pattern(i32 tile_x, i32 tile_z, u32 x, u32 z, u32 y, f32 scale) noexcept {
    const cy::i64 global_x = (static_cast<cy::i64>(tile_x) * cy::environment::kTileCells) + x;
    const cy::i64 global_z = (static_cast<cy::i64>(tile_z) * cy::environment::kTileCells) + z;
    const cy::i64 mixed = (global_x * 7) + (global_z * 13) + (static_cast<cy::i64>(y) * 31);
    const auto folded = static_cast<f32>(((mixed % 241) + 241) % 241);
    return (folded / 241.0F) * scale;
}

[[nodiscard]] cy::Status write_pattern(FieldStore& store, const ProducerToken& token,
                                       const FieldDeclaration& declaration, i32 tile_x,
                                       i32 tile_z) noexcept {
    TileAddress address;
    address.field = declaration.id();
    address.level = 0;
    address.layer = 0;
    address.x = tile_x;
    address.z = tile_z;
    cy::Expected<cy::environment::FieldWriter, cy::Error> writer = store.open_writer(token);
    if (!writer) {
        return cy::make_unexpected(writer.error());
    }
    if (cy::Status staged = writer->stage(address); !staged) {
        return staged;
    }
    for (u32 y = 0; y < declaration.vertical_cells; ++y) {
        for (u32 z = 0; z < cy::environment::kTileCells; ++z) {
            for (u32 x = 0; x < cy::environment::kTileCells; ++x) {
                const f32 value = pattern(tile_x, tile_z, x, z, y, 1.0F);
                FieldValue sample = FieldValue::scalar(value);
                if (declaration.components() == 3) {
                    sample = FieldValue::vec3(value * 40.0F, -value * 40.0F, value * 20.0F);
                }
                if (cy::Status written = writer->set(address, x, y, z, sample); !written) {
                    return written;
                }
            }
        }
    }
    return writer->publish();
}

/// The sentinel every answer word carries before the dispatch. A signalling-NaN payload, so it
/// cannot be produced by the sampler's arithmetic and cannot be mistaken for a plausible answer.
constexpr u32 kSentinel = 0x7F800BADU;
constexpr u32 kAnswerWords = 5;  // four components and the `resolved` flag

/// Distance in units-in-the-last-place between two floats, which is what a bit-exact comparison
/// reports when it is not exact. Both sides are finite by construction here; a NaN on either side
/// is the sentinel check's business and is reported as an enormous distance rather than silently
/// zero.
[[nodiscard]] u32 ulp_distance(f32 a, f32 b) noexcept {
    if (a == b) {
        return 0;
    }
    if (std::isnan(a) || std::isnan(b)) {
        return 0xFFFFFFFFU;
    }
    u32 left = 0;
    u32 right = 0;
    std::memcpy(&left, &a, sizeof(left));
    std::memcpy(&right, &b, sizeof(right));
    // Two's-complement ordering of IEEE 754: the sign-magnitude encoding is folded so that the
    // integer difference counts representable values across zero as well as within one sign.
    const auto ordered = [](u32 bits) -> u32 {
        return ((bits & 0x80000000U) != 0) ? (0x80000000U - (bits & 0x7FFFFFFFU))
                                           : (bits + 0x80000000U);
    };
    const u32 lo = ordered(left);
    const u32 hi = ordered(right);
    return (lo > hi) ? (lo - hi) : (hi - lo);
}

/// One dispatch of the probe, end to end: buffers, layout, pipelines, submit, read back.
///
/// Both entry points share every buffer and both are dispatched, because the second answers a
/// different half of the requirement — "without per-draw binding" — and a suite that ran only the
/// directly bound one would leave the bindless table as a declaration nothing executed.
struct ProbeRun {
    cy::Array<f32> bound;
    cy::Array<f32> bindless;
    u32 sentinels_alive = 0;

    explicit ProbeRun(cy::Allocator& memory) noexcept : bound(memory), bindless(memory) {}
};

/// Everything the probe creates, destroyed in reverse on the way out.
///
/// A destructor rather than a cleanup label, because `run_probe` returns early on every device
/// error and this build has no exceptions: a leak here is not a slow leak, it is VMA aborting the
/// process at teardown with "some allocations were not freed", which is how this was found.
struct ProbeResources {
    rhi::Device* device = nullptr;
    cy::Array<rhi::BufferHandle> buffers;
    cy::Array<rhi::ShaderModuleHandle> modules;
    cy::Array<rhi::ComputePipelineHandle> pipelines;
    cy::Array<rhi::DescriptorSetLayoutHandle> set_layouts;
    rhi::PipelineLayoutHandle layout;

    explicit ProbeResources(cy::Allocator& memory) noexcept
        : buffers(memory), modules(memory), pipelines(memory), set_layouts(memory) {}

    ProbeResources(const ProbeResources&) = delete;
    ProbeResources& operator=(const ProbeResources&) = delete;

    ~ProbeResources() {
        if (device == nullptr) {
            return;
        }
        (void)device->wait_idle();
        for (rhi::ComputePipelineHandle handle : pipelines.span()) {
            device->destroy_compute_pipeline(handle);
        }
        if (!layout.is_null()) {
            device->destroy_pipeline_layout(layout);
        }
        for (rhi::DescriptorSetLayoutHandle handle : set_layouts.span()) {
            device->destroy_descriptor_set_layout(handle);
        }
        for (rhi::ShaderModuleHandle handle : modules.span()) {
            device->destroy_shader_module(handle);
        }
        for (rhi::BufferHandle handle : buffers.span()) {
            device->destroy_buffer(handle);
        }
    }
};

[[nodiscard]] cy::Status run_probe(rhi::Device& device, cy::Span<const u32> image_words,
                                   cy::Span<const f32> positions, u32 count,
                                   ProbeRun& out) noexcept {
    ProbeResources owned(allocator());
    owned.device = &device;
    const u64 image_bytes = static_cast<u64>(image_words.size()) * sizeof(u32);
    const u64 position_bytes = static_cast<u64>(positions.size()) * sizeof(f32);
    const u64 answer_bytes = static_cast<u64>(count) * kAnswerWords * sizeof(f32);

    rhi::BufferDescription image_desc;
    image_desc.name = "field image";
    image_desc.size = image_bytes;
    image_desc.usage = rhi::BufferUsage::Storage;
    image_desc.memory = rhi::MemoryUse::Upload;
    cy::Expected<rhi::BufferHandle, cy::Error> image = device.create_buffer(image_desc);
    if (!image) {
        return cy::make_unexpected(image.error());
    }
    if (cy::Status kept = owned.buffers.push_back(*image); !kept) {
        return kept;
    }

    rhi::BufferDescription position_desc = image_desc;
    position_desc.name = "field probe positions";
    position_desc.size = position_bytes;
    cy::Expected<rhi::BufferHandle, cy::Error> position_buffer =
        device.create_buffer(position_desc);
    if (!position_buffer) {
        return cy::make_unexpected(position_buffer.error());
    }
    if (cy::Status kept = owned.buffers.push_back(*position_buffer); !kept) {
        return kept;
    }

    // HOST-VISIBLE DEVICE-LOCAL, AND NOT `Readback`, and the reason is a rule this engine already
    // enforces: `rhi::validate` refuses a `Readback` buffer that does not declare
    // `TransferDestination`, "because nothing else can fill it". A compute dispatch can, but the
    // arrangement that rule describes — device-local storage, a transfer copy, a host read — needs
    // a BARRIER between the dispatch and the copy, and `rhi::CommandBuffer` deliberately has no
    // barrier call: `barrier.h`'s recorder is reachable only by the render graph's executor, and
    // `just quality-layers` fails a build where a barrier symbol appears outside it.
    //
    // `HostVisibleDeviceLocal` is the case `rhi-and-render-graph` singles out and it is exactly
    // this one: memory both sides touch without a copy. The timeline wait below is the memory
    // dependency Vulkan defines between a signalled fence and a host read of host-visible memory,
    // which is the same dependency src/rendering/virtual_geometry/src/gpu.cpp's own read-back
    // relies on.
    rhi::BufferDescription answer_desc;
    answer_desc.name = "field probe answers";
    answer_desc.size = answer_bytes;
    answer_desc.usage = rhi::BufferUsage::Storage;
    answer_desc.memory = rhi::MemoryUse::HostVisibleDeviceLocal;
    cy::Expected<rhi::BufferHandle, cy::Error> answers = device.create_buffer(answer_desc);
    if (!answers) {
        return cy::make_unexpected(answers.error());
    }
    if (cy::Status kept = owned.buffers.push_back(*answers); !kept) {
        return kept;
    }

    std::memcpy(device.buffer_mapped_pointer(*image), image_words.data(), image_bytes);
    std::memcpy(device.buffer_mapped_pointer(*position_buffer), positions.data(), position_bytes);

    // --- The layout. Set 0 is the per-frame set and carries `cy/field.slang`'s bindless table at
    // binding 3, exactly where the module declares it; set 1 is this probe's own.
    constexpr u32 kTableSlots = 4;
    rhi::DescriptorBinding scene_bindings[1];
    scene_bindings[0].binding = 3;
    scene_bindings[0].kind = rhi::DescriptorKind::StorageBuffer;
    scene_bindings[0].count = kTableSlots;
    scene_bindings[0].stages = rhi::ShaderStage::Compute;
    rhi::DescriptorSetLayoutDescription scene_layout_desc;
    scene_layout_desc.name = "field scene set";
    scene_layout_desc.bindings = cy::Span<const rhi::DescriptorBinding>(scene_bindings, 1);
    cy::Expected<rhi::DescriptorSetLayoutHandle, cy::Error> scene_layout =
        device.create_descriptor_set_layout(scene_layout_desc);
    if (!scene_layout) {
        return cy::make_unexpected(scene_layout.error());
    }
    if (cy::Status kept = owned.set_layouts.push_back(*scene_layout); !kept) {
        return kept;
    }

    rhi::DescriptorBinding probe_bindings[3];
    for (u32 index = 0; index < 3; ++index) {
        probe_bindings[index].binding = index;
        probe_bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        probe_bindings[index].count = 1;
        probe_bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription probe_layout_desc;
    probe_layout_desc.name = "field probe set";
    probe_layout_desc.bindings = cy::Span<const rhi::DescriptorBinding>(probe_bindings, 3);
    cy::Expected<rhi::DescriptorSetLayoutHandle, cy::Error> probe_layout =
        device.create_descriptor_set_layout(probe_layout_desc);
    if (!probe_layout) {
        return cy::make_unexpected(probe_layout.error());
    }
    if (cy::Status kept = owned.set_layouts.push_back(*probe_layout); !kept) {
        return kept;
    }

    const rhi::DescriptorSetLayoutHandle set_layouts[2] = {*scene_layout, *probe_layout};
    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, 16};
    rhi::PipelineLayoutDescription layout_desc;
    layout_desc.name = "field probe layout";
    layout_desc.set_layouts = cy::Span<const rhi::DescriptorSetLayoutHandle>(set_layouts, 2);
    layout_desc.push_constants = cy::Span<const rhi::PushConstantRange>(&range, 1);
    cy::Expected<rhi::PipelineLayoutHandle, cy::Error> layout =
        device.create_pipeline_layout(layout_desc);
    if (!layout) {
        return cy::make_unexpected(layout.error());
    }
    owned.layout = *layout;

    // --- The two pipelines, from the checked-in SPIR-V.
    const auto make_pipeline =
        [&](const char* name,
            cy::Span<const u32> code) -> cy::Expected<rhi::ComputePipelineHandle, cy::Error> {
        rhi::ShaderModuleDescription module_desc;
        module_desc.name = name;
        module_desc.stage = rhi::ShaderStage::Compute;
        module_desc.spirv = code;
        module_desc.entry_point = name;
        cy::Expected<rhi::ShaderModuleHandle, cy::Error> module =
            device.create_shader_module(module_desc);
        if (!module) {
            return cy::make_unexpected(module.error());
        }
        if (cy::Status kept = owned.modules.push_back(*module); !kept) {
            return cy::make_unexpected(kept.error());
        }
        rhi::ComputePipelineDescription pipeline_desc;
        pipeline_desc.name = name;
        pipeline_desc.layout = *layout;
        pipeline_desc.shader = *module;
        cy::Expected<rhi::ComputePipelineHandle, cy::Error> created =
            device.create_compute_pipeline(pipeline_desc);
        if (!created) {
            return created;
        }
        if (cy::Status kept = owned.pipelines.push_back(*created); !kept) {
            return cy::make_unexpected(kept.error());
        }
        return created;
    };
    cy::Expected<rhi::ComputePipelineHandle, cy::Error> bound_pipeline = make_pipeline(
        "sampleFieldBound",
        cy::Span<const u32>(cy::render_test::kFieldProbeBoundSpirv,
                            sizeof(cy::render_test::kFieldProbeBoundSpirv) / sizeof(u32)));
    if (!bound_pipeline) {
        return cy::make_unexpected(bound_pipeline.error());
    }
    cy::Expected<rhi::ComputePipelineHandle, cy::Error> bindless_pipeline = make_pipeline(
        "sampleFieldBindless",
        cy::Span<const u32>(cy::render_test::kFieldProbeBindlessSpirv,
                            sizeof(cy::render_test::kFieldProbeBindlessSpirv) / sizeof(u32)));
    if (!bindless_pipeline) {
        return cy::make_unexpected(bindless_pipeline.error());
    }

    // --- The descriptor sets. Every slot of the table gets the same image, so the bindless entry
    // reads the field whichever slot the push constant names — and the case then asserts that the
    // two entry points agree, which is the claim, rather than that one slot happened to be written.
    cy::Expected<rhi::DescriptorSetHandle, cy::Error> scene_set =
        device.allocate_descriptor_set(*scene_layout, false);
    if (!scene_set) {
        return cy::make_unexpected(scene_set.error());
    }
    rhi::DescriptorWrite table_writes[kTableSlots];
    for (u32 slot = 0; slot < kTableSlots; ++slot) {
        table_writes[slot].binding = 3;
        table_writes[slot].array_index = slot;
        table_writes[slot].kind = rhi::DescriptorKind::StorageBuffer;
        table_writes[slot].buffer = *image;
    }
    if (cy::Status written = device.update_descriptor_set(
            *scene_set, cy::Span<const rhi::DescriptorWrite>(table_writes, kTableSlots));
        !written) {
        return written;
    }

    cy::Expected<rhi::DescriptorSetHandle, cy::Error> probe_set =
        device.allocate_descriptor_set(*probe_layout, false);
    if (!probe_set) {
        return cy::make_unexpected(probe_set.error());
    }
    rhi::DescriptorWrite probe_writes[3];
    const rhi::BufferHandle probe_buffers[3] = {*image, *position_buffer, *answers};
    for (u32 index = 0; index < 3; ++index) {
        probe_writes[index].binding = index;
        probe_writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        probe_writes[index].buffer = probe_buffers[index];
    }
    if (cy::Status written = device.update_descriptor_set(
            *probe_set, cy::Span<const rhi::DescriptorWrite>(probe_writes, 3));
        !written) {
        return written;
    }

    // --- One dispatch per entry point, each preceded by a sentinel prefill and followed by a read.
    const auto dispatch_and_read = [&](rhi::ComputePipelineHandle pipeline, u32 slot,
                                       cy::Array<f32>& into) -> cy::Status {
        auto* words = static_cast<u32*>(device.buffer_mapped_pointer(*answers));
        if (words == nullptr) {
            return cy::fail(cy::ErrorCode::Internal, "the answer buffer is not mapped");
        }
        const u32 total = count * kAnswerWords;
        for (u32 index = 0; index < total; ++index) {
            words[index] = kSentinel;
        }

        cy::Expected<rhi::CommandBufferHandle, cy::Error> command =
            device.acquire_command_buffer(rhi::QueueKind::Graphics, false);
        if (!command) {
            return cy::make_unexpected(command.error());
        }
        if (cy::Status begun = device.begin_command_buffer(*command); !begun) {
            return begun;
        }
        rhi::CommandBuffer& commands = *device.command_buffer(*command);
        const rhi::DescriptorSetHandle sets[2] = {*scene_set, *probe_set};
        const u32 push[4] = {count, slot, 0, 0};
        commands.bind_compute_pipeline(pipeline);
        commands.bind_descriptor_sets(*layout, 0,
                                      cy::Span<const rhi::DescriptorSetHandle>(sets, 2));
        commands.push_constants(
            *layout, rhi::ShaderStage::Compute, 0,
            cy::Span<const u8>(reinterpret_cast<const u8*>(push), sizeof(push)));
        commands.dispatch((count + 63) / 64, 1, 1);
        if (cy::Status ended = device.end_command_buffer(*command); !ended) {
            return ended;
        }
        rhi::SubmitInfo submit;
        submit.queue = rhi::QueueKind::Graphics;
        submit.command_buffers = cy::Span<const rhi::CommandBufferHandle>(&*command, 1);
        cy::Expected<u64, cy::Error> value = device.submit(submit);
        if (!value) {
            return cy::make_unexpected(value.error());
        }
        if (cy::Status waited =
                device.wait_timeline(rhi::QueueKind::Graphics, *value, 30'000'000'000ULL);
            !waited) {
            return waited;
        }

        if (cy::Status reserved = into.reserve(total); !reserved) {
            return reserved;
        }
        for (u32 index = 0; index < total; ++index) {
            if (words[index] == kSentinel) {
                ++out.sentinels_alive;
            }
            f32 value_out = 0.0F;
            std::memcpy(&value_out, &words[index], sizeof(value_out));
            if (cy::Status pushed = into.push_back(value_out); !pushed) {
                return pushed;
            }
        }
        return cy::ok();
    };

    if (cy::Status ran = dispatch_and_read(*bound_pipeline, 0, out.bound); !ran) {
        return ran;
    }
    // SLOT 2 AND NOT SLOT 0, so that the bindless entry is demonstrably indexing the table rather
    // than reading whatever happens to be first in it.
    return dispatch_and_read(*bindless_pipeline, 2, out.bindless);
}

/// What one field's comparison produced. Counted per field, never aggregated across fields — the
/// spike's own refusal counted variety over four fields together and one live field carried three
/// dead ones past the threshold.
struct Agreement {
    u32 compared = 0;
    u32 bit_identical = 0;
    u32 worst_ulp = 0;
    f32 worst_absolute = 0.0F;
    u32 unresolved = 0;
    u32 binding_paths_differ = 0;
    u32 distinct_answers = 0;
};

/// Compare one run against the processor's answers for the same positions.
///
/// `component_for_variety` is which component the distinct-answer count watches. It is a parameter
/// because a field's first component can be the one that varies least, and the refusal has to be
/// able to look at a live one.
[[nodiscard]] Agreement compare(const ProbeRun& run, const cy::Array<FieldValue>& expected,
                                u32 components, u32 component_for_variety) noexcept {
    Agreement agreement;
    f32 seen[64] = {};
    u32 seen_count = 0;
    const auto count = static_cast<u32>(expected.size());
    for (u32 index = 0; index < count; ++index) {
        const usize base = static_cast<usize>(index) * kAnswerWords;
        const f32* device_value = &run.bound[base];
        const f32* bindless_value = &run.bindless[base];
        if (device_value[4] == 0.0F) {
            ++agreement.unresolved;
        }
        for (u32 word = 0; word < kAnswerWords; ++word) {
            if (device_value[word] != bindless_value[word]) {
                ++agreement.binding_paths_differ;
            }
        }
        for (u32 component = 0; component < components; ++component) {
            const f32 processor = expected[index].components[component];
            const f32 device = device_value[component];
            ++agreement.compared;
            if (processor == device) {
                ++agreement.bit_identical;
            }
            const u32 ulp = ulp_distance(processor, device);
            agreement.worst_ulp = (ulp > agreement.worst_ulp) ? ulp : agreement.worst_ulp;
            const f32 difference =
                (processor > device) ? (processor - device) : (device - processor);
            agreement.worst_absolute =
                (difference > agreement.worst_absolute) ? difference : agreement.worst_absolute;
        }
        // A distinct count over a bounded table: enough to tell "the content varies" from "every
        // answer is the same number", which is all this refusal needs to decide.
        bool known = false;
        for (u32 slot = 0; slot < seen_count; ++slot) {
            if (seen[slot] == device_value[component_for_variety]) {
                known = true;
                break;
            }
        }
        if (!known && seen_count < 64) {
            seen[seen_count++] = device_value[component_for_variety];
        }
    }
    agreement.distinct_answers = seen_count;
    return agreement;
}

/// Add one position to the probe's input, with the processor's answer for it.
[[nodiscard]] cy::Status add_position(const FieldGpuImage& image, const WorldVec3d& at,
                                      cy::Array<f32>& positions,
                                      cy::Array<FieldValue>& expected) noexcept {
    const FieldImageLocal local = image_local(image, at);
    if (cy::Status pushed = positions.push_back(local.x); !pushed) {
        return pushed;
    }
    if (cy::Status pushed = positions.push_back(local.y); !pushed) {
        return pushed;
    }
    if (cy::Status pushed = positions.push_back(local.z); !pushed) {
        return pushed;
    }
    return expected.push_back(sample_field_image(image.words.span(), local.x, local.y, local.z));
}

}  // namespace

// ==================================================================================================
// TWO CLAIMS PER FIELD, AND THE SECOND IS WHY THE FIRST IS EVIDENCE
// ==================================================================================================
//
// This is `src/environment/tests/test_gpu.cpp`'s own split, made against a device rather than
// against a second processor-side implementation, and its header explains why it exists: the
// declared-precision bound alone did NOT notice `decode_point()`'s `UNorm8` branch dividing by 256
// instead of 255, because the error that introduces is far inside the field's own quantum. The
// lattice-centre claim was added, the same mutation was run again, and it went red.
//
//   * ACROSS A SWEEP that crosses tile boundaries, agreement within the field's declared precision
//     — the specification's own sentence — with the bit-identical fraction and the worst ULP
//     reported beside it, so a tolerance quietly absorbing a real divergence shows up as a number
//     that moved.
//   * AT LATTICE CENTRES, agreement EXACTLY. The interpolation weight is zero on both sides, so the
//     sample IS one stored value decoded and there is no arithmetic to differ in. Any difference at
//     all is a decode that disagrees, and this is the claim a bound cannot be widened past.
//
// The sweep's ULP figures are reported and not asserted, and that is deliberate rather than lax.
// Measured here: 3 ULP worst on the quantised scalar and 19 on the volumetric vector, and the
// difference is arithmetic rather than a defect — a planar field sums four weighted taps and a
// four-cell column sums eight, so the volumetric blend carries twice the rounding and its worst
// case lands on components near zero, where a ULP is a vanishingly small absolute quantity
// (9.06e-06 against a declared precision of 8e-4). A bound chosen to sit above whichever of those
// two numbers happened to be larger would be a number chosen until the test passed.
//
// ==================================================================================================
// WHY THE EXACT CLAIM IS EXACT FOR F32 AND THREE ULP FOR A QUANTISED ENCODING
// ==================================================================================================
//
// A quantised decode is `rangeMin + ((raw / 255) * span)`, and **Vulkan specifies `OpFDiv` to 2.5
// ULP rather than to correct rounding**. Measured, at a lattice centre where there is no
// interpolation arithmetic at all: a stored `UNorm8` byte of 51 decodes to 0x3E4CCCCD on the
// processor and 0x3E4CCCCE on this device — one ULP, from the division and nothing else. That is
// the device conforming, not disagreeing, and no amount of care in `cy/field.slang` removes it:
// `precise` on the decode was tried and changed nothing, which is what identified the division.
//
// So the bound at lattice centres is **3 ULP** — 2.5 rounded up to a whole one — and it is the
// Vulkan specification's number rather than the measurement's, which is 1. An `F32` encoding does
// no arithmetic at all: the stored bits ARE the answer, and its claim is **bit-identity**, asserted
// with no tolerance and measured at 6912 of 6912.
//
// Three ULP is still far tighter than any decode defect. The two mutations this suite exists to
// catch — dividing by 256 instead of 255, and by 65536 instead of 65535 — move the answer by about
// 65 000 and about 250 ULP respectively, and both stay inside the field's DECLARED PRECISION, which
// is why the declared-precision bound cannot be the check.

CY_TEST_CASE("the device sampler agrees with the processor bit for bit, on a quantised scalar") {
    cy::render_test::DeviceFixture fixture("vulkan", "cy_field_device");
    // SKIPS LOUDLY AND NAMES THE BACKEND IT GOT INSTEAD, which is device.h's own rule: a suite that
    // failed on a machine with no GPU is a suite somebody disables, and one that passed silently is
    // worse. The ledger criterion carries `requires = "gpu"` so a machine without one reports NOT
    // EVALUATED rather than a pass.
    if (!fixture.is(rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }

    FieldRegistry registry(allocator());
    FieldStore store(allocator(), registry, partition());
    const FieldDeclaration moisture = moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(token.has_value());
    // Nine tiles, including negative coordinates — the half of the arithmetic a world east and
    // north of its origin never exercises.
    for (i32 z = -1; z <= 1; ++z) {
        for (i32 x = -1; x <= 1; ++x) {
            CY_REQUIRE(write_pattern(store, *token, moisture, x, z).has_value());
        }
    }

    cy::Expected<FieldGpuImage, cy::Error> image =
        build_field_image(store, moisture.id(), FieldResidency::Local);
    CY_REQUIRE(image.has_value());
    CY_REQUIRE_EQ(image->tiles, 9u);

    // Level 0 cells are 2 m and a tile is 32 m, so this sweep runs from the middle of tile (-1,-1)
    // to the middle of tile (1,1) and crosses both boundaries in both directions — where the two
    // implementations' TILE LOOKUPS have to agree as well as their arithmetic.
    constexpr u32 kSteps = 48;
    cy::Array<f32> sweep_positions(allocator());
    cy::Array<FieldValue> sweep_expected(allocator());
    for (u32 iz = 0; iz < kSteps; ++iz) {
        for (u32 ix = 0; ix < kSteps; ++ix) {
            const double span = 64.0 / static_cast<double>(kSteps);
            CY_REQUIRE(add_position(*image,
                                    WorldVec3d{-20.0 + (static_cast<double>(ix) * span), 0.0,
                                               -20.0 + (static_cast<double>(iz) * span)},
                                    sweep_positions, sweep_expected)
                           .has_value());
        }
    }

    // The cell centres of the same region: (cell + 0.5) * 2 m, so the fractional coordinate is
    // exactly zero on both sides.
    cy::Array<f32> centre_positions(allocator());
    cy::Array<FieldValue> centre_expected(allocator());
    for (i32 iz = -10; iz <= 21; ++iz) {
        for (i32 ix = -10; ix <= 21; ++ix) {
            CY_REQUIRE(add_position(*image,
                                    WorldVec3d{(static_cast<double>(ix) + 0.5) * 2.0, 0.0,
                                               (static_cast<double>(iz) + 0.5) * 2.0},
                                    centre_positions, centre_expected)
                           .has_value());
        }
    }

    ProbeRun sweep(allocator());
    cy::Status ran = run_probe(fixture.device(), image->words.span(), sweep_positions.span(),
                               static_cast<u32>(sweep_expected.size()), sweep);
    CY_CHECK(ran.has_value());
    if (!ran) {
        // Reported, then out. This build compiles doctest with
        // `DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS`, so a failed REQUIRE keeps going and
        // the comparison below would index empty arrays — a red case that reports an abort instead
        // of the device error that caused it.
        CY_TEST_FAIL_CHECK("the sweep dispatch did not complete: " << ran.error().message);
        return;
    }
    ProbeRun centres(allocator());
    ran = run_probe(fixture.device(), image->words.span(), centre_positions.span(),
                    static_cast<u32>(centre_expected.size()), centres);
    CY_CHECK(ran.has_value());
    if (!ran) {
        CY_TEST_FAIL_CHECK("the lattice-centre dispatch did not complete: " << ran.error().message);
        return;
    }

    // THE DISPATCHES RAN. Four of them — two entry points over two position sets — and not one
    // sentinel word survives.
    CY_TEST_MESSAGE("sentinels alive: ", sweep.sentinels_alive + centres.sentinels_alive, " of ",
                    (sweep_expected.size() + centre_expected.size()) * kAnswerWords * 2);
    CY_CHECK_EQ(sweep.sentinels_alive, 0u);
    CY_CHECK_EQ(centres.sentinels_alive, 0u);

    const Agreement across = compare(sweep, sweep_expected, moisture.components(), 0);
    const Agreement exact = compare(centres, centre_expected, moisture.components(), 0);
    CY_TEST_MESSAGE("moisture sweep: ", across.compared, " comparisons, ", across.bit_identical,
                    " bit-identical, worst ", across.worst_ulp, " ulp / ", across.worst_absolute,
                    " against declared precision ", moisture.resolved_precision(), "; ",
                    across.distinct_answers, " distinct answers; ", across.unresolved,
                    " unresolved");
    CY_TEST_MESSAGE("moisture lattice centres: ", exact.compared, " comparisons, ",
                    exact.bit_identical, " bit-identical, worst ", exact.worst_ulp, " ulp");

    // EVERY POSITION RESOLVED. A comparison of two declared defaults agrees perfectly and proves
    // nothing, which is what `m10:sky-field-round-trip` turned out to be.
    CY_CHECK_EQ(across.unresolved, 0u);
    CY_CHECK_EQ(exact.unresolved, 0u);
    // THE CONTENT VARIES. A uniform field agrees under a completely wrong lattice.
    CY_CHECK_GT(across.distinct_answers, 16u);
    // THE BINDING PATH DOES NOT CHANGE THE ANSWER. Bit for bit, both entry points, every word.
    CY_CHECK_EQ(across.binding_paths_differ, 0u);
    CY_CHECK_EQ(exact.binding_paths_differ, 0u);
    // THE SPECIFICATION'S OWN BOUND, across the sweep.
    CY_CHECK_LE(across.worst_absolute, moisture.resolved_precision());
    // AND THE CLAIM A TOLERANCE CANNOT ABSORB: at a lattice centre the sample is one stored byte
    // decoded, so the only arithmetic left is the decode's own division — which Vulkan specifies to
    // 2.5 ULP. Three, and see this file's header for why that is the specification's number rather
    // than the measurement's.
    CY_CHECK_LE(exact.worst_ulp, 3u);
    CY_CHECK_EQ(fixture.validation_errors(), 0u);
}

CY_TEST_CASE("the device sampler agrees with the processor on a volumetric vector field") {
    cy::render_test::DeviceFixture fixture("vulkan", "cy_field_device_volumetric");
    if (!fixture.is(rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }

    FieldRegistry registry(allocator());
    FieldStore store(allocator(), registry, partition());
    const FieldDeclaration wind = wind_like();
    CY_REQUIRE(registry.declare(wind).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(wind.id(), "weather.wind", ProducerKind::System);
    CY_REQUIRE(token.has_value());
    for (i32 z = -1; z <= 1; ++z) {
        for (i32 x = -1; x <= 1; ++x) {
            CY_REQUIRE(write_pattern(store, *token, wind, x, z).has_value());
        }
    }

    cy::Expected<FieldGpuImage, cy::Error> image =
        build_field_image(store, wind.id(), FieldResidency::Local);
    CY_REQUIRE(image.has_value());

    // ALTITUDES INSIDE THE COLUMN, which is the whole reason this case exists. Four cells of 25 m
    // from an origin of zero puts the column between 0 m and 100 m; the sweep samples at 10, 35, 60
    // and 85 m, each of which sits between two vertical lattice points, so `cyFieldVerticalWeight`
    // returns something other than 1.0 and the vertical tap actually blends. Every field M11.a's
    // spike measured was planar, and reassociating the bilinear weight there produced a
    // byte-identical report: the mutation could not fail.
    constexpr double kAltitudes[4] = {10.0, 35.0, 60.0, 85.0};
    constexpr u32 kSteps = 24;
    cy::Array<f32> sweep_positions(allocator());
    cy::Array<FieldValue> sweep_expected(allocator());
    for (double altitude : kAltitudes) {
        for (u32 iz = 0; iz < kSteps; ++iz) {
            for (u32 ix = 0; ix < kSteps; ++ix) {
                const double span = 128.0 / static_cast<double>(kSteps);
                CY_REQUIRE(
                    add_position(*image,
                                 WorldVec3d{-48.0 + (static_cast<double>(ix) * span), altitude,
                                            -48.0 + (static_cast<double>(iz) * span)},
                                 sweep_positions, sweep_expected)
                        .has_value());
            }
        }
    }

    // The lattice centres of the volumetric field: 4 m cells horizontally, and a vertical CELL
    // CENTRE at (j + 0.5) * 25 m, where the vertical weight is zero as well as the two horizontal
    // ones. A volumetric sample there is still one stored triple decoded.
    cy::Array<f32> centre_positions(allocator());
    cy::Array<FieldValue> centre_expected(allocator());
    for (u32 j = 0; j < 4; ++j) {
        for (i32 iz = -8; iz <= 15; ++iz) {
            for (i32 ix = -8; ix <= 15; ++ix) {
                CY_REQUIRE(add_position(*image,
                                        WorldVec3d{(static_cast<double>(ix) + 0.5) * 4.0,
                                                   (static_cast<double>(j) + 0.5) * 25.0,
                                                   (static_cast<double>(iz) + 0.5) * 4.0},
                                        centre_positions, centre_expected)
                               .has_value());
            }
        }
    }

    ProbeRun sweep(allocator());
    cy::Status ran = run_probe(fixture.device(), image->words.span(), sweep_positions.span(),
                               static_cast<u32>(sweep_expected.size()), sweep);
    CY_CHECK(ran.has_value());
    if (!ran) {
        CY_TEST_FAIL_CHECK("the sweep dispatch did not complete: " << ran.error().message);
        return;
    }
    ProbeRun centres(allocator());
    ran = run_probe(fixture.device(), image->words.span(), centre_positions.span(),
                    static_cast<u32>(centre_expected.size()), centres);
    CY_CHECK(ran.has_value());
    if (!ran) {
        CY_TEST_FAIL_CHECK("the lattice-centre dispatch did not complete: " << ran.error().message);
        return;
    }
    CY_CHECK_EQ(sweep.sentinels_alive, 0u);
    CY_CHECK_EQ(centres.sentinels_alive, 0u);

    // Component 1, not 0: the pattern's second component is the negated one, so watching it means
    // the variety refusal is looking at a value the decode had to get the sign of right.
    const Agreement across = compare(sweep, sweep_expected, wind.components(), 1);
    const Agreement exact = compare(centres, centre_expected, wind.components(), 1);
    CY_TEST_MESSAGE("wind sweep (4 vertical cells): ", across.compared, " comparisons, ",
                    across.bit_identical, " bit-identical, worst ", across.worst_ulp, " ulp / ",
                    across.worst_absolute, " against declared precision ",
                    wind.resolved_precision(), "; ", across.distinct_answers, " distinct answers; ",
                    across.unresolved, " unresolved");
    CY_TEST_MESSAGE("wind lattice centres: ", exact.compared, " comparisons, ", exact.bit_identical,
                    " bit-identical, worst ", exact.worst_ulp, " ulp");

    CY_CHECK_EQ(across.unresolved, 0u);
    CY_CHECK_EQ(exact.unresolved, 0u);
    CY_CHECK_GT(across.distinct_answers, 16u);
    CY_CHECK_EQ(across.binding_paths_differ, 0u);
    CY_CHECK_EQ(exact.binding_paths_differ, 0u);
    CY_CHECK_LE(across.worst_absolute, wind.resolved_precision());
    // AN F32 FIELD DECODES WITH NO ARITHMETIC AT ALL: at a lattice centre the stored bits ARE the
    // answer, so this one is asserted with NO tolerance. It is the strongest claim in the suite and
    // the only one of the two that a conforming device cannot move.
    CY_CHECK_EQ(exact.bit_identical, exact.compared);
    CY_CHECK_EQ(exact.worst_ulp, 0u);
    CY_CHECK_EQ(fixture.validation_errors(), 0u);
}
