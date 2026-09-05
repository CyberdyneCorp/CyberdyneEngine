// The Expected -> CyResult mapping, and the thread-local last error. Task 2.5.

#include <cy/abi/errors.h>

#include <cy/core/base/error.h>

#include <cstring>
#include <type_traits>

namespace cy::abi {
namespace {

// THE MAPPING, ASSERTED PAIR BY PAIR.
//
// The conversion below is a cast. These assertions are what make the cast correct, and what makes
// adding a value to either enum without the other a compile error rather than a module being told
// "unknown" about a failure the engine classified. There is deliberately no switch: a switch is a
// second copy of the enum, and a second copy drifts.
constexpr u32 code_value(ErrorCode code) noexcept {
    return static_cast<std::underlying_type_t<ErrorCode>>(code);
}

static_assert(code_value(ErrorCode::None) == CY_RESULT_OK);
static_assert(code_value(ErrorCode::Unknown) == CY_RESULT_UNKNOWN);
static_assert(code_value(ErrorCode::InvalidArgument) == CY_RESULT_INVALID_ARGUMENT);
static_assert(code_value(ErrorCode::OutOfRange) == CY_RESULT_OUT_OF_RANGE);
static_assert(code_value(ErrorCode::NotFound) == CY_RESULT_NOT_FOUND);
static_assert(code_value(ErrorCode::AlreadyExists) == CY_RESULT_ALREADY_EXISTS);
static_assert(code_value(ErrorCode::PermissionDenied) == CY_RESULT_PERMISSION_DENIED);
static_assert(code_value(ErrorCode::Unsupported) == CY_RESULT_UNSUPPORTED);
static_assert(code_value(ErrorCode::NotImplemented) == CY_RESULT_NOT_IMPLEMENTED);
static_assert(code_value(ErrorCode::Unavailable) == CY_RESULT_UNAVAILABLE);
static_assert(code_value(ErrorCode::Timeout) == CY_RESULT_TIMEOUT);
static_assert(code_value(ErrorCode::OutOfMemory) == CY_RESULT_OUT_OF_MEMORY);
static_assert(code_value(ErrorCode::BufferTooSmall) == CY_RESULT_BUFFER_TOO_SMALL);
static_assert(code_value(ErrorCode::Io) == CY_RESULT_IO);
static_assert(code_value(ErrorCode::Internal) == CY_RESULT_INTERNAL);

// The highest engine code, and therefore the boundary between "an engine error travelled across"
// and "the boundary itself failed". Written as the enumerator rather than as 14, so that a value
// appended to `cy::ErrorCode` moves it without this file being edited.
constexpr u32 kLastEngineCode = code_value(ErrorCode::Internal);

// The thread-local record. A fixed buffer rather than an allocation: this exists once per thread
// that has ever crossed the ABI, it is written on a failure path, and a diagnostic longer than this
// belongs in the log rather than in a return value.
//
// `thread_local` and not a per-thread slot the engine hands out, because `cy_get_last_error` takes
// no arguments — the specification's signature — so the thread is the only key available.
struct LastError {
    CyResult result = CY_RESULT_OK;
    char message[kLastErrorCapacity] = {};
};

LastError& last_error() noexcept {
    thread_local LastError error;
    return error;
}

// Copy at most capacity - 1 bytes and always terminate. Truncation is silent by design: an error
// path that failed to report because the message was long would be worse than a short message.
void copy_message(char* destination, usize capacity, const char* source) noexcept {
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    usize length = std::strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

}  // namespace

CyResult to_result(ErrorCode code) noexcept {
    const u32 value = code_value(code);
    if (value > kLastEngineCode) {
        // A code appended to `cy::ErrorCode` after this ABI's major version was fixed. It cannot be
        // given a number here without inventing an ABI value a module was not compiled to know, so
        // it degrades to the one value that means exactly that.
        return CY_RESULT_UNKNOWN;
    }
    return static_cast<CyResult>(value);
}

ErrorCode to_error_code(CyResult result) noexcept {
    const auto value = static_cast<u32>(result);
    if (value > kLastEngineCode) {
        // CY_RESULT_VERSION_MISMATCH and its neighbours name failures that exist because there is a
        // boundary. `cy::ErrorCode` has no word for them, and adding one would put a value in the
        // engine's vocabulary that nothing inside the engine can produce.
        return ErrorCode::Unsupported;
    }
    return static_cast<ErrorCode>(value);
}

CyResult report(const Error& error) noexcept {
    LastError& record = last_error();
    record.result = to_result(error.code);
    copy_message(record.message, kLastErrorCapacity, error.message);
    return record.result;
}

CyResult report(CyResult result, const char* message) noexcept {
    LastError& record = last_error();
    record.result = result;
    copy_message(record.message, kLastErrorCapacity, message);
    return result;
}

void clear_last_error() noexcept {
    LastError& record = last_error();
    record.result = CY_RESULT_OK;
    record.message[0] = '\0';
}

const char* last_error_message() noexcept {
    return last_error().message;
}

CyResult last_error_code() noexcept {
    return last_error().result;
}

CyResult unwrap(const Status& status) noexcept {
    if (!status) {
        return report(status.error());
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi

extern "C" const char* cy_result_name(CyResult result) {
    switch (result) {
        case CY_RESULT_OK:
            return "CY_RESULT_OK";
        case CY_RESULT_UNKNOWN:
            return "CY_RESULT_UNKNOWN";
        case CY_RESULT_INVALID_ARGUMENT:
            return "CY_RESULT_INVALID_ARGUMENT";
        case CY_RESULT_OUT_OF_RANGE:
            return "CY_RESULT_OUT_OF_RANGE";
        case CY_RESULT_NOT_FOUND:
            return "CY_RESULT_NOT_FOUND";
        case CY_RESULT_ALREADY_EXISTS:
            return "CY_RESULT_ALREADY_EXISTS";
        case CY_RESULT_PERMISSION_DENIED:
            return "CY_RESULT_PERMISSION_DENIED";
        case CY_RESULT_UNSUPPORTED:
            return "CY_RESULT_UNSUPPORTED";
        case CY_RESULT_NOT_IMPLEMENTED:
            return "CY_RESULT_NOT_IMPLEMENTED";
        case CY_RESULT_UNAVAILABLE:
            return "CY_RESULT_UNAVAILABLE";
        case CY_RESULT_TIMEOUT:
            return "CY_RESULT_TIMEOUT";
        case CY_RESULT_OUT_OF_MEMORY:
            return "CY_RESULT_OUT_OF_MEMORY";
        case CY_RESULT_BUFFER_TOO_SMALL:
            return "CY_RESULT_BUFFER_TOO_SMALL";
        case CY_RESULT_IO:
            return "CY_RESULT_IO";
        case CY_RESULT_INTERNAL:
            return "CY_RESULT_INTERNAL";
        case CY_RESULT_VERSION_MISMATCH:
            return "CY_RESULT_VERSION_MISMATCH";
        case CY_RESULT_SCHEMA_TOO_NEW:
            return "CY_RESULT_SCHEMA_TOO_NEW";
        case CY_RESULT_SCHEMA_UNMIGRATABLE:
            return "CY_RESULT_SCHEMA_UNMIGRATABLE";
        case CY_RESULT_MODULE_LOAD_FAILED:
            return "CY_RESULT_MODULE_LOAD_FAILED";
    }
    // Reached only by a value from a newer ABI than this build. Naming it is more useful than
    // asserting, because the caller is on the other side of a boundary and cannot be trusted.
    return "CY_RESULT_<unknown>";
}
