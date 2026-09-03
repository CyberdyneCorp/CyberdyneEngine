#include <cy/core/base/error.h>

namespace cy {

const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None:
            return "None";
        case ErrorCode::Unknown:
            return "Unknown";
        case ErrorCode::InvalidArgument:
            return "InvalidArgument";
        case ErrorCode::OutOfRange:
            return "OutOfRange";
        case ErrorCode::NotFound:
            return "NotFound";
        case ErrorCode::AlreadyExists:
            return "AlreadyExists";
        case ErrorCode::PermissionDenied:
            return "PermissionDenied";
        case ErrorCode::Unsupported:
            return "Unsupported";
        case ErrorCode::NotImplemented:
            return "NotImplemented";
        case ErrorCode::Unavailable:
            return "Unavailable";
        case ErrorCode::Timeout:
            return "Timeout";
        case ErrorCode::OutOfMemory:
            return "OutOfMemory";
        case ErrorCode::BufferTooSmall:
            return "BufferTooSmall";
        case ErrorCode::Io:
            return "Io";
        case ErrorCode::Internal:
            return "Internal";
    }
    // Reachable only through a cast from an integer outside the enumeration.
    return "<invalid>";
}

}  // namespace cy
