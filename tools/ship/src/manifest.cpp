// SPDX-License-Identifier: MIT
// The shipping manifest's text form. See manifest.h.

#include <cy/ship/manifest.h>
#include <cy/ship/signature.h>

#include <charconv>
#include <cstdio>
#include <set>

namespace cy::ship {

namespace {

[[nodiscard]] std::string digest_text(const assets::ContentHash& digest) {
    char text[assets::ContentHash::kTextLength + 1];
    digest.format(text);
    return text;
}

/// The next space-separated field of `rest`, consumed. Empty when there is none.
[[nodiscard]] std::string_view take_field(std::string_view& rest) noexcept {
    const usize space = rest.find(' ');
    const std::string_view field = rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    return field;
}

[[nodiscard]] bool parse_u64(std::string_view text, u64& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] bool parse_digest(std::string_view text, assets::ContentHash& out) noexcept {
    const auto parsed = assets::ContentHash::parse(text);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

/// One manifest line being read: its number, for the diagnostic, and what is left of it.
struct Line {
    usize number = 0;
    std::string_view rest;
};

class Reader {
public:
    Reader(Manifest& out, std::string& error) noexcept : out_(out), error_(error) {}

    [[nodiscard]] bool line(Line line) {
        line_ = line.number;
        const std::string_view keyword = take_field(line.rest);
        if (keyword == "file" || keyword == "chunk") {
            return sized(keyword == "file" ? FileKind::File : FileKind::Chunk, line.rest);
        }
        if (keyword == "sealed") {
            return sealed(line.rest);
        }
        if (keyword == "entry") {
            return entry(line.rest);
        }
        return header(keyword, line.rest);
    }

    [[nodiscard]] bool fail(const std::string& why) {
        error_ = "line " + std::to_string(line_) + ": " + why;
        return false;
    }

private:
    [[nodiscard]] bool header(std::string_view keyword, std::string_view value) {
        struct Text {
            const char* name;
            std::string* field;
        };
        const Text texts[] = {{"game", &out_.game},       {"title", &out_.title},
                              {"version", &out_.version}, {"revision", &out_.revision},
                              {"target", &out_.target},   {"profile", &out_.profile},
                              {"signer", &out_.signer}};
        for (const Text& text : texts) {
            if (keyword == text.name) {
                *text.field = std::string(value);
                return true;
            }
        }
        if (keyword == "chunk-bytes") {
            return parse_u64(value, out_.chunk_bytes) && out_.chunk_bytes > 0
                       ? true
                       : fail("chunk-bytes is not a positive number");
        }
        if (keyword == "content-bytes") {
            return parse_u64(value, out_.content_bytes) ? true
                                                        : fail("content-bytes is not a number");
        }
        return fail("unknown keyword '" + std::string(keyword) + "'");
    }

    [[nodiscard]] bool path(std::string_view text) {
        return valid_relative_path(text)
                   ? true
                   : fail("'" + std::string(text) + "' is not a relative path inside the artefact");
    }

    [[nodiscard]] bool sized(FileKind kind, std::string_view rest) {
        ManifestFile file;
        file.kind = kind;
        if (!parse_u64(take_field(rest), file.bytes) ||
            !parse_digest(take_field(rest), file.digest)) {
            return fail("a file is `<bytes> <blake3> <path>`");
        }
        file.path = std::string(rest);
        if (!path(rest)) {
            return false;
        }
        out_.files.push_back(std::move(file));
        return true;
    }

    [[nodiscard]] bool sealed(std::string_view rest) {
        if (!path(rest)) {
            return false;
        }
        ManifestFile file;
        file.kind = FileKind::Sealed;
        file.path = std::string(rest);
        out_.files.push_back(std::move(file));
        return true;
    }

    [[nodiscard]] bool entry(std::string_view rest) {
        ContentEntry entry;
        if (!parse_u64(take_field(rest), entry.offset) ||
            !parse_u64(take_field(rest), entry.bytes) ||
            !parse_digest(take_field(rest), entry.digest)) {
            return fail("an entry is `<offset> <bytes> <blake3> <name>`");
        }
        entry.name = std::string(rest);
        if (!path(rest)) {
            return false;
        }
        out_.content.push_back(std::move(entry));
        return true;
    }

    Manifest& out_;
    std::string& error_;
    usize line_ = 0;
};

/// The chunks tile the content stream exactly, every entry lies inside it, and no path repeats.
[[nodiscard]] bool consistent(const Manifest& manifest, std::string& error) {
    u64 covered = 0;
    for (const ManifestFile* chunk : manifest.chunks()) {
        if (chunk->bytes == 0 || chunk->bytes > manifest.chunk_bytes) {
            error = chunk->path + " is not between one byte and chunk-bytes long";
            return false;
        }
        covered += chunk->bytes;
    }
    if (covered != manifest.content_bytes) {
        error = "the chunks hold " + std::to_string(covered) + " bytes and the content stream is " +
                std::to_string(manifest.content_bytes);
        return false;
    }
    for (const ContentEntry& entry : manifest.content) {
        if (entry.offset > manifest.content_bytes ||
            entry.bytes > manifest.content_bytes - entry.offset) {
            error = "content entry " + entry.name + " lies outside the content stream";
            return false;
        }
    }
    std::set<std::string_view> paths;
    for (const ManifestFile& file : manifest.files) {
        if (!paths.insert(file.path).second) {
            error = file.path + " is listed twice";
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<const ManifestFile*> Manifest::chunks() const {
    std::vector<const ManifestFile*> result;
    for (const ManifestFile& file : files) {
        if (file.kind == FileKind::Chunk) {
            result.push_back(&file);
        }
    }
    return result;
}

const ContentEntry* Manifest::find_content(std::string_view name) const noexcept {
    for (const ContentEntry& entry : content) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::string write_manifest(const Manifest& manifest) {
    std::string text = "cyship " + std::to_string(manifest.format) + "\n";
    const std::pair<const char*, const std::string*> headers[] = {
        {"game", &manifest.game},       {"title", &manifest.title},
        {"version", &manifest.version}, {"revision", &manifest.revision},
        {"target", &manifest.target},   {"profile", &manifest.profile},
        {"signer", &manifest.signer}};
    for (const auto& [name, value] : headers) {
        text += std::string(name) + " " + *value + "\n";
    }
    text += "chunk-bytes " + std::to_string(manifest.chunk_bytes) + "\n";
    text += "content-bytes " + std::to_string(manifest.content_bytes) + "\n";
    for (const ManifestFile& file : manifest.files) {
        if (file.kind == FileKind::Sealed) {
            text += "sealed " + file.path + "\n";
            continue;
        }
        text += (file.kind == FileKind::Chunk ? "chunk " : "file ") + std::to_string(file.bytes) +
                " " + digest_text(file.digest) + " " + file.path + "\n";
    }
    for (const ContentEntry& entry : manifest.content) {
        text += "entry " + std::to_string(entry.offset) + " " + std::to_string(entry.bytes) + " " +
                digest_text(entry.digest) + " " + entry.name + "\n";
    }
    return text;
}

bool parse_manifest(std::string_view text, Manifest& out, std::string& error) {
    out = Manifest{};
    const std::string expected = "cyship " + std::to_string(kManifestFormat);
    Reader reader(out, error);
    usize number = 0;
    while (!text.empty()) {
        const usize newline = text.find('\n');
        if (newline == std::string_view::npos) {
            return reader.fail("the manifest does not end with a newline");
        }
        const std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline + 1);
        ++number;
        if (number == 1) {
            if (line != expected) {
                error = "not a format " + std::to_string(kManifestFormat) + " shipping manifest";
                return false;
            }
            continue;
        }
        if (!reader.line(Line{number, line})) {
            return false;
        }
    }
    if (number == 0) {
        error = "the manifest is empty";
        return false;
    }
    return consistent(out, error);
}

bool valid_relative_path(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos || path.find('\n') != std::string_view::npos) {
        return false;
    }
    while (!path.empty()) {
        const usize slash = path.find('/');
        const std::string_view component = path.substr(0, slash);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (slash != std::string_view::npos && path.empty()) {
            return false;  // a trailing slash names a directory, not a file
        }
    }
    return true;
}

std::string write_signature_file(std::string_view key_id, const u8* signature, usize size) {
    return "ed25519 " + std::string(key_id) + " " + to_hex(signature, size) + "\n";
}

bool parse_signature_file(std::string_view text, std::string& key_id, u8* signature, usize size,
                          std::string& error) {
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    if (take_field(text) != "ed25519") {
        error = "the signature file does not hold an ed25519 signature";
        return false;
    }
    key_id = std::string(take_field(text));
    if (key_id.size() != 16 || !parse_hex(text, signature, size)) {
        error = "the signature file is not `ed25519 <key id> <signature>`";
        return false;
    }
    return true;
}

}  // namespace cy::ship
