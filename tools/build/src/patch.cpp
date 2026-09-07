#include <cy/build/patch.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>

#include <unistd.h>

#include <algorithm>
#include <ranges>
#include <unordered_map>

#include "text.h"

namespace cy::build {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

[[nodiscard]] Allocator& patch_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

[[nodiscard]] std::string hash_text(const assets::ContentHash& hash) {
    char text[assets::ContentHash::kTextLength + 1] = {};
    hash.format(text);
    return {static_cast<const char*>(text)};
}

void append_number(std::string& out, u64 value) {
    char buffer[24] = {};
    usize length = 0;
    if (value == 0) {
        buffer[length++] = '0';
    }
    while (value != 0) {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    for (usize index = length; index > 0; --index) {
        out.push_back(buffer[index - 1]);
    }
}

[[nodiscard]] Expected<u64, Error> parse_number(std::string_view word) {
    u64 value = 0;
    if (word.empty()) {
        return make_unexpected(invalid("expected a number"));
    }
    for (const char character : word) {
        if (character < '0' || character > '9') {
            return make_unexpected(invalid("expected a number"));
        }
        value = (value * 10) + static_cast<u64>(character - '0');
    }
    return value;
}

/// The exit status a hard interruption leaves. Distinct from every status the tool produces on
/// purpose, so a test can tell "killed at the requested stage" from "failed on its own".
constexpr int kHardInterruptStatus = 97;

[[nodiscard]] bool interrupts_at(const PatchInterrupt& interrupt, PatchStage stage) noexcept {
    return interrupt.stage == stage;
}

/// Stop here. `_exit` rather than `exit`: no destructor runs, no stream is flushed, no temporary is
/// removed. That is the point — a test that let the process tidy up would prove nothing about a
/// power loss.
[[noreturn]] void hard_stop() {
    _exit(kHardInterruptStatus);
}

}  // namespace

const char* patch_stage_name(PatchStage stage) noexcept {
    switch (stage) {
        case PatchStage::Begin:
            return "begin";
        case PatchStage::Fetch:
            return "fetch";
        case PatchStage::Verify:
            return "verify";
        case PatchStage::Commit:
            return "commit";
        case PatchStage::Switch:
            return "switch";
        case PatchStage::Complete:
            return "complete";
    }
    return "unknown";
}

Expected<PatchStage, Error> patch_stage_from_name(std::string_view name) noexcept {
    const PatchStage stages[] = {PatchStage::Begin,  PatchStage::Verify, PatchStage::Fetch,
                                 PatchStage::Commit, PatchStage::Switch, PatchStage::Complete};
    for (const PatchStage stage : stages) {
        if (name == patch_stage_name(stage)) {
            return stage;
        }
    }
    return make_unexpected(invalid("not a patch stage"));
}

u64 PatchManifest::transferred_bytes() const noexcept {
    u64 total = 0;
    for (const PatchChunk& chunk : added) {
        total += chunk.size;
    }
    return total;
}

PatchManifest diff(const PackageSet& from, const PackageSet& to) {
    PatchManifest patch;
    patch.produces = to.build_id;
    patch.applies_to.push_back(from.build_id);
    patch.target = to;

    // Keyed by CONTENT, not by name. A chunk that moved from one bundle to another, or that is
    // reachable under a second name, is already installed and must not be transferred again — which
    // is the difference between a chunk-level patch and a file-level one.
    std::unordered_map<std::string, const PackageEntry*> present;
    for (const Bundle& bundle : from.bundles) {
        for (const PackageEntry& entry : bundle.entries) {
            present.emplace(hash_text(entry.digest), &entry);
        }
    }
    std::unordered_map<std::string, const PackageEntry*> wanted;
    for (const Bundle& bundle : to.bundles) {
        for (const PackageEntry& entry : bundle.entries) {
            wanted.emplace(hash_text(entry.digest), &entry);
            if (!present.contains(hash_text(entry.digest))) {
                patch.added.push_back(
                    PatchChunk{entry.name, entry.digest, entry.size, bundle.name});
            }
        }
    }
    for (const Bundle& bundle : from.bundles) {
        for (const PackageEntry& entry : bundle.entries) {
            if (!wanted.contains(hash_text(entry.digest))) {
                patch.removed.push_back(
                    PatchChunk{entry.name, entry.digest, entry.size, bundle.name});
            }
        }
    }

    const auto by_name = [](const PatchChunk& a, const PatchChunk& b) {
        return a.name != b.name ? a.name < b.name : a.digest < b.digest;
    };
    std::ranges::sort(patch.added, by_name);
    std::ranges::sort(patch.removed, by_name);
    return patch;
}

namespace {

void write_chunks(std::string& out, const char* keyword, const std::vector<PatchChunk>& chunks) {
    for (const PatchChunk& chunk : chunks) {
        out += "  ";
        out += keyword;
        out += ' ';
        out += text::quote(chunk.name);
        out += ' ';
        out += hash_text(chunk.digest);
        out += ' ';
        append_number(out, chunk.size);
        out += ' ';
        out += text::quote(chunk.bundle);
        out += '\n';
    }
}

/// Re-emit one line of the embedded target manifest, for `read_package` to parse.
///
/// Re-serialised rather than parsed in place: `read_package` owns that format, and two readers of
/// one format is how they come to disagree about a file name inside a shipped patch.
///
/// Two details are load-bearing. The indent is reproduced one level SHALLOWER, because the package
/// format distinguishes a provenance field from a bundle by depth. And every word is quoted: the
/// reader strips quotes, so quoting a keyword or a digest is harmless, whereas guessing which words
/// were quoted originally is not.
/// One of the patch's own top-level records. Separated from the loop so that the loop reads as the
/// three states it has — header, target, chunk — rather than as five nested conditions.
void read_header_line(const text::Line& line, PatchManifest& patch) {
    const std::string_view keyword = line.word(0);
    if (keyword == "produces") {
        patch.produces = std::string(line.word(1));
    } else if (keyword == "applies-to") {
        patch.applies_to.emplace_back(line.word(1));
    }
}

void append_target_line(const text::Line& line, std::string& target) {
    for (u32 indent = 1; indent < line.depth; ++indent) {
        target += "  ";
    }
    for (const std::string& word : line.words) {
        target += text::quote(word);
        target += ' ';
    }
    target += '\n';
}

[[nodiscard]] Expected<PatchChunk, Error> read_chunk_line(const text::Line& line) {
    if (line.words.size() < 5) {
        return make_unexpected(
            invalid("a patch chunk needs a name, a digest, a size and a bundle"));
    }
    const Expected<assets::ContentHash, Error> digest = assets::ContentHash::parse(line.word(2));
    if (!digest) {
        return make_unexpected(digest.error());
    }
    const Expected<u64, Error> size = parse_number(line.word(3));
    if (!size) {
        return make_unexpected(size.error());
    }
    return PatchChunk{std::string(line.word(1)), *digest, *size, std::string(line.word(4))};
}

}  // namespace

std::string write_patch(const PatchManifest& patch) {
    std::string out = "cypatch 1\n";
    out += "produces ";
    out += patch.produces;
    out += '\n';
    for (const std::string& build : patch.applies_to) {
        out += "applies-to ";
        out += build;
        out += '\n';
    }
    out += "chunks\n";
    write_chunks(out, "add", patch.added);
    write_chunks(out, "remove", patch.removed);
    out += "target\n";
    // The target manifest, indented by two so that it cannot be confused with this document's own
    // records. Carried inline because a patch that needed a second file to be fetched alongside it
    // would have two things to interrupt instead of one.
    const std::string target = write_package(patch.target);
    usize start = 0;
    while (start < target.size()) {
        const usize newline = std::min(target.find('\n', start), target.size());
        out += "  ";
        out.append(target, start, newline - start);
        out += '\n';
        start = newline + 1;
    }
    return out;
}

Expected<PatchManifest, Error> read_patch(std::string_view document) {
    const Expected<std::vector<text::Line>, Error> lines = text::read(document);
    if (!lines) {
        return make_unexpected(lines.error());
    }
    if (lines->empty() || (*lines)[0].word(0) != "cypatch" || (*lines)[0].word(1) != "1") {
        return make_unexpected(invalid("not a cypatch 1 document"));
    }

    PatchManifest patch;
    std::string target;
    bool in_target = false;
    for (usize index = 1; index < lines->size(); ++index) {
        const text::Line& line = (*lines)[index];
        const std::string_view keyword = line.word(0);
        if (line.depth == 0) {
            in_target = keyword == "target";
            read_header_line(line, patch);
            continue;
        }
        if (in_target) {
            append_target_line(line, target);
            continue;
        }
        if (keyword != "add" && keyword != "remove") {
            continue;
        }
        const Expected<PatchChunk, Error> chunk = read_chunk_line(line);
        if (!chunk) {
            return make_unexpected(chunk.error());
        }
        (keyword == "add" ? patch.added : patch.removed).push_back(*chunk);
    }

    if (!target.empty()) {
        const Expected<PackageSet, Error> parsed = read_package(target);
        if (!parsed) {
            return make_unexpected(parsed.error());
        }
        patch.target = *parsed;
    }
    return patch;
}

// --- Installation --------------------------------------------------------------------------------

std::string Installation::staging_root() const {
    return root_ + "/staging";
}
std::string Installation::current_path() const {
    return root_ + "/current";
}

std::string Installation::manifest_path(std::string_view build_id) const {
    std::string path = root_;
    path += "/manifests/";
    path.append(build_id);
    path += ".cypackage";
    return path;
}

Status Installation::open(std::string root) {
    root_ = std::move(root);
    if (Status made = assets::fs::create_directories((root_ + "/manifests").c_str()); !made) {
        return made;
    }
    if (Status pointed = chunks_.configure(root_ + "/chunks"); !pointed) {
        return pointed;
    }
    // Anything an interrupted application left is unreferenced content. Discarded here, so that a
    // caller never has to remember to — the same contract `fs::discard_temporaries` has for saves.
    return rollback();
}

Status Installation::rollback() {
    const std::string staging = staging_root();
    if (!assets::fs::exists(staging.c_str())) {
        return ok();
    }
    return assets::fs::remove_directory_recursive(staging.c_str());
}

Expected<std::string, Error> Installation::current_build() const {
    Array<u8> bytes(patch_allocator());
    if (Status read = assets::fs::read_whole(current_path().c_str(), bytes); !read) {
        return make_unexpected(Error{ErrorCode::NotFound, "nothing is installed", 0});
    }
    std::string build(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!build.empty() && (build.back() == '\n' || build.back() == '\r')) {
        build.pop_back();
    }
    return build;
}

Expected<PackageSet, Error> Installation::current_package() const {
    const Expected<std::string, Error> build = current_build();
    if (!build) {
        return make_unexpected(build.error());
    }
    Array<u8> bytes(patch_allocator());
    if (Status read = assets::fs::read_whole(manifest_path(*build).c_str(), bytes); !read) {
        return make_unexpected(read.error());
    }
    return read_package(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

Status Installation::install(const PackageSet& packages, const ArtefactStore& source) {
    Array<u8> bytes(patch_allocator());
    for (const PackageEntry& entry : packages.entries()) {
        if (Status read = source.get(entry.digest, bytes); !read) {
            return read;
        }
        if (!chunks_.put(bytes.data(), bytes.size())) {
            return make_unexpected(Error{ErrorCode::Io, "a chunk could not be installed", 0});
        }
    }
    const std::string manifest = write_package(packages);
    if (Status written = assets::fs::write_atomic(manifest_path(packages.build_id).c_str(),
                                                  manifest.data(), manifest.size());
        !written) {
        return written;
    }
    return assets::fs::write_atomic(current_path().c_str(), packages.build_id.data(),
                                    packages.build_id.size());
}

Status Installation::verify() const {
    const Expected<PackageSet, Error> packages = current_package();
    if (!packages) {
        return make_unexpected(packages.error());
    }
    for (const PackageEntry& entry : packages->entries()) {
        if (Status checked = chunks_.verify(entry.digest); !checked) {
            return checked;
        }
    }
    return ok();
}

Status Installation::read(std::string_view name, Array<u8>& out) const {
    const Expected<PackageSet, Error> packages = current_package();
    if (!packages) {
        return make_unexpected(packages.error());
    }
    for (const PackageEntry& entry : packages->entries()) {
        if (entry.name == name) {
            return chunks_.get(entry.digest, out);
        }
    }
    return make_unexpected(Error{ErrorCode::NotFound, "the build in force has no such chunk", 0});
}

namespace {

/// Where a staged chunk waits. Named by its digest, so a fetch that is interrupted halfway through
/// and repeated writes the same name rather than accumulating.
[[nodiscard]] std::string staged_path(const std::string& staging,
                                      const assets::ContentHash& digest) {
    return staging + "/" + hash_text(digest) + ".cyart";
}

}  // namespace

Status StoreChunkSource::fetch(const assets::ContentHash& digest, Array<u8>& out) const {
    // Deliberately a raw read: the patcher verifies, and a source that verified here would make the
    // patcher's own check unreachable.
    return assets::fs::read_whole(store_->path_of(digest).c_str(), out);
}

Expected<PatchResult, Error> Installation::apply(const PatchManifest& patch,
                                                 const ChunkSource& source,
                                                 PatchInterrupt interrupt) {
    PatchResult result;
    result.reached = PatchStage::Begin;

    // Applicability, before a byte moves. "WHEN a patch is offered THEN its manifest SHALL state
    // which builds it applies to" — and a statement nothing checks is a comment.
    const Expected<std::string, Error> installed = current_build();
    if (!installed) {
        return make_unexpected(installed.error());
    }
    if (std::ranges::find(patch.applies_to, *installed) == patch.applies_to.end()) {
        return make_unexpected(invalid("this patch does not apply to the installed build"));
    }
    if (interrupts_at(interrupt, PatchStage::Begin)) {
        if (interrupt.hard) {
            hard_stop();
        }
        return result;
    }

    const std::string staging = staging_root();
    if (Status made = assets::fs::create_directories(staging.c_str()); !made) {
        return make_unexpected(made.error());
    }

    // --- Fetch ----------------------------------------------------------------------------------
    result.reached = PatchStage::Fetch;
    Array<u8> bytes(patch_allocator());
    for (const PatchChunk& chunk : patch.added) {
        if (chunks_.contains(chunk.digest)) {
            continue;  // already installed under another name: content addressing, doing its job
        }
        if (Status read = source.fetch(chunk.digest, bytes); !read) {
            // Reported rather than raised: the caller needs the chunk's name.
            result.failed_chunk = chunk.name;
            const Status rolled = rollback();
            (void)rolled;
            return result;
        }
        if (Status written = assets::fs::write_atomic(staged_path(staging, chunk.digest).c_str(),
                                                      bytes.data(), bytes.size());
            !written) {
            const Status rolled = rollback();
            (void)rolled;
            return make_unexpected(written.error());
        }
        ++result.fetched_chunks;
        result.fetched_bytes += bytes.size();
        if (interrupts_at(interrupt, PatchStage::Fetch)) {
            if (interrupt.hard) {
                hard_stop();
            }
            const Status rolled = rollback();
            (void)rolled;
            return result;
        }
    }

    // --- Verify ---------------------------------------------------------------------------------
    result.reached = PatchStage::Verify;
    if (interrupts_at(interrupt, PatchStage::Verify)) {
        if (interrupt.hard) {
            hard_stop();
        }
        const Status rolled = rollback();
        (void)rolled;
        return result;
    }
    for (const PatchChunk& chunk : patch.added) {
        const std::string path = staged_path(staging, chunk.digest);
        if (!assets::fs::exists(path.c_str())) {
            continue;  // skipped above because the installation already holds it
        }
        if (Status read = assets::fs::read_whole(path.c_str(), bytes); !read) {
            result.failed_chunk = chunk.name;
            const Status rolled = rollback();
            (void)rolled;
            return result;
        }
        if (assets::content_hash(bytes.data(), bytes.size()) != chunk.digest) {
            // "Verification failure SHALL abort the patch and report which chunk failed." The
            // installation is untouched: nothing has been committed and `current` still names the
            // build in force.
            result.failed_chunk = chunk.name;
            const Status rolled = rollback();
            (void)rolled;
            return result;
        }
    }

    // --- Commit ---------------------------------------------------------------------------------
    //
    // Staged chunks move into the content-addressed store. This cannot damage the build in force:
    // different content has a different name, so every write here is additive.
    result.reached = PatchStage::Commit;
    for (const PatchChunk& chunk : patch.added) {
        const std::string path = staged_path(staging, chunk.digest);
        if (!assets::fs::exists(path.c_str())) {
            continue;
        }
        if (Status read = assets::fs::read_whole(path.c_str(), bytes); !read) {
            const Status rolled = rollback();
            (void)rolled;
            return make_unexpected(read.error());
        }
        if (!chunks_.put(bytes.data(), bytes.size())) {
            const Status rolled = rollback();
            (void)rolled;
            return make_unexpected(Error{ErrorCode::Io, "a chunk could not be committed", 0});
        }
        if (interrupts_at(interrupt, PatchStage::Commit)) {
            if (interrupt.hard) {
                hard_stop();
            }
            const Status rolled = rollback();
            (void)rolled;
            return result;
        }
    }

    const std::string manifest = write_package(patch.target);
    if (Status written = assets::fs::write_atomic(manifest_path(patch.produces).c_str(),
                                                  manifest.data(), manifest.size());
        !written) {
        const Status rolled = rollback();
        (void)rolled;
        return make_unexpected(written.error());
    }

    // --- Switch ---------------------------------------------------------------------------------
    //
    // One atomic write. Everything before this point is unreferenced content and a manifest nothing
    // points at; the installation in force is whatever `current` says, and until this line it says
    // the old build.
    result.reached = PatchStage::Switch;
    if (interrupts_at(interrupt, PatchStage::Switch)) {
        if (interrupt.hard) {
            hard_stop();
        }
        const Status rolled = rollback();
        (void)rolled;
        return result;
    }
    if (Status switched = assets::fs::write_atomic(current_path().c_str(), patch.produces.data(),
                                                   patch.produces.size());
        !switched) {
        const Status rolled = rollback();
        (void)rolled;
        return make_unexpected(switched.error());
    }

    const Status rolled = rollback();
    (void)rolled;
    result.reached = PatchStage::Complete;
    result.applied = true;
    return result;
}

}  // namespace cy::build
