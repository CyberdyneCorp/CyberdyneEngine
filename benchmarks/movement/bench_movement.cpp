// SPDX-License-Identifier: MIT
// The movement step at strategy scale: 100 000 units. Design §11 states the budget on the x86-64
// reference runner:
//
//     movement step    100 000 units with separation <= 4 ms per tick on 8 workers,
//                      and <= 2.5x the same kernel in f32
//
// THE RATIO IS AGAINST THE SAME ALGORITHM. `movement/kernel-fixed` and `movement/kernel-f32` run
// one text — cy::movement::CrowdKernel, integration, the grid and a separation pass — instantiated
// over `FixedPolicy` and over the `F32Policy` below. Their quotient is the cost of fixed point
// itself. `movement/step` is the whole authoritative tick (the kernel, static obstacles, the clamp
// to a converted navigation mesh, heights and headings) on one thread, and
// `movement/step-8-workers` the same tick on a job system of eight workers, whose result is the
// same bits.
//
// The units circle the map's centre at different radii and speeds, so the crowd neither disperses
// nor piles up as the runner repeats the body, and pairs keep meeting: every sample does separation
// work rather than measuring an empty grid.

#include <cy/bench/bench.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/movement/crowd_kernel.h>
#include <cy/movement/mover.h>
#include <cy/movement/nav_mesh.h>
#include <cy/navigation/navmesh.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>

namespace {

using cy::f32;
using cy::i32;
using cy::i64;
using cy::u32;
using cy::u64;
using cy::detmath::Fixed;
using cy::detmath::FixedVec2;

constexpr u32 kUnits = 100'000;
constexpr i32 kMapMetres = 1024;
constexpr u32 kRow = 316;  // units per row of the starting lattice, 3.2 m apart

/// The f32 arithmetic of the crowd kernel: the yardstick design §11 measures `Fixed` against.
struct F32Vec {
    f32 x = 0.0F;
    f32 y = 0.0F;
};
F32Vec operator+(F32Vec a, F32Vec b) {
    return {a.x + b.x, a.y + b.y};
}
F32Vec operator-(F32Vec a, F32Vec b) {
    return {a.x - b.x, a.y - b.y};
}
F32Vec operator*(F32Vec v, f32 k) {
    return {v.x * k, v.y * k};
}

struct F32Policy {
    using Scalar = f32;
    using Vec = F32Vec;
    using Wide = f32;

    static Wide length_squared(Vec v) { return (v.x * v.x) + (v.y * v.y); }
    static Wide square(Scalar s) { return s * s; }
    static Scalar sqrt(Wide w) { return std::sqrt(w); }
    static Vec clamp_length(Vec v, Scalar limit) {
        const f32 squared = length_squared(v);
        if (squared <= limit * limit) {
            return v;
        }
        return v * (limit / std::sqrt(squared));
    }
    static i64 cell(Scalar value, int shift) {
        return static_cast<i64>(std::floor(std::ldexp(value, -shift)));
    }
    static Scalar zero() { return 0.0F; }
    static Wide wide_zero() { return 0.0F; }
};

/// A deterministic spread in [0, 1) for unit `i` and salt `s`, without a float library call.
[[nodiscard]] u32 spread(u32 i, u32 s) {
    u64 z = (u64{i} << 32U) ^ s ^ 0x9E37'79B9'7F4A'7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return static_cast<u32>((z ^ (z >> 31U)) >> 32U);
}

/// Unit `i`'s start, in raw Q32.32: a 3.2 m lattice from the corner with up to 1 m of jitter.
[[nodiscard]] FixedVec2 start_of(u32 i) {
    const i64 step = (Fixed::kOneRaw * 16) / 5;
    const i64 jitter_x = static_cast<i64>(spread(i, 1) >> 8U) << 8U;  // [0, 1) m
    const i64 jitter_z = static_cast<i64>(spread(i, 2) >> 8U) << 8U;
    return FixedVec2{Fixed::from_raw((Fixed::kOneRaw * 8) + (step * (i % kRow)) + jitter_x),
                     Fixed::from_raw((Fixed::kOneRaw * 8) + (step * (i / kRow)) + jitter_z)};
}

/// A velocity tangent to the circle about the map's centre through `at`, at 1 to 4 m/s.
[[nodiscard]] FixedVec2 circling(FixedVec2 at, u32 i) {
    const Fixed centre = Fixed::from_int(kMapMetres / 2);
    const FixedVec2 radial{at.x - centre, at.y - centre};
    const Fixed speed = Fixed::from_int(1) + Fixed::from_raw(static_cast<i64>(spread(i, 3)) * 3);
    return cy::detmath::clamp_length(FixedVec2{-radial.y, radial.x}, speed);
}

[[nodiscard]] cy::movement::CrowdGridParams grid() {
    cy::movement::CrowdGridParams params;
    params.cell_shift = 1;
    params.origin_x = 0;
    params.origin_z = 0;
    params.cells_x = static_cast<u32>(kMapMetres / 2);
    params.cells_z = static_cast<u32>(kMapMetres / 2);
    return params;
}

/// The crowd kernel over `Policy`, seeded with the same units in both kinds of arithmetic.
template <class Policy, class Convert>
struct KernelWorld {
    explicit KernelWorld(Convert convert) : kernel(cy::system_allocator(cy::MemoryDomain::World)) {
        for (u32 i = 0; i < kUnits; ++i) {
            const FixedVec2 at = start_of(i);
            (void)kernel.push(convert(at), convert(Fixed::half()), convert(Fixed::from_int(4)),
                              convert(Fixed::from_int(16)));
            kernel.desired[i] = convert(circling(at, i));
        }
    }

    void step() {
        kernel.integrate(0, kUnits, dt);
        kernel.commit();
        (void)kernel.build_grid(grid());
        kernel.separate(0, kUnits, share, max_push);
        kernel.commit();
    }

    cy::movement::CrowdKernel<Policy> kernel;
    Policy::Scalar dt{};
    Policy::Scalar share{};
    Policy::Scalar max_push{};
};

struct ToF32 {
    [[nodiscard]] f32 operator()(Fixed value) const {
        return static_cast<f32>(static_cast<double>(value.raw) * 0x1.0p-32);
    }
    [[nodiscard]] F32Vec operator()(FixedVec2 value) const {
        return {(*this)(value.x), (*this)(value.y)};
    }
};

struct ToFixed {
    [[nodiscard]] Fixed operator()(Fixed value) const { return value; }
    [[nodiscard]] FixedVec2 operator()(FixedVec2 value) const { return value; }
};

[[nodiscard]] KernelWorld<F32Policy, ToF32>& f32_world() {
    static auto* world = [] {
        auto* made = new KernelWorld<F32Policy, ToF32>(ToF32{});
        made->dt = 1.0F / 60.0F;
        made->share = 0.5F;
        made->max_push = 0.25F;
        return made;
    }();
    return *world;
}

[[nodiscard]] KernelWorld<cy::movement::FixedPolicy, ToFixed>& fixed_world() {
    static auto* world = [] {
        auto* made = new KernelWorld<cy::movement::FixedPolicy, ToFixed>(ToFixed{});
        made->dt = Fixed::one() / Fixed::from_int(60);
        made->share = Fixed::half();
        made->max_push = Fixed::from_raw(Fixed::kOneRaw / 4);
        return made;
    }();
    return *world;
}

/// A flat, open navigation mesh over the whole map in 8 m quads, converted into a Fixed world.
struct MoverWorld {
    MoverWorld()
        : source(cy::system_allocator(cy::MemoryDomain::World), cy::Name::intern("bench.nav"),
                 static_cast<f32>(kMapMetres)),
          mesh(cy::system_allocator(cy::MemoryDomain::World)),
          mover(cy::system_allocator(cy::MemoryDomain::World), cy::movement::MoverParams{}) {
        constexpr u32 kQuads = kMapMetres / 8;
        cy::navigation::NavTileData data(cy::system_allocator(cy::MemoryDomain::World));
        for (u32 row = 0; row <= kQuads; ++row) {
            for (u32 column = 0; column <= kQuads; ++column) {
                (void)data.vertices().push_back(
                    cy::Vec3{static_cast<f32>(column * 8), 0.0F, static_cast<f32>(row * 8)});
            }
        }
        for (u32 row = 0; row < kQuads; ++row) {
            for (u32 column = 0; column < kQuads; ++column) {
                cy::navigation::NavPoly poly;
                poly.first_corner = static_cast<u32>(data.corners().size());
                poly.corner_count = 4;
                (void)data.polys().push_back(poly);
                const u32 base = row * (kQuads + 1);
                for (const u32 index :
                     {base + column, base + column + 1, base + kQuads + 1 + column + 1,
                      base + kQuads + 1 + column}) {
                    (void)data.corners().push_back(index);
                }
            }
        }
        data.finalise();
        (void)source.add_tile(static_cast<cy::navigation::NavTileData&&>(data));
        (void)mesh.convert(source);
        for (u32 i = 0; i < kUnits; ++i) {
            cy::movement::UnitDesc unit;
            unit.entity = u64{i} + 1;
            unit.position = start_of(i);
            (void)mover.add(unit);
        }
        mover.bind(&mesh, nullptr);
        for (u32 i = 0; i < kUnits; ++i) {
            mover.set_desired_velocity(i, circling(start_of(i), i));
        }
    }

    cy::navigation::NavMesh source;
    cy::movement::FixedNavMesh mesh;
    cy::movement::KinematicMover mover;
};

[[nodiscard]] MoverWorld& mover_world() {
    static auto* world = new MoverWorld();
    return *world;
}

[[nodiscard]] cy::jobs::JobSystem& eight_workers() {
    static auto* jobs = [] {
        auto* made = new cy::jobs::JobSystem();
        cy::jobs::JobSystemConfig config;
        config.worker_count = 8;
        (void)made->start(config);
        return made;
    }();
    return *jobs;
}

}  // namespace

CY_BENCHMARK_STARTING_AT(
    "movement/kernel-f32",
    "The crowd kernel over 100 000 units in f32: integration, the grid, one separation pass. NOT "
    "engine code: the yardstick movement/kernel-fixed is held to (design §11: at most 2.5x). A "
    "regression here is the kernel's algorithm or the machine, not fixed point.",
    1) {
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        f32_world().step();
    }
    CY_BENCH_KEEP(f32_world().kernel.position[0].x);
}

CY_BENCHMARK_STARTING_AT(
    "movement/kernel-fixed",
    "The same crowd kernel over the same 100 000 units in Fixed, one thread. Against "
    "movement/kernel-f32 it is the cost of fixed point itself; a regression is felt by every "
    "authoritative unit of a lockstep session every tick.",
    1) {
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        fixed_world().step();
    }
    CY_BENCH_KEEP(fixed_world().kernel.position[0].x.raw);
}

CY_BENCHMARK_STARTING_AT(
    "movement/step",
    "One authoritative tick of 100 000 units on one thread: the kernel, static obstacles, the "
    "clamp to a converted navigation mesh, heights and headings. A regression is the movement "
    "budget of the strategy-scale lockstep benchmark.",
    1) {
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        (void)mover_world().mover.step();
    }
    CY_BENCH_KEEP(mover_world().mover.position(0).x.raw);
}

CY_BENCHMARK_STARTING_AT(
    "movement/step-8-workers",
    "The same tick on a job system of eight workers — design §11's budget is 4 ms. The result is "
    "the serial tick's bits; a regression means a pass stopped running in parallel ranges.",
    1) {
    for (std::uint64_t i = 0; i < CY_BENCH_ITERATIONS; ++i) {
        (void)mover_world().mover.step(&eight_workers());
    }
    CY_BENCH_KEEP(mover_world().mover.position(0).x.raw);
}
