// SPDX-License-Identifier: MIT
// integration.ship_manifest — the signed shipping manifest, its startup check, and a game's launch.
// Issue #87.
//
// Every refusal case builds a real installation with `pack`, damages it the way a player's disk, a
// half-finished download or a person would, and requires the check to NAME what is wrong. A check
// that answered "corrupt" would pass half of these and help nobody.

#include <cy/ship/installation.h>
#include <cy/ship/launch.h>
#include <cy/ship/pack.h>
#include <cy/ship/settings.h>
#include <cy/ship/signature.h>
#include <cy/test/test.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace cy::ship;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

/// A directory under the suite's working directory that removes itself.
class Scratch {
public:
    explicit Scratch(const char* label) {
        static std::atomic<unsigned> serial{0};
        path_ = fs::current_path() /
                ("cy_ship_test_" + std::string(label) + "_" + std::to_string(serial.fetch_add(1)));
        std::error_code code;
        fs::remove_all(path_, code);
        fs::create_directories(path_, code);
        CY_REQUIRE(!code);
    }
    ~Scratch() {
        std::error_code code;
        fs::remove_all(path_, code);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& path, const std::string& bytes) {
    std::error_code code;
    fs::create_directories(path.parent_path(), code);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CY_REQUIRE(static_cast<bool>(file));
}

[[nodiscard]] std::string read_file(const fs::path& path) {
    std::string bytes;
    CY_REQUIRE(cy::ship::read_file(path, bytes));
    return bytes;
}

/// Bytes that differ at every position, so that a chunk cut in the wrong place cannot read back.
[[nodiscard]] std::string pattern(usize size, u8 seed) {
    std::string bytes(size, '\0');
    for (usize index = 0; index < size; ++index) {
        bytes[index] = static_cast<char>(((index * 131U) + seed) & 0xFFU);
    }
    return bytes;
}

/// A staged game: an executable, a library, a document, and three content files that span four
/// chunks of 1000 bytes — the second file crosses two chunk boundaries.
struct Fixture {
    Scratch scratch;
    fs::path root;
    fs::path content;
    Manifest manifest;

    explicit Fixture(const char* label) : scratch(label) {
        root = scratch.path() / "game";
        content = scratch.path() / "content-staging";
        write_file(root / "bin" / "game", pattern(700, 1));
        write_file(root / "lib" / "libCyGame_g0.so", pattern(300, 2));
        write_file(root / "README.txt", "how to play\n");
        write_file(content / "map.cypak", pattern(900, 3));
        write_file(content / "audio" / "wind.cyaudio", pattern(2200, 4));
        write_file(content / "module.toml", "[module]\nname = \"game\"\n");
    }

    void pack_with(const SecretKey& key) {
        PackOptions options;
        options.root = root;
        options.content = content;
        options.header.game = "test";
        options.header.title = "The Test Game";
        options.header.version = "1.2.3";
        options.header.revision = "0123456789abcdef";
        options.header.target = "linux-x86_64";
        options.header.profile = "dev";
        options.chunk_bytes = 1000;
        options.key = key;
        std::string error;
        CY_REQUIRE_MESSAGE(pack(options, manifest, error), error);
    }

    [[nodiscard]] Location location() const { return Location{root, kManifestName}; }

    [[nodiscard]] Verification verify_with(const PublicKey& key) const {
        return verify_installation(location(), key);
    }

    [[nodiscard]] Verification verify() const {
        return verify_with(public_key(development_secret_key()));
    }
};

[[nodiscard]] bool names(const Verification& verification, ProblemKind kind,
                         const std::string& path) {
    return std::ranges::any_of(verification.problems, [&](const Problem& problem) {
        return problem.kind == kind && problem.path == path;
    });
}

[[nodiscard]] SecretKey key_from(const char* hex) {
    SecretKey key;
    CY_REQUIRE(parse_hex(hex, key.seed, sizeof(key.seed)));
    return key;
}

}  // namespace

CY_TEST_CASE("Ed25519 signatures match RFC 8032's test vectors") {
    // RFC 8032 §7.1, TEST 1 (the empty message) and TEST 2 (one byte, 0x72).
    struct Vector {
        const char* secret;
        const char* published;
        std::string message;
        const char* signature;
    };
    const Vector vectors[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e3970"
         "1cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "r",  // 0x72
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613"
         "d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    };
    for (const Vector& vector : vectors) {
        const SecretKey secret = key_from(vector.secret);
        const PublicKey key = public_key(secret);
        CY_CHECK_EQ(to_hex(key.bytes, sizeof(key.bytes)), std::string(vector.published));
        const Signature signature = sign(secret, vector.message);
        CY_CHECK_EQ(to_hex(signature.bytes, sizeof(signature.bytes)),
                    std::string(vector.signature));
        CY_CHECK(verify(key, vector.message, signature));
    }
}

CY_TEST_CASE("a signature holds only for its own bytes and its own key") {
    const SecretKey secret = development_secret_key();
    const PublicKey key = public_key(secret);
    const Signature signature = sign(secret, "cyship 1\ngame rts\n");
    CY_CHECK(verify(key, "cyship 1\ngame rts\n", signature));
    CY_CHECK_FALSE(verify(key, "cyship 1\ngame rtS\n", signature));
    Signature flipped = signature;
    flipped.bytes[17] ^= 0x01U;
    CY_CHECK_FALSE(verify(key, "cyship 1\ngame rts\n", flipped));
    const PublicKey other =
        public_key(key_from("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"));
    CY_CHECK_FALSE(verify(other, "cyship 1\ngame rts\n", signature));
}

CY_TEST_CASE(
    "a manifest round-trips through its text, and a path outside the artefact is refused") {
    Manifest manifest;
    manifest.game = "rts";
    manifest.title = "CyberdyneEngine RTS";
    manifest.version = "0.1.0";
    manifest.revision = "abc";
    manifest.target = "linux-x86_64";
    manifest.profile = "release";
    manifest.signer = "0011223344556677";
    manifest.chunk_bytes = 10;
    manifest.content_bytes = 10;
    ManifestFile chunk;
    chunk.kind = FileKind::Chunk;
    chunk.path = "content/chunk-0000.cychunk";
    chunk.bytes = 10;
    manifest.files.push_back(chunk);
    ManifestFile spaced;
    spaced.path = "docs/Read Me.txt";
    spaced.bytes = 3;
    manifest.files.push_back(spaced);
    ManifestFile sealed;
    sealed.kind = FileKind::Sealed;
    sealed.path = "MacOS/cyberdyne-rts";
    manifest.files.push_back(sealed);
    manifest.content.push_back({"frontier.cypak", 0, 10, {}});
    const std::string text = write_manifest(manifest);
    Manifest read;
    std::string error;
    CY_REQUIRE_MESSAGE(parse_manifest(text, read, error), error);
    CY_CHECK_EQ(write_manifest(read), text);
    CY_CHECK_EQ(read.files[1].path, std::string("docs/Read Me.txt"));
    CY_CHECK(read.files[2].kind == FileKind::Sealed);

    for (const char* climbing : {"../escape", "/etc/passwd", "a/../../b", "C:/x", "a\\b", "a//b"}) {
        CY_CHECK_FALSE(valid_relative_path(climbing));
        std::string bad = text;
        bad.replace(bad.find("docs/Read Me.txt"), std::strlen("docs/Read Me.txt"), climbing);
        Manifest refused;
        CY_CHECK_FALSE(parse_manifest(bad, refused, error));
    }
    std::string twice = text;
    twice += "sealed docs/Read Me.txt\n";
    CY_CHECK_FALSE(parse_manifest(twice, read, error));
    CY_CHECK(error.find("listed twice") != std::string::npos);
    std::string short_stream = text;
    short_stream.replace(short_stream.find("content-bytes 10"), 16, "content-bytes 11");
    Manifest refused;
    CY_CHECK_FALSE(parse_manifest(short_stream, refused, error));
}

CY_TEST_CASE("the manifest lists every file of the installation") {
    Fixture game("lists");
    game.pack_with(development_secret_key());
    std::vector<std::string> on_disk;
    for (const auto& entry : fs::recursive_directory_iterator(game.root)) {
        if (entry.is_regular_file()) {
            on_disk.push_back(entry.path().lexically_relative(game.root).generic_string());
        }
    }
    std::vector<std::string> listed = {kManifestName, std::string(kManifestName) + ".sig"};
    for (const ManifestFile& file : game.manifest.files) {
        listed.push_back(file.path);
    }
    std::ranges::sort(on_disk);
    std::ranges::sort(listed);
    CY_CHECK_EQ(listed, on_disk);
    // 3123 content bytes (900 + 2200 + 23) in 1000-byte chunks are four chunks.
    CY_CHECK_EQ(game.manifest.chunks().size(), 4U);
    CY_CHECK_EQ(game.manifest.content_bytes, 3123U);
    CY_CHECK_EQ(game.manifest.content.size(), 3U);
    const Verification verification = game.verify();
    CY_CHECK(verification.ok());
    CY_CHECK_EQ(verification.files, 3U);
    CY_CHECK_EQ(verification.chunks, 4U);
}

CY_TEST_CASE("a bundle's chunks go where its code signature seals them, and nowhere outside it") {
    Fixture game("bundle");
    PackOptions options;
    options.root = game.root;
    options.content = game.content;
    options.manifest = "Resources/ship.manifest";
    options.chunk_directory = "Resources/content";
    options.header.game = "test";
    options.chunk_bytes = 1000;
    options.key = development_secret_key();
    options.sealed = {"bin/game"};
    Manifest manifest;
    std::string error;
    CY_REQUIRE_MESSAGE(pack(options, manifest, error), error);
    CY_CHECK(fs::exists(game.root / "Resources" / "content" / "chunk-0003.cychunk"));
    CY_CHECK_FALSE(fs::exists(game.root / "content"));
    CY_CHECK(verify_installation(Location{game.root, "Resources/ship.manifest"},
                                 public_key(development_secret_key()))
                 .ok());
    options.chunk_directory = "../outside";
    CY_CHECK_FALSE(pack(options, manifest, error));
}

CY_TEST_CASE("every content entry reads back out of the chunks, across their boundaries") {
    Fixture game("reads");
    game.pack_with(development_secret_key());
    for (const char* name : {"map.cypak", "audio/wind.cyaudio", "module.toml"}) {
        std::string bytes;
        std::string error;
        CY_REQUIRE_MESSAGE(read_content(game.location(), game.manifest, name, bytes, error), error);
        CY_CHECK_EQ(bytes, read_file(game.content / name));
    }
}

CY_TEST_CASE("a tampered chunk is refused, and the refusal names the chunk") {
    Fixture game("tampered");
    game.pack_with(development_secret_key());
    const fs::path chunk = game.root / "content" / "chunk-0002.cychunk";
    std::string bytes = read_file(chunk);
    bytes[500] = static_cast<char>(bytes[500] ^ 0x40);
    write_file(chunk, bytes);
    const Verification verification = game.verify();
    CY_CHECK_FALSE(verification.ok());
    CY_REQUIRE_EQ(verification.problems.size(), 1U);
    CY_CHECK(names(verification, ProblemKind::Modified, "content/chunk-0002.cychunk"));
    // REQUIRE does not leave the case in this build (no exceptions), so the index is guarded.
    CY_CHECK((!verification.problems.empty() &&
              verification.problems[0].describe().find("content/chunk-0002.cychunk was modified") !=
                  std::string::npos));
}

CY_TEST_CASE("a chunk cut short is refused by its size") {
    Fixture game("short");
    game.pack_with(development_secret_key());
    const fs::path chunk = game.root / "content" / "chunk-0000.cychunk";
    write_file(chunk, read_file(chunk).substr(0, 999));
    const Verification verification = game.verify();
    CY_REQUIRE(names(verification, ProblemKind::Modified, "content/chunk-0000.cychunk"));
    CY_CHECK((!verification.problems.empty() &&
              verification.problems[0].detail.find("999 bytes") != std::string::npos));
}

CY_TEST_CASE("a missing file is reported by name, and so is every other one") {
    Fixture game("missing");
    game.pack_with(development_secret_key());
    fs::remove(game.root / "lib" / "libCyGame_g0.so");
    fs::remove(game.root / "content" / "chunk-0003.cychunk");
    const Verification verification = game.verify();
    CY_CHECK_EQ(verification.problems.size(), 2U);
    CY_CHECK(names(verification, ProblemKind::Missing, "lib/libCyGame_g0.so"));
    CY_CHECK(names(verification, ProblemKind::Missing, "content/chunk-0003.cychunk"));
}

CY_TEST_CASE("a file the manifest does not list is refused by name") {
    Fixture game("unlisted");
    game.pack_with(development_secret_key());
    write_file(game.root / "lib" / "libinjected.so", "not ours");
    const Verification verification = game.verify();
    CY_CHECK_EQ(verification.problems.size(), 1U);
    CY_CHECK(names(verification, ProblemKind::Unlisted, "lib/libinjected.so"));
    // A macOS bundle's code signature and Finder's litter are not the game's files.
    fs::remove(game.root / "lib" / "libinjected.so");
    write_file(game.root / "_CodeSignature" / "CodeResources", "sealed");
    write_file(game.root / "lib" / ".DS_Store", "finder");
    CY_CHECK(game.verify().ok());
}

CY_TEST_CASE("a manifest changed after signing is refused before any file is read") {
    Fixture game("resigned");
    game.pack_with(development_secret_key());
    const fs::path manifest = game.root / kManifestName;
    std::string text = read_file(manifest);
    text.replace(text.find("version 1.2.3"), 13, "version 9.9.9");
    write_file(manifest, text);
    fs::remove(game.root / "README.txt");
    const Verification verification = game.verify();
    CY_CHECK_EQ(verification.problems.size(), 1U);
    CY_CHECK(names(verification, ProblemKind::BadSignature, kManifestName));

    fs::remove(game.root / (std::string(kManifestName) + ".sig"));
    CY_CHECK(names(game.verify(), ProblemKind::BadSignature, std::string(kManifestName) + ".sig"));
}

CY_TEST_CASE("an installation signed by a key this build does not trust is refused") {
    Fixture game("untrusted");
    const SecretKey stranger =
        key_from("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
    game.pack_with(stranger);
    const Verification refused = game.verify();
    CY_CHECK(names(refused, ProblemKind::UntrustedKey, std::string(kManifestName) + ".sig"));
    CY_CHECK(game.verify_with(public_key(stranger)).ok());
}

CY_TEST_CASE("materialised content is verified, and a modified cached file is rewritten") {
    Fixture game("materialise");
    game.pack_with(development_secret_key());
    const fs::path cache = game.scratch.path() / "cache";
    fs::path out;
    std::string error;
    CY_REQUIRE_MESSAGE(materialise_content(game.location(), game.manifest, cache, out, error),
                       error);
    CY_CHECK_EQ(read_file(out / "audio" / "wind.cyaudio"),
                read_file(game.content / "audio" / "wind.cyaudio"));
    // The same size, one byte different: a cached file is trusted by its digest, never its size.
    std::string edited = read_file(out / "map.cypak");
    edited[10] = static_cast<char>(edited[10] ^ 0x01);
    write_file(out / "map.cypak", edited);
    CY_REQUIRE(materialise_content(game.location(), game.manifest, cache, out, error));
    CY_CHECK_EQ(read_file(out / "map.cypak"), read_file(game.content / "map.cypak"));
}

namespace {

constexpr Setting kSettings[] = {
    {"vsync", "on", SettingKind::Switch, nullptr, "--no-vsync", "Wait for the display."},
    {"window", "1280x720", SettingKind::Size, "--window", nullptr, "The window's size."},
    {"fullscreen", "off", SettingKind::Switch, "--fullscreen", nullptr, "Borderless."},
};
constexpr const char* kDefaults[] = {"--play"};

[[nodiscard]] LaunchSpec spec() {
    LaunchSpec spec{"test", "TestGame"};
    spec.settings = kSettings;
    spec.setting_count = 3;
    spec.settings_when = "--play";
    spec.default_arguments = kDefaults;
    spec.default_argument_count = 1;
    return spec;
}

[[nodiscard]] std::vector<std::string> arguments_of(Launch& launch) {
    return {launch.argv(), launch.argv() + launch.argc()};
}

}  // namespace

CY_TEST_CASE("a launch out of a build tree changes nothing") {
    Scratch scratch("tree");
    char program[] = "game";
    char flag[] = "--battle";
    char* argv[] = {program, flag, nullptr};
    Launch launch;
    CY_REQUIRE(start_at(scratch.path() / "samples" / "game", 2, argv, spec(), nullptr, launch));
    CY_CHECK_FALSE(launch.packaged());
    CY_CHECK_EQ(arguments_of(launch), (std::vector<std::string>{"game", "--battle"}));
    CY_CHECK_EQ(std::string(launch.library("/build/libCyGame_g0.so", "libCyGame_g0.so")),
                std::string("/build/libCyGame_g0.so"));
    CY_CHECK_EQ(std::string(launch.content("/build/map.cypak", "map.cypak")),
                std::string("/build/map.cypak"));
}

CY_TEST_CASE("a packaged launch verifies, materialises, and writes and applies the settings") {
    Fixture game("launch");
    game.pack_with(development_secret_key());
    const LaunchDirectories directories{game.scratch.path() / "user" / "data",
                                        game.scratch.path() / "user" / "config",
                                        game.scratch.path() / "user" / "cache"};
    char program[] = "game";
    char* argv[] = {program, nullptr};
    Launch launch;
    CY_REQUIRE(start_at(game.root / "bin" / "game", 1, argv, spec(), &directories, launch));
    CY_CHECK(launch.packaged());
    // No arguments: the defaults, with the settings file's translation ahead of them.
    CY_CHECK_EQ(arguments_of(launch),
                (std::vector<std::string>{"game", "--window", "1280x720", "--play"}));
    CY_CHECK(fs::exists(directories.config / "settings.cfg"));
    CY_CHECK_EQ(read_file(launch.content("/nowhere", "map.cypak")),
                read_file(game.content / "map.cypak"));
    CY_CHECK_EQ(fs::path(launch.library("/nowhere", "libCyGame_g0.so")),
                game.root / "lib" / "libCyGame_g0.so");

    // An edited settings file is applied, a typo in it is not a refusal, and the player's own
    // arguments come after it so that a flag typed on the command line wins.
    write_file(directories.config / "settings.cfg",
               "vsync = off\nwindow = huge\nfullscreen = on\nnonsense = 1\n");
    char play[] = "--play";
    char window[] = "--window";
    char size[] = "640x360";
    char* played[] = {program, play, window, size, nullptr};
    Launch again;
    CY_REQUIRE(start_at(game.root / "bin" / "game", 4, played, spec(), &directories, again));
    CY_CHECK_EQ(arguments_of(again), (std::vector<std::string>{"game", "--no-vsync", "--fullscreen",
                                                               "--play", "--window", "640x360"}));
}

CY_TEST_CASE("a packaged launch from a tampered installation does not start") {
    Fixture game("refused");
    game.pack_with(development_secret_key());
    write_file(game.root / "content" / "chunk-0001.cychunk", pattern(1000, 9));
    const LaunchDirectories directories{game.scratch.path() / "d", game.scratch.path() / "c",
                                        game.scratch.path() / "k"};
    char program[] = "game";
    char* argv[] = {program, nullptr};
    Launch launch;
    CY_CHECK_FALSE(start_at(game.root / "bin" / "game", 1, argv, spec(), &directories, launch));
    CY_CHECK(names(launch.verification(), ProblemKind::Modified, "content/chunk-0001.cychunk"));
    CY_CHECK_FALSE(fs::exists(directories.cache));

    // Content intact, and a library planted beside the executable: still refused.
    Fixture planted("planted");
    planted.pack_with(development_secret_key());
    write_file(planted.root / "bin" / "libplanted.so", "not ours");
    Launch again;
    CY_CHECK_FALSE(start_at(planted.root / "bin" / "game", 1, argv, spec(), &directories, again));
    CY_CHECK(names(again.verification(), ProblemKind::Unlisted, "bin/libplanted.so"));
}

CY_TEST_CASE("settings: a value the game cannot take is a warning, never a refusal") {
    Scratch scratch("settings");
    Settings settings;
    std::string error;
    CY_REQUIRE(load_settings(scratch.path() / "settings.cfg", kSettings, 3, settings, error));
    CY_CHECK(settings.created);
    write_file(scratch.path() / "settings.cfg", "vsync = sometimes\nwindow = 0x720\nnot a line\n");
    CY_REQUIRE(load_settings(scratch.path() / "settings.cfg", kSettings, 3, settings, error));
    std::vector<std::string> flags;
    settings_arguments(kSettings, 3, settings, flags);
    CY_CHECK(flags.empty());
    CY_CHECK_EQ(settings.warnings.size(), 3U);
    CY_CHECK(valid_setting(SettingKind::Size, "1920x1080"));
    CY_CHECK_FALSE(valid_setting(SettingKind::Size, "1920x"));
    CY_CHECK_FALSE(valid_setting(SettingKind::Count, "-3"));
}
