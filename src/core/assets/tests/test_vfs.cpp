// The virtual filesystem: layered mounts, priority, patch masking, and the remote seam. Task 3.3.2.

#include "temp_dir.h"

#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace cy::assets;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

VirtualPath path_of(const char* raw) {
    auto path = VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

/// The bytes as text. `reinterpret_cast` rather than a cast through `void*`: -Wcast-align has
/// nothing to complain about here — `char` has the weakest alignment there is — and the two-step
/// cast is what clang-tidy flags.
std::string as_text(const cy::Array<u8>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string contents(const VirtualFileSystem& files, const char* raw) {
    cy::Array<u8> bytes;
    CY_REQUIRE(files.read(path_of(raw), bytes).has_value());
    return as_text(bytes);
}

cy::Expected<MountId, cy::Error> mount_memory(VirtualFileSystem& files, MemoryMount*& out,
                                              cy::i32 priority) {
    auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "memory");
    CY_REQUIRE(mount.has_value());
    out = mount.value().get();
    return files.mount_owned(std::move(mount.value()), priority);
}

/// A host machine, in process. `core-assets-and-io`'s remote mount without a transport: the
/// interface is the seam, and this is what proves the mount is correct against it.
class FakeHost final : public RemoteFileProvider {
public:
    void add(const char* path, const char* text) { files_.push_back(Row{path_of(path), text}); }

    [[nodiscard]] cy::Expected<u64, cy::Error> stat(const VirtualPath& path) noexcept override {
        ++stats;
        for (const Row& row : files_) {
            if (row.path == path) {
                return static_cast<u64>(row.text.size());
            }
        }
        return cy::fail(cy::ErrorCode::NotFound, "the host does not have that file");
    }

    [[nodiscard]] cy::Status fetch(const VirtualPath& path, u64 offset, void* destination,
                                   usize size) noexcept override {
        ++fetches;
        for (const Row& row : files_) {
            if (row.path == path) {
                if (offset + size > row.text.size()) {
                    return cy::fail(cy::ErrorCode::OutOfRange, "past the end");
                }
                std::memcpy(destination, row.text.data() + offset, size);
                return cy::ok();
            }
        }
        return cy::fail(cy::ErrorCode::NotFound, "the host does not have that file");
    }

    [[nodiscard]] cy::Status list(const VirtualPath& directory, bool, VirtualVisitor visitor,
                                  void* user) noexcept override {
        for (const Row& row : files_) {
            if (!row.path.is_within(directory)) {
                continue;
            }
            VirtualEntry entry;
            entry.path = &row.path;
            entry.size = row.text.size();
            if (!visitor(user, entry)) {
                break;
            }
        }
        return cy::ok();
    }

    unsigned stats = 0;
    unsigned fetches = 0;

private:
    struct Row {
        VirtualPath path;
        std::string text;
    };
    std::vector<Row> files_;
};

}  // namespace

CY_TEST_CASE("A memory mount serves what was added to it") {
    VirtualFileSystem files;
    MemoryMount* memory = nullptr;
    CY_REQUIRE(mount_memory(files, memory, mount_priority::kMemory).has_value());
    CY_REQUIRE(memory->add(path_of("data/greeting.txt"), "hello", 5).has_value());

    CY_CHECK(files.exists(path_of("data/greeting.txt")));
    CY_CHECK_EQ(files.size_of(path_of("data/greeting.txt")).value(), 5u);
    CY_CHECK(contents(files, "data/greeting.txt") == "hello");
    CY_CHECK_FALSE(files.exists(path_of("data/absent.txt")));

    // A range read serves part of a file.
    char window[3] = {};
    CY_REQUIRE(files.read_range(path_of("data/greeting.txt"), 1, window, 3).has_value());
    CY_CHECK(std::memcmp(window, "ell", 3) == 0);
}

CY_TEST_CASE("Scenario: Patch overrides base content") {
    // WHEN a patch package is mounted above a base package and both contain the same asset
    // THEN the patch's version SHALL be served, and an entry marked as deleted in the patch SHALL
    //      mask the base entry entirely.
    //
    // Exercised here at the NAMESPACE level, where the rule lives: any mount can mask, and package
    // mounts inherit the behaviour rather than reimplementing it (test_package.cpp mounts real
    // packages through the same machinery).
    VirtualFileSystem files;
    MemoryMount* base = nullptr;
    MemoryMount* patch = nullptr;
    CY_REQUIRE(mount_memory(files, base, mount_priority::kBasePackage).has_value());
    CY_REQUIRE(mount_memory(files, patch, mount_priority::kPatchPackage).has_value());

    CY_REQUIRE(base->add(path_of("art/stone.ktx2"), "base stone", 10).has_value());
    CY_REQUIRE(base->add(path_of("art/wood.ktx2"), "base wood", 9).has_value());
    CY_REQUIRE(base->add(path_of("art/removed.ktx2"), "base removed", 12).has_value());

    // The patch replaces one asset and deletes another.
    CY_REQUIRE(patch->add(path_of("art/stone.ktx2"), "patched stone", 13).has_value());
    CY_REQUIRE(patch->mark_deleted(path_of("art/removed.ktx2")).has_value());

    CY_CHECK(contents(files, "art/stone.ktx2") == "patched stone");
    CY_CHECK(contents(files, "art/wood.ktx2") == "base wood");  // untouched, falls through

    // Masked entirely: the base's copy is not served, and the answer is NotFound rather than the
    // stale content.
    CY_CHECK_FALSE(files.exists(path_of("art/removed.ktx2")));
    const auto masked = files.read_range(path_of("art/removed.ktx2"), 0, nullptr, 0);
    CY_REQUIRE_FALSE(masked.has_value());
    CY_CHECK(masked.error().code == cy::ErrorCode::NotFound);
}

CY_TEST_CASE("A masked path is absent from a listing too") {
    VirtualFileSystem files;
    MemoryMount* base = nullptr;
    MemoryMount* patch = nullptr;
    CY_REQUIRE(mount_memory(files, base, mount_priority::kBasePackage).has_value());
    CY_REQUIRE(mount_memory(files, patch, mount_priority::kPatchPackage).has_value());
    CY_REQUIRE(base->add(path_of("art/a.bin"), "a", 1).has_value());
    CY_REQUIRE(base->add(path_of("art/b.bin"), "b", 1).has_value());
    CY_REQUIRE(patch->add(path_of("art/a.bin"), "A", 1).has_value());
    CY_REQUIRE(patch->mark_deleted(path_of("art/b.bin")).has_value());

    struct Sink {
        std::string names;
    } sink;
    CY_REQUIRE(files
                   .enumerate(
                       path_of("art"), true,
                       [](void* user, const VirtualEntry& entry) noexcept {
                           static_cast<Sink*>(user)->names += entry.path->c_str();
                           static_cast<Sink*>(user)->names += ";";
                           return true;
                       },
                       &sink)
                   .has_value());
    // One entry, once, and not the masked one.
    CY_CHECK(sink.names == "art/a.bin;");
}

CY_TEST_CASE("Equal priorities resolve most-recently-mounted first") {
    VirtualFileSystem files;
    MemoryMount* first = nullptr;
    MemoryMount* second = nullptr;
    CY_REQUIRE(mount_memory(files, first, 100).has_value());
    CY_REQUIRE(mount_memory(files, second, 100).has_value());
    CY_REQUIRE(first->add(path_of("x.bin"), "first", 5).has_value());
    CY_REQUIRE(second->add(path_of("x.bin"), "second", 6).has_value());
    CY_CHECK(contents(files, "x.bin") == "second");
}

CY_TEST_CASE("Unmounting removes a layer") {
    VirtualFileSystem files;
    MemoryMount* base = nullptr;
    MemoryMount* over = nullptr;
    CY_REQUIRE(mount_memory(files, base, 100).has_value());
    const auto over_id = mount_memory(files, over, 200);
    CY_REQUIRE(over_id.has_value());
    CY_REQUIRE(base->add(path_of("x.bin"), "base", 4).has_value());
    CY_REQUIRE(over->add(path_of("x.bin"), "over", 4).has_value());
    CY_CHECK(contents(files, "x.bin") == "over");

    CY_REQUIRE(files.unmount(over_id.value()).has_value());
    CY_CHECK(contents(files, "x.bin") == "base");
    CY_CHECK_EQ(files.mount_count(), 1u);
    CY_CHECK_FALSE(files.unmount(over_id.value()).has_value());
}

CY_TEST_CASE("A directory mount serves a real directory and cannot be escaped") {
    const test::TempDir directory("directory_mount");
    CY_REQUIRE(fs::create_directories((directory.file("art")).c_str()).has_value());
    CY_REQUIRE(fs::write_atomic(directory.file("art/stone.txt").c_str(), "stone", 5).has_value());

    VirtualFileSystem files;
    auto mount = DirectoryMount::create(directory.c_str(), MountKind::Project, false);
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(files.mount_owned(std::move(mount.value()), mount_priority::kProject).has_value());

    CY_CHECK(contents(files, "art/stone.txt") == "stone");

    // The escape is impossible before a mount ever sees it: a VirtualPath cannot hold an unresolved
    // `..`, so there is no path to hand a mount that would leave its root.
    CY_CHECK_FALSE(VirtualPath::normalise("../../../etc/passwd").has_value());

    // Read-only means read-only.
    const auto refused = files.write(path_of("art/stone.txt"), "x", 1);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::PermissionDenied);
}

CY_TEST_CASE("A writable mount takes the write, atomically") {
    const test::TempDir directory("user_mount");
    VirtualFileSystem files;
    auto mount = DirectoryMount::create(directory.c_str(), MountKind::User, true);
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(files.mount_owned(std::move(mount.value()), mount_priority::kUser).has_value());

    // The parent directory does not exist yet, which is the common case for a first save.
    CY_REQUIRE(files.write(path_of("saves/slot1/profile.sav"), "saved", 5).has_value());
    CY_CHECK(contents(files, "saves/slot1/profile.sav") == "saved");

    const auto leftovers = fs::discard_temporaries(directory.file("saves/slot1").c_str());
    CY_REQUIRE(leftovers.has_value());
    CY_CHECK_EQ(leftovers.value(), 0u);
}

CY_TEST_CASE("Scenario: Development file serving") {
    // WHEN a device runs with a remote mount configured
    // THEN assets SHALL be fetched from the host machine on demand, so iteration does not require
    //      repackaging.
    FakeHost host;
    host.add("art/stone.ktx2", "stone from the host");
    host.add("art/wood.ktx2", "wood from the host");

    VirtualFileSystem files;
    auto mount = cy::make_unique<RemoteMount>(cy::current_allocator(), host);
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(files.mount_owned(std::move(mount.value()), mount_priority::kRemote).has_value());

    // ON DEMAND is the whole requirement: mounting fetched nothing.
    CY_CHECK_EQ(host.fetches, 0u);

    CY_CHECK(contents(files, "art/stone.ktx2") == "stone from the host");
    CY_CHECK_EQ(host.fetches, 1u);

    // The second asset costs a second fetch, and the first one was not re-fetched.
    CY_CHECK(contents(files, "art/wood.ktx2") == "wood from the host");
    CY_CHECK_EQ(host.fetches, 2u);

    CY_CHECK_FALSE(files.exists(path_of("art/absent.ktx2")));
}

CY_TEST_CASE("A local mount takes precedence over the host") {
    // Which is what makes a remote mount an iteration aid rather than a source of surprises: a file
    // the project has locally is the one served.
    FakeHost host;
    host.add("art/stone.ktx2", "from the host");

    VirtualFileSystem files;
    MemoryMount* local = nullptr;
    CY_REQUIRE(mount_memory(files, local, mount_priority::kProject).has_value());
    auto mount = cy::make_unique<RemoteMount>(cy::current_allocator(), host);
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(files.mount_owned(std::move(mount.value()), mount_priority::kRemote).has_value());
    CY_REQUIRE(local->add(path_of("art/stone.ktx2"), "from the project", 16).has_value());

    CY_CHECK(contents(files, "art/stone.ktx2") == "from the project");
    CY_CHECK_EQ(host.fetches, 0u);
}

CY_TEST_CASE("Reading a path no mount serves reports NotFound") {
    VirtualFileSystem files;
    const auto missing = files.resolve(path_of("nothing/here"));
    CY_REQUIRE_FALSE(missing.has_value());
    CY_CHECK(missing.error().code == cy::ErrorCode::NotFound);
}
