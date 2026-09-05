// cy/abi/errors.h — how cy::Expected<T, Error> becomes a CyResult. Task 2.5.
//
// `native-abi`: "No exception, C++ or otherwise, SHALL propagate across the ABI. Fallible functions
// SHALL return a `CyResult` status code, with a thread-local last-error message retrievable via
// `cy_get_last_error`."
//
// --- THE MAPPING IS A CAST, ON PURPOSE ----------------------------------------------------------
//
// `CyResult`'s first fifteen values are `cy::ErrorCode`'s enumerators in `cy::ErrorCode`'s order,
// so translating one to the other is a numeric conversion rather than a switch. A switch would be a
// second copy of the enum: it compiles fine when a value is added to only one of the two, and the
// first symptom is a module told "unknown" about a failure the engine classified. The pairing is
// instead asserted enumerator by enumerator in src/abi/src/errors.cpp, so adding a value to either
// enum without the other is a compile error.
//
// The ABI's own results start at 100. They have no `cy::ErrorCode` — a version mismatch and a
// schema that is too new are failures that exist because there is a boundary — and mapping one back
// gives `ErrorCode::Unsupported` with the message preserved, because inventing an engine error code
// for a boundary failure would put a value in `cy::ErrorCode` that nothing inside the engine can
// produce.
//
// --- THE MESSAGE IS THREAD-LOCAL AND COPIED -----------------------------------------------------
//
// `cy::Error::message` is a borrowed pointer with no ownership (there are no allocators at layer
// 0), and a module may hold the pointer `cy_get_last_error` returns across the rest of its call.
// Those two facts together mean the ABI must copy: the storage is a fixed thread-local buffer,
// truncating rather than allocating, and it is valid until the same thread's next failing call.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::abi {

/// How many bytes of a last-error message survive. Longer messages are truncated with an ellipsis
/// rather than allocated for: this buffer exists per thread that has ever crossed the ABI, and a
/// diagnostic that needs more than this is a diagnostic that should have been logged.
inline constexpr usize kLastErrorCapacity = 512;

/// The ABI result for an engine error code. Total, and a conversion rather than a lookup.
[[nodiscard]] CyResult to_result(ErrorCode code) noexcept;

/// The engine error code for an ABI result. `CY_RESULT_OK` maps to `ErrorCode::None`; the ABI's own
/// results from 100 up map to `ErrorCode::Unsupported`, because they name failures the engine's
/// vocabulary does not have.
[[nodiscard]] ErrorCode to_error_code(CyResult result) noexcept;

/// Record a failure for this thread and return its code. Every ABI entry that fails goes through
/// here, which is what makes `cy_get_last_error` meaningful rather than occasionally set.
CyResult report(const Error& error) noexcept;

/// The same for a code and a literal, for the failures that never had an `Error` — an argument the
/// ABI itself rejected, or a module reporting its own.
CyResult report(CyResult result, const char* message) noexcept;

/// Clear this thread's last error. Called by every ABI entry that succeeds, so that a stale message
/// from an earlier call cannot be read as this one's.
void clear_last_error() noexcept;

/// This thread's last error message. Never null; empty when nothing has failed.
[[nodiscard]] const char* last_error_message() noexcept;

/// This thread's last error code, `CY_RESULT_OK` when nothing has failed.
[[nodiscard]] CyResult last_error_code() noexcept;

/// The shim every fallible ABI entry is written with: unwrap into `*out` or report.
///
/// This is the "mechanical enough to generate" shape design.md names — an `Expected<T, Error>`
/// becomes a `CyResult` and an out-parameter, the output is left untouched on failure (which is
/// `native-abi`'s "Failure is reported by return value" scenario), and the message is recorded.
template <class T>
[[nodiscard]] CyResult unwrap(Expected<T, Error>&& expected, T* out) noexcept {
    if (!expected) {
        return report(expected.error());
    }
    if (out == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "output pointer is null");
    }
    *out = static_cast<T&&>(expected.value());
    clear_last_error();
    return CY_RESULT_OK;
}

/// The void case: a `Status` becomes a bare `CyResult`.
[[nodiscard]] CyResult unwrap(const Status& status) noexcept;

}  // namespace cy::abi
