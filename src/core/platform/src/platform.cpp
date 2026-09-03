#include <cy/core/platform/platform.h>

#include <cstring>

namespace cy {

Expected<usize, Error> write_to_buffer(char* buffer, usize capacity, std::string_view text) {
    if (buffer == nullptr || capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "a text-returning call needs a buffer to write to");
    }
    if (text.size() + 1 > capacity) {
        // Not truncated: a half path is more dangerous than no path, and the caller can size the
        // buffer from the error rather than from a value that looked plausible.
        return fail(ErrorCode::BufferTooSmall,
                    "the buffer is too small for the value; no partial value was written",
                    static_cast<i64>(text.size() + 1));
    }
    if (!text.empty()) {
        std::memcpy(buffer, text.data(), text.size());
    }
    buffer[text.size()] = '\0';
    return text.size();
}

}  // namespace cy
