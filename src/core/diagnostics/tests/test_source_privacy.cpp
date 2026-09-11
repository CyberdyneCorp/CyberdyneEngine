// A source location is classified data, not a name — and no artefact carries a build-machine path.
//
// `diagnostics-profiling-and-crash`, "Privacy classification", states both halves:
//
//   * "A source location is classified data, not a name. ... Registering such a value as an event
//   or
//     scope *name* places it structurally beyond redaction, and SHALL NOT be done."
//   * "WHEN any produced trace, log or crash artefact is inspected for strings THEN it SHALL
//   contain
//     no absolute path from the build machine, and the check SHALL be a gate rather than a review."
//   * "Compiler flags that strip source prefixes are a mitigation and not the mechanism."
//
// M0's gate found the instance — `__FILE__` reaching the name table — and repaired it with
// `-fmacro-prefix-map`. This file is the CLASS: every check below passes on a toolchain with no
// prefix-mapping at all, because none of them depends on what the compiler did to `__FILE__`. The
// paths they push through the writer are absolute string literals that no build ever produced.
//
// HOW TO MAKE IT FAIL, which is the only thing that makes it a check:
//   * delete the `is_absolute()` branch of `sanitise_source_path()`  -> cases 1 and 3 go red;
//   * make `CY_LOG` register the site with `register_name()` again   -> case 5 goes red;
//   * let the writer emit a location's path regardless of the policy -> case 4 goes red.

#include "harness.h"

#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/source.h>
#include <cy/core/diagnostics/trace.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace cy::diag;

namespace {

CY_LOG_CATEGORY(sample_category, "source.sample")

/// An absolute path that belongs to no build on any machine. Nothing the compiler does can produce
/// it, so a check about it is a check about the writer.
constexpr const char* kForeignFile = "/opt/NOTTHISMACHINE/someone/plugin/src/widget.cpp";
constexpr const char* kForeignPrefix = "/opt/NOTTHISMACHINE";
constexpr const char* kForeignBasename = "widget.cpp";

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

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string sanitised(const char* path, PathForm& form) {
    char buffer[kMaxSourcePathBytes] = {};
    u32 length = 0;
    form = sanitise_source_path(path, buffer, sizeof(buffer), &length);
    return {buffer, length};
}

/// Write one capture under `policy`, with a log record whose site is `kForeignFile`.
std::string capture_with_foreign_location(const char* path, cy::ExportPolicy policy,
                                          TraceStats& stats) {
    TraceConfig config;
    config.path = path;
    config.policy = policy;
    config.consumer_thread = false;

    const auto opened = trace_open(config);
    CY_CHECK(opened.has_value(), "the trace opens");

    // The runtime half: a location whose path this build did not produce, exactly as an assertion
    // in a plugin compiled with someone else's flags would hand it to the bridge.
    const LocationId foreign = register_source_location(kForeignFile, 4242);
    CY_CHECK(foreign != kInvalidLocation, "the foreign location interns");
    log_emit(sample_category(), LogLevel::Error, register_name("source.foreign"), foreign, nullptr,
             0);

    // The compile-time half: this file's own `__FILE__`, through the macro every subsystem uses.
    CY_LOG(sample_category(), LogLevel::Warning, "source.local");

    trace_flush();
    const auto closed = trace_close();
    CY_CHECK(closed.has_value(), "the trace closes");
    if (closed.has_value()) {
        stats = closed.value();
    }
    return read_file(path);
}

void case_1_absolute_paths_keep_only_their_basename() {
    PathForm form = PathForm::Empty;
    const std::string out = sanitised(kForeignFile, form);
    CY_CHECK(form == PathForm::Basename, "an absolute path outside the root is reduced");
    CY_CHECK(out == kForeignBasename, "and what survives is the file's own name");

    PathForm windows_form = PathForm::Empty;
    const std::string windows = sanitised(R"(C:\Users\someone\game\src\pawn.cpp)", windows_form);
    CY_CHECK(windows_form == PathForm::Basename, "a Windows absolute path is reduced too");
    CY_CHECK(windows == "pawn.cpp", "MSVC has no prefix-map, so this is the only mechanism there");
}

void case_2_the_declared_root_is_stripped() {
    const std::string root = source_root();
    CY_CHECK(!root.empty(), "this build declared its source root");
    PathForm form = PathForm::Empty;
    const std::string out =
        sanitised((root + "/src/core/diagnostics/src/writer.cpp").c_str(), form);
    CY_CHECK(form == PathForm::Relative, "a path under the root becomes relative");
    CY_CHECK(out == "src/core/diagnostics/src/writer.cpp", "and keeps everything below the root");

    PathForm already = PathForm::Empty;
    const std::string relative = sanitised("src/gameplay/src/command.cpp", already);
    CY_CHECK(already == PathForm::Relative, "an already-relative path passes through");
    CY_CHECK(relative == "src/gameplay/src/command.cpp", "unchanged");
}

void case_3_no_artefact_carries_a_build_machine_path() {
    TraceStats stats{};
    const std::string bytes =
        capture_with_foreign_location("source_upload.cytrace", cy::ExportPolicy::upload(), stats);
    CY_CHECK(!bytes.empty(), "the artefact was written");

    // The gate. Both directions, because a check that only asserts an absence passes when nothing
    // was written at all.
    CY_CHECK(!contains(bytes, kForeignPrefix), "no absolute path from another machine survives");
    CY_CHECK(!contains(bytes, source_root()), "and none from this one either");
    CY_CHECK(contains(bytes, kForeignBasename),
             "the file's name did survive, so this is not vacuous");
    // The basename rather than the relative path: with `-fmacro-prefix-map` this file's __FILE__ is
    // already relative and the whole path is present, and without it the writer reduces it to this.
    // Asserting the half that holds EITHER WAY is what makes this check independent of the
    // compiler.
    CY_CHECK(contains(bytes, "test_source_privacy.cpp"),
             "and this file's own site is present, in whatever form the mechanism left it");

    // One location was reduced, and the artefact says so rather than rewriting it silently.
    CY_CHECK(stats.events_written >= 2, "both records reached the artefact");
}

void case_4_a_tighter_policy_removes_the_path_entirely() {
    TraceStats stats{};
    const std::string bytes = capture_with_foreign_location("source_public.cytrace",
                                                            cy::ExportPolicy::public_only(), stats);
    CY_CHECK(!bytes.empty(), "the artefact was written");
    CY_CHECK(!contains(bytes, kForeignPrefix), "still no absolute path");
    CY_CHECK(!contains(bytes, kForeignBasename),
             "a Public-only artefact carries no source path at all, because a path is Developer");
    CY_CHECK(!contains(bytes, "test_source_privacy.cpp"), "including this file's own");
    CY_CHECK(stats.redacted_fields > 0, "and the removal is counted rather than silent");
}

void case_5_no_registered_name_is_a_source_path() {
    // The class, not the instance: whatever any subsystem in this process registered, no NAME in
    // the metadata table looks like a source file. A name is beyond the writer's redaction by
    // construction, so nothing that could ever be a path may be one.
    const RegistryStats counts = registry_stats();
    u32 offenders = 0;
    for (NameId id = 1; id <= counts.names; ++id) {
        const char* name = lookup_name(id);
        if (name == nullptr) {
            continue;
        }
        const std::string text = name;
        const bool looks_like_a_path =
            text.find('/') != std::string::npos || text.find('\\') != std::string::npos;
        const bool looks_like_a_source_file =
            text.ends_with(".cpp") || text.ends_with(".h") || text.ends_with(".cc");
        if (looks_like_a_path && looks_like_a_source_file) {
            std::fprintf(stderr, "    name %u is a source path: %s\n", id, name);
            ++offenders;
        }
    }
    CY_CHECK_EQ(offenders, 0u, "no source location was registered as a name");
    CY_CHECK(source_location_count() >= 2u, "and locations went into the table that is classified");
}

}  // namespace

int main() {
    case_1_absolute_paths_keep_only_their_basename();
    case_2_the_declared_root_is_stripped();
    case_3_no_artefact_carries_a_build_machine_path();
    case_4_a_tighter_policy_removes_the_path_entirely();
    case_5_no_registered_name_is_a_source_path();
    return cy_test::summarise("test_source_privacy");
}
