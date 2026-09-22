// THE DIGEST ONE CONTINUOUS-INTEGRATION LEG PUBLISHES FOR ANOTHER LEG TO COMPARE. M11.a section 4.
//
// ================================================================================================
// WHY THIS FILE EXISTS, AND WHY IT IS ONE FILE RATHER THAN THREE
// ================================================================================================
//
// Three criteria in three ledgers look for exactly one shape and each fails or reports NOT
// EVALUATED for want of it:
//
//   m9:lockstep-cross-platform              a simulation state hash compared between two
//                                           architectures. Declared a gap rather than
//                                           `where = "ci"` precisely because `where = "ci"` would
//                                           have let a single-leg suite satisfy it.
//   m10:pcg-regeneration-cross-platform     a generated region's digest compared between two
//                                           architectures.
//   m10:pcg-gpu-domain-agreement            the GPU execution domain against the CPU domain on more
//                                           than one vendor's driver.
//
// `ci.yml`'s build and test matrices already carry linux-x86_64, linux-arm64, macos-arm64 and
// windows-x86_64, and they run independently: nothing publishes a digest and nothing downloads one
// to compare. This suite is the PUBLISHER half of the job that closes that;
// `tools/ci/cross_leg_digests.py` is the COMPARATOR half, and `just test-determinism
// --compare-legs` runs it.
//
// ================================================================================================
// WHAT IS PUBLISHED, AND WHY EACH FIELD IS THE ENGINE'S OWN RATHER THAN THIS FILE'S
// ================================================================================================
//
//   sim-state-digest    The `ToySession` of src/replay/tests/sim.h — a real `ecs::World`, a real
//                       four-participant `gameplay::GameSession`, real `determinism::StateCodec`s —
//                       run for `kGoldenTicks` ticks, with the per-tick state hash folded in tick
//                       order. A digest of a fixture invented here would be a digest of a counter,
//                       and a counter reproduces across architectures whatever the engine does.
//   pcg-world-digest    `cy::pcg::test::world_digest()` over the forest graph's 8x8 region world:
//                       EVERY stage's raster in every region plus the accepted points, which is the
//                       comparison `test_determinism.cpp` already makes between two runs on ONE
//                       host. This suite makes the same comparison between two hosts.
//   pcg-identity-digest The identities, folded separately, because the M10 spike measured a
//                       configuration whose output was bit-identical and whose identities had all
//                       moved.
//   pcg-gpu-domain      What `cy::pcg::ExecutionDomain` offers. It is `none` on every leg today and
//                       the field exists to say so out loud — see the last case in this file.
//
// The leg's identity — os, architecture, compiler, endianness and the four build-time
// floating-point facts `determinism::BuildConfiguration` reads — is published beside the digests,
// and the COMPARATOR groups on the ARCHITECTURE THIS BINARY DETECTED rather than on the workflow's
// label for the leg. A job that mislabelled two x86-64 runners as two architectures would otherwise
// satisfy "compared between two architectures" with one.
//
// ================================================================================================
// HOW THIS SUITE IS MADE TO FAIL, WHICH IS THE ONLY REASON TO BELIEVE IT
// ================================================================================================
//
// Applied to this tree and watched; the numbers are in the change's `verified_failing`.
//
//   * fold the per-tick hashes with `+` instead of `cy::hash_combine` and "the published simulation
//     digest is the committed golden session's" goes red: the digest stops being a function of the
//     ORDER of the ticks, which is the property a cross-architecture comparison is about.
//   * seed the fold with `kGoldenTicks` rather than `kFoldSeed` and the same case goes red — the
//     published number is pinned to a committed expectation rather than to itself.
//   * make `publish()` omit `arch` and "a published digest carries every field the comparison
//     needs" goes red naming the field, which is what stops a leg from publishing a digest the
//     comparator would have to guess the architecture of.
//   * add a `Gpu` enumerator to `cy::pcg::ExecutionDomain` and "the generator has no GPU execution
//     domain" goes red, which is the forcing function that makes somebody update this publisher on
//     the day the domain exists.
//   * make `world_digest()` return a constant and "the published generation digest is a function of
//     the world's content" goes red. That case is here because of what M11.a's field spike found in
//     ITSELF: a refusal that counted variety across four fields together ran to completion and
//     reported a speed-up on a mutation that made the content constant, because one live field
//     carried the aggregate. Two legs agreeing on a constant are not two legs that agreed.

#include "golden_session.h"

#include <cy/core/determinism/profile.h>
#include <cy/core/memory/hash.h>
#include <cy/pcg/execute.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <bit>
#include <cstdlib>
#include <string>

#include "fixtures.h"

using cy::u64;
using cy::determinism_test::kGoldenCheckpointEvery;
using cy::determinism_test::kGoldenHashesName;
using cy::determinism_test::kGoldenTicks;
using cy::determinism_test::Recording;
using cy::pcg::ExecutionDomain;
using cy::pcg::GenerationContext;
using cy::pcg::GenerationWorld;
using cy::pcg::Generator;
using cy::pcg::RegionCoord;
using cy::pcg::RegionExtent;
using cy::pcg::RegionState;
namespace pcg_test = cy::pcg::test;

namespace {

/// The published file's format version. The comparator refuses a digest it does not understand
/// rather than comparing fields it guessed the meaning of.
constexpr int kSchema = 1;

/// The seed the per-tick fold starts from, and the seed of the PCG world. Committed constants: a
/// digest whose seed moved is a digest of a different question.
constexpr u64 kFoldSeed = 0xC1'BE'12'0A'5EED'0001ULL;
constexpr u64 kPcgSeed = 0xc0ffee'1234'5678ULL;

/// The PCG world's edge in regions. The same 8x8 — a 512 m world, 16 384 raster cells — that
/// `src/pcg/tests/test_determinism.cpp` compares between two runs on one host, so that a
/// disagreement found between two hosts is comparable with the agreement already measured on one.
constexpr cy::i32 kPcgEdge = 8;

/// What one leg publishes. Every field is a number some other leg computes the same way, or a fact
/// about the leg the comparator needs to decide whether two legs are comparable at all.
struct LegDigest {
    u64 sim_state = 0;
    u64 sim_final = 0;
    u64 pcg_world = 0;
    u64 pcg_identity = 0;
    bool complete = false;
};

[[nodiscard]] const char* architecture() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm32";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    // Never "assume x86_64": the comparator's two-architecture rule is made of this string, and a
    // leg that guessed would let one architecture satisfy a claim about two.
    return "unknown";
#endif
}

[[nodiscard]] const char* operating_system() noexcept {
#if defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string compiler() {
#if defined(__clang__)
    return "clang-" + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(_MSC_VER)
    return "msvc-" + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    return "gcc-" + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string hex(u64 value) {
    char text[17];
    (void)std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

/// The simulation half: the golden session run live, its per-tick state hashes folded in tick
/// order.
///
/// `cy::hash_combine` and not a sum, because the fold has to be ORDER-SENSITIVE — two legs whose
/// ticks produced the same multiset of hashes in a different order have diverged, and a sum would
/// report them identical. It is also seed-free and allocation-free, so the number is a function of
/// the session and of nothing about the process.
[[nodiscard]] bool simulation_digest(u64& state, u64& final_hash) {
    Recording recording(cy::replay_test::allocator());
    if (!cy::determinism_test::record_session(kGoldenTicks, kGoldenCheckpointEvery, recording)) {
        return false;
    }
    u64 fold = kFoldSeed;
    for (const u64 hash : recording.hashes) {
        fold = cy::hash_combine(fold, hash);
    }
    state = fold;
    final_hash = recording.hashes.empty() ? 0 : recording.hashes[recording.hashes.size() - 1];
    return true;
}

/// The generation half: the forest graph over `kPcgEdge` squared regions, cooked.
///
/// `ExecutionDomain::Cook` is the domain `test_determinism.cpp`'s own precondition uses, so the two
/// measurements are of one thing. There is no GPU domain to run instead — the last case in this
/// file is where that is asserted rather than assumed.
[[nodiscard]] bool generation_digest_of(u64 seed, u64& world_digest, u64& identity_digest) {
    const cy::pcg::FlatSpatialQuery surface;
    const GenerationContext context = pcg_test::context_of(seed, surface);
    GenerationWorld world(pcg_test::allocator(), pcg_test::extent_of(kPcgEdge));
    cy::Expected<Generator, cy::Error> generator = pcg_test::make_generator(world);
    if (!generator || !generator->generate_all(ExecutionDomain::Cook, context)) {
        return false;
    }
    world_digest = pcg_test::world_digest(world);

    cy::pcg::Digest identities;
    const RegionExtent& extent = world.extent();
    for (cy::i32 z = extent.min_z; z <= extent.max_z; ++z) {
        for (cy::i32 x = extent.min_x; x <= extent.max_x; ++x) {
            const RegionState* state = world.find(RegionCoord{x, z, extent.level});
            identities.u64_value(state == nullptr ? 0 : pcg_test::region_identity_digest(*state));
        }
    }
    identity_digest = identities.value();
    return true;
}

[[nodiscard]] LegDigest measure() {
    LegDigest digest;
    digest.complete = simulation_digest(digest.sim_state, digest.sim_final) &&
                      generation_digest_of(kPcgSeed, digest.pcg_world, digest.pcg_identity);
    return digest;
}

/// Both measurements, taken once for the whole binary. Two of them, because "the same digest twice
/// in one process" is a case rather than an assumption — a publisher that was not repeatable within
/// one process could not be compared between two.
struct Measurements {
    LegDigest first;
    LegDigest second;
};

[[nodiscard]] const Measurements& measurements() {
    static const Measurements taken{measure(), measure()};
    return taken;
}

/// Every stage's own `execution_domain_name()`, searched for a device. Deliberately
/// case-insensitive and deliberately a substring: the day somebody adds `ExecutionDomain::Gpu`,
/// this finds it under any spelling.
[[nodiscard]] bool names_a_device() noexcept {
    for (cy::u8 index = 0; index < static_cast<cy::u8>(ExecutionDomain::kCount); ++index) {
        const char* name = cy::pcg::execution_domain_name(static_cast<ExecutionDomain>(index));
        for (const char* cursor = name; *cursor != '\0'; ++cursor) {
            const bool gpu = (cursor[0] == 'g' || cursor[0] == 'G') &&
                             (cursor[1] == 'p' || cursor[1] == 'P') &&
                             (cursor[2] == 'u' || cursor[2] == 'U');
            if (gpu) {
                return true;
            }
        }
    }
    return false;
}

/// The published file. One `key value` per line, because the comparator has to run on three
/// operating systems' runners and a format needing a library is a format a leg cannot read.
[[nodiscard]] std::string publish(const LegDigest& digest) {
    const cy::determinism::BuildConfiguration build =
        cy::determinism::BuildConfiguration::from_build();
    std::string text =
        "# CyberEngine cross-leg state digest. Published by determinism.cross_leg, compared by\n"
        "# tools/ci/cross_leg_digests.py. One leg's answer to the questions m9:lockstep-cross-\n"
        "# platform and m10:pcg-regeneration-cross-platform ask between two.\n";
    text += "schema " + std::to_string(kSchema) + "\n";
    // The workflow's name for the leg, which is a LABEL and never the comparator's evidence.
    const char* label = std::getenv("CY_CROSS_LEG_LABEL");
    text +=
        "label " + std::string(label != nullptr && label[0] != '\0' ? label : "unlabelled") + "\n";
    text += "os " + std::string(operating_system()) + "\n";
    text += "arch " + std::string(architecture()) + "\n";
    text += "compiler " + compiler() + "\n";
    text += "pointer-bits " + std::to_string(sizeof(void*) * 8) + "\n";
    text += "endian " + std::string(std::endian::native == std::endian::little ? "little" : "big") +
            "\n";
    text += "contraction-off " + std::to_string(build.contraction_off ? 1 : 0) + "\n";
    text += "fast-math " + std::to_string(build.fast_math ? 1 : 0) + "\n";
    text += "target-has-fma " + std::to_string(build.target_has_fma ? 1 : 0) + "\n";
    text +=
        "deterministic-math " + std::to_string(build.deterministic_math_available ? 1 : 0) + "\n";
    text += "sim-ticks " + std::to_string(kGoldenTicks) + "\n";
    text += "sim-state-digest " + hex(digest.sim_state) + "\n";
    text += "sim-final-hash " + hex(digest.sim_final) + "\n";
    text += "pcg-seed " + hex(kPcgSeed) + "\n";
    text += "pcg-regions " + std::to_string(kPcgEdge * kPcgEdge) + "\n";
    text += "pcg-world-digest " + hex(digest.pcg_world) + "\n";
    text += "pcg-identity-digest " + hex(digest.pcg_identity) + "\n";
    // `none` rather than an omitted line: a comparator that saw no field could not tell a leg with
    // no GPU domain from a leg whose publisher predated the field.
    text += "pcg-gpu-domain " + std::string(names_a_device() ? "present" : "none") + "\n";
    return text;
}

/// Whether `text` has a `key <value>` line with something after the key. A leading newline is
/// prepended so the first line is matched by the same expression as every other one.
[[nodiscard]] bool carries(const std::string& text, const char* key) {
    const std::string padded = "\n" + text;
    const std::string needle = std::string("\n") + key + " ";
    const std::string::size_type at = padded.find(needle);
    return at != std::string::npos && padded[at + needle.size()] != '\n';
}

/// The keys `tools/ci/cross_leg_digests.py` reads. Restated here so that dropping one from
/// `publish()` is a failure in this suite rather than a comparison that silently skipped a claim.
constexpr const char* kRequiredKeys[] = {
    "schema",
    "os",
    "arch",
    "compiler",
    "pointer-bits",
    "endian",
    "sim-ticks",
    "sim-state-digest",
    "sim-final-hash",
    "pcg-seed",
    "pcg-regions",
    "pcg-world-digest",
    "pcg-identity-digest",
    "pcg-gpu-domain",
};

}  // namespace

CY_TEST_CASE("cross-leg: the published simulation digest is the committed golden session's") {
    const Measurements& taken = measurements();
    CY_REQUIRE(taken.first.complete);

    // The published number is tied to the artefact `determinism.golden_replay` already checks, and
    // not computed by a path nobody else looks at. A publisher that drifted away from the committed
    // session would otherwise publish a digest two legs agreed on and neither leg's simulation
    // matched.
    std::string committed;
    CY_REQUIRE(
        cy::test::read_file(cy::determinism_test::golden_path(kGoldenHashesName), committed));
    const cy::determinism_test::GoldenHashes golden = cy::determinism_test::parse_hashes(committed);
    CY_REQUIRE(golden.hashes.size() == static_cast<cy::usize>(kGoldenTicks));

    u64 fold = kFoldSeed;
    for (const u64 hash : golden.hashes) {
        fold = cy::hash_combine(fold, hash);
    }
    CY_CHECK_EQ(taken.first.sim_state, fold);
    CY_CHECK_EQ(taken.first.sim_final, golden.hashes.back());
}

CY_TEST_CASE("cross-leg: the published digests are the same twice in one process") {
    const Measurements& taken = measurements();
    CY_REQUIRE(taken.first.complete);
    CY_REQUIRE(taken.second.complete);
    // Repeatable within one process is the weakest half of the claim and the precondition for the
    // interesting one: two legs cannot be compared on a number one leg does not agree with itself
    // about.
    CY_CHECK_EQ(taken.first.sim_state, taken.second.sim_state);
    CY_CHECK_EQ(taken.first.sim_final, taken.second.sim_final);
    CY_CHECK_EQ(taken.first.pcg_world, taken.second.pcg_world);
    CY_CHECK_EQ(taken.first.pcg_identity, taken.second.pcg_identity);
}

CY_TEST_CASE("cross-leg: the PCG world matches the cross-architecture golden") {
    const Measurements& taken = measurements();
    CY_REQUIRE(taken.first.complete);
    // This was measured on Linux x86_64 and macOS ARM64 with contraction disabled. The old ARM64
    // build fused PCG raster arithmetic and disagreed while the accepted-point identities matched.
    CY_CHECK_EQ(taken.first.pcg_world, 0x7bc3'77fd'2fc0'872bULL);
}

CY_TEST_CASE("cross-leg: a published digest carries every field the comparison needs") {
    const Measurements& taken = measurements();
    CY_REQUIRE(taken.first.complete);
    const std::string text = publish(taken.first);

    // Compared as text rather than asserted as a boolean, so a failure NAMES the missing key. A
    // check that printed `false` would send the next reader to re-derive which of fourteen fields
    // went away.
    for (const char* key : kRequiredKeys) {
        const std::string expected = std::string(key) + ": published";
        CY_CHECK_EQ(std::string(key) + (carries(text, key) ? ": published" : ": MISSING"),
                    expected);
    }
    // The architecture is the comparator's evidence for "two architectures", so a leg that could
    // not name its own is a leg that must not be counted as one. Checked here rather than left to
    // the comparator, because the comparator would see `unknown` and have to guess whether it was a
    // new platform or a broken publisher.
    CY_CHECK(std::string(architecture()) != "unknown");
    CY_CHECK(std::string(operating_system()) != "unknown");

    // Publishing is what a continuous-integration leg does; a developer's run writes nothing. The
    // path is absolute because CTest runs a suite in its own binary directory.
    const char* out = std::getenv("CY_CROSS_LEG_DIGEST_OUT");
    if (out != nullptr && out[0] != '\0') {
        const std::string wrote = std::string(cy::test::write_file(out, text) ? "wrote "
                                                                              : "COULD "
                                                                                "NOT WRITE ") +
                                  out;
        CY_CHECK_EQ(wrote, std::string("wrote ") + out);
    }
}

CY_TEST_CASE("cross-leg: the published generation digest is a function of the world's content") {
    // THE SPIKE'S LESSON, APPLIED ONE LAYER UP. M11.a's field spike shipped a refusal that counted
    // variety across four fields together, so a mutation making the content constant RAN TO
    // COMPLETION AND REPORTED A SPEED-UP. The comparator refuses a digest that is zero, which
    // catches the crudest version; this catches the version that is not zero and not a function of
    // anything either. Two legs agreeing on a constant are not two legs that agreed.
    const Measurements& taken = measurements();
    CY_REQUIRE(taken.first.complete);

    u64 moved_world = 0;
    u64 moved_identity = 0;
    CY_REQUIRE(generation_digest_of(kPcgSeed ^ 1ULL, moved_world, moved_identity));
    CY_CHECK_NE(taken.first.pcg_world, moved_world);
    CY_CHECK_NE(taken.first.pcg_identity, moved_identity);
}

CY_TEST_CASE("cross-leg: the generator has no GPU execution domain, and the digest says so") {
    // `procedural-content-generation` — "CPU and GPU execution" — requires a graph to be executable
    // on the GPU, and `m10:pcg-gpu-domain-agreement` asks whether a GPU domain reproduces the CPU
    // one on more than one vendor's driver. THERE IS NO GPU DOMAIN IN THIS TREE: `ExecutionDomain`
    // is Editor, Cook, Runtime, Streaming, Dynamic. So the criterion cannot be closed by a job, and
    // this case is what makes that a measured fact rather than a claim in a report.
    //
    // It goes RED the day somebody adds the domain, which is the point: the publisher above has to
    // learn to run it before a comparison of it can mean anything.
    CY_CHECK(!names_a_device());
    CY_CHECK_EQ(static_cast<int>(ExecutionDomain::kCount), 5);
    CY_CHECK(carries(publish(measurements().first), "pcg-gpu-domain"));
}
