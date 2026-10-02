// The invariant: a field cannot be exported at a level it was never classified for.
//
// design.md section 2, and `diagnostics-profiling-and-crash` — "Privacy classification". Three
// things are proved here, in order of how badly each would matter if it were false:
//
//   1. A value classified above the artefact's declared ceiling does not appear in the artefact's
//      bytes. Not merely marked, not merely hidden by a reader: absent.
//   2. A field whose id carries no classification is treated as unclassified, redacted and counted
//   —
//      so a field that escaped registration cannot escape the policy either.
//   3. A Secret value is admitted by no policy at any ceiling, because credentials, tokens and
//      personal files are never captured automatically.
//
// The compile-time half of the invariant — that a field cannot be *declared* without a
// classification — is proved by test_field_macro.py, which is a compilation, not a run.

#include "trace_reader.h"

#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>

using namespace cy::diag;

namespace {

CY_TRACE_NAME(sample_event, "privacy.sample")
CY_TRACE_CATEGORY(sample_category, "privacy")

CY_TRACE_FIELD(frame_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(build_branch, string, cy::Privacy::Developer)
CY_TRACE_FIELD(account_region, string, cy::Privacy::PotentiallyPersonal)
CY_TRACE_FIELD(user_path, string, cy::Privacy::Sensitive)
CY_TRACE_FIELD(session_token, string, cy::Privacy::Secret)

constexpr const char* kBranch = "BRANCH-CANARY-main";
constexpr const char* kRegion = "REGION-CANARY-eu";
constexpr const char* kUserPath = "USERPATH-CANARY-/home/someone/save.dat";
constexpr const char* kToken = "TOKEN-CANARY-abcdef0123456789";

FieldValue text_of(FieldId field, const char* value) {
    return field_text(field, value, static_cast<u32>(std::strlen(value)));
}

/// Write one capture under `policy` and read it back.
cy_test::Capture write_capture(const char* path, cy::ExportPolicy policy, TraceStats& stats) {
    TraceConfig config;
    config.path = path;
    config.policy = policy;
    config.consumer_thread = false;

    const auto opened = trace_open(config);
    CY_CHECK_MESSAGE(opened.has_value(), "the trace opens");

    const FieldValue fields[] = {
        field_u64(frame_index(), 42),
        text_of(build_branch(), kBranch),
        text_of(account_region(), kRegion),
        text_of(user_path(), kUserPath),
        text_of(session_token(), kToken),
        // An id nothing registered: unclassified, and therefore not exportable at any level.
        field_u64(90001, 7),
    };
    trace_instant(sample_event(), sample_category(), Channel::Critical, fields, 6);
    trace_flush();
    const auto closed = trace_close();
    CY_CHECK_MESSAGE(closed.has_value(), "the trace closes");
    if (closed.has_value()) {
        stats = closed.value();
    }
    return cy_test::read_capture(path);
}

const cy_test::ReadField* find_field(const cy_test::Capture& capture, const char* name) {
    u32 wanted = 0;
    for (const auto& entry : capture.fields) {
        if (entry.second.name == name) {
            wanted = entry.first;
        }
    }
    for (const auto& record : capture.records) {
        for (const auto& field : record.fields) {
            if (field.field == wanted && wanted != 0) {
                return &field;
            }
        }
    }
    return nullptr;
}

}  // namespace

CY_TEST_CASE(
    "privacy: the upload policy removes values above its ceiling and counts unclassified fields") {
    TraceStats stats;
    const cy_test::Capture capture =
        write_capture("cy_diag_privacy_upload.cytrace", cy::ExportPolicy::upload(), stats);
    CY_CHECK_MESSAGE(capture.valid, "the capture parses");

    // 1. The classified values are not in the file at all.
    CY_CHECK_MESSAGE(capture.contains_bytes(kBranch),
                     "a developer value survives the upload policy");
    CY_CHECK_MESSAGE(capture.contains_bytes(kRegion), "a potentially-personal value survives it");
    CY_CHECK_MESSAGE(!capture.contains_bytes(kUserPath),
                     "a sensitive value is absent from the artefact");
    CY_CHECK_MESSAGE(!capture.contains_bytes(kToken), "a secret value is absent from the artefact");

    // The field entries remain, so the gap is visible rather than silently misleading.
    const cy_test::ReadField* path = find_field(capture, "user_path");
    const cy_test::ReadField* token = find_field(capture, "session_token");
    const cy_test::ReadField* branch = find_field(capture, "build_branch");
    CY_CHECK_MESSAGE((path != nullptr && path->redacted()),
                     "the sensitive field is marked redacted");
    CY_CHECK_MESSAGE((token != nullptr && token->redacted()),
                     "the secret field is marked redacted");
    CY_CHECK_MESSAGE((branch != nullptr && !branch->redacted() && branch->text == kBranch),
                     "the admitted field keeps its value");

    // 2. The unregistered field was redacted and counted.
    CY_CHECK_MESSAGE(stats.unclassified_fields >= 1, "an unclassified field is reported");
    CY_CHECK_MESSAGE(stats.redacted_fields >= 2, "the fields above the ceiling are counted");

    // The artefact declares what it may contain, and each field carries its classification.
    const auto declared = capture.identity.find("export_policy");
    CY_CHECK_MESSAGE(
        (declared != capture.identity.end() && declared->second == "potentially-personal"),
        "the artefact declares its ceiling");
    for (const auto& entry : capture.fields) {
        if (entry.second.name == "user_path") {
            CY_CHECK_MESSAGE(entry.second.privacy == static_cast<u8>(cy::Privacy::Sensitive),
                             "the metadata table carries the classification");
        }
    }
}

CY_TEST_CASE("privacy: the local policy keeps sensitive values and still admits no secret") {
    TraceStats stats;
    const cy_test::Capture capture =
        write_capture("cy_diag_privacy_local.cytrace", cy::ExportPolicy::local(), stats);
    CY_CHECK_MESSAGE(capture.valid, "the capture parses");
    CY_CHECK_MESSAGE(capture.contains_bytes(kUserPath),
                     "a sensitive value is kept by the local policy");
    // No policy admits Secret, at any ceiling.
    CY_CHECK_MESSAGE(!capture.contains_bytes(kToken), "a secret value is absent even locally");
}

CY_TEST_CASE(
    "privacy: a policy can be tightened and never widened, and no policy admits a secret") {
    const cy::ExportPolicy local = cy::ExportPolicy::local();
    CY_CHECK_MESSAGE(local.allows(cy::Privacy::Sensitive), "local admits sensitive");
    CY_CHECK_MESSAGE(!local.allows(cy::Privacy::Secret), "no policy admits secret");

    const cy::ExportPolicy upload = cy::ExportPolicy::upload();
    CY_CHECK_MESSAGE(!upload.allows(cy::Privacy::Sensitive), "upload excludes sensitive");
    CY_CHECK_MESSAGE(upload.allows(cy::Privacy::PotentiallyPersonal),
                     "upload admits potentially personal");

    // A project may tighten and may not widen: tightening a public-only policy to sensitive leaves
    // it public-only.
    const cy::ExportPolicy tightened = upload.tighten(cy::Privacy::Public);
    CY_CHECK_MESSAGE(!tightened.allows(cy::Privacy::PotentiallyPersonal),
                     "tightening excludes more");
    const cy::ExportPolicy retightened =
        cy::ExportPolicy::public_only().tighten(cy::Privacy::Sensitive);
    CY_CHECK_MESSAGE(!retightened.allows(cy::Privacy::Developer),
                     "tighten() cannot widen a policy");
}
