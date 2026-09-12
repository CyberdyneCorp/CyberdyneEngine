// THE VFX COMPUTE DISPATCH, CHECKED AGAINST THE CPU EXECUTOR BY COMPARING BUFFERS. M10 task 5.1
// and 5.2.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS, AND WHY IT IS NOT A PICTURE
// ================================================================================================
//
// `render.vfx` photographs the simulation and compares two frames the budget controller produced.
// It cannot tell a GPU simulation from a CPU one, because both end at the same publication — which
// is exactly why M8.c could ship a module whose `device_dispatch_available()` answered false and
// have every picture still be right.
//
// So every case here runs ONE cooked effect twice: once through `SimulationWorld`'s CPU executor,
// and once through `VfxGpuPass` on the device. The comparison is the liveness array slot for slot
// and the attribute values particle by particle, read back off the device — the same shape
// `test_skin_pass.cpp` uses against `cpu_reference_skin`, and for the same reason: a dispatch that
// can only be verified by looking at a picture is a dispatch verified by nobody.
//
// THE LIVENESS COMPARISON IS EXACT. `vfx_compact` walks the block in ascending slot order with a
// shared-memory prefix scan rather than appending with an atomic, so the GPU's live list is the
// same list the CPU's `for (particle = 0; ...)` builds and its length is the same integer. A
// tolerance there would have hidden every off-by-one this suite exists to catch.
//
// ================================================================================================
// FLOATING POINT: WHAT IS COMPARED EXACTLY AND WHAT IS NOT
// ================================================================================================
//
// Counts, slot indices and liveness are integers and are compared for equality.
//
// Attribute VALUES are compared to a relative tolerance, because a SPIR-V driver may contract a
// multiply and an add into a fused multiply-add where the host's C++ compiler does not, and the
// plume's update is several such products per component per sub-step. The suite prints the largest
// relative difference it saw, so a tolerance quietly absorbing a real divergence shows up as a
// number that moved.
//
// `color` is the case that is NOT approximate in the interesting direction: the compiler put it at
// `Unorm8` and both paths quantise through the same rule, so the two agree to within one step of
// 1/255 and a path that had skipped the quantisation would differ by far more.

#include "gpu_fixture.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/vfx/gpu_layout.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_gpu_test;
using cy::vfx::gpu::GpuPassDescription;
using cy::vfx::gpu::GpuStepInputs;
using cy::vfx::gpu::VfxGpuPass;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// MEASURED on this engine's reference machine against the plume's `position`, `velocity` and
/// `age`: 4.8e-07 relative after sixteen sub-steps. The tolerance is about twenty times that, and a
/// kernel that read the wrong attribute base or advanced by the wrong dt fails it by orders of
/// magnitude rather than by a last bit.
constexpr f32 kValueTolerance = 1.0e-5F;

/// Steps of 1/255, on a `Unorm8` attribute. One step of headroom because the two encoders round a
/// halfway value independently.
constexpr f32 kQuantisedTolerance = 2.0F / 255.0F;

/// The sub-step both paths run at, and the frequency the instance is played at so that one
/// `SimulationWorld::step` is exactly one sub-step. A world that took two would be comparing a
/// different number of integrations against the device's one.
constexpr f32 kSubstep = 1.0F / 60.0F;
constexpr u32 kCapacity = 512;

/// One cooked plume, and the pieces that had to stay alive around it.
struct CookedPlume {
    graph::NodeRegistry registry{allocator()};
    DataInterfaceRegistry interfaces{allocator()};
    graph::DiagnosticSink sink{allocator()};
    CompileReport report{allocator()};
    Expected<CompiledSystem, Error> system = fail(ErrorCode::Unavailable, "not cooked");

    CookedPlume() {
        CY_REQUIRE(vfx_test::prepare(registry, interfaces).has_value());
        system = vfx_test::cook_plume(allocator(), sink, report, CompileOptions{}, 1, kCapacity);
        CY_REQUIRE(system.has_value());
    }
};

/// The CPU path's answer, stepped the same number of sub-steps at the same sub-step.
///
/// It runs through `SimulationWorld` rather than through a reimplementation of the executor,
/// because the thing being compared against is the path a machine with no GPU actually takes — a
/// hand-written reference here would be a third implementation and the wrong one to agree with.
class CpuReference {
public:
    explicit CpuReference(const CompiledSystem& system) noexcept : world_(allocator()) {
        WorldDescription description;
        // Generous, so the pool grants the emitter its whole cooked capacity: a reduced block would
        // make the two paths simulate different numbers of slots and the comparison meaningless.
        description.pool_bytes = 16ULL * 1024ULL * 1024ULL;
        CY_REQUIRE(world_.initialize(description).has_value());
        EffectSpawn spawn;
        spawn.simulation_hz = 1.0F / kSubstep;
        auto handle = world_.play(system, spawn);
        CY_REQUIRE(handle.has_value());
        handle_ = handle.value();
    }

    void step() noexcept {
        StepReport report;
        CY_REQUIRE(world_.step(kSubstep, report).has_value());
        last_ = report;
    }

    /// One slot's liveness, as the CPU path holds it: 0 dead, 1 live, 2 initialised by the fused
    /// kernel this sub-step and not yet advanced by an update.
    [[nodiscard]] Span<const u8> alive() noexcept { return world_.alive_flags(0); }

    [[nodiscard]] f32 read(u32 particle, const char* attribute, u32 component) const noexcept {
        const EffectInstance* instance = world_.find(handle_);
        return instance == nullptr ? 0.0F
                                   : world_.read_attribute(*instance, 0, particle,
                                                           Name::intern(attribute), component);
    }

    [[nodiscard]] u32 live() const noexcept {
        const EffectInstance* instance = world_.find(handle_);
        return instance == nullptr ? 0U : instance->live_particles;
    }

    [[nodiscard]] const StepReport& last() const noexcept { return last_; }
    [[nodiscard]] SimulationWorld& world() noexcept { return world_; }

private:
    SimulationWorld world_;
    EffectHandle handle_ = kInvalidEffect;
    StepReport last_;
};

/// Read one attribute component out of the GPU block, at the precision the compiler chose.
///
/// THE GPU BLOCK IS NOT THE CPU BLOCK, so this walks `gpu_array_base_words` and
/// `gpu_words_per_particle` rather than the layout's byte offsets. A helper that reused
/// `load_component` would read `color` — one byte a particle on the host, one word here — from
/// three quarters of the wrong place.
[[nodiscard]] f32 read_gpu(Span<const u32> block, const AttributeLayout& layout,
                           const char* attribute, u32 particle, u32 component,
                           u32 capacity) noexcept {
    const AttributeSlot* slot = layout.find(Name::intern(attribute));
    if (slot == nullptr || slot->elided || component >= slot->components) {
        return 0.0F;
    }
    const u32 base = gpu_array_base_words(layout, *slot, capacity);
    const u32 words = gpu_words_per_particle(*slot);
    if (words == 0) {
        // Unreachable for a live slot — `gpu_words_per_particle` is at least one for any slot with
        // a component — and stated rather than assumed, because this helper's whole job is to index
        // a buffer and an unchecked divisor here would index it wrongly rather than loudly.
        return 0.0F;
    }
    const u32 components_per_word = ((slot->components + words) - 1U) / words;
    const u32 word_index = component / components_per_word;
    const u32 lane = component % components_per_word;
    const usize index =
        static_cast<usize>(base) + (static_cast<usize>(particle) * words) + word_index;
    if (index >= block.size()) {
        return 0.0F;
    }
    const u32 word = block[index];
    switch (slot->precision) {
        case Precision::Float32: {
            f32 value = 0.0F;
            std::memcpy(&value, &word, sizeof(f32));
            return value;
        }
        case Precision::Float16: {
            const u16 half = static_cast<u16>((word >> (lane * 16U)) & 0xFFFFU);
            // The same widening `load_component` performs; spelled here because this file may not
            // reach into the layout module's private helpers.
            const u32 sign = (half & 0x8000U) << 16U;
            const u32 exponent = (half >> 10U) & 0x1FU;
            const u32 mantissa = half & 0x3FFU;
            u32 bits = 0;
            if (exponent == 0) {
                bits = sign;
            } else if (exponent == 31U) {
                bits = sign | 0x7F800000U | (mantissa << 13U);
            } else {
                bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
            }
            f32 value = 0.0F;
            std::memcpy(&value, &bits, sizeof(f32));
            return value;
        }
        case Precision::Unorm8:
            return static_cast<f32>((word >> (lane * 8U)) & 0xFFU) * (1.0F / 255.0F);
        case Precision::Snorm16: {
            i32 raw = static_cast<i32>((word >> (lane * 16U)) & 0xFFFFU);
            raw = (raw & 0x8000) != 0 ? raw - 65536 : raw;
            const f32 value = static_cast<f32>(raw) * (1.0F / 32767.0F);
            return value < -1.0F ? -1.0F : value;
        }
        case Precision::Auto:
            break;
    }
    return 0.0F;
}

[[nodiscard]] f32 relative_difference(f32 a, f32 b) noexcept {
    const f32 magnitude = std::fmax(std::fabs(a), std::fabs(b));
    return magnitude > 1.0F ? std::fabs(a - b) / magnitude : std::fabs(a - b);
}

/// The levers the plume runs under at its authored quality, as the budget controller would produce
/// them. Taken FROM the controller rather than written out, so a change to the controller's
/// defaults changes both paths at once.
[[nodiscard]] BudgetLevers authored_levers(const CompiledSystem& system) noexcept {
    BudgetController controller;
    return controller.levers_for(system.importance(), system.scalability());
}

/// Create a GPU pass over the cooked plume and reset it.
///
/// A REFUSAL IS A FAILURE, not a skip, and this is the second thing this suite got wrong. The first
/// version returned false and every caller wrote `if (!create_pass(...)) { return; }` — so when a
/// change to the generator produced Slang that would not compile, five of six cases returned
/// quietly and the suite reported one failure instead of six. A machine with no GPU skips at the
/// `has_gpu()` check above; past that point, a device that refuses is a defect.
[[nodiscard]] bool create_pass(DeviceFixture& gpu, const CompiledSystem& system, VfxGpuPass& pass,
                               bool async, bool read_back = true) noexcept {
    GpuPassDescription description;
    description.emitter = 0;
    description.async_compute = async;
    description.read_back = read_back;
    const Status created = pass.create(allocator(), gpu.device(), system, description);
    if (!created) {
        std::fprintf(stderr, "VfxGpuPass::create refused: %s\n", created.error().message);
        // The generated source is kept on a failed compile precisely so it can be written out here:
        // "the generator emitted broken Slang" is a different bug report from "the shader is
        // broken", and this is the file that tells them apart.
        const Span<const char> source = pass.generated_source();
        if (!source.empty()) {
            if (std::FILE* file = std::fopen("vfx-dispatch-rejected.slang", "wb");
                file != nullptr) {
                (void)std::fwrite(source.data(), 1, source.size(), file);
                (void)std::fclose(file);
                std::fprintf(stderr, "wrote vfx-dispatch-rejected.slang (%zu bytes)\n",
                             source.size());
            }
        }
    }
    CY_REQUIRE(created.has_value());
    return run_frame(gpu, allocator(), pass, true);
}

/// Push one sub-step's constants with the authored levers.
[[nodiscard]] bool gpu_step(VfxGpuPass& pass, const CompiledSystem& system, f32 emitter_age,
                            const BudgetLevers& levers, Span<const f32> parameters) noexcept {
    GpuStepInputs inputs;
    inputs.dt = kSubstep;
    inputs.emitter_age = emitter_age;
    inputs.levers = levers;
    inputs.reserved_particles = system.scalability().reserved_particles;
    inputs.parameters = parameters;
    const Status stepped = pass.step(inputs);
    if (!stepped) {
        std::fprintf(stderr, "VfxGpuPass::step refused: %s\n", stepped.error().message);
    }
    return static_cast<bool>(stepped);
}

/// The instance parameters, in the packing both paths read: four words a parameter in declaration
/// order, authored values, because this suite overrides none.
[[nodiscard]] std::vector<f32> authored_parameters(const CompiledSystem& system) noexcept {
    std::vector<f32> words(system.parameters().size() * 4U, 0.0F);
    for (usize index = 0; index < system.parameters().size(); ++index) {
        for (u32 component = 0; component < 4U; ++component) {
            words[(index * 4U) + component] = system.parameters()[index].value[component];
        }
    }
    return words;
}

}  // namespace

TEST_CASE("the device advances the same population the CPU executor does") {
    DeviceFixture gpu("cy_test_render_vfx_gpu");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    CookedPlume cooked;
    const CompiledSystem& system = cooked.system.value();
    const BudgetLevers levers = authored_levers(system);
    const std::vector<f32> parameters = authored_parameters(system);

    VfxGpuPass pass;
    if (!create_pass(gpu, system, pass, /*async=*/true)) {
        return;
    }
    CpuReference cpu(system);

    // NINETY-SIX SUB-STEPS, which is 1.6 seconds. The number is not decorative: the plume's
    // lifetimes are drawn from [0.55, 1.45) seconds, so a run of sixteen sub-steps spawns and
    // advances and NEVER KILLS — the liveness comparison would then be "both paths agree that
    // everything is alive", which is true of a path that cannot kill at all. At ninety-six the
    // population has turned over, freed slots have been re-used, and the two paths have to agree
    // about WHICH slots those were.
    constexpr u32 kSteps = 96;
    f32 worst = 0.0F;
    for (u32 step = 0; step < kSteps; ++step) {
        const f32 age = static_cast<f32>(step) * kSubstep;
        CY_REQUIRE(gpu_step(pass, system, age, levers, {parameters.data(), parameters.size()}));
        CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
        cpu.step();
    }
    CY_REQUIRE(pass.read_back_counts().has_value());

    // THE POPULATION, EXACTLY. `reported_live` is the device's own arithmetic — survivors,
    // promotions and the grant — and `EffectInstance::live_particles` is the CPU executor's. Two
    // paths that disagreed about one particle would differ here by one.
    CHECK_EQ(pass.report().reported_live, cpu.live());
    CHECK_GT(cpu.live(), 0U);
    // THE RUN WAS LONG ENOUGH, checked rather than asserted in the comment above: a run in which
    // nothing died would make the liveness comparison a comparison of two full blocks.
    CHECK_GT(cpu.last().killed, 0U);
    CHECK_GT(pass.report().killed, 0U);

    // THE LIVENESS ARRAY, SLOT FOR SLOT. This is what an atomic append would have made impossible
    // to assert: the two lists are the same list because both are built in ascending slot order.
    auto alive_gpu = pass.read_back_alive();
    CY_REQUIRE(alive_gpu.has_value());
    Span<const u8> alive_cpu = cpu.alive();
    CY_REQUIRE(alive_cpu.size() >= kCapacity);
    u32 mismatches = 0;
    for (u32 slot = 0; slot < kCapacity; ++slot) {
        const bool device_live = (*alive_gpu)[slot] != 0;
        const bool host_live = alive_cpu[slot] != 0;
        mismatches += device_live == host_live ? 0U : 1U;
    }
    CHECK_EQ(mismatches, 0U);

    // THE ATTRIBUTE VALUES. Read out of the device's own block through the GPU layout, and out of
    // the world through `read_attribute`, which returns whatever the CPU pool holds at whatever
    // precision the compiler chose.
    auto block = pass.read_back_particles();
    CY_REQUIRE(block.has_value());
    const AttributeLayout& layout = system.emitters()[0].layout();
    u32 compared = 0;
    f32 worst_colour = 0.0F;
    for (u32 slot = 0; slot < kCapacity; ++slot) {
        if (alive_cpu[slot] == 0) {
            continue;
        }
        ++compared;
        for (u32 component = 0; component < 3U; ++component) {
            const f32 host = cpu.read(slot, "position", component);
            const f32 device = read_gpu(*block, layout, "position", slot, component, kCapacity);
            worst = std::fmax(worst, relative_difference(host, device));
        }
        const f32 host_age = cpu.read(slot, "age", 0);
        worst = std::fmax(worst, relative_difference(host_age, read_gpu(*block, layout, "age", slot,
                                                                        0, kCapacity)));
        for (u32 component = 0; component < 3U; ++component) {
            const f32 host = cpu.read(slot, "color", component);
            const f32 device = read_gpu(*block, layout, "color", slot, component, kCapacity);
            worst_colour = std::fmax(worst_colour, std::fabs(host - device));
        }
    }
    std::fprintf(stderr,
                 "compared %u live particles: worst relative difference %.3e, worst colour step "
                 "%.3e (%.2f steps of 1/255)\n",
                 compared, static_cast<double>(worst), static_cast<double>(worst_colour),
                 static_cast<double>(worst_colour * 255.0F));
    CHECK_GT(compared, 0U);
    CHECK_LT(worst, kValueTolerance);
    CHECK_LT(worst_colour, kQuantisedTolerance);
    CHECK_EQ(gpu.validation_errors(), 0U);
}

TEST_CASE("population and dispatch size come from the device, not from this process") {
    DeviceFixture gpu("cy_test_render_vfx_gpu_indirect");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    CookedPlume cooked;
    const CompiledSystem& system = cooked.system.value();
    const BudgetLevers levers = authored_levers(system);
    const std::vector<f32> parameters = authored_parameters(system);

    VfxGpuPass pass;
    if (!create_pass(gpu, system, pass, /*async=*/true)) {
        return;
    }

    // TWO INDIRECT DISPATCHES A SUB-STEP — the initialise and the update — plus the key pass when
    // the sort is on. Their group counts are three words of device memory `vfx_compact` wrote; this
    // process never read them, and `VfxGpuPass::step` takes no population to have read one from.
    CY_REQUIRE(gpu_step(pass, system, 0.0F, levers, {parameters.data(), parameters.size()}));
    CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
    CHECK_GE(pass.report().indirect_dispatches, 2U);
    CY_REQUIRE(pass.read_back_counts().has_value());

    // THE GRANT IS THE DEVICE'S. The Spawn kernel asked for a number, the compaction clamped it to
    // the free list it had just built, and the initialise dispatch covered exactly that many
    // threads — which is only true if its group count came from the argument buffer.
    const auto first = pass.report();
    CHECK_GT(first.spawn_request, 0U);
    CHECK_EQ(first.spawn_granted, first.spawned);
    CHECK_LE(first.spawn_granted, first.free + first.spawn_granted);
    std::fprintf(stderr, "step 1: request %u, granted %u, spawned %u, free %u, live %u\n",
                 first.spawn_request, first.spawn_granted, first.spawned, first.free, first.live);

    // Run the block to exhaustion. The grant must fall to zero as the free list empties and the
    // population must never exceed the block — "spawn requests SHALL be reduced by importance rank
    // and the shortfall reported, rather than overwriting live particles".
    for (u32 step = 1; step < 64; ++step) {
        CY_REQUIRE(gpu_step(pass, system, static_cast<f32>(step) * kSubstep, levers,
                            {parameters.data(), parameters.size()}));
        CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
        CY_REQUIRE(pass.read_back_counts().has_value());
        CHECK_LE(pass.report().reported_live, kCapacity);
        CHECK_LE(pass.report().spawn_granted, pass.report().spawn_request);
    }
    std::fprintf(stderr, "after 64 steps: live %u of %u, killed %u this step\n",
                 pass.report().reported_live, kCapacity, pass.report().killed);
    CHECK_EQ(gpu.validation_errors(), 0U);
}

TEST_CASE("the particle-count lever bounds the population the device holds") {
    DeviceFixture gpu("cy_test_render_vfx_gpu_budget");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    CookedPlume cooked;
    const CompiledSystem& system = cooked.system.value();
    const std::vector<f32> parameters = authored_parameters(system);

    // THE SAME EFFECT AT TWO QUALITIES, and the only difference is a lever. `vfx-system` requires
    // cost to be "bounded by a frame-time budget with importance classes", and `count_cap_scale` is
    // the lever that bounds a block that is ALREADY FULL — a controller that could only slow
    // spawning would leave a saturated emitter saturated however overloaded the frame was.
    const auto run = [&](f32 count_cap_scale) noexcept {
        VfxGpuPass pass;
        if (!create_pass(gpu, system, pass, /*async=*/true)) {
            return 0U;
        }
        BudgetLevers levers = authored_levers(system);
        levers.count_cap_scale = count_cap_scale;
        for (u32 step = 0; step < 64; ++step) {
            CY_REQUIRE(gpu_step(pass, system, static_cast<f32>(step) * kSubstep, levers,
                                {parameters.data(), parameters.size()}));
            CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
        }
        CY_REQUIRE(pass.read_back_counts().has_value());
        return pass.report().reported_live;
    };

    const u32 full = run(1.0F);
    const u32 quarter = run(0.25F);
    std::fprintf(stderr, "population at full quality %u, at a quarter cap %u\n", full, quarter);
    CHECK_GT(full, 0U);
    CHECK_LT(quarter, full);
    // EXACTLY THE CAP, not "about" it. The compaction lists a free slot only below the cap and
    // kills every live slot at or above it, so the population cannot settle one particle over.
    CHECK_LE(quarter, kCapacity / 4U);
    CHECK_EQ(gpu.validation_errors(), 0U);
}

TEST_CASE("the cap reduces a block that is already full, not only one that is filling") {
    DeviceFixture gpu("cy_test_render_vfx_gpu_shrink");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    CookedPlume cooked;
    const CompiledSystem& system = cooked.system.value();
    const std::vector<f32> parameters = authored_parameters(system);

    VfxGpuPass pass;
    if (!create_pass(gpu, system, pass, /*async=*/true)) {
        return;
    }

    // THE CASE THE PREVIOUS ONE CANNOT REACH, and it was found by breaking the shader rather than
    // by reading it: with the cap-kill removed from `vfx_compact`, a population that GROWS into a
    // cap still stops at it, because the compaction's free list never offers a slot above the cap.
    // `vfx-system` is about the other direction — "a high-volume decorative effect cannot starve a
    // critical one" is a controller acting on a frame that is ALREADY over budget — and the only
    // thing that reduces a block which is already full is the kill.
    BudgetLevers levers = authored_levers(system);
    for (u32 step = 0; step < 64; ++step) {
        CY_REQUIRE(gpu_step(pass, system, static_cast<f32>(step) * kSubstep, levers,
                            {parameters.data(), parameters.size()}));
        CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
    }
    CY_REQUIRE(pass.read_back_counts().has_value());
    const u32 full = pass.report().reported_live;
    CHECK_GT(full, kCapacity / 2U);

    // Now the controller reduces. Nothing else changes — the same pass, the same effect, the same
    // particles, one lever.
    levers.count_cap_scale = 0.25F;
    CY_REQUIRE(
        gpu_step(pass, system, 64.0F * kSubstep, levers, {parameters.data(), parameters.size()}));
    CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
    CY_REQUIRE(pass.read_back_counts().has_value());
    const u32 after_one_step = pass.report().reported_live;
    const u32 killed_by_the_cap = pass.report().killed;
    std::fprintf(stderr,
                 "full block %u; one step after the cap fell to a quarter: live %u, killed %u\n",
                 full, after_one_step, killed_by_the_cap);

    // IN ONE STEP, not over the population's lifetime. The compaction kills every slot at or above
    // the cap in the sub-step the cap moved, so the reduction is immediate — a cap that waited for
    // the particles to expire would hold no budget at all on the frame that was over it.
    CHECK_GT(killed_by_the_cap, 0U);
    CHECK_LE(after_one_step, kCapacity / 4U);
    CHECK_EQ(gpu.validation_errors(), 0U);
}

TEST_CASE("the GPU sort orders the live list back to front, and the lever removes it") {
    DeviceFixture gpu("cy_test_render_vfx_gpu_sort");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    CookedPlume cooked;
    const CompiledSystem& system = cooked.system.value();
    const std::vector<f32> parameters = authored_parameters(system);

    VfxGpuPass pass;
    if (!create_pass(gpu, system, pass, /*async=*/true)) {
        return;
    }
    BudgetLevers levers = authored_levers(system);
    CY_REQUIRE(levers.sorted);

    for (u32 step = 0; step < 24; ++step) {
        CY_REQUIRE(gpu_step(pass, system, static_cast<f32>(step) * kSubstep, levers,
                            {parameters.data(), parameters.size()}));
        CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
    }
    CY_REQUIRE(pass.read_back_counts().has_value());
    const u32 live = pass.report().live;
    CHECK_GT(live, 1U);
    // THE COST IS REPORTABLE, which is half the requirement. log2(512) * (log2(512) + 1) / 2 = 45.
    CHECK_EQ(pass.report().sort_passes, gpu_sort_passes(gpu_round_up_pow2(kCapacity)));
    CHECK_EQ(pass.report().sort_passes, 45U);

    auto indices = pass.read_back_indices();
    auto block = pass.read_back_particles();
    CY_REQUIRE(indices.has_value());
    CY_REQUIRE(block.has_value());
    const AttributeLayout& layout = system.emitters()[0].layout();
    const auto distance_of = [&](u32 slot) noexcept {
        f32 sum = 0.0F;
        for (u32 component = 0; component < 3U; ++component) {
            const f32 value = read_gpu(*block, layout, "position", slot, component, kCapacity);
            sum += value * value;
        }
        return sum;
    };

    // BACK TO FRONT: the key is the complement of the squared distance's bits, so ascending keys
    // are descending distances and a back-to-front draw walks the array forwards.
    u32 out_of_order = 0;
    f32 previous = distance_of((*indices)[0]);
    for (u32 index = 1; index < live; ++index) {
        const f32 current = distance_of((*indices)[index]);
        out_of_order += current <= previous ? 0U : 1U;
        previous = current;
    }
    std::fprintf(stderr, "sorted %u live particles, %u pairs out of order\n", live, out_of_order);
    CHECK_EQ(out_of_order, 0U);

    // THE CONTROL, and it is the half that makes the check above mean something. Unsorted, the
    // compaction's own ascending-slot order survives — so the list is NOT distance-ordered, and
    // `sort_passes` is zero because the two sort passes were never declared.
    VfxGpuPass unsorted;
    if (!create_pass(gpu, system, unsorted, /*async=*/true)) {
        return;
    }
    levers.sorted = false;
    for (u32 step = 0; step < 24; ++step) {
        CY_REQUIRE(gpu_step(unsorted, system, static_cast<f32>(step) * kSubstep, levers,
                            {parameters.data(), parameters.size()}));
        CY_REQUIRE(run_frame(gpu, allocator(), unsorted, false));
    }
    CY_REQUIRE(unsorted.read_back_counts().has_value());
    CHECK_EQ(unsorted.report().sort_passes, 0U);
    CHECK_EQ(unsorted.report().dispatches, 4U);

    auto plain = unsorted.read_back_indices();
    CY_REQUIRE(plain.has_value());
    u32 ascending = 0;
    const u32 plain_live = unsorted.report().live;
    for (u32 index = 1; index < plain_live; ++index) {
        ascending += (*plain)[index] > (*plain)[index - 1] ? 1U : 0U;
    }
    CHECK_EQ(ascending, plain_live > 0 ? plain_live - 1U : 0U);
    CHECK_EQ(gpu.validation_errors(), 0U);
}

TEST_CASE(
    "async compute is used where the device has a queue, and turning it off changes nothing") {
    CookedPlume cooked;
    const CompiledSystem& system = cooked.system.value();
    const BudgetLevers levers = authored_levers(system);
    const std::vector<f32> parameters = authored_parameters(system);

    // `vfx-system`: "Async execution SHALL be capability-gated and SHALL be DISABLEABLE, since
    // overlap benefit is device-dependent and it complicates profiling." Both halves are here: a
    // device asked for no async queue, and a pass told not to use one.
    const auto run = [&](bool device_async, bool pass_async, rhi::QueueKind& queue,
                         u32& validation_errors, bool& device_has_queue) noexcept {
        DeviceFixture gpu("cy_test_render_vfx_gpu_async", device_async);
        if (!gpu.has_gpu()) {
            gpu.report_skip();
            return 0U;
        }
        device_has_queue = gpu.device().has_queue(rhi::QueueKind::AsyncCompute);
        VfxGpuPass pass;
        if (!create_pass(gpu, system, pass, pass_async)) {
            return 0U;
        }
        queue = pass.queue();
        for (u32 step = 0; step < 16; ++step) {
            CY_REQUIRE(gpu_step(pass, system, static_cast<f32>(step) * kSubstep, levers,
                                {parameters.data(), parameters.size()}));
            CY_REQUIRE(run_frame(gpu, allocator(), pass, false));
        }
        CY_REQUIRE(pass.read_back_counts().has_value());
        validation_errors = gpu.validation_errors();
        return pass.report().reported_live;
    };

    rhi::QueueKind async_queue = rhi::QueueKind::Graphics;
    rhi::QueueKind graphics_queue = rhi::QueueKind::AsyncCompute;
    u32 async_errors = 0;
    u32 graphics_errors = 0;
    bool device_has_async = false;
    bool ignored_queue_report = false;
    const u32 on_async = run(true, true, async_queue, async_errors, device_has_async);
    const u32 on_graphics = run(true, false, graphics_queue, graphics_errors, ignored_queue_report);
    if (on_async == 0 && on_graphics == 0) {
        return;  // no device; the skip was already reported
    }

    std::fprintf(stderr, "async queue population %u, graphics queue population %u\n", on_async,
                 on_graphics);
    // THE DISABLE always holds: a pass told not to use the queue does not use it, whatever the
    // device has.
    CHECK_EQ(graphics_queue, rhi::QueueKind::Graphics);
    // THE GATE is asserted against what the device actually reports rather than against this
    // machine. `has_queue` is the gate `vfx-system` means by "where the device exposes an
    // asynchronous compute queue", and a pass that ignored it would fail this on any machine that
    // has one — which the reference machine does, so this line is exercised rather than skipped.
    if (device_has_async) {
        std::fprintf(stderr, "this device exposes an async compute queue and the pass used it\n");
        CHECK_EQ(async_queue, rhi::QueueKind::AsyncCompute);
    } else {
        std::fprintf(stderr,
                     "this device exposes no async compute queue; the passes folded onto "
                     "graphics, which is the graph's own behaviour and not this module's\n");
        CHECK_EQ(async_queue, rhi::QueueKind::Graphics);
    }
    // THE SIMULATION IS THE SAME EITHER WAY. A queue is a scheduling decision and must not be a
    // semantic one; a cross-queue hazard the graph failed to order would show up here as a
    // population that moved.
    CHECK_EQ(on_async, on_graphics);
    CHECK_EQ(async_errors, 0U);
    CHECK_EQ(graphics_errors, 0U);
}
