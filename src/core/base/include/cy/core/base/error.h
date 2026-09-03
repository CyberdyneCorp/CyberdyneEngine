// The error model. Task 3.1.1.
//
// `engine-architecture` compiles the engine with -fno-exceptions: a fallible operation returns
// cy::Expected<T, Error> and a programmer error trips CY_ASSERT. Nothing throws, so Error is the
// only way a failure crosses an interface boundary.
//
// Error is trivially copyable and holds no storage of its own. `message` points at a string literal
// or at storage that outlives the Error — there are no allocators at M0 (design.md §9), so an Error
// that owned its text would have nowhere to put it. Detail that has to be formatted is formatted at
// the point of reporting, not carried here.

#pragma once

#include <cy/core/base/types.h>

namespace cy {

// The classification a caller switches on. `system_code` carries the platform's own number — errno,
// GetLastError(), an SDL result — for the cases where the classification is not enough.
enum class ErrorCode : u32 {
    None = 0,
    Unknown,
    InvalidArgument,
    OutOfRange,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    Unsupported,     // the platform cannot do this at all
    NotImplemented,  // the engine has not written it yet
    Unavailable,     // possible in principle, not right now
    Timeout,
    OutOfMemory,
    BufferTooSmall,
    Io,
    Internal,
};

// The enumerator's own spelling, for a diagnostic. Never null.
const char* error_code_name(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    const char* message = "";
    i64 system_code = 0;
};

}  // namespace cy
