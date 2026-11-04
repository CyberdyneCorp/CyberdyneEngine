// SPDX-License-Identifier: MIT
// Verifying and materialising a packaged game. See installation.h.

#include <cy/ship/installation.h>

#include <chrono>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace cy::ship {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string digest_text(const assets::ContentHash& digest) {
    char text[assets::ContentHash::kTextLength + 1];
    digest.format(text);
    return text;
}

/// The signature, the trusted key and the manifest's parse: the gate every other check sits
/// behind. False, with the one problem recorded, when the manifest is not to be read further.
[[nodiscard]] bool check_signature(const Location& location, const PublicKey& trusted,
                                   Verification& out) {
    const fs::path manifest_path = location.root / fs::path(location.manifest);
    const std::string signature_name = location.manifest + kSignatureSuffix;
    std::string text;
    std::string signature_text;
    if (!read_file(manifest_path, text)) {
        out.problems.push_back({ProblemKind::Unreadable, location.manifest, "it cannot be read"});
        return false;
    }
    if (!read_file(location.root / fs::path(signature_name), signature_text)) {
        out.problems.push_back({ProblemKind::BadSignature, signature_name,
                                "it is missing, so nothing vouches for the manifest"});
        return false;
    }
    std::string key;
    Signature signature;
    std::string error;
    if (!parse_signature_file(signature_text, key, signature.bytes, sizeof(signature.bytes),
                              error)) {
        out.problems.push_back({ProblemKind::BadSignature, signature_name, error});
        return false;
    }
    if (key != key_id(trusted)) {
        out.problems.push_back(
            {ProblemKind::UntrustedKey, signature_name,
             "it is signed by key " + key + " and this build trusts " + key_id(trusted)});
        return false;
    }
    if (!verify(trusted, text, signature)) {
        out.problems.push_back({ProblemKind::BadSignature, location.manifest,
                                "its signature does not hold: the manifest was changed after it "
                                "was signed"});
        return false;
    }
    out.bytes_hashed += text.size();
    out.manifest_digest = digest_text(assets::content_hash(text.data(), text.size()));
    if (!parse_manifest(text, out.manifest, error)) {
        out.problems.push_back({ProblemKind::Unreadable, location.manifest, error});
        return false;
    }
    return true;
}

/// One listed file: present, and the size and digest the manifest says.
void check_file(const Location& location, const ManifestFile& file, Verification& out) {
    const fs::path path = location.root / fs::path(file.path);
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        out.problems.push_back({ProblemKind::Missing, file.path, "it is not there"});
        return;
    }
    if (file.kind == FileKind::Sealed) {
        return;
    }
    assets::ContentHash digest;
    u64 bytes = 0;
    if (!digest_file(path, digest, bytes)) {
        out.problems.push_back({ProblemKind::Missing, file.path, "it cannot be read"});
        return;
    }
    out.bytes_hashed += bytes;
    if (bytes != file.bytes) {
        out.problems.push_back({ProblemKind::Modified, file.path,
                                "it is " + std::to_string(bytes) + " bytes and the manifest says " +
                                    std::to_string(file.bytes)});
    } else if (digest != file.digest) {
        out.problems.push_back({ProblemKind::Modified, file.path,
                                "its BLAKE3 digest is not the one the signed manifest records"});
    }
}

/// Every file on disk that the manifest does not list, by name.
void check_unlisted(const Location& location, Verification& out) {
    std::set<std::string> listed;
    for (const ManifestFile& file : out.manifest.files) {
        listed.insert(file.path);
    }
    std::error_code error;
    for (fs::recursive_directory_iterator it(location.root, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_directory(error)) {
            continue;
        }
        const std::string relative = it->path().lexically_relative(location.root).generic_string();
        if (!listed.contains(relative) && !exempt_from_listing(relative, location.manifest)) {
            out.problems.push_back(
                {ProblemKind::Unlisted, relative, "the signed manifest does not list it"});
        }
    }
}

/// The content stream read across chunk boundaries.
class ContentStream {
public:
    ContentStream(const Location& location, const Manifest& manifest)
        : location_(location), chunks_(manifest.chunks()) {}

    [[nodiscard]] bool read(u64 offset, u64 size, std::string& out, std::string& error) const {
        out.clear();
        out.reserve(static_cast<usize>(size));
        u64 start = 0;
        for (const ManifestFile* chunk : chunks_) {
            const u64 end = start + chunk->bytes;
            if (size > 0 && offset < end && offset + size > start) {
                const u64 from = offset > start ? offset - start : 0;
                const u64 until = (offset + size < end ? offset + size : end) - start;
                if (!append(*chunk, from, until - from, out, error)) {
                    return false;
                }
            }
            start = end;
        }
        if (out.size() != size) {
            error = "the content stream ends before the entry does";
            return false;
        }
        return true;
    }

private:
    [[nodiscard]] bool append(const ManifestFile& chunk, u64 from, u64 count, std::string& out,
                              std::string& error) const {
        std::ifstream file(location_.root / fs::path(chunk.path), std::ios::binary);
        if (!file) {
            error = chunk.path + " cannot be read";
            return false;
        }
        file.seekg(static_cast<std::streamoff>(from));
        const usize at = out.size();
        out.resize(at + static_cast<usize>(count));
        file.read(out.data() + at, static_cast<std::streamsize>(count));
        if (std::cmp_not_equal(file.gcount(), count)) {
            error = chunk.path + " is shorter than the manifest says";
            return false;
        }
        return true;
    }

    const Location& location_;
    std::vector<const ManifestFile*> chunks_;
};

[[nodiscard]] bool write_atomically(const fs::path& target, const std::string& bytes,
                                    std::string& error) {
    std::error_code code;
    fs::create_directories(target.parent_path(), code);
    fs::path temporary = target;
    temporary += ".partial";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            error = "cannot write " + temporary.string();
            return false;
        }
    }
    fs::rename(temporary, target, code);
    if (code) {
        error = "cannot move " + temporary.string() + " into place: " + code.message();
        return false;
    }
    return true;
}

}  // namespace

std::string Problem::describe() const {
    static constexpr const char* kVerbs[] = {"cannot be used",
                                             "is not signed by this build's key",
                                             "is signed by another key",
                                             "is missing",
                                             "was modified",
                                             "is not listed"};
    return path + " " + kVerbs[static_cast<usize>(kind)] + ": " + detail;
}

bool locate_installation(const fs::path& executable, Location& out) {
    const fs::path root = executable.parent_path().parent_path();
    for (const char* candidate : {kManifestName, "Resources/ship.manifest"}) {
        std::error_code error;
        if (fs::is_regular_file(root / fs::path(candidate), error)) {
            out.root = root;
            out.manifest = candidate;
            return true;
        }
    }
    return false;
}

Verification verify_installation(const Location& location, const PublicKey& trusted) {
    const auto started = std::chrono::steady_clock::now();
    Verification out;
    if (check_signature(location, trusted, out)) {
        for (const ManifestFile& file : out.manifest.files) {
            check_file(location, file, out);
            ++(file.kind == FileKind::Chunk ? out.chunks : out.files);
        }
        check_unlisted(location, out);
    }
    out.milliseconds =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    return out;
}

bool read_content(const Location& location, const Manifest& manifest, std::string_view name,
                  std::string& bytes, std::string& error) {
    const ContentEntry* entry = manifest.find_content(name);
    if (entry == nullptr) {
        error = "the manifest has no content entry " + std::string(name);
        return false;
    }
    if (!ContentStream(location, manifest).read(entry->offset, entry->bytes, bytes, error)) {
        return false;
    }
    if (assets::content_hash(bytes.data(), bytes.size()) != entry->digest) {
        error = "content entry " + entry->name + " does not match its digest";
        return false;
    }
    return true;
}

bool materialise_content(const Location& location, const Manifest& manifest, const fs::path& cache,
                         fs::path& out, std::string& error) {
    out = cache;
    std::string bytes;
    for (const ContentEntry& entry : manifest.content) {
        const fs::path target = out / fs::path(entry.name);
        assets::ContentHash digest;
        u64 size = 0;
        if (digest_file(target, digest, size) && size == entry.bytes && digest == entry.digest) {
            continue;
        }
        if (!read_content(location, manifest, entry.name, bytes, error) ||
            !write_atomically(target, bytes, error)) {
            return false;
        }
    }
    return true;
}

bool read_file(const fs::path& path, std::string& out) {
    // Sized, then read whole: GCC 13's -Wnull-dereference misreads istreambuf_iterator's buffer.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<usize>(size));
    file.seekg(0);
    file.read(out.data(), size);
    return static_cast<std::streamoff>(file.gcount()) == size;
}

bool digest_file(const fs::path& path, assets::ContentHash& digest, u64& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    assets::ContentHasher hasher;
    std::string block(1U << 20U, '\0');
    bytes = 0;
    while (file) {
        file.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto count = static_cast<usize>(file.gcount());
        hasher.update(block.data(), count);
        bytes += count;
    }
    if (file.bad()) {
        return false;
    }
    digest = hasher.finish();
    return true;
}

bool exempt_from_listing(std::string_view relative, std::string_view manifest) noexcept {
    if (relative == manifest) {
        return true;
    }
    if (relative.size() == manifest.size() + 4 && relative.starts_with(manifest) &&
        relative.substr(manifest.size()) == kSignatureSuffix) {
        return true;
    }
    const usize slash = relative.rfind('/');
    const std::string_view leaf =
        slash == std::string_view::npos ? relative : relative.substr(slash + 1);
    return relative.starts_with("_CodeSignature/") || leaf == ".DS_Store";
}

}  // namespace cy::ship
