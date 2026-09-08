#pragma once
// The closed loop, modelled: seven subsystems whose cost the levers actually change, a readback
// latency, and measurement noise. Shared by `test_arbiter.cpp` and `test_sweep.cpp`.
//
// WHY A MODEL AND NOT A GPU. The criterion of task 10.1 is "convergence without oscillation under a
// step load", and `design.md` §2.1 is emphatic that a control law over a DISCRETE ladder must be
// certified over a sweep of loads rather than one — because where the equilibrium lands relative to
// a ladder boundary is what decides whether it oscillates, and one step magnitude can make any law
// look stable. The spike's own first draft passed at one load and oscillated on 27 of 71.
//
// A sweep is 71 loads x 1,400 frames. Nothing that renders can be run 100,000 times inside a test
// budget, and a law certified on one load is not certified. So the arbiter is built to name no
// device — see this module's CMakeLists.txt — and the model here is what closes the loop around it.
//
// What the model does NOT claim: it is not a measurement of any real renderer's cost curve. Its
// job is to be a plant with the one property that matters — a discrete ladder whose steps change
// the measured cost by the declared ratio — so that the control law is what is under test.

#include <cy/core/base/types.h>
#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/profiles.h>
#include <cy/rendering/arbiter/subsystem.h>

namespace cy::rendering::test {

/// Deterministic, and deliberately not `<random>`: a test whose result depends on a standard
/// library's engine is a test that reports different numbers on a different platform.
class Xorshift {
public:
    explicit Xorshift(u32 seed) noexcept : state_(seed | 1U) {}

    [[nodiscard]] u32 next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return state_;
    }

    /// A symmetric multiplier in [1 - amplitude, 1 + amplitude].
    [[nodiscard]] f32 jitter(f32 amplitude) noexcept {
        const f32 unit = (static_cast<f32>(next() % 2001U) / 1000.0F) - 1.0F;
        return 1.0F + (unit * amplitude);
    }

private:
    u32 state_;
};

/// Two frames of GPU-timestamp readback latency, which is what an engine actually has.
/// `design.md` §2.8 measured that one to eight frames costs nothing at any gain from 0.20 to 2.00.
inline constexpr u32 kReadbackFrames = 2;

struct FrameSample {
    f32 frame_ms = 0.0F;
    f32 subsystem_ms[kBudgetSubsystemCount] = {};
    u8 position[kBudgetSubsystemCount] = {};
    bool at_minimum[kBudgetSubsystemCount] = {};
};

/// What one stepped frame did, for a caller counting lever changes.
struct StepResult {
    ArbiterReport report;
    /// Any controller moved a position, or resolution scale moved.
    bool lever_changed = false;
    /// The cost the frame actually had this tick, before the readback delay.
    f32 true_frame_ms = 0.0F;
};

/// The plant plus the loop.
class Renderer {
public:
    Renderer() noexcept = default;

    /// Register the profile's subsystems against both the arbiter and a controller each, and take
    /// the profile's declared base costs as the model's nominal costs at load 1.0.
    [[nodiscard]] Status build(const RendererProfile& profile) noexcept {
        profile_ = profile;
        if (auto applied = apply_profile(profile, arbiter_); !applied) {
            return applied;
        }
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            registered_[index] = profile.registered[index];
            if (!registered_[index]) {
                continue;
            }
            if (auto declared = controllers_[index].declare(profile.subsystems[index]); !declared) {
                return declared;
            }
            nominal_ms_[index] = profile.subsystems[index].base_cost_ms;
            controllers_[index].set_allocation_ms(
                arbiter_.allocation_ms(static_cast<BudgetSubsystem>(index)));
        }
        return {};
    }

    /// Move the OPERATING POINT: scale every subsystem's nominal cost, so the nominal state sits
    /// closer to or further from the budget without changing the step load.
    ///
    /// The sweep that certifies this arbiter varies the step magnitude, because one magnitude can
    /// make any control law look stable. M7's gate established that one NOMINAL COST can do the
    /// same: the law is clean with generous headroom and oscillates on 7 of 71 loads when the
    /// nominal state sits 0.90 ms below a 13.90 ms budget. Both axes have to be swept or the
    /// convergence claim is only tested where it is comfortable.
    void scale_nominal(f32 factor) noexcept {
        for (f32& nominal : nominal_ms_) {
            nominal *= factor;
        }
    }

    /// Every registered subsystem's nominal cost at authored quality, which is what the envelope
    /// in `rendering-architecture` measures headroom against.
    [[nodiscard]] f32 nominal_total_ms() const noexcept {
        f32 total = 0.0F;
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            total += registered_[index] ? nominal_ms_[index] : 0.0F;
        }
        return total;
    }

    void set_load(f32 load) noexcept { load_ = load; }
    void set_noise(f32 amplitude) noexcept { noise_ = amplitude; }
    void set_pinned(bool pinned) noexcept {
        // "Pinned mode is total": one call, and the caller has no way to pin half of it.
        arbiter_.set_pinned(pinned);
        for (SubsystemController& controller : controllers_) {
            controller.set_pinned(pinned);
        }
    }

    [[nodiscard]] BudgetArbiter& arbiter() noexcept { return arbiter_; }
    [[nodiscard]] const SubsystemController& controller(BudgetSubsystem subsystem) const noexcept {
        return controllers_[static_cast<u32>(subsystem)];
    }

    /// The frame cost the model would have right now, noise excluded.
    [[nodiscard]] f32 true_frame_ms() const noexcept {
        f32 total = profile_.arbiter.non_allocatable_ms;
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            if (registered_[index]) {
                total += true_subsystem_ms(index);
            }
        }
        return total;
    }

    [[nodiscard]] StepResult step() noexcept {
        StepResult result;

        // 1. The plant: what this frame actually cost, with measurement noise on each part.
        FrameSample sample;
        f32 total = profile_.arbiter.non_allocatable_ms;
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            if (!registered_[index]) {
                continue;
            }
            const f32 cost = true_subsystem_ms(index) * rng_.jitter(noise_);
            sample.subsystem_ms[index] = cost;
            sample.position[index] = controllers_[index].position();
            sample.at_minimum[index] = controllers_[index].at_minimum();
            total += cost;
        }
        sample.frame_ms = total;
        result.true_frame_ms = true_frame_ms();

        // 2. The readback: measurements arrive `kReadbackFrames` late.
        const FrameSample delayed = ring_[ring_head_];
        ring_[ring_head_] = sample;
        ring_head_ = (ring_head_ + 1U) % kReadbackFrames;
        if (!primed_) {
            primed_ = ++primed_count_ >= kReadbackFrames;
            return result;
        }

        // 3. Only the arbiter is given the frame.
        arbiter_.report_frame_ms(delayed.frame_ms);
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            if (!registered_[index]) {
                continue;
            }
            const auto subsystem = static_cast<BudgetSubsystem>(index);
            arbiter_.report_subsystem(subsystem, delayed.subsystem_ms[index],
                                      delayed.position[index], delayed.at_minimum[index]);
            controllers_[index].report_measured_ms(delayed.subsystem_ms[index]);
        }

        const f32 scale_before = arbiter_.resolution_scale();
        result.report = arbiter_.update();
        result.lever_changed = arbiter_.resolution_scale() != scale_before;

        // 4. The allocations and the grants reach the controllers, which then run.
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            if (!registered_[index]) {
                continue;
            }
            controllers_[index].set_allocation_ms(result.report.allocation_ms[index]);
            if (result.report.relax_granted[index]) {
                controllers_[index].grant_relax_step();
            }
            const SubsystemUpdate update = controllers_[index].update();
            result.lever_changed = result.lever_changed || update.tightened || update.relaxed;
        }
        return result;
    }

private:
    [[nodiscard]] f32 true_subsystem_ms(u32 index) const noexcept {
        const SubsystemDeclaration& declaration = profile_.subsystems[index];
        const f32 ladder = declaration.ladder.cost_at(controllers_[index].position());
        const f32 scale = arbiter_.resolution_scale();
        const f32 sensitivity = declaration.resolution_sensitivity;
        const f32 pixels = 1.0F - sensitivity + (sensitivity * scale * scale);
        return nominal_ms_[index] * load_ * ladder * pixels;
    }

    RendererProfile profile_;
    BudgetArbiter arbiter_;
    SubsystemController controllers_[kBudgetSubsystemCount];
    bool registered_[kBudgetSubsystemCount] = {};
    f32 nominal_ms_[kBudgetSubsystemCount] = {};
    FrameSample ring_[kReadbackFrames] = {};
    u32 ring_head_ = 0;
    u32 primed_count_ = 0;
    bool primed_ = false;
    f32 load_ = 1.0F;
    f32 noise_ = 0.03F;
    Xorshift rng_{0x5EED1234U};
};

}  // namespace cy::rendering::test
