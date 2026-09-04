// The `.cypak` read path: the directory, variants, chunks, framing, mapping and the refusals.
// Task 3.3.3.

#include "temp_dir.h"

#include <cy/core/assets/package.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <utility>

using namespace cy::assets;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

cy::Array<u8> corpus(usize size, u8 seed) {
    cy::Array<u8> bytes;
    CY_REQUIRE(bytes.resize(size).has_value());
    for (usize i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>(((i / 32) * 5) + (i % 11) + seed);
    }
    return bytes;
}

PackageManifest manifest_at(u32 content_version, u32 minimum) {
    PackageManifest manifest;
    CY_REQUIRE(manifest.set_build_id("test-build").has_value());
    CY_REQUIRE(manifest.set_bundle("base").has_value());
    manifest.compatibility.content_version = content_version;
    manifest.compatibility.minimum_content_version = minimum;
    return manifest;
}

PackageOpenOptions options_at(u32 content_version, u32 minimum) {
    PackageOpenOptions options;
    options.runtime.content_version = content_version;
    options.runtime.minimum_content_version = minimum;
    return options;
}

VirtualPath path_of(const char* raw) {
    auto path = VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

}  // namespace

CY_TEST_CASE("A package round-trips through the writer and the reader") {
    const test::TempDir directory("package_round_trip");
    const std::string path = directory.file("base.cypak");
    const cy::AssetId mesh = mint_asset_id();
    const cy::AssetId texture = mint_asset_id();
    const cy::Array<u8> mesh_bytes = corpus(40000, 1);
    const cy::Array<u8> texture_bytes = corpus(9000, 2);

    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        PackageWriter::EntryOptions mesh_options;
        mesh_options.kind = AssetKind::Mesh;
        CY_REQUIRE(
            writer.add(mesh, VariantKey::any(), mesh_bytes.span(), mesh_options).has_value());
        PackageWriter::EntryOptions texture_options;
        texture_options.kind = AssetKind::Texture;
        CY_REQUIRE(writer.add(texture, VariantKey::any(), texture_bytes.span(), texture_options)
                       .has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    auto reader = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE(reader.has_value());
    CY_CHECK_EQ(reader.value()->entry_count(), 2u);
    CY_CHECK(std::string(reader.value()->manifest().build_id) == "test-build");
    CY_CHECK(std::string(reader.value()->manifest().bundle) == "base");

    const PackageEntry* entry = reader.value()->find(mesh, VariantKey::any());
    CY_REQUIRE(entry != nullptr);
    CY_CHECK(entry->kind == AssetKind::Mesh);
    CY_CHECK_EQ(entry->uncompressed_size, mesh_bytes.size());

    cy::Array<u8> payload;
    CY_REQUIRE(reader.value()->read_entry(*entry, payload, nullptr).has_value());
    CY_CHECK_EQ(payload.size(), mesh_bytes.size());
    CY_CHECK(std::memcmp(payload.data(), mesh_bytes.data(), mesh_bytes.size()) == 0);

    // An id the package does not hold is null rather than a wrong answer.
    CY_CHECK(reader.value()->find(mint_asset_id(), VariantKey::any()) == nullptr);
}

CY_TEST_CASE("Scenario: Platform variants") {
    // WHEN a texture is cooked for desktop (BC7) and mobile (ASTC)
    // THEN both variants SHALL be addressable by the same AssetId plus a variant key.
    const test::TempDir directory("package_variants");
    const std::string path = directory.file("variants.cypak");
    const cy::AssetId texture = mint_asset_id();
    const VariantKey desktop = VariantKey::parse("desktop-bc7").value();
    const VariantKey mobile = VariantKey::parse("mobile-astc").value();

    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        PackageWriter::EntryOptions options;
        options.kind = AssetKind::Texture;
        CY_REQUIRE(writer.add(texture, desktop, corpus(2048, 3).span(), options).has_value());
        CY_REQUIRE(writer.add(texture, mobile, corpus(2048, 4).span(), options).has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    auto reader = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE(reader.has_value());

    const PackageEntry* for_desktop = reader.value()->find(texture, desktop);
    const PackageEntry* for_mobile = reader.value()->find(texture, mobile);
    CY_REQUIRE(for_desktop != nullptr);
    CY_REQUIRE(for_mobile != nullptr);
    CY_CHECK(for_desktop->id == for_mobile->id);            // one asset
    CY_CHECK(for_desktop->variant != for_mobile->variant);  // two variants
    CY_CHECK(for_desktop->content != for_mobile->content);  // two payloads

    // A variant this package does not carry is not silently substituted: serving a mobile texture
    // to a desktop build would be worse than reporting it missing.
    CY_CHECK(reader.value()->find(texture, VariantKey::parse("console-bc7").value()) == nullptr);
}

CY_TEST_CASE("An asset with no variant is served to any variant request") {
    const test::TempDir directory("package_any_variant");
    const std::string path = directory.file("any.cypak");
    const cy::AssetId id = mint_asset_id();
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer.add(id, VariantKey::any(), corpus(64, 5).span(), {}).has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }
    auto reader = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE(reader.has_value());
    CY_CHECK(reader.value()->find(id, VariantKey::parse("desktop-bc7").value()) != nullptr);
}

CY_TEST_CASE("Scenario: Shared chunks deduplicate") {
    // WHEN identical content appears in two packages
    // THEN it SHALL be stored once as a shared content-addressed chunk.
    const test::TempDir directory("package_dedup");
    const cy::Array<u8> shared = corpus(20000, 6);
    const cy::AssetId first = mint_asset_id();
    const cy::AssetId second = mint_asset_id();
    const cy::AssetId patched = mint_asset_id();

    // Within one package: two entries, one chunk.
    const std::string base_path = directory.file("base.cypak");
    ContentHash shared_hash;
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer.add(first, VariantKey::any(), shared.span(), {}).has_value());
        CY_REQUIRE(writer.add(second, VariantKey::any(), shared.span(), {}).has_value());
        CY_CHECK_EQ(writer.entry_count(), 2u);
        CY_CHECK_EQ(writer.chunks_written(), 1u);  // stored once
        CY_CHECK_EQ(writer.bytes_deduplicated(), shared.size());
        CY_REQUIRE(writer.write(base_path.c_str()).has_value());
    }

    auto base = PackageReader::open(base_path.c_str(), options_at(1, 1));
    CY_REQUIRE(base.has_value());
    CY_CHECK_EQ(base.value()->chunk_count(), 1u);
    const PackageEntry* base_entry = base.value()->find(first, VariantKey::any());
    CY_REQUIRE(base_entry != nullptr);
    CY_CHECK(base.value()->find(second, VariantKey::any())->chunk == base_entry->chunk);
    shared_hash = base_entry->content;

    // Across two packages: the patch ships the reference, not the bytes.
    const std::string patch_path = directory.file("patch.cypak");
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer
                       .add_external(patched, VariantKey::any(), shared_hash, shared.size(),
                                     AssetKind::Binary)
                       .has_value());
        CY_REQUIRE(writer.write(patch_path.c_str()).has_value());
    }
    auto patch = PackageReader::open(patch_path.c_str(), options_at(1, 1));
    CY_REQUIRE(patch.has_value());
    CY_CHECK_EQ(patch.value()->chunk_count(), 0u);  // the patch carries no payload at all

    const PackageEntry* patch_entry = patch.value()->find(patched, VariantKey::any());
    CY_REQUIRE(patch_entry != nullptr);
    CY_CHECK(patch_entry->is_external());

    // On its own the patch cannot serve it, and says so rather than serving nothing.
    cy::Array<u8> alone;
    CY_CHECK_FALSE(patch.value()->read_entry(*patch_entry, alone, nullptr).has_value());

    // With the base mounted beside it, the chunk resolves.
    PackageSet set;
    CY_REQUIRE(set.add(*patch.value(), mount_priority::kPatchPackage).has_value());
    CY_REQUIRE(set.add(*base.value(), mount_priority::kBasePackage).has_value());
    CY_CHECK(set.holder_of(shared_hash) == base.value().get());

    cy::Array<u8> resolved;
    CY_REQUIRE(set.read_entry(*patch.value(), *patch_entry, resolved).has_value());
    CY_CHECK_EQ(resolved.size(), shared.size());
    CY_CHECK(std::memcmp(resolved.data(), shared.data(), shared.size()) == 0);
}

CY_TEST_CASE("Scenario: Memory-mapped read") {
    // WHEN an uncompressed entry is aligned and the platform supports mapping
    // THEN it SHALL be memory-mapped rather than copied into a buffer.
    const test::TempDir directory("package_mapping");
    const std::string path = directory.file("mapped.cypak");
    const cy::AssetId aligned_id = mint_asset_id();
    const cy::AssetId compressed_id = mint_asset_id();
    const cy::Array<u8> payload = corpus(12000, 7);

    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        PackageWriter::EntryOptions mappable;
        mappable.method = CompressionMethod::None;
        mappable.method_is_explicit = true;
        mappable.align_for_mapping = true;
        CY_REQUIRE(writer.add(aligned_id, VariantKey::any(), payload.span(), mappable).has_value());

        PackageWriter::EntryOptions dense;
        dense.method = CompressionMethod::Zstd;
        dense.method_is_explicit = true;
        CY_REQUIRE(writer.add(compressed_id, VariantKey::any(), corpus(12000, 8).span(), dense)
                       .has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    auto reader = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE(reader.has_value());
    const PackageEntry* entry = reader.value()->find(aligned_id, VariantKey::any());
    CY_REQUIRE(entry != nullptr);

    if (memory_mapping_available()) {
        auto view = reader.value()->map_entry(*entry);
        CY_REQUIRE(view.has_value());
        CY_CHECK_EQ(view.value().size(), payload.size());
        CY_CHECK(std::memcmp(view.value().data(), payload.data(), payload.size()) == 0);

        // Not copied: the view points inside the file's mapping, so no read was performed for it.
        CY_CHECK_EQ(reader.value()->bytes_read(), 0u);

        // A compressed entry is not mappable, and says which condition it failed.
        const PackageEntry* dense = reader.value()->find(compressed_id, VariantKey::any());
        CY_REQUIRE(dense != nullptr);
        const auto refused = reader.value()->map_entry(*dense);
        CY_REQUIRE_FALSE(refused.has_value());
        CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);
    } else {
        CY_TEST_MESSAGE("memory mapping is not available on this platform");
        CY_CHECK_FALSE(reader.value()->map_entry(*entry).has_value());
    }

    // Either way the copying path serves the same bytes, which is why mapping is an optimisation.
    cy::Array<u8> copied;
    CY_REQUIRE(reader.value()->read_entry(*entry, copied, nullptr).has_value());
    CY_CHECK(std::memcmp(copied.data(), payload.data(), payload.size()) == 0);
}

CY_TEST_CASE("Scenario: Partial read of a large asset, through the package") {
    // The same requirement as in test_compression.cpp, but end to end: the frames are on disk and
    // the count is of bytes the reader actually pulled out of the file.
    const test::TempDir directory("package_partial");
    const std::string path = directory.file("large.cypak");
    const cy::AssetId id = mint_asset_id();
    const usize frame_bytes = usize{64} * 1024;
    const cy::Array<u8> payload = corpus(frame_bytes * 12, 9);

    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        PackageWriter::EntryOptions options;
        options.kind = AssetKind::Texture;
        options.method = CompressionMethod::Zstd;
        options.method_is_explicit = true;
        options.frame_bytes = static_cast<u32>(frame_bytes);
        CY_REQUIRE(writer.add(id, VariantKey::any(), payload.span(), options).has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    // Opened WITHOUT the mapping, so that "bytes read" is bytes pulled through the file.
    PackageOpenOptions open_options = options_at(1, 1);
    open_options.map_file = false;
    auto reader = PackageReader::open(path.c_str(), open_options);
    CY_REQUIRE(reader.has_value());
    const PackageEntry* entry = reader.value()->find(id, VariantKey::any());
    CY_REQUIRE(entry != nullptr);

    cy::Array<u8> window;
    CY_REQUIRE(window.resize(2048).has_value());
    u64 bytes_from_disk = 0;
    u32 frames_touched = 0;
    const u64 offset = frame_bytes * 7;
    CY_REQUIRE(reader.value()
                   ->read_entry_range(*entry, offset, window.data(), window.size(),
                                      &bytes_from_disk, &frames_touched)
                   .has_value());
    CY_CHECK(std::memcmp(window.data(), payload.data() + offset, window.size()) == 0);
    CY_CHECK_EQ(frames_touched, 1u);

    // The whole entry, for comparison. The partial read cost a small fraction of it.
    cy::Array<u8> whole;
    u64 whole_bytes = 0;
    CY_REQUIRE(reader.value()->read_entry(*entry, whole, &whole_bytes).has_value());
    CY_CHECK(bytes_from_disk * 6 < whole_bytes);
}

CY_TEST_CASE("Scenario: Incompatible package is refused") {
    // WHEN a package's declared compatibility versions do not match the runtime
    // THEN mounting SHALL fail with a diagnostic rather than partially loading.
    const test::TempDir directory("package_incompatible");
    const std::string path = directory.file("future.cypak");
    {
        PackageWriter writer;
        // Produced at content version 7 and needing at least 5.
        CY_REQUIRE(writer.set_manifest(manifest_at(7, 5)).has_value());
        CY_REQUIRE(
            writer.add(mint_asset_id(), VariantKey::any(), corpus(128, 10).span(), {}).has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    // A runtime that only implements content version 3 refuses it.
    const auto refused = PackageReader::open(path.c_str(), options_at(3, 1));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);

    // A runtime too NEW for the package refuses it too, which is the other half of a version range.
    const auto too_old = PackageReader::open(path.c_str(), options_at(12, 9));
    CY_REQUIRE_FALSE(too_old.has_value());
    CY_CHECK(too_old.error().code == cy::ErrorCode::Unsupported);

    // A runtime inside the range opens it.
    CY_CHECK(PackageReader::open(path.c_str(), options_at(7, 5)).has_value());
}

CY_TEST_CASE("A file that is not a package is refused rather than misparsed") {
    const test::TempDir directory("package_not_a_package");

    // Long enough to have a header's worth of bytes, so the magic is what rejects it rather than
    // the length. Both refusals matter and both are checked.
    const std::string notes = directory.file("notes.txt");
    const std::string text(400, 'x');
    CY_REQUIRE(fs::write_atomic(notes.c_str(), text.data(), text.size()).has_value());
    const auto refused = PackageReader::open(notes.c_str(), options_at(1, 1));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::InvalidArgument);

    const std::string stub = directory.file("stub.cypak");
    CY_REQUIRE(fs::write_atomic(stub.c_str(), "short", 5).has_value());
    const auto too_short = PackageReader::open(stub.c_str(), options_at(1, 1));
    CY_REQUIRE_FALSE(too_short.has_value());
    CY_CHECK(too_short.error().code == cy::ErrorCode::Io);

    CY_CHECK_FALSE(
        PackageReader::open(directory.file("absent.cypak").c_str(), options_at(1, 1)).has_value());
}

CY_TEST_CASE("Scenario: Tampered package") {
    // WHEN an entry's payload does not match its recorded hash and verification is enabled
    // THEN the load SHALL fail with an integrity error.
    const test::TempDir directory("package_tampered");
    const std::string path = directory.file("tampered.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(4096, 11);
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        PackageWriter::EntryOptions options;
        options.method = CompressionMethod::None;
        options.method_is_explicit = true;
        CY_REQUIRE(writer.add(id, VariantKey::any(), payload.span(), options).has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    // Verification passes on the package as written.
    {
        PackageOpenOptions verified = options_at(1, 1);
        verified.verify_on_read = true;
        auto reader = PackageReader::open(path.c_str(), verified);
        CY_REQUIRE(reader.has_value());
        const PackageEntry* entry = reader.value()->find(id, VariantKey::any());
        CY_REQUIRE(entry != nullptr);
        CY_CHECK(reader.value()->verify_entry(*entry).has_value());
    }

    // Flip one byte of the payload region, leaving the directory and its recorded hash alone.
    {
        cy::Array<u8> file;
        CY_REQUIRE(fs::read_whole(path.c_str(), file).has_value());
        const usize victim = 200;  // inside the payload region, after the 112-byte header
        file[victim] = static_cast<u8>(file[victim] ^ 0xFF);
        CY_REQUIRE(fs::write_atomic(path.c_str(), file.data(), file.size()).has_value());
    }

    PackageOpenOptions verified = options_at(1, 1);
    verified.verify_on_read = true;
    auto reader = PackageReader::open(path.c_str(), verified);
    CY_REQUIRE(reader.has_value());
    const PackageEntry* entry = reader.value()->find(id, VariantKey::any());
    CY_REQUIRE(entry != nullptr);

    cy::Array<u8> read;
    const auto tampered = reader.value()->read_entry(*entry, read, nullptr);
    CY_REQUIRE_FALSE(tampered.has_value());
    CY_CHECK(tampered.error().code == cy::ErrorCode::Io);
    CY_CHECK_EQ(reader.value()->integrity_failures(), 1u);

    // Without verification the same read succeeds — which is exactly why the option exists, and
    // why a shipping build serving untrusted content turns it on.
    auto unverified = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE(unverified.has_value());
    cy::Array<u8> ignored;
    CY_CHECK(unverified.value()
                 ->read_entry(*unverified.value()->find(id, VariantKey::any()), ignored, nullptr)
                 .has_value());
}

CY_TEST_CASE("A truncated package is refused at open") {
    const test::TempDir directory("package_truncated");
    const std::string path = directory.file("short.cypak");
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer.add(mint_asset_id(), VariantKey::any(), corpus(4096, 12).span(), {})
                       .has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }
    cy::Array<u8> file;
    CY_REQUIRE(fs::read_whole(path.c_str(), file).has_value());
    CY_REQUIRE(file.resize(file.size() - 64).has_value());
    CY_REQUIRE(fs::write_atomic(path.c_str(), file.data(), file.size()).has_value());

    const auto refused = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Io);
}

CY_TEST_CASE("An entry declares its dependencies") {
    const test::TempDir directory("package_dependencies");
    const std::string path = directory.file("material.cypak");
    const cy::AssetId material = mint_asset_id();
    const cy::AssetId textures[3] = {mint_asset_id(), mint_asset_id(), mint_asset_id()};

    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        PackageWriter::EntryOptions options;
        options.kind = AssetKind::Material;
        CY_REQUIRE(writer
                       .add(material, VariantKey::any(), corpus(256, 13).span(), options,
                            cy::Span<const cy::AssetId>(textures, 3))
                       .has_value());
        for (const cy::AssetId texture : textures) {
            PackageWriter::EntryOptions texture_options;
            texture_options.kind = AssetKind::Texture;
            CY_REQUIRE(
                writer.add(texture, VariantKey::any(), corpus(512, 14).span(), texture_options)
                    .has_value());
        }
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    auto reader = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE(reader.has_value());
    const PackageEntry* entry = reader.value()->find(material, VariantKey::any());
    CY_REQUIRE(entry != nullptr);
    const cy::Span<const cy::AssetId> dependencies = reader.value()->dependencies(*entry);
    CY_REQUIRE_EQ(dependencies.size(), 3u);
    for (usize i = 0; i < 3; ++i) {
        CY_CHECK(dependencies[i] == textures[i]);
    }
}

CY_TEST_CASE("A package appears in the virtual filesystem, and a patch masks it") {
    // The package mount is what puts patch masking and mount priority on the SAME machinery as
    // every other mount, rather than giving packages a second resolution order.
    const test::TempDir directory("package_mount");
    const cy::AssetId replaced = mint_asset_id();
    const cy::AssetId removed = mint_asset_id();
    const std::string base_path = directory.file("base.cypak");
    const std::string patch_path = directory.file("patch.cypak");

    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer.add(replaced, VariantKey::any(), corpus(64, 15).span(), {}).has_value());
        CY_REQUIRE(writer.add(removed, VariantKey::any(), corpus(64, 16).span(), {}).has_value());
        CY_REQUIRE(writer.write(base_path.c_str()).has_value());
    }
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer.add(replaced, VariantKey::any(), corpus(64, 17).span(), {}).has_value());
        CY_REQUIRE(writer.mark_deleted(removed, VariantKey::any()).has_value());
        CY_REQUIRE(writer.write(patch_path.c_str()).has_value());
    }

    VirtualFileSystem files;
    for (const auto& entry :
         {std::pair<const std::string*, cy::i32>{&base_path, mount_priority::kBasePackage},
          std::pair<const std::string*, cy::i32>{&patch_path, mount_priority::kPatchPackage}}) {
        auto reader = PackageReader::open(entry.first->c_str(), options_at(1, 1));
        CY_REQUIRE(reader.has_value());
        auto mount =
            cy::make_unique<PackageMount>(cy::current_allocator(), std::move(reader.value()));
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), entry.second).has_value());
    }

    const auto replaced_path = package_entry_path(replaced, VariantKey::any());
    const auto removed_path = package_entry_path(removed, VariantKey::any());
    CY_REQUIRE(replaced_path.has_value());
    CY_REQUIRE(removed_path.has_value());

    cy::Array<u8> served;
    CY_REQUIRE(files.read(replaced_path.value(), served).has_value());
    const cy::Array<u8> patched = corpus(64, 17);
    CY_CHECK(std::memcmp(served.data(), patched.data(), patched.size()) == 0);

    // The patch's deleted-entry marker masks the base entirely.
    CY_CHECK_FALSE(files.exists(removed_path.value()));

    // The synthetic path is not a directory tree, and a path outside it belongs to nobody.
    CY_CHECK_FALSE(files.exists(path_of("packaged/not-an-id")));
}

CY_TEST_CASE("An encrypted package is refused rather than read as plaintext") {
    // Nothing writes an encrypted package — the flag is declared and the writer never sets it — so
    // the flag is set by hand here, which is exactly what a hostile or a future file would do.
    const test::TempDir directory("package_encrypted");
    const std::string path = directory.file("encrypted.cypak");
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(
            writer.add(mint_asset_id(), VariantKey::any(), corpus(64, 18).span(), {}).has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }
    cy::Array<u8> file;
    CY_REQUIRE(fs::read_whole(path.c_str(), file).has_value());
    file[12] = static_cast<u8>(file[12] | static_cast<u8>(PackageFlags::EncryptedPayload));
    CY_REQUIRE(fs::write_atomic(path.c_str(), file.data(), file.size()).has_value());

    const auto refused = PackageReader::open(path.c_str(), options_at(1, 1));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);
}

CY_TEST_CASE("The same inputs produce the same package bytes") {
    // The directory and the chunk table are sorted before anything is written, so the file does not
    // depend on the order entries were added in. A cook step whose output differed run to run would
    // defeat content addressing and every cache built on it.
    const test::TempDir directory("package_reproducible");
    const cy::AssetId a = mint_asset_id();
    const cy::AssetId b = mint_asset_id();
    const cy::Array<u8> first_bytes = corpus(3000, 19);
    const cy::Array<u8> second_bytes = corpus(3000, 20);

    const std::string forward = directory.file("forward.cypak");
    const std::string backward = directory.file("backward.cypak");
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer.add(a, VariantKey::any(), first_bytes.span(), {}).has_value());
        CY_REQUIRE(writer.add(b, VariantKey::any(), second_bytes.span(), {}).has_value());
        CY_REQUIRE(writer.write(forward.c_str()).has_value());
    }
    {
        PackageWriter writer;
        CY_REQUIRE(writer.set_manifest(manifest_at(1, 1)).has_value());
        CY_REQUIRE(writer.add(b, VariantKey::any(), second_bytes.span(), {}).has_value());
        CY_REQUIRE(writer.add(a, VariantKey::any(), first_bytes.span(), {}).has_value());
        CY_REQUIRE(writer.write(backward.c_str()).has_value());
    }

    cy::Array<u8> one;
    cy::Array<u8> two;
    CY_REQUIRE(fs::read_whole(forward.c_str(), one).has_value());
    CY_REQUIRE(fs::read_whole(backward.c_str(), two).has_value());
    CY_REQUIRE_EQ(one.size(), two.size());
    CY_CHECK(std::memcmp(one.data(), two.data(), one.size()) == 0);
}
