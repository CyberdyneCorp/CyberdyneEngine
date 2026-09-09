// cy_import — the import pipeline's command-line front end. M5 task 5.1.
//
// Deliberately thin. Everything worth asserting about an import — which sub-assets it produced,
// what the cache said and why, which diagnostic a mis-tagged normal map raised — is on
// `ImportResult` and `ImportReport`, and a tool that owned any of it would be a tool the tests had
// to run as a process to check. This file parses arguments, builds three objects and prints a
// report.
//
// It is the same split tools/cook/ made, for the same reason.

#include <cy/core/assets/derived_cache.h>
#include <cy/core/assets/file.h>
#include <cy/core/assets/path.h>
#include <cy/core/jobs/job_system.h>
#include <cy/import/pipeline.h>
#include <cy/import/sidecar.h>

#include <cstdio>
#include <cstring>

#include <string>
#include <vector>

namespace {

constexpr const char* kUsage =
    "cy_import — import source assets into cooked content.\n"
    "\n"
    "usage: cy_import [options] <source>...\n"
    "\n"
    "  --project DIR     the project root every source path is relative to (default: .)\n"
    "  --out DIR         where cooked assets are written (default: none, import only)\n"
    "  --cache DIR       the local derived-data cache (default: none, every import is cold)\n"
    "  --shared DIR      a shared cache to read and not write\n"
    "  --write-shared    also write the shared cache. What continuous integration passes.\n"
    "  --variant KEY     the platform variant to cook for, such as desktop-bc7\n"
    "  --profile NAME    client | dedicated-server | editor (default: client)\n"
    "  --set NAME=VALUE  an import option, repeatable. Booleans are true/false.\n"
    "  --no-cache        ignore the cache and re-import, for diagnosing the cache itself\n"
    "  --no-mint         refuse to invent an asset id: fail instead, naming the asset. What a\n"
    "                    shipping cook and continuous integration pass, because a minted id makes\n"
    "                    two cold builds of one project produce different bytes.\n"
    "  --jobs N          worker threads for the import phase (default: the machine's)\n"
    "  --list-importers  print what this build can import, with each importer's options\n"
    "  --json            print the run as JSON on stdout instead of the human report, for a\n"
    "                    caller that has to act on it. What the editor's asset.import uses.\n"
    "  --help            this text\n";

/// Print every importer and every option it declares, which is what a person or a machine caller
/// reads before setting one. `editor-agent-interface` requires a description a caller that cannot
/// see the interface can act on; an import option is one of the things such a caller sets.
void list_importers(const cy::import::ImporterRegistry& registry) {
    for (cy::usize index = 0; index < registry.size(); ++index) {
        const cy::import::Importer* importer = registry.at(index);
        const cy::import::ImporterInfo info = importer->info();
        const cy::import::OptionsSchema schema = importer->schema();
        std::printf("%.*s (version %u)\n", static_cast<int>(info.name.size()), info.name.data(),
                    info.version);
        std::printf("  %.*s\n", static_cast<int>(info.description.size()), info.description.data());
        std::printf("  extensions:");
        for (const std::string_view extension : info.extensions) {
            std::printf(" %.*s", static_cast<int>(extension.size()), extension.data());
        }
        std::printf("\n");
        for (const cy::import::OptionSpec& option : schema.options()) {
            std::printf("  --set %.*s=<%s>\n", static_cast<int>(option.name.size()),
                        option.name.data(), cy::import::option_type_name(option.type));
            std::printf("      %.*s\n", static_cast<int>(option.description.size()),
                        option.description.data());
        }
        std::printf("\n");
    }
}

/// Parse `NAME=VALUE` against whichever importer claims the source, so that a value's type comes
/// from the schema rather than from how it happens to be spelled on the command line.
[[nodiscard]] bool apply_option(const cy::import::OptionsSchema& schema,
                                cy::import::ImportOptions& options, const std::string& assignment) {
    const std::string::size_type equals = assignment.find('=');
    if (equals == std::string::npos) {
        std::fprintf(stderr, "cy_import: --set wants NAME=VALUE, got '%s'\n", assignment.c_str());
        return false;
    }
    const std::string name = assignment.substr(0, equals);
    const std::string value = assignment.substr(equals + 1);
    const cy::import::OptionSpec* declared = schema.find(name);
    if (declared == nullptr) {
        std::fprintf(stderr, "cy_import: no importer option named '%s'\n", name.c_str());
        return false;
    }

    cy::import::OptionValue parsed;
    switch (declared->type) {
        case cy::import::OptionType::Bool:
            if (value != "true" && value != "false") {
                std::fprintf(stderr, "cy_import: '%s' is a bool; write true or false\n",
                             name.c_str());
                return false;
            }
            parsed = cy::import::OptionValue::of_bool(value == "true");
            break;
        case cy::import::OptionType::Int:
            parsed = cy::import::OptionValue::of_int(std::strtoll(value.c_str(), nullptr, 10));
            break;
        case cy::import::OptionType::Float:
            parsed = cy::import::OptionValue::of_float(std::strtod(value.c_str(), nullptr));
            break;
        case cy::import::OptionType::Text:
        case cy::import::OptionType::Enumeration:
            // The string must outlive the options, and the caller's argv does — argv lives for the
            // whole process. Taking a view of it rather than copying is what keeps `OptionValue`
            // free of storage; see options.h.
            parsed = declared->type == cy::import::OptionType::Text
                         ? cy::import::OptionValue::of_text(
                               std::string_view(assignment.data() + equals + 1, value.size()))
                         : cy::import::OptionValue::of_enumeration(
                               std::string_view(assignment.data() + equals + 1, value.size()));
            break;
    }
    if (cy::Status set = options.set(schema, name, parsed); !set) {
        std::fprintf(stderr, "cy_import: %s\n", set.error().message);
        return false;
    }
    return true;
}

/// Write a JSON string body, escaping what a report row can actually contain.
///
/// A path, a diagnostic and an importer name are the only strings that reach this, so the escape
/// set is the one JSON requires of them rather than a full encoder: quote, backslash and the
/// control characters. A caller that fed it arbitrary bytes would get valid JSON with the bytes
/// replaced, which is the right failure for a diagnostic.
void put_json_string(const char* text) {
    std::fputc('"', stdout);
    for (const char* at = text; *at != '\0'; ++at) {
        const auto character = static_cast<unsigned char>(*at);
        if (character == '"' || character == '\\') {
            std::fprintf(stdout, "\\%c", *at);
        } else if (character < 0x20) {
            std::fprintf(stdout, "\\u%04x", character);
        } else {
            std::fputc(*at, stdout);
        }
    }
    std::fputc('"', stdout);
}

/// The sub-asset name-to-id table this source's `.import` record holds, or nothing when it has
/// none.
///
/// Read back rather than carried out of the pipeline, because the record IS the authority: it is
/// what makes a reference survive a re-import, and a second copy of the table inside the tool would
/// be a second thing that could disagree with the file a review reads.
void print_sub_assets(const std::string& project, const cy::assets::VirtualPath& source,
                      const cy::import::ImporterRegistry& registry) {
    std::fputs(",\n    \"assets\": [", stdout);
    cy::Expected<cy::assets::VirtualPath, cy::Error> record_path =
        cy::import::import_record_path_for(source);
    if (!record_path) {
        std::fputs("]", stdout);
        return;
    }
    std::string native = project;
    native += "/";
    native += record_path.value().view();
    cy::Array<cy::u8> bytes;
    if (!cy::assets::fs::read_whole(native.c_str(), bytes)) {
        std::fputs("]", stdout);
        return;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // The options are not wanted here, and passing a null schema is how `parse` says so — it reads
    // the identity and the sub-assets and skips the option values.
    cy::Expected<cy::import::ImportRecord, cy::Error> record =
        cy::import::ImportRecord::parse(text, nullptr);
    if (!record) {
        std::fputs("]", stdout);
        return;
    }
    (void)registry;
    for (cy::usize index = 0; index < record.value().binding_count(); ++index) {
        const std::string name(record.value().binding_name(index));
        char id_text[cy::AssetId::kTextLength + 1] = {};
        (void)record.value().binding_id(index).format(id_text);
        std::fputs(index == 0 ? "\n      {\"name\": " : ",\n      {\"name\": ", stdout);
        put_json_string(name.c_str());
        std::fputs(", \"id\": ", stdout);
        put_json_string(id_text);
        std::fputs("}", stdout);
    }
    std::fputs(record.value().binding_count() == 0 ? "]" : "\n    ]", stdout);
}

/// The whole run, as JSON on stdout.
///
/// The human report is a paragraph a person reads; this is the same facts in the shape a caller
/// acts on. It exists because `asset.import` — the editor command M8.a task 3.1 adds — has to know
/// WHICH sub-assets an import produced and what identity each one holds, and parsing a paragraph
/// for that is how a tool boundary becomes a source of bugs.
void print_json(const cy::import::ImportPipeline& pipeline, const std::string& project,
                const cy::Array<cy::assets::VirtualPath>& sources,
                const cy::import::ImporterRegistry& registry) {
    std::fputs("{\n  \"sources\": [", stdout);
    const cy::Span<const cy::import::AssetImportOutcome> rows = pipeline.report().rows();
    for (cy::usize index = 0; index < rows.size(); ++index) {
        const cy::import::AssetImportOutcome& row = rows[index];
        char id_text[cy::AssetId::kTextLength + 1] = {};
        (void)row.id.format(id_text);
        char absent[512] = {};
        (void)cy::import::format_absent_model_steps(row.steps, absent, sizeof(absent));

        std::fputs(index == 0 ? "\n    {\n" : ",\n    {\n", stdout);
        std::fputs("      \"source\": ", stdout);
        put_json_string(row.source);
        std::fputs(",\n      \"importer\": ", stdout);
        put_json_string(row.importer);
        std::fputs(",\n      \"id\": ", stdout);
        put_json_string(id_text);
        std::fputs(",\n      \"cache\": ", stdout);
        put_json_string(cy::assets::cache_outcome_name(row.cache));
        std::fputs(",\n      \"cache_reason\": ", stdout);
        put_json_string(row.cache_reason);
        std::fprintf(stdout,
                     ",\n      \"sub_assets\": %zu,\n      \"warnings\": %zu,\n"
                     "      \"errors\": %zu,\n      \"cooked_bytes\": %zu,\n"
                     "      \"minted_ids\": %zu,\n      \"duration_micros\": %llu",
                     row.sub_assets, row.warnings, row.errors, row.cooked_bytes, row.minted_ids,
                     static_cast<unsigned long long>(row.duration_micros));
        // Named rather than warned about: a format's absent capability is not a defect in the file.
        // Empty when the importer reaches every step, and absent-as-a-concept when the sequence
        // does not apply to it at all — which is why this is a string and not a list of numbers.
        std::fputs(",\n      \"steps_not_reached\": ", stdout);
        put_json_string(absent);
        if (index < sources.size()) {
            print_sub_assets(project, sources[index], registry);
        }
        std::fputs("\n    }", stdout);
    }
    std::fprintf(stdout,
                 "\n  ],\n  \"warnings\": %zu,\n  \"errors\": %zu,\n  \"cache_hits\": %zu,\n"
                 "  \"cache_misses\": %zu\n}\n",
                 pipeline.report().total_warnings(), pipeline.report().total_errors(),
                 pipeline.report().cache_hits(), pipeline.report().cache_misses());
}

}  // namespace

int main(int argc, char** argv) {
    std::string project = ".";
    std::string output;
    std::string cache_directory;
    std::string shared_directory;
    std::string variant_text;
    std::string profile_text = "client";
    bool refuse_minting = false;
    // Held by value for the whole of main, because an option's text value is a view into one of
    // these and the options outlive the parsing loop.
    std::vector<std::string> assignments;
    std::vector<std::string> sources;
    bool write_shared = false;
    bool ignore_cache = false;
    bool listing = false;
    bool as_json = false;
    unsigned worker_count = 0;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&](const char* name) -> const char* {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "cy_import: %s wants a value\n", name);
                return nullptr;
            }
            return argv[++index];
        };
        if (argument == "--help" || argument == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (argument == "--list-importers") {
            listing = true;
        } else if (argument == "--json") {
            as_json = true;
        } else if (argument == "--project") {
            const char* value = next("--project");
            if (value == nullptr) {
                return 2;
            }
            project = value;
        } else if (argument == "--out") {
            const char* value = next("--out");
            if (value == nullptr) {
                return 2;
            }
            output = value;
        } else if (argument == "--cache") {
            const char* value = next("--cache");
            if (value == nullptr) {
                return 2;
            }
            cache_directory = value;
        } else if (argument == "--shared") {
            const char* value = next("--shared");
            if (value == nullptr) {
                return 2;
            }
            shared_directory = value;
        } else if (argument == "--write-shared") {
            write_shared = true;
        } else if (argument == "--variant") {
            const char* value = next("--variant");
            if (value == nullptr) {
                return 2;
            }
            variant_text = value;
        } else if (argument == "--profile") {
            const char* value = next("--profile");
            if (value == nullptr) {
                return 2;
            }
            profile_text = value;
        } else if (argument == "--set") {
            const char* value = next("--set");
            if (value == nullptr) {
                return 2;
            }
            assignments.emplace_back(value);
        } else if (argument == "--no-mint") {
            refuse_minting = true;
        } else if (argument == "--no-cache") {
            ignore_cache = true;
        } else if (argument == "--jobs") {
            const char* value = next("--jobs");
            if (value == nullptr) {
                return 2;
            }
            worker_count = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        } else if (!argument.empty() && argument[0] == '-') {
            std::fprintf(stderr, "cy_import: unknown option '%s'\n", argument.c_str());
            std::fputs(kUsage, stderr);
            return 2;
        } else {
            sources.push_back(argument);
        }
    }

    cy::import::ImporterRegistry registry;
    if (cy::Status registered = cy::import::register_builtin_importers(registry); !registered) {
        std::fprintf(stderr, "cy_import: %s\n", registered.error().message);
        return 1;
    }
    if (listing) {
        list_importers(registry);
        return 0;
    }
    if (sources.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    cy::assets::DerivedCache cache;
    cy::assets::DerivedCacheTiers tiers;
    tiers.local = cache_directory.c_str();
    tiers.shared = write_shared ? "" : shared_directory.c_str();
    tiers.remote = write_shared ? shared_directory.c_str() : "";
    tiers.write_remote = write_shared;
    if (cy::Status configured = cache.configure(tiers); !configured) {
        std::fprintf(stderr, "cy_import: %s\n", configured.error().message);
        return 1;
    }

    cy::import::ImportPipeline pipeline(registry, cache);
    if (cy::Status set = pipeline.set_project_root(project); !set) {
        std::fprintf(stderr, "cy_import: %s\n", set.error().message);
        return 1;
    }
    if (cy::Status set = pipeline.set_output_directory(output); !set) {
        std::fprintf(stderr, "cy_import: %s\n", set.error().message);
        return 1;
    }

    cy::import::ImportSettings settings;
    settings.ignore_cache = ignore_cache;
    if (!variant_text.empty()) {
        cy::Expected<cy::assets::VariantKey, cy::Error> variant =
            cy::assets::VariantKey::parse(variant_text);
        if (!variant) {
            std::fprintf(stderr, "cy_import: %s\n", variant.error().message);
            return 2;
        }
        settings.variant = variant.value();
    }
    cy::Expected<cy::import::CookProfile, cy::Error> profile =
        cy::import::cook_profile_from_name(profile_text);
    if (!profile) {
        std::fprintf(stderr, "cy_import: '%s' is not a cook profile\n", profile_text.c_str());
        return 2;
    }
    settings.profile = profile.value();
    settings.minting =
        refuse_minting ? cy::import::MintPolicy::Refuse : cy::import::MintPolicy::Mint;

    cy::Array<cy::assets::VirtualPath> paths;
    for (const std::string& source : sources) {
        cy::Expected<cy::assets::VirtualPath, cy::Error> path =
            cy::assets::VirtualPath::normalise(source);
        if (!path) {
            std::fprintf(stderr, "cy_import: '%s': %s\n", source.c_str(), path.error().message);
            return 2;
        }
        if (cy::Status pushed = paths.push_back(path.value()); !pushed) {
            return 1;
        }
    }

    // Options are applied against the schema of whichever importer claims the FIRST source. A run
    // that mixes a glTF and a texture and sets an option belonging to one of them is refused here
    // rather than silently applying it to neither.
    cy::import::ImportOptions options;
    if (!assignments.empty()) {
        cy::import::Importer* importer = registry.find_for_source(paths[0]);
        if (importer == nullptr) {
            std::fprintf(stderr, "cy_import: no importer claims '%s', so --set has no schema\n",
                         sources[0].c_str());
            return 2;
        }
        const cy::import::OptionsSchema schema = importer->schema();
        for (const std::string& assignment : assignments) {
            if (!apply_option(schema, options, assignment)) {
                return 2;
            }
        }
        settings.options = &options;
    }

    cy::jobs::JobSystem job_system;
    bool started = false;
    if (paths.size() > 1) {
        cy::jobs::JobSystemConfig configuration;
        if (worker_count != 0) {
            configuration.worker_count = worker_count;
        }
        if (cy::Status begun = job_system.start(configuration); begun) {
            started = true;
        } else {
            std::fprintf(stderr, "cy_import: no job system (%s); importing serially\n",
                         begun.error().message);
        }
    }

    cy::Expected<cy::usize, cy::Error> imported =
        pipeline.import_all(cy::Span<const cy::assets::VirtualPath>(paths.data(), paths.size()),
                            settings, started ? &job_system : nullptr);
    if (started) {
        job_system.shutdown();
    }
    if (!imported) {
        std::fprintf(stderr, "cy_import: %s\n", imported.error().message);
        return 1;
    }

    if (as_json) {
        print_json(pipeline, project, paths, registry);
    } else {
        std::vector<char> text(static_cast<std::size_t>(64) * 1024, '\0');
        (void)pipeline.report().format(text.data(), text.size());
        std::fputs(text.data(), stdout);
    }
    return pipeline.report().total_errors() == 0 ? 0 : 1;
}
