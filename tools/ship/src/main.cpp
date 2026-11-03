// SPDX-License-Identifier: MIT
// cy_ship — pack, verify and sign a packaged game's installation. Issue #87.
//
//   cy_ship pack --root <dir> --content <dir> --game <id> --title <text> --version <semver>
//                --revision <sha> --target <os-arch> --profile <p> [--manifest <path>]
//                [--chunk-bytes <n>] [--sealed <path>]... [--key <secret file>]
//   cy_ship verify --root <dir> [--manifest <path>]       the startup check, by itself
//   cy_ship read --root <dir> --entry <name> --out <file> one content entry out of the chunks
//   cy_ship keygen --out <secret file>                    a new key; prints its public half
//   cy_ship public-key [--key <secret file>]              the public key a secret implies
//   cy_ship trusted                                       the key this build trusts
//
// `--key` absent, or `--key development`, signs with the published development key (signature.h).
// The environment variable CY_SHIP_SIGNING_KEY, holding the secret's 64 hex digits, is read when
// `--key` is absent, which is how continuous integration hands a release key over without writing
// it to disk. Exit 0 on success, 2 on a usage error, 3 when an installation does not verify.

#include <cy/ship/installation.h>
#include <cy/ship/pack.h>
#include <cy/ship/signature.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cy::ship;

constexpr int kUsage = 2;
constexpr int kRefused = 3;

/// `--name value` pairs and repeated `--sealed` values.
struct Arguments {
    std::vector<std::pair<std::string, std::string>> named;
    std::vector<std::string> sealed;

    [[nodiscard]] const char* get(std::string_view name) const {
        for (const auto& [key, value] : named) {
            if (key == name) {
                return value.c_str();
            }
        }
        return nullptr;
    }
};

[[nodiscard]] bool parse_arguments(int argc, char** argv, Arguments& out) {
    for (int index = 2; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (!flag.starts_with("--") || index + 1 >= argc) {
            std::fprintf(stderr, "cy_ship: '%s' is not `--name value`\n", argv[index]);
            return false;
        }
        const std::string name(flag.substr(2));
        const char* value = argv[++index];
        if (name == "sealed") {
            out.sealed.emplace_back(value);
        } else {
            out.named.emplace_back(name, value);
        }
    }
    return true;
}

[[nodiscard]] bool read_secret(const char* path, SecretKey& key) {
    std::string text;
    if (path == nullptr || std::strcmp(path, "development") == 0) {
        const char* environment = path == nullptr ? std::getenv("CY_SHIP_SIGNING_KEY") : nullptr;
        if (environment == nullptr || *environment == '\0') {
            key = development_secret_key();
            return true;
        }
        text = environment;
    } else {
        if (!read_file(path, text)) {
            std::fprintf(stderr, "cy_ship: cannot read the secret key %s\n", path);
            return false;
        }
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (!parse_hex(text, key.seed, sizeof(key.seed))) {
        std::fprintf(stderr, "cy_ship: the secret key is not 64 hex digits\n");
        return false;
    }
    return true;
}

[[nodiscard]] bool required(const Arguments& arguments, std::initializer_list<const char*> names) {
    return std::ranges::all_of(names, [&](const char* name) {
        if (arguments.get(name) != nullptr) {
            return true;
        }
        std::fprintf(stderr, "cy_ship: --%s is required\n", name);
        return false;
    });
}

[[nodiscard]] Location location_of(const Arguments& arguments) {
    Location location;
    location.root = arguments.get("root");
    const char* manifest = arguments.get("manifest");
    location.manifest = manifest != nullptr ? manifest : kManifestName;
    return location;
}

int command_pack(const Arguments& arguments) {
    if (!required(arguments, {"root", "content", "game", "title", "version", "revision", "target",
                              "profile"})) {
        return kUsage;
    }
    PackOptions options;
    options.root = arguments.get("root");
    options.content = arguments.get("content");
    if (const char* manifest = arguments.get("manifest"); manifest != nullptr) {
        options.manifest = manifest;
    }
    if (const char* bytes = arguments.get("chunk-bytes"); bytes != nullptr) {
        options.chunk_bytes = std::strtoull(bytes, nullptr, 10);
    }
    options.header.game = arguments.get("game");
    options.header.title = arguments.get("title");
    options.header.version = arguments.get("version");
    options.header.revision = arguments.get("revision");
    options.header.target = arguments.get("target");
    options.header.profile = arguments.get("profile");
    options.sealed = arguments.sealed;
    if (!read_secret(arguments.get("key"), options.key)) {
        return kUsage;
    }
    Manifest manifest;
    std::string error;
    if (!pack(options, manifest, error)) {
        std::fprintf(stderr, "cy_ship pack: %s\n", error.c_str());
        return 1;
    }
    std::printf("cy_ship pack files=%zu chunks=%zu entries=%zu content_bytes=%llu signer=%s%s\n",
                manifest.files.size() - manifest.chunks().size(), manifest.chunks().size(),
                manifest.content.size(), static_cast<unsigned long long>(manifest.content_bytes),
                manifest.signer.c_str(),
                manifest.signer == key_id(public_key(development_secret_key()))
                    ? " (the development key)"
                    : "");
    return 0;
}

int command_verify(const Arguments& arguments) {
    if (!required(arguments, {"root"})) {
        return kUsage;
    }
    const Location location = location_of(arguments);
    PublicKey key = trusted_key();
    if (const char* hex = arguments.get("public-key");
        hex != nullptr && !parse_hex(hex, key.bytes, sizeof(key.bytes))) {
        std::fprintf(stderr, "cy_ship: --public-key is not 64 hex digits\n");
        return kUsage;
    }
    const Verification verification = verify_installation(location, key);
    for (const Problem& problem : verification.problems) {
        std::printf("problem %s\n", problem.describe().c_str());
    }
    std::printf("cy_ship verify ok=%d files=%llu chunks=%llu bytes=%llu problems=%zu ms=%.2f\n",
                verification.ok() ? 1 : 0, static_cast<unsigned long long>(verification.files),
                static_cast<unsigned long long>(verification.chunks),
                static_cast<unsigned long long>(verification.bytes_hashed),
                verification.problems.size(), verification.milliseconds);
    return verification.ok() ? 0 : kRefused;
}

int command_read(const Arguments& arguments) {
    if (!required(arguments, {"root", "entry", "out"})) {
        return kUsage;
    }
    const Location location = location_of(arguments);
    const Verification verification = verify_installation(location, trusted_key());
    std::string bytes;
    std::string error;
    if (!verification.ok() ||
        !read_content(location, verification.manifest, arguments.get("entry"), bytes, error)) {
        std::fprintf(stderr, "cy_ship read: %s\n",
                     verification.ok() ? error.c_str() : "the installation does not verify");
        return kRefused;
    }
    std::ofstream out(arguments.get("out"), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out ? 0 : 1;
}

int command_keygen(const Arguments& arguments) {
    if (!required(arguments, {"out"})) {
        return kUsage;
    }
    // std::random_device is the operating system's generator on every toolchain this engine
    // supports (getrandom, arc4random, BCryptGenRandom); a seed is 32 bytes of it.
    std::random_device device;
    SecretKey key;
    for (cy::u8& byte : key.seed) {
        byte = static_cast<cy::u8>(device() & 0xFFU);
    }
    std::ofstream out(arguments.get("out"), std::ios::binary | std::ios::trunc);
    out << to_hex(key.seed, sizeof(key.seed)) << "\n";
    if (!out) {
        std::fprintf(stderr, "cy_ship keygen: cannot write %s\n", arguments.get("out"));
        return 1;
    }
    const PublicKey published = public_key(key);
    std::printf("%s\n", to_hex(published.bytes, sizeof(published.bytes)).c_str());
    return 0;
}

int command_public_key(const Arguments& arguments) {
    SecretKey key;
    if (!read_secret(arguments.get("key"), key)) {
        return kUsage;
    }
    const PublicKey published = public_key(key);
    std::printf("%s\n", to_hex(published.bytes, sizeof(published.bytes)).c_str());
    return 0;
}

int command_trusted() {
    const PublicKey key = trusted_key();
    std::printf("%s %s\n", to_hex(key.bytes, sizeof(key.bytes)).c_str(),
                trusts_development_key() ? "development" : "configured");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (argc < 2 || !parse_arguments(argc, argv, arguments)) {
        std::fprintf(
            stderr,
            "usage: cy_ship pack|verify|read|keygen|public-key|trusted [--name value]...\n");
        return kUsage;
    }
    const std::string_view command = argv[1];
    struct Command {
        const char* name;
        int (*run)(const Arguments&);
    };
    static constexpr Command kCommands[] = {{"pack", command_pack},
                                            {"verify", command_verify},
                                            {"read", command_read},
                                            {"keygen", command_keygen},
                                            {"public-key", command_public_key}};
    for (const Command& entry : kCommands) {
        if (command == entry.name) {
            return entry.run(arguments);
        }
    }
    if (command == "trusted") {
        return command_trusted();
    }
    std::fprintf(stderr, "cy_ship: unknown command '%s'\n", argv[1]);
    return kUsage;
}
