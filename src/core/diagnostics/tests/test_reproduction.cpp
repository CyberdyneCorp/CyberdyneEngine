// The reproduction artefact, and the two refusals that make it worth having.
//
// `diagnostics-profiling-and-crash` — "Reproduction artefacts". The requirement's last sentence is
// the one with teeth: "Where determinism is insufficient to reproduce exactly, the artefact SHALL
// state so rather than implying fidelity it does not have." An artefact that points at a replay
// slice and says nothing about what replaying it will produce sends a reader looking for a
// divergence nobody promised was absent.
//
// Four claims:
//   1. An artefact with a slice and an exact fidelity is written and says so.
//   2. An artefact claiming less than exact fidelity WITHOUT a reason is REFUSED, not written.
//   3. An artefact with no replay slice is REFUSED: it would reproduce nothing.
//   4. The crash artefact carries the LINK, not the artefact, and states its absence when there is
//      none — because "a crash may have no reproduction and a reproduction may have no crash".
//
// HOW TO MAKE IT FAIL:
//   * drop the `fidelity_reason` check in write_reproduction()  -> case 2 goes red;
//   * drop the `replay_log_path` check                          -> case 3 goes red;
//   * stop writing the [reproduction] section of the report     -> case 4 goes red.

#include "harness.h"

#include <cy/core/diagnostics/crash.h>
#include <cy/core/diagnostics/health.h>
#include <cy/core/diagnostics/reproduction.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

using namespace cy::diag;

namespace {

std::string read_file(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return {};
    }
    std::string bytes;
    char buffer[4096];
    // A short read is end of file (or an error); reading again afterwards is undefined, so the loop
    // stops on the short read rather than on a zero one.
    for (;;) {
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read != 0) {
            bytes.append(buffer, read);
        }
        if (read < sizeof(buffer)) {
            break;
        }
    }
    std::fclose(file);
    return bytes;
}

/// A path under a directory this process owns.
///
/// M9's CLOSING GATE FOUND THREE `.cyrepro` FILES SITTING IN THE REPOSITORY ROOT. These cases wrote
/// `repro_exact.cyrepro` and its two siblings by RELATIVE name, so running the binary by hand from
/// the working tree — which is what a person does, and what the ledger's crash criterion does with
/// its own probe — dropped artefacts into the source tree next to files whose own header declares
/// them `potentially-personal`. A test that writes outside its own scratch directory is a test that
/// pollutes whatever directory it is invoked from.
const char* scratch_dir() {
    static char directory[] = "/tmp/cy-reproduction-XXXXXX";
    static const char* made = ::mkdtemp(directory);
    return made != nullptr ? made : ".";
}

std::string scratch(const char* name) {
    return std::string(scratch_dir()) + "/" + name;
}

bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

Reproduction exact_record() {
    Reproduction record;
    record.replay_log_path = "session-0041.cyreplay";
    record.capture_path = "capture-tick-0001.cytrace";
    record.crash_report_path = "crash-1234.txt";
    record.first_tick = 900;
    record.last_tick = 1024;
    record.checkpoint_tick = 896;
    record.session_seed = 0xC0FFEEULL;
    record.log_hash = 0x0123456789ABCDEFULL;
    record.final_state_hash = 0xFEDCBA9876543210ULL;
    record.external_result_count = 3;
    record.fidelity = Fidelity::Exact;
    record.profile = "Lockstep";
    record.build_identity = "reproduction-test";
    return record;
}

void case_1_an_exact_artefact_is_written_and_says_so() {
    const auto written = write_reproduction(scratch("repro_exact.cyrepro").c_str(), exact_record());
    CY_CHECK(written.has_value(), "an artefact with a slice and a stated fidelity is written");

    const std::string text = read_file(scratch("repro_exact.cyrepro").c_str());
    CY_CHECK(has(text, "fidelity: exact"), "and it states its fidelity");
    CY_CHECK(has(text, "checkpoint_tick: 896"),
             "and the checkpoint a player seeks to before advancing");
    CY_CHECK(has(text, "replay_log: session-0041.cyreplay"),
             "and names the slice, rather than "
             "containing it");
    CY_CHECK(has(text, "crash_report: crash-1234.txt"), "and the crash artefact it is linked to");
    CY_CHECK(has(text, "log_hash: 0x0123456789abcdef"),
             "and the slice's hash, so a player can tell it was handed the right one");
}

void case_2_a_fidelity_short_of_exact_needs_a_reason() {
    Reproduction record = exact_record();
    record.fidelity = Fidelity::Approximate;
    record.fidelity_reason = "";
    // Remove it first: "no file was left behind" is a claim about THIS run, and a leftover from a
    // previous one would make the check pass or fail for the wrong reason.
    (void)std::remove("repro_silent.cyrepro");
    const auto refused = write_reproduction("repro_silent.cyrepro", record);
    CY_CHECK(!refused.has_value(), "an unexplained approximation is refused, not written");
    CY_CHECK(read_file("repro_silent.cyrepro").empty(), "and no file was left behind");

    record.fidelity_reason = "an unrecorded inference result is consumed at tick 1002";
    const auto accepted = write_reproduction(scratch("repro_approximate.cyrepro").c_str(), record);
    CY_CHECK(accepted.has_value(), "the same artefact with a reason is written");
    const std::string text = read_file(scratch("repro_approximate.cyrepro").c_str());
    CY_CHECK(has(text, "fidelity: approximate"), "and it does not imply exactness");
    CY_CHECK(has(text, "unrecorded inference result"), "and says what stops it being exact");

    // The strongest form of the same rule.
    record.fidelity = Fidelity::NotReproducible;
    record.fidelity_reason = "the session declared no determinism profile";
    CY_CHECK(write_reproduction(scratch("repro_none.cyrepro").c_str(), record).has_value(),
             "a window that will not reproduce is still worth recording");
    CY_CHECK(has(read_file(scratch("repro_none.cyrepro").c_str()), "fidelity: not-reproducible"),
             "provided it says so");
}

void case_3_an_artefact_with_no_slice_is_refused() {
    Reproduction record = exact_record();
    record.replay_log_path = "";
    (void)std::remove("repro_empty.cyrepro");
    CY_CHECK(!write_reproduction("repro_empty.cyrepro", record).has_value(),
             "a reproduction artefact that points at no slice reproduces nothing");
}

void case_4_the_crash_artefact_carries_the_link() {
    health_reset();
    clear_reproduction_link();

    CrashConfig config;
    // NOT ".". A crash report carries a backtrace, an absolute module path per frame and a header
    // declaring itself `potentially-personal`; writing it into whatever directory the binary was
    // invoked from is how five of them ended up in the repository root before M9's closing gate
    // removed them.
    config.directory = scratch_dir();
    config.engine_version = "test";
    config.build_identity = "reproduction-test";
    CY_CHECK(install_crash_handler(config).has_value(), "a crash handler installs");

    // No reproduction registered: the report says so rather than omitting the section.
    CrashSignal signal;
    signal.description = "synthetic";
    CY_CHECK(write_crash_report(signal).has_value(), "a report is written");
    const std::string absent = read_file(crash_report_path());
    CY_CHECK(has(absent, "[reproduction]"), "the report has the section");
    CY_CHECK(has(absent, "<none registered for this session>"),
             "and states the absence, because a crash may have no reproduction");

    // A condition the process died in, and a reproduction it can be replayed from.
    health_report(HealthCondition::PacketLoss, HealthSeverity::Degraded, 12);
    health_report(HealthCondition::RollbackFrequency, HealthSeverity::Critical, 40);
    set_reproduction_link(scratch("repro_exact.cyrepro").c_str(), Fidelity::Exact, 900, 1024);

    CY_CHECK(write_crash_report(signal).has_value(), "a second report is written");
    const std::string present = read_file(crash_report_path());
    CY_CHECK(has(present, scratch("repro_exact.cyrepro").c_str()),
             "the report names the reproduction artefact");
    CY_CHECK(has(present, "fidelity: exact"), "and what replaying it will produce");
    CY_CHECK(has(present, "[health] 2 active"), "and the health state the process died in");
    CY_CHECK(has(present, "rollback_frequency critical"), "condition by condition");
    CY_CHECK(has(present, "packet_loss degraded"), "at the level each was reported at");

    uninstall_crash_handler();
    clear_reproduction_link();
    health_reset();
}

}  // namespace

int main() {
    case_1_an_exact_artefact_is_written_and_says_so();
    case_2_a_fidelity_short_of_exact_needs_a_reason();
    case_3_an_artefact_with_no_slice_is_refused();
    case_4_the_crash_artefact_carries_the_link();
    return cy_test::summarise("test_reproduction");
}
