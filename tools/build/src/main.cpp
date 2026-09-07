// cy_build — the front end over the derivation graph. M6 section 7.
//
// A thin front end, deliberately: `cy_build_graph` holds every decision and this file holds
// argument parsing and printing. The interesting assertions — which nodes ran, whether two builds
// produced one set of bytes, whether an interrupted patch left the previous build playable — are
// made against the library by `tools/build/tests/`, not against this binary's exit code.
//
// Subcommands:
//   toolchain                     what every derivation key contributes about the toolchain
//   build                         run a description's graph
//   explain    --source <name>    which nodes a change to that source would reach
//   audit      --node <name>      why a node is in the build, and what references it
//   determinism                   build the description twice into two roots and diff the artefacts
//   patch      --from --to        the chunks that differ between two package manifests
//   install    --package          install a build into an installation root
//   apply      --patch            apply a patch, optionally interrupted at a named stage
//   verify     --install          every chunk the build in force names, re-digested

#include <cy/build/description.h>
#include <cy/build/package.h>
#include <cy/build/patch.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace cy;
using namespace cy::build;

[[nodiscard]] Allocator& tool_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

/// Every option this tool takes, as `--name value`. A flag is `--name` with no value.
struct Arguments {
    std::string command;
    std::unordered_map<std::string, std::string> options;

    [[nodiscard]] std::string value(const char* name, const char* fallback = "") const {
        const auto found = options.find(name);
        return found == options.end() ? std::string(fallback) : found->second;
    }
    [[nodiscard]] bool has(const char* name) const { return options.contains(name); }
};

[[nodiscard]] Arguments parse_arguments(int argc, char** argv) {
    Arguments arguments;
    if (argc > 1) {
        arguments.command = argv[1];
    }
    for (int index = 2; index < argc; ++index) {
        std::string argument = argv[index];
        if (!argument.starts_with("--")) {
            continue;
        }
        argument.erase(0, 2);
        const usize equals = argument.find('=');
        if (equals != std::string::npos) {
            arguments.options[argument.substr(0, equals)] = argument.substr(equals + 1);
            continue;
        }
        const bool has_value = index + 1 < argc && std::strncmp(argv[index + 1], "--", 2) != 0;
        arguments.options[argument] = has_value ? argv[++index] : "";
    }
    return arguments;
}

[[nodiscard]] int fail(const char* message) {
    std::fprintf(stderr, "cy_build: %s\n", message);
    return 1;
}

[[nodiscard]] int fail(const char* message, const Error& error) {
    std::fprintf(stderr, "cy_build: %s: %s\n", message, error.message);
    return 1;
}

[[nodiscard]] Expected<std::string, Error> read_text(const std::string& path) {
    Array<u8> bytes(tool_allocator());
    if (Status read = assets::fs::read_whole(path.c_str(), bytes); !read) {
        return make_unexpected(read.error());
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] Status write_text(const std::string& path, const std::string& content) {
    return assets::fs::write_atomic(path.c_str(), content.data(), content.size());
}

/// The graph, its producers and its sources — assembled the same way by every subcommand that needs
/// them, so a description means the same thing to `build`, to `explain` and to `determinism`.
struct Project {
    ProducerRegistry producers;
    BuildGraph graph;
    std::unique_ptr<DirectorySourceProvider> sources;
};

[[nodiscard]] Status load_project(const Arguments& arguments, Project& out) {
    if (Status added = out.producers.add_builtins(); !added) {
        return added;
    }
    const Expected<std::string, Error> document = read_text(arguments.value("description"));
    if (!document) {
        return make_unexpected(document.error());
    }
    if (Status read = read_description(*document, out.graph, &out.producers); !read) {
        return read;
    }
    out.sources = std::make_unique<DirectorySourceProvider>(arguments.value("project", "."));
    return ok();
}

[[nodiscard]] BuildConfig configure_from(const Arguments& arguments, const Project& project,
                                         const std::string& artefacts,
                                         const std::string& cache_root) {
    BuildConfig config;
    config.graph = &project.graph;
    config.producers = &project.producers;
    config.sources = project.sources.get();
    config.artefact_root = artefacts;
    config.cache.local = cache_root.c_str();
    config.policy = arguments.value("policy", "enforce") == "audit" ? AccessPolicy::Audit
                                                                    : AccessPolicy::Enforce;
    config.workers =
        static_cast<u32>(std::strtoul(arguments.value("workers", "0").c_str(), nullptr, 10));
    return config;
}

void print_report(const BuildReport& report) {
    for (const NodeResult& result : report.nodes) {
        char key[assets::DerivationKey::kTextLength + 1] = {};
        result.key.format(key);
        std::printf("%-10s %-40s %.16s %s\n", node_outcome_name(result.outcome),
                    result.name.c_str(), key, result.reason.c_str());
    }
    std::printf("ran %llu  cached %llu  rebuilt %llu  skipped %llu  failed %llu  bytes %llu\n",
                static_cast<unsigned long long>(report.ran),
                static_cast<unsigned long long>(report.cached),
                static_cast<unsigned long long>(report.rebuilt),
                static_cast<unsigned long long>(report.skipped),
                static_cast<unsigned long long>(report.failed),
                static_cast<unsigned long long>(report.bytes_produced));
    for (const AccessViolation& violation : report.violations) {
        std::printf("violation  %s  %s  %s\n", violation_kind_name(violation.kind),
                    violation.node.c_str(), violation.name.c_str());
    }
}

[[nodiscard]] Expected<BuildReport, Error> run_build(const Arguments& arguments,
                                                     const Project& project,
                                                     const std::string& artefacts,
                                                     const std::string& cache) {
    BuildService service;
    if (Status configured = service.configure(configure_from(arguments, project, artefacts, cache));
        !configured) {
        return make_unexpected(configured.error());
    }
    return service.build();
}

[[nodiscard]] int command_toolchain() {
    const ToolchainFingerprint& fingerprint = current_toolchain();
    char digest[assets::ContentHash::kTextLength + 1] = {};
    fingerprint.digest().format(digest);
    std::printf("%s", fingerprint.describe().c_str());
    std::printf("digest    %s\n", digest);
    return 0;
}

[[nodiscard]] int command_build(const Arguments& arguments) {
    Project project;
    if (Status loaded = load_project(arguments, project); !loaded) {
        return fail("the description could not be read", loaded.error());
    }
    const Expected<BuildReport, Error> report =
        run_build(arguments, project, arguments.value("out", "build/derived/artefacts"),
                  arguments.value("cache", "build/derived/cache"));
    if (!report) {
        return fail("the build could not run", report.error());
    }
    print_report(*report);

    if (arguments.has("package")) {
        Provenance provenance;
        provenance.project = arguments.value("project", ".");
        provenance.revision = arguments.value("revision");
        provenance.platform = arguments.value("platform", "host");
        provenance.profile = arguments.value("profile", "client");
        char digest[assets::ContentHash::kTextLength + 1] = {};
        current_toolchain().digest().format(digest);
        provenance.toolchain = digest;

        const Expected<PackageSet, Error> packages =
            assemble(project.graph, *report, std::move(provenance));
        if (!packages) {
            return fail("the build could not be packaged", packages.error());
        }
        if (Status written = write_text(arguments.value("package"), write_package(*packages));
            !written) {
            return fail("the package manifest could not be written", written.error());
        }
        std::printf("%s", bundle_report(*packages).c_str());
        std::printf("build %s\n", packages->build_id.c_str());
    }
    return report->succeeded() ? 0 : 1;
}

[[nodiscard]] int command_explain(const Arguments& arguments) {
    Project project;
    if (Status loaded = load_project(arguments, project); !loaded) {
        return fail("the description could not be read", loaded.error());
    }
    for (const NodeId id : project.graph.dependents_of_source(arguments.value("source"))) {
        std::printf("%s\n", project.graph.node(id).name.c_str());
    }
    return 0;
}

[[nodiscard]] int command_audit(const Arguments& arguments) {
    Project project;
    if (Status loaded = load_project(arguments, project); !loaded) {
        return fail("the description could not be read", loaded.error());
    }
    const Expected<AuditAnswer, Error> answer = audit(project.graph, arguments.value("node"));
    if (!answer) {
        return fail("no such node", answer.error());
    }
    std::printf("why:\n");
    for (const std::string& step : answer->chain) {
        std::printf("  %s\n", step.c_str());
    }
    std::printf("referenced by:\n");
    for (const std::string& dependent : answer->dependents) {
        std::printf("  %s\n", dependent.c_str());
    }
    return 0;
}

/// The two-run determinism gate design.md §1.9 item 6 requires, in the shape `just generate-check`
/// already has for codegen: produce the artefacts twice into two roots, diff them, and refuse an
/// artefact whose bytes carry an absolute path.
[[nodiscard]] int command_determinism(const Arguments& arguments) {
    Project project;
    if (Status loaded = load_project(arguments, project); !loaded) {
        return fail("the description could not be read", loaded.error());
    }
    const std::string root = arguments.value("out", "build/derived/determinism");
    // What an artefact must never contain. Resolved to an absolute path here, because a relative
    // one would never appear in an artefact and the check would pass by construction.
    char resolved[4096] = {};
    const char* working = ::getcwd(resolved, sizeof(resolved));
    const std::string absolute_root = working != nullptr ? std::string(working) : std::string("/");
    const Expected<BuildReport, Error> first =
        run_build(arguments, project, root + "/first", root + "/cache-first");
    const Expected<BuildReport, Error> second =
        run_build(arguments, project, root + "/second", root + "/cache-second");
    if (!first || !second) {
        return fail("a determinism run could not build");
    }
    if (!first->succeeded() || !second->succeeded()) {
        print_report(!first->succeeded() ? *first : *second);
        return fail("a determinism run failed");
    }

    int differences = 0;
    for (usize index = 0; index < first->nodes.size(); ++index) {
        const NodeResult& a = first->nodes[index];
        const NodeResult& b = second->nodes[index];
        if (a.result != b.result) {
            std::fprintf(stderr, "cy_build: %s is not deterministic\n", a.name.c_str());
            ++differences;
        }
    }

    // The second half of design.md §1.9 item 6, and the half that a digest comparison cannot do:
    // two runs from the SAME directory agree with each other while both embed that directory. An
    // artefact carrying an absolute path is deterministic and unshareable, and the failure only
    // shows up on the second machine.
    ArtefactStore store;
    int leaked = 0;
    if (store.configure(root + "/second")) {
        Array<u8> bytes(tool_allocator());
        for (const NodeResult& result : second->nodes) {
            for (const NodeOutput& output : result.outputs) {
                if (!store.get(output.digest, bytes)) {
                    continue;
                }
                const std::string_view content(reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size());
                if (content.find(absolute_root) != std::string_view::npos) {
                    std::fprintf(stderr, "cy_build: %s/%s embeds an absolute path\n",
                                 result.name.c_str(), output.name.c_str());
                    ++leaked;
                }
            }
        }
    }

    std::printf("determinism: %zu nodes, %d differing, %d embedding an absolute path\n",
                first->nodes.size(), differences, leaked);
    return differences == 0 && leaked == 0 ? 0 : 1;
}

[[nodiscard]] int command_patch(const Arguments& arguments) {
    const Expected<std::string, Error> from_text = read_text(arguments.value("from"));
    const Expected<std::string, Error> to_text = read_text(arguments.value("to"));
    if (!from_text || !to_text) {
        return fail("a package manifest could not be read");
    }
    const Expected<PackageSet, Error> from = read_package(*from_text);
    const Expected<PackageSet, Error> to = read_package(*to_text);
    if (!from || !to) {
        return fail("a package manifest could not be parsed");
    }
    const PatchManifest patch = diff(*from, *to);
    if (Status written = write_text(arguments.value("out"), write_patch(patch)); !written) {
        return fail("the patch could not be written", written.error());
    }
    std::printf("patch %s -> %s: %zu added, %zu removed, %llu bytes\n", from->build_id.c_str(),
                to->build_id.c_str(), patch.added.size(), patch.removed.size(),
                static_cast<unsigned long long>(patch.transferred_bytes()));
    return 0;
}

[[nodiscard]] int command_install(const Arguments& arguments) {
    const Expected<std::string, Error> manifest = read_text(arguments.value("package"));
    if (!manifest) {
        return fail("the package manifest could not be read", manifest.error());
    }
    const Expected<PackageSet, Error> packages = read_package(*manifest);
    if (!packages) {
        return fail("the package manifest could not be parsed", packages.error());
    }
    ArtefactStore source;
    if (Status pointed = source.configure(arguments.value("artefacts")); !pointed) {
        return fail("the artefact store could not be opened", pointed.error());
    }
    Installation installation;
    if (Status opened = installation.open(arguments.value("install")); !opened) {
        return fail("the installation could not be opened", opened.error());
    }
    if (Status installed = installation.install(*packages, source); !installed) {
        return fail("the build could not be installed", installed.error());
    }
    std::printf("installed %s\n", packages->build_id.c_str());
    return 0;
}

[[nodiscard]] int command_apply(const Arguments& arguments) {
    const Expected<std::string, Error> document = read_text(arguments.value("patch"));
    if (!document) {
        return fail("the patch could not be read", document.error());
    }
    const Expected<PatchManifest, Error> patch = read_patch(*document);
    if (!patch) {
        return fail("the patch could not be parsed", patch.error());
    }
    ArtefactStore source;
    if (Status pointed = source.configure(arguments.value("artefacts")); !pointed) {
        return fail("the artefact store could not be opened", pointed.error());
    }
    Installation installation;
    if (Status opened = installation.open(arguments.value("install")); !opened) {
        return fail("the installation could not be opened", opened.error());
    }

    PatchInterrupt interrupt;
    const std::string stage = arguments.value("crash-at", arguments.value("stop-at").c_str());
    if (!stage.empty()) {
        const Expected<PatchStage, Error> parsed = patch_stage_from_name(stage);
        if (!parsed) {
            return fail("not a patch stage", parsed.error());
        }
        interrupt.stage = *parsed;
        interrupt.hard = arguments.has("crash-at");
    }

    const StoreChunkSource chunks(source);
    const Expected<PatchResult, Error> result = installation.apply(*patch, chunks, interrupt);
    if (!result) {
        return fail("the patch was refused", result.error());
    }
    std::printf("patch reached %s, applied %s%s%s\n", patch_stage_name(result->reached),
                result->applied ? "yes" : "no",
                result->failed_chunk.empty() ? "" : ", failed chunk ",
                result->failed_chunk.c_str());
    return result->applied ? 0 : 2;
}

[[nodiscard]] int command_verify(const Arguments& arguments) {
    if (arguments.has("install")) {
        Installation installation;
        if (Status opened = installation.open(arguments.value("install")); !opened) {
            return fail("the installation could not be opened", opened.error());
        }
        if (Status verified = installation.verify(); !verified) {
            return fail("the installation is damaged", verified.error());
        }
        const Expected<std::string, Error> build = installation.current_build();
        std::printf("installation verified: %s\n", build ? build->c_str() : "?");
        return 0;
    }

    ArtefactStore store;
    if (Status pointed = store.configure(arguments.value("artefacts")); !pointed) {
        return fail("the artefact store could not be opened", pointed.error());
    }
    u64 corrupt = 0;
    const Expected<u64, Error> checked = store.verify_all(&corrupt);
    if (!checked) {
        return fail("the artefact store could not be walked", checked.error());
    }
    std::printf("artefacts verified: %llu checked, %llu corrupt\n",
                static_cast<unsigned long long>(*checked),
                static_cast<unsigned long long>(corrupt));
    return corrupt == 0 ? 0 : 1;
}

[[nodiscard]] int usage() {
    std::fprintf(stderr,
                 "usage: cy_build <toolchain|build|explain|audit|determinism|patch|install|apply|"
                 "verify> [--option value ...]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const Arguments arguments = parse_arguments(argc, argv);
    if (arguments.command == "toolchain") {
        return command_toolchain();
    }
    if (arguments.command == "build") {
        return command_build(arguments);
    }
    if (arguments.command == "explain") {
        return command_explain(arguments);
    }
    if (arguments.command == "audit") {
        return command_audit(arguments);
    }
    if (arguments.command == "determinism") {
        return command_determinism(arguments);
    }
    if (arguments.command == "patch") {
        return command_patch(arguments);
    }
    if (arguments.command == "install") {
        return command_install(arguments);
    }
    if (arguments.command == "apply") {
        return command_apply(arguments);
    }
    if (arguments.command == "verify") {
        return command_verify(arguments);
    }
    return usage();
}
