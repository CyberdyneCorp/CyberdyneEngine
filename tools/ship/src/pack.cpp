// SPDX-License-Identifier: MIT
// Chunks, manifest and signature over a staged installation. See pack.h.

#include <cy/ship/installation.h>
#include <cy/ship/pack.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <utility>

namespace cy::ship {

namespace fs = std::filesystem;

namespace {

/// Every regular file under `directory`, as sorted `/`-separated relative paths. False, naming it,
/// when the tree holds a symbolic link: an archive keeps a link and a zip file does not, so a
/// shipped link is a file that exists on one target and not another.
[[nodiscard]] bool list_files(const fs::path& directory, std::vector<std::string>& out,
                              std::string& error) {
    std::error_code code;
    for (fs::recursive_directory_iterator it(directory, code), end; !code && it != end;
         it.increment(code)) {
        const std::string relative = it->path().lexically_relative(directory).generic_string();
        if (it->is_symlink(code)) {
            error = relative + " is a symbolic link; a shipped artefact holds files";
            return false;
        }
        if (it->is_regular_file(code)) {
            out.push_back(relative);
        }
    }
    if (code) {
        error = "cannot list " + directory.string() + ": " + code.message();
        return false;
    }
    std::ranges::sort(out);
    return true;
}

/// Writes the content stream as chunk files of at most `limit` bytes each.
class ChunkWriter {
public:
    ChunkWriter(fs::path root, u64 limit) : root_(std::move(root)), limit_(limit) {}

    [[nodiscard]] bool write(const char* data, u64 size, std::string& error) {
        while (size > 0) {
            if (!file_.is_open() && !open(error)) {
                return false;
            }
            const u64 room = limit_ - current_;
            const u64 count = size < room ? size : room;
            file_.write(data, static_cast<std::streamsize>(count));
            hasher_.update(data, static_cast<usize>(count));
            current_ += count;
            data += count;
            size -= count;
            if (current_ == limit_ && !close(error)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool close(std::string& error) {
        if (!file_.is_open()) {
            return true;
        }
        file_.close();
        if (!file_) {
            error = "cannot write " + paths_.back();
            return false;
        }
        ManifestFile chunk;
        chunk.kind = FileKind::Chunk;
        chunk.path = paths_.back();
        chunk.bytes = current_;
        chunk.digest = hasher_.finish();
        chunks_.push_back(std::move(chunk));
        return true;
    }

    [[nodiscard]] std::vector<ManifestFile>& chunks() noexcept { return chunks_; }

private:
    [[nodiscard]] bool open(std::string& error) {
        char name[48];
        std::snprintf(name, sizeof(name), "content/chunk-%04zu.cychunk", chunks_.size());
        std::error_code code;
        fs::create_directories(root_ / "content", code);
        file_.open(root_ / fs::path(name), std::ios::binary | std::ios::trunc);
        if (!file_) {
            error = std::string("cannot create ") + name;
            return false;
        }
        paths_.emplace_back(name);
        hasher_.reset();
        current_ = 0;
        return true;
    }

    fs::path root_;
    u64 limit_;
    u64 current_ = 0;
    std::ofstream file_;
    assets::ContentHasher hasher_;
    std::vector<std::string> paths_;
    std::vector<ManifestFile> chunks_;
};

/// The content directory streamed into chunks, each file an entry.
[[nodiscard]] bool write_content(const PackOptions& options, Manifest& out, std::string& error) {
    std::vector<std::string> names;
    if (!list_files(options.content, names, error)) {
        return false;
    }
    ChunkWriter writer(options.root, options.chunk_bytes);
    for (const std::string& name : names) {
        std::string bytes;
        if (!valid_relative_path(name)) {
            error = name + " is not a name a manifest can carry";
            return false;
        }
        if (!read_file(options.content / fs::path(name), bytes)) {
            error = "cannot read content " + name;
            return false;
        }
        ContentEntry entry;
        entry.name = name;
        entry.offset = out.content_bytes;
        entry.bytes = bytes.size();
        entry.digest = assets::content_hash(bytes.data(), bytes.size());
        if (!writer.write(bytes.data(), bytes.size(), error)) {
            return false;
        }
        out.content_bytes += bytes.size();
        out.content.push_back(std::move(entry));
    }
    if (!writer.close(error)) {
        return false;
    }
    out.files = std::move(writer.chunks());
    return true;
}

/// Every other file under the root, by digest — or as sealed, when the options say so.
[[nodiscard]] bool list_root(const PackOptions& options, Manifest& out, std::string& error) {
    std::vector<std::string> paths;
    if (!list_files(options.root, paths, error)) {
        return false;
    }
    for (const std::string& path : paths) {
        const bool chunk = std::ranges::any_of(
            out.files, [&](const ManifestFile& file) { return file.path == path; });
        if (chunk || exempt_from_listing(path, options.manifest)) {
            continue;
        }
        ManifestFile file;
        file.path = path;
        if (std::ranges::find(options.sealed, path) != options.sealed.end()) {
            file.kind = FileKind::Sealed;
        } else if (!digest_file(options.root / fs::path(path), file.digest, file.bytes)) {
            error = "cannot read " + path;
            return false;
        }
        out.files.push_back(std::move(file));
    }
    return true;
}

[[nodiscard]] bool write_text(const fs::path& path, const std::string& text, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        error = "cannot write " + path.string();
        return false;
    }
    return true;
}

}  // namespace

bool pack(const PackOptions& options, Manifest& out, std::string& error) {
    if (options.chunk_bytes == 0) {
        error = "chunk-bytes must be positive";
        return false;
    }
    out = options.header;
    out.format = kManifestFormat;
    out.chunk_bytes = options.chunk_bytes;
    out.content_bytes = 0;
    out.files.clear();
    out.content.clear();
    const PublicKey key = public_key(options.key);
    out.signer = key_id(key);
    if (!write_content(options, out, error) || !list_root(options, out, error)) {
        return false;
    }
    const std::string text = write_manifest(out);
    Manifest check;
    if (!parse_manifest(text, check, error)) {
        error = "the manifest written does not read back: " + error;
        return false;
    }
    const Signature signature = sign(options.key, text);
    const fs::path manifest = options.root / fs::path(options.manifest);
    std::error_code code;
    fs::create_directories(manifest.parent_path(), code);
    fs::path signature_path = manifest;
    signature_path += kSignatureSuffix;
    return write_text(manifest, text, error) &&
           write_text(signature_path,
                      write_signature_file(out.signer, signature.bytes, sizeof(signature.bytes)),
                      error);
}

}  // namespace cy::ship
