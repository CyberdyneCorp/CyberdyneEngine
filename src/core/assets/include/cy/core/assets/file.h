#ifndef CY_CORE_ASSETS_FILE_H
#define CY_CORE_ASSETS_FILE_H
// File and directory access. Task 3.3.5.
//
// `core-assets-and-io` — "File and directory access": stream-based access with read, write, seek,
// size, flush and memory mapping, plus directory enumeration, creation, move, copy and delete; and
// **writes to the user mount are atomic where the platform allows** — write to a temporary file
// then rename, so an interrupted save cannot corrupt existing data.
//
// This is the NATIVE filesystem. The virtual filesystem in vfs.h is layered over it and is what
// game code uses; this layer is what a mount is implemented in terms of, and what a tool that
// legitimately knows about a real directory uses.
//
// BLOCKING IS THE POINT OF THIS FILE, and it is why nothing here may be called from a job worker.
// `core-jobs-and-concurrency` refuses a blocking region on a thread executing a job; the asset
// system reaches these calls through cy::jobs::AsyncService, whose whole reason to exist is that it
// is the thread where blocking is legal. A call here on a worker is a defect that the job system's
// own counter reports — see asset_system.cpp, which never makes one.
//
// ERRORS, NOT EXCEPTIONS. Every operation returns Expected or Status. std::filesystem's throwing
// overloads are never used; the std::error_code ones are, and their code lands in
// `Error::system_code` so a caller that must distinguish ENOSPC from EACCES still can.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::assets {

enum class FileMode : u8 {
    /// Open an existing file for reading. Fails with NotFound when it does not exist.
    Read = 0,
    /// Create or truncate for writing.
    Write = 1,
    /// Open for writing at the end, creating if absent.
    Append = 2,
    /// Open an existing file for both. Does not create; a writer that means "create" says Write.
    ReadWrite = 3,
};

enum class SeekOrigin : u8 { Begin = 0, Current = 1, End = 2 };

/// One open file. Move-only: two owners of a descriptor is two closes of it.
class File {
public:
    File() noexcept = default;
    ~File();

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    /// `path` is a NATIVE path — this layer is below the virtual filesystem and does not normalise.
    [[nodiscard]] static Expected<File, Error> open(const char* path, FileMode mode) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ >= 0; }

    /// Read from the current position, advancing it. Returns the number of bytes read, which is
    /// less than `size` at end of file and is not an error.
    [[nodiscard]] Expected<usize, Error> read(void* destination, usize size) noexcept;

    /// Read from an absolute offset without moving the current position. This is what a package
    /// reader uses: one file, many concurrent range reads, no shared cursor to serialise on.
    [[nodiscard]] Expected<usize, Error> read_at(u64 offset, void* destination,
                                                 usize size) noexcept;

    /// Write the whole buffer, or fail. A short write is retried rather than reported, because
    /// every caller here would only retry it.
    [[nodiscard]] Status write(const void* source, usize size) noexcept;

    [[nodiscard]] Expected<u64, Error> seek(i64 offset, SeekOrigin origin) noexcept;
    [[nodiscard]] Expected<u64, Error> tell() const noexcept;
    [[nodiscard]] Expected<u64, Error> size() const noexcept;

    /// Push this file's buffered data to the storage device. Slow on purpose: it is what makes the
    /// atomic-write sequence a durability guarantee rather than an ordering hope.
    [[nodiscard]] Status flush() noexcept;

    void close() noexcept;

private:
    // A native descriptor, -1 when closed. An int rather than a void* because the two platforms
    // that need a HANDLE will hold it in a union here, and neither has been compiled yet.
    int handle_ = -1;
};

/// A read-only memory mapping of a whole file or a range of one.
///
/// `core-assets-and-io` — "Memory-mapped read": an uncompressed, aligned entry is mapped rather
/// than copied into a buffer. `MappedFile::map` reports Unsupported where the platform has no
/// mapping, and every caller has a copying path, so mapping is an optimisation and never a
/// requirement.
class MappedFile {
public:
    MappedFile() noexcept = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    /// Map `length` bytes at `offset`. A length of zero maps to the end of the file. The offset
    /// need not be page-aligned: the mapping is made from the page below it and `data()` points at
    /// the byte the caller asked for.
    [[nodiscard]] static Expected<MappedFile, Error> map(const char* path, u64 offset = 0,
                                                         usize length = 0) noexcept;

    [[nodiscard]] const u8* data() const noexcept { return data_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] bool is_mapped() const noexcept { return data_ != nullptr; }

    void unmap() noexcept;

private:
    const u8* data_ = nullptr;  ///< the byte the caller asked for
    usize size_ = 0;
    void* base_ = nullptr;  ///< the page-aligned mapping, which is what is unmapped
    usize base_size_ = 0;
};

/// True when this build can memory-map a file at all. Compiled in, so a test asserts the mapped
/// path where it exists and the copying path where it does not, rather than being skipped.
[[nodiscard]] bool memory_mapping_available() noexcept;

/// The page granularity a mapping offset is rounded to, for a package writer choosing alignment.
[[nodiscard]] usize memory_mapping_granularity() noexcept;

// --- Directories and whole-file operations ------------------------------------------------------

/// What `enumerate` reports for one entry. `name` is the entry's own name, not a path, and it is
/// valid only for the duration of the call.
struct DirectoryEntry {
    const char* name = "";
    bool is_directory = false;
    u64 size = 0;
};

/// Called once per entry. Returning false stops the walk, which is how a caller that has found what
/// it wanted avoids paying for the rest of a large directory.
using DirectoryVisitor = bool (*)(void* user, const DirectoryEntry& entry) noexcept;

namespace fs {

[[nodiscard]] bool exists(const char* path) noexcept;
[[nodiscard]] bool is_directory(const char* path) noexcept;
[[nodiscard]] Expected<u64, Error> file_size(const char* path) noexcept;

[[nodiscard]] Status create_directories(const char* path) noexcept;
[[nodiscard]] Status remove_file(const char* path) noexcept;
[[nodiscard]] Status remove_directory_recursive(const char* path) noexcept;
[[nodiscard]] Status move_file(const char* from, const char* to) noexcept;
[[nodiscard]] Status copy_file(const char* from, const char* to) noexcept;

/// Walk a directory. `recursive` descends; entries are visited in a SORTED order, so a listing is
/// the same on every platform and in every filesystem — an unordered walk is a cook step whose
/// output depends on the order the kernel happened to return.
[[nodiscard]] Status enumerate(const char* path, bool recursive, DirectoryVisitor visitor,
                               void* user) noexcept;

/// Read a whole file into `out`, replacing its contents.
[[nodiscard]] Status read_whole(const char* path, Array<u8>& out) noexcept;

/// The suffix of the temporary file `write_atomic` uses. Public because `discard_temporaries`
/// recognises it and because a project's own tooling should not have to guess.
inline constexpr const char* kTemporarySuffix = ".cytmp";

/// Write a whole file atomically: to `<path>.<n>.cytmp`, flushed, then renamed over `path`.
///
/// `core-assets-and-io` — "Interrupted save": a process killed during a save leaves the previous
/// file intact and a temporary that is discarded on next start. Rename is atomic on every
/// filesystem the engine targets, so the file at `path` is at every instant either entirely the old
/// contents or entirely the new.
[[nodiscard]] Status write_atomic(const char* path, const void* data, usize size) noexcept;

/// Remove the temporaries an interrupted save left in a directory. Called at start-up on the user
/// mount; it is the second half of the interrupted-save requirement, and it reports how many it
/// removed so that a repeated crash is visible rather than silent.
[[nodiscard]] Expected<usize, Error> discard_temporaries(const char* directory) noexcept;

}  // namespace fs

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_FILE_H
