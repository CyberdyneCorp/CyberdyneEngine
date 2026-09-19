// Reading and writing a pipeline-cache file — Metal gap 6's half that is not a signature.
//
// `save_pipeline_cache` and `load_pipeline_cache` take a PATH rather than a blob, because
// `MTLBinaryArchive` serialises to a URL and cannot be handed over as bytes without a file. That
// leaves every backend needing the same two file operations, and two backends each writing them is
// two chances to disagree about what an absent file means. It means A COLD START, here, once.

#include <cy/backends/rhi/pipeline_cache_file.h>

#include <cy/core/assets/file.h>

namespace cy::rhi {

Status write_pipeline_cache_file(const char* path, Span<const u8> bytes) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "save_pipeline_cache(): no path");
    }
    Expected<assets::File, Error> file = assets::File::open(path, assets::FileMode::Write);
    if (!file) {
        return make_unexpected(file.error());
    }
    if (!bytes.empty()) {
        if (Status written = file->write(bytes.data(), bytes.size()); !written) {
            return written;
        }
    }
    return file->flush();
}

Expected<bool, Error> read_pipeline_cache_file(const char* path, Array<u8>& out) noexcept {
    out.clear();
    if (path == nullptr || path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "load_pipeline_cache(): no path");
    }
    // AN ABSENT FILE IS A COLD START AND NOT A FAILURE. A first run has no cache; reporting that as
    // an error would make every first run log one, and a log line nobody can act on is a log line
    // everybody learns to ignore.
    if (!assets::fs::exists(path)) {
        return false;
    }
    Expected<assets::File, Error> file = assets::File::open(path, assets::FileMode::Read);
    if (!file) {
        return make_unexpected(file.error());
    }
    Expected<u64, Error> size = file->size();
    if (!size) {
        return make_unexpected(size.error());
    }
    if (*size == 0) {
        return true;
    }
    if (Status sized = out.resize(static_cast<usize>(*size)); !sized) {
        return make_unexpected(sized.error());
    }
    Expected<usize, Error> read = file->read(out.data(), out.size());
    if (!read) {
        return make_unexpected(read.error());
    }
    if (*read != out.size()) {
        return fail(ErrorCode::Io, "load_pipeline_cache(): the cache file ended early");
    }
    return true;
}

}  // namespace cy::rhi
