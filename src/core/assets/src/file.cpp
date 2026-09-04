// Native file and directory access. Task 3.3.5.
//
// PORTABILITY. Directory operations go through std::filesystem's std::error_code overloads, which
// are portable and do not throw. The two things std::filesystem does not offer — positional reads
// and memory mapping — are POSIX here, behind CY_ASSETS_POSIX_IO. design.md section 9 asks for no
// new platform-conditional code without a stated reason; the reason is that there is no portable
// spelling of either, and the conditional is confined to this file with the non-POSIX branch
// returning Unsupported rather than silently doing something else. The Windows implementation is
// CreateFileMapping/MapViewOfFile and ReadFile with an OVERLAPPED offset; it is NOT written here,
// because it cannot be compiled or tested on this machine and prose that has never run is worse
// than an honest refusal.

#include <cy/core/assets/file.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#    define CY_ASSETS_POSIX_IO 1
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#else
#    define CY_ASSETS_POSIX_IO 0
#endif

namespace cy::assets {
namespace {

namespace stdfs = std::filesystem;

/// One place the platform's own number becomes an Error, so every call reports it the same way.
Error from_errno(ErrorCode code, const char* message) noexcept {
    return Error{code, message, static_cast<i64>(errno)};
}

/// Map an std::error_code onto the engine's classification. The value travels in `system_code`, so
/// nothing is lost by the classification being coarse.
Error from_error_code(const std::error_code& code, const char* message) noexcept {
    ErrorCode classification = ErrorCode::Io;
    if (code == std::errc::no_such_file_or_directory) {
        classification = ErrorCode::NotFound;
    } else if (code == std::errc::permission_denied) {
        classification = ErrorCode::PermissionDenied;
    } else if (code == std::errc::file_exists) {
        classification = ErrorCode::AlreadyExists;
    }
    return Error{classification, message, static_cast<i64>(code.value())};
}

#if CY_ASSETS_POSIX_IO
int posix_flags(FileMode mode) noexcept {
    switch (mode) {
        case FileMode::Read:
            return O_RDONLY;
        case FileMode::Write:
            return O_WRONLY | O_CREAT | O_TRUNC;
        case FileMode::Append:
            return O_WRONLY | O_CREAT | O_APPEND;
        case FileMode::ReadWrite:
            return O_RDWR;
    }
    return O_RDONLY;
}
#endif

/// A counter that makes two concurrent atomic writes to one path pick different temporaries.
/// Combined with the process id so that two processes do not collide either.
u64 next_temporary_serial() noexcept {
    static std::atomic<u64> serial{0};
    return serial.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// --- File --------------------------------------------------------------------------------------
//
// READ, WRITE, SEEK AND FLUSH ARE NOT CONST, and clang-tidy's `readability-make-member-function-
// const` is right about the language and wrong about the meaning. None of them writes a member: the
// state they change is the KERNEL'S — the file position, the page cache, the bytes on the device —
// reached through a descriptor this object happens to hold by value. Marking them const would say
// "calling this twice from two threads is fine", which is exactly what a shared file position makes
// false. The NOLINTs below carry that reason rather than repeating it at each one.

File::~File() {
    close();
}

File::File(File&& other) noexcept : handle_(other.handle_) {
    other.handle_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

Expected<File, Error> File::open(const char* path, FileMode mode) noexcept {
#if CY_ASSETS_POSIX_IO
    CY_ASSERT(path != nullptr);
    const int handle = ::open(path, posix_flags(mode), 0644);
    if (handle < 0) {
        const ErrorCode code = errno == ENOENT   ? ErrorCode::NotFound
                               : errno == EACCES ? ErrorCode::PermissionDenied
                                                 : ErrorCode::Io;
        return make_unexpected(from_errno(code, "the file could not be opened"));
    }
    File file;
    file.handle_ = handle;
    return file;
#else
    (void)path;
    (void)mode;
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const): see the note above.
Expected<usize, Error> File::read(void* destination, usize size) noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ < 0) {
        return fail(ErrorCode::InvalidArgument, "read on a file that is not open");
    }
    auto* cursor = static_cast<u8*>(destination);
    usize total = 0;
    while (total < size) {
        const ::ssize_t got = ::read(handle_, cursor + total, size - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return make_unexpected(from_errno(ErrorCode::Io, "the file could not be read"));
        }
        if (got == 0) {
            break;  // end of file: a short read, not a failure
        }
        total += static_cast<usize>(got);
    }
    return total;
#else
    (void)destination;
    (void)size;
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const): see the note above.
Expected<usize, Error> File::read_at(u64 offset, void* destination, usize size) noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ < 0) {
        return fail(ErrorCode::InvalidArgument, "read_at on a file that is not open");
    }
    auto* cursor = static_cast<u8*>(destination);
    usize total = 0;
    while (total < size) {
        const ::ssize_t got =
            ::pread(handle_, cursor + total, size - total, static_cast<::off_t>(offset + total));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return make_unexpected(from_errno(ErrorCode::Io, "the file could not be read"));
        }
        if (got == 0) {
            break;
        }
        total += static_cast<usize>(got);
    }
    return total;
#else
    (void)offset;
    (void)destination;
    (void)size;
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const): see the note above.
Status File::write(const void* source, usize size) noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ < 0) {
        return fail(ErrorCode::InvalidArgument, "write on a file that is not open");
    }
    const auto* cursor = static_cast<const u8*>(source);
    usize total = 0;
    while (total < size) {
        const ::ssize_t put = ::write(handle_, cursor + total, size - total);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return make_unexpected(from_errno(ErrorCode::Io, "the file could not be written"));
        }
        total += static_cast<usize>(put);
    }
    return ok();
#else
    (void)source;
    (void)size;
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const): see the note above.
Expected<u64, Error> File::seek(i64 offset, SeekOrigin origin) noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ < 0) {
        return fail(ErrorCode::InvalidArgument, "seek on a file that is not open");
    }
    int whence = SEEK_END;
    if (origin == SeekOrigin::Begin) {
        whence = SEEK_SET;
    } else if (origin == SeekOrigin::Current) {
        whence = SEEK_CUR;
    }
    const ::off_t position = ::lseek(handle_, static_cast<::off_t>(offset), whence);
    if (position < 0) {
        return make_unexpected(from_errno(ErrorCode::Io, "the file could not be sought"));
    }
    return static_cast<u64>(position);
#else
    (void)offset;
    (void)origin;
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

Expected<u64, Error> File::tell() const noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ < 0) {
        return fail(ErrorCode::InvalidArgument, "tell on a file that is not open");
    }
    const ::off_t position = ::lseek(handle_, 0, SEEK_CUR);
    if (position < 0) {
        return make_unexpected(from_errno(ErrorCode::Io, "the file position could not be read"));
    }
    return static_cast<u64>(position);
#else
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

Expected<u64, Error> File::size() const noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ < 0) {
        return fail(ErrorCode::InvalidArgument, "size on a file that is not open");
    }
    struct ::stat information = {};
    if (::fstat(handle_, &information) != 0) {
        return make_unexpected(from_errno(ErrorCode::Io, "the file size could not be read"));
    }
    return static_cast<u64>(information.st_size);
#else
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const): see the note above.
Status File::flush() noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ < 0) {
        return fail(ErrorCode::InvalidArgument, "flush on a file that is not open");
    }
    if (::fsync(handle_) != 0) {
        // EINVAL on a filesystem or a device that cannot sync — a pipe, or some tmpfs mounts. The
        // data is already where it is going, so this is not an error the caller can act on.
        if (errno != EINVAL) {
            return make_unexpected(from_errno(ErrorCode::Io, "the file could not be flushed"));
        }
    }
    return ok();
#else
    return fail(ErrorCode::Unsupported, "file access is not implemented for this platform");
#endif
}

void File::close() noexcept {
#if CY_ASSETS_POSIX_IO
    if (handle_ >= 0) {
        (void)::close(handle_);
        handle_ = -1;
    }
#endif
}

// --- MappedFile --------------------------------------------------------------------------------

MappedFile::~MappedFile() {
    unmap();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_), size_(other.size_), base_(other.base_), base_size_(other.base_size_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.base_ = nullptr;
    other.base_size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        unmap();
        data_ = other.data_;
        size_ = other.size_;
        base_ = other.base_;
        base_size_ = other.base_size_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.base_ = nullptr;
        other.base_size_ = 0;
    }
    return *this;
}

Expected<MappedFile, Error> MappedFile::map(const char* path, u64 offset, usize length) noexcept {
#if CY_ASSETS_POSIX_IO
    CY_ASSERT(path != nullptr);
    const int handle = ::open(path, O_RDONLY);
    if (handle < 0) {
        const ErrorCode code = errno == ENOENT ? ErrorCode::NotFound : ErrorCode::Io;
        return make_unexpected(from_errno(code, "the file could not be opened for mapping"));
    }

    struct ::stat information = {};
    if (::fstat(handle, &information) != 0) {
        (void)::close(handle);
        return make_unexpected(from_errno(ErrorCode::Io, "the file size could not be read"));
    }
    const u64 total = static_cast<u64>(information.st_size);
    if (offset > total) {
        (void)::close(handle);
        return fail(ErrorCode::OutOfRange, "the mapping offset is past the end of the file");
    }
    const auto span = length == 0 ? static_cast<usize>(total - offset) : length;
    if (span == 0) {
        (void)::close(handle);
        return fail(ErrorCode::InvalidArgument, "an empty range cannot be mapped");
    }
    if (offset + span > total) {
        (void)::close(handle);
        return fail(ErrorCode::OutOfRange, "the mapping extends past the end of the file");
    }

    // mmap requires a page-aligned offset, so the mapping starts at the page below and `data_`
    // points into it. A caller therefore never has to align its own offsets.
    const usize granularity = memory_mapping_granularity();
    const u64 aligned = offset - (offset % granularity);
    const auto slack = static_cast<usize>(offset - aligned);
    const usize base_span = span + slack;

    void* mapping =
        ::mmap(nullptr, base_span, PROT_READ, MAP_PRIVATE, handle, static_cast<::off_t>(aligned));
    // The descriptor is closed immediately: the mapping holds its own reference to the file, which
    // is what keeps a mapped package independent of the process's descriptor budget.
    (void)::close(handle);
    if (mapping == MAP_FAILED) {
        return make_unexpected(from_errno(ErrorCode::Io, "the file could not be mapped"));
    }

    MappedFile mapped;
    mapped.base_ = mapping;
    mapped.base_size_ = base_span;
    mapped.data_ = static_cast<const u8*>(mapping) + slack;
    mapped.size_ = span;
    return mapped;
#else
    (void)path;
    (void)offset;
    (void)length;
    return fail(ErrorCode::Unsupported, "memory mapping is not implemented for this platform");
#endif
}

void MappedFile::unmap() noexcept {
#if CY_ASSETS_POSIX_IO
    if (base_ != nullptr) {
        (void)::munmap(base_, base_size_);
    }
#endif
    base_ = nullptr;
    base_size_ = 0;
    data_ = nullptr;
    size_ = 0;
}

bool memory_mapping_available() noexcept {
    return CY_ASSETS_POSIX_IO != 0;
}

usize memory_mapping_granularity() noexcept {
#if CY_ASSETS_POSIX_IO
    const long page = ::sysconf(_SC_PAGESIZE);
    return page > 0 ? static_cast<usize>(page) : usize{4096};
#else
    return 4096;
#endif
}

// --- Directories -------------------------------------------------------------------------------

namespace fs {

bool exists(const char* path) noexcept {
    std::error_code code;
    return stdfs::exists(stdfs::path(path), code) && !code;
}

bool is_directory(const char* path) noexcept {
    std::error_code code;
    return stdfs::is_directory(stdfs::path(path), code) && !code;
}

Expected<u64, Error> file_size(const char* path) noexcept {
    std::error_code code;
    const std::uintmax_t size = stdfs::file_size(stdfs::path(path), code);
    if (code) {
        return make_unexpected(from_error_code(code, "the file size could not be read"));
    }
    return static_cast<u64>(size);
}

Status create_directories(const char* path) noexcept {
    std::error_code code;
    stdfs::create_directories(stdfs::path(path), code);
    if (code) {
        return make_unexpected(from_error_code(code, "the directory could not be created"));
    }
    return ok();
}

Status remove_file(const char* path) noexcept {
    std::error_code code;
    if (!stdfs::remove(stdfs::path(path), code)) {
        if (code) {
            return make_unexpected(from_error_code(code, "the file could not be removed"));
        }
        return fail(ErrorCode::NotFound, "there is no such file");
    }
    return ok();
}

Status remove_directory_recursive(const char* path) noexcept {
    std::error_code code;
    stdfs::remove_all(stdfs::path(path), code);
    if (code) {
        return make_unexpected(from_error_code(code, "the directory could not be removed"));
    }
    return ok();
}

Status move_file(const char* from, const char* to) noexcept {
    std::error_code code;
    stdfs::rename(stdfs::path(from), stdfs::path(to), code);
    if (code) {
        return make_unexpected(from_error_code(code, "the file could not be moved"));
    }
    return ok();
}

Status copy_file(const char* from, const char* to) noexcept {
    std::error_code code;
    stdfs::copy_file(stdfs::path(from), stdfs::path(to), stdfs::copy_options::overwrite_existing,
                     code);
    if (code) {
        return make_unexpected(from_error_code(code, "the file could not be copied"));
    }
    return ok();
}

Status enumerate(const char* path, bool recursive, DirectoryVisitor visitor, void* user) noexcept {
    CY_ASSERT(visitor != nullptr);
    std::error_code code;
    const stdfs::path root(path);
    if (!stdfs::is_directory(root, code) || code) {
        return fail(ErrorCode::NotFound, "there is no such directory");
    }

    // Collected and sorted before visiting: see the declaration. The relative name is what the
    // visitor receives, so a recursive walk reports "textures/stone.ktx2" rather than an absolute
    // path that would differ between machines.
    struct Row {
        std::string name;
        bool is_directory;
        u64 size;
    };
    std::vector<Row> rows;

    auto collect = [&](const stdfs::directory_entry& entry) {
        std::error_code local;
        const bool directory = entry.is_directory(local);
        u64 size = 0;
        if (!directory) {
            const std::uintmax_t measured = entry.file_size(local);
            size = local ? 0 : static_cast<u64>(measured);
        }
        std::string name = stdfs::relative(entry.path(), root, local).generic_string();
        rows.push_back(Row{std::move(name), directory, size});
    };

    if (recursive) {
        for (stdfs::recursive_directory_iterator it(root, code), last; !code && it != last;
             it.increment(code)) {
            collect(*it);
        }
    } else {
        for (stdfs::directory_iterator it(root, code), last; !code && it != last;
             it.increment(code)) {
            collect(*it);
        }
    }
    if (code) {
        return make_unexpected(from_error_code(code, "the directory could not be enumerated"));
    }

    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.name < b.name; });
    for (const Row& row : rows) {
        DirectoryEntry entry;
        entry.name = row.name.c_str();
        entry.is_directory = row.is_directory;
        entry.size = row.size;
        if (!visitor(user, entry)) {
            break;
        }
    }
    return ok();
}

Status read_whole(const char* path, Array<u8>& out) noexcept {
    Expected<File, Error> file = File::open(path, FileMode::Read);
    if (!file) {
        return make_unexpected(file.error());
    }
    Expected<u64, Error> size = file.value().size();
    if (!size) {
        return make_unexpected(size.error());
    }
    out.clear();
    if (Status grown = out.resize(static_cast<usize>(size.value())); !grown) {
        return grown;
    }
    if (size.value() == 0) {
        return ok();
    }
    Expected<usize, Error> read = file.value().read(out.data(), out.size());
    if (!read) {
        return make_unexpected(read.error());
    }
    if (read.value() != out.size()) {
        return fail(ErrorCode::Io, "the file was shorter than its reported size");
    }
    return ok();
}

Status write_atomic(const char* path, const void* data, usize size) noexcept {
    // <path>.<pid>.<serial>.cytmp — unique per process and per call, so two concurrent saves of one
    // file do not overwrite each other's temporary and then rename twice.
    char temporary[4096] = {};
    const int written =
        std::snprintf(temporary, sizeof(temporary), "%s.%llu.%llu%s", path,
                      static_cast<unsigned long long>(
#if CY_ASSETS_POSIX_IO
                          ::getpid()
#else
                          0
#endif
                              ),
                      static_cast<unsigned long long>(next_temporary_serial()), kTemporarySuffix);
    if (written < 0 || static_cast<usize>(written) >= sizeof(temporary)) {
        return fail(ErrorCode::InvalidArgument, "the path is too long for an atomic write");
    }

    {
        Expected<File, Error> file = File::open(temporary, FileMode::Write);
        if (!file) {
            return make_unexpected(file.error());
        }
        if (Status put = file.value().write(data, size); !put) {
            (void)remove_file(temporary);
            return put;
        }
        // Flushed BEFORE the rename. Without it the rename can be visible while the data is not,
        // and the interrupted-save guarantee would hold for the directory entry and not for the
        // contents it points at.
        if (Status flushed = file.value().flush(); !flushed) {
            (void)remove_file(temporary);
            return flushed;
        }
    }

    if (Status moved = move_file(temporary, path); !moved) {
        (void)remove_file(temporary);
        return moved;
    }
    return ok();
}

Expected<usize, Error> discard_temporaries(const char* directory) noexcept {
    struct Sink {
        const char* directory;
        usize removed;
    } sink{directory, 0};

    Status walked = enumerate(
        directory, false,
        [](void* user, const DirectoryEntry& entry) noexcept {
            auto* state = static_cast<Sink*>(user);
            const std::string_view name(entry.name);
            const std::string_view suffix(kTemporarySuffix);
            if (entry.is_directory || name.size() <= suffix.size() ||
                name.substr(name.size() - suffix.size()) != suffix) {
                return true;
            }
            std::string full(state->directory);
            full += '/';
            full += entry.name;
            if (remove_file(full.c_str())) {
                ++state->removed;
            }
            return true;
        },
        &sink);
    if (!walked) {
        return make_unexpected(walked.error());
    }
    return sink.removed;
}

}  // namespace fs

}  // namespace cy::assets
