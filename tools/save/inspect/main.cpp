// SPDX-License-Identifier: MIT
// cy_save_inspect — what is in a save, why each field is there, and what changed between two.
//
//     cy_save_inspect show <save-dir> [--generation <n>] [--why] [--build <id>] [--exact-build]
//     cy_save_inspect diff <save-dir>[@<n>] <save-dir>[@<n>]
//
// `save-and-persistence` — "Save diagnostics and inspection". The work is `cy/save/inspect.h`; this
// is its front end over a save directory on disk (a `FilesystemBackend` root, the layout
// `SaveArchive` writes). The output is the line format `render_inspection`, `render_origins` and
// `render_diff` document, one fact per line, so it can be read and grepped alike.
//
//   show   the manifest, the retained generations, whether THIS build would restore the save and,
//          if not, the named reason; entity counts; size by scope, region, component and plugin.
//          `--why` adds one line per saved field: component, field, persistence trait, entity, and
//          the generation it has held its value since. `--build` checks the save against a build
//          identity (under the migratable policy unless `--exact-build`), which is how "why was
//          this not restored" is asked of a save somebody else's build refused.
//   diff   the semantic difference between two generations — of one save or of two — in stable
//          identifiers. `@<n>` picks a generation; the active one otherwise.
//
// The schema is THIS build's: every type `register_generated_types()` registers is explained by
// name, trait and owning module. A type it does not register — a plugin this build lacks — is
// reported as preserved state, by identifier, which is the true answer to why it is there.
//
// Exit status: 0 when the command ran, 1 when the save could not be read at all, 2 on a usage
// error. A save this build would not restore is still exit 0: the inspector's job is to say why,
// and it did.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/reflect.h>
#include <cy/core/reflect/registry.h>
#include <cy/save/archive.h>
#include <cy/save/inspect.h>
#include <cy/save/storage.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using namespace cy;
using namespace cy::save;

constexpr int kReadFailure = 1;
constexpr int kUsageExit = 2;

void usage() {
    std::fprintf(stderr,
                 "usage: cy_save_inspect show <save-dir> [--generation <n>] [--why] "
                 "[--build <id>] [--exact-build]\n"
                 "       cy_save_inspect diff <save-dir>[@<n>] <save-dir>[@<n>]\n");
}

Allocator& tool_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

void print(const Array<char>& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

bool parse_generation(const char* text, u32& out) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 0xFFFF'FFFFUL) {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

/// One save directory opened for reading, and the generation asked for.
struct OpenSave {
    FilesystemBackend store;
    SaveArchive archive{tool_allocator()};
    u32 generation = 0;
};

bool open_save(const std::string& spec, OpenSave& out) {
    std::string directory = spec;
    const std::string::size_type at = spec.rfind('@');
    if (at != std::string::npos && at + 1 < spec.size()) {
        if (!parse_generation(spec.c_str() + at + 1, out.generation)) {
            std::fprintf(stderr, "cy_save_inspect: '%s' is not a generation number\n",
                         spec.c_str() + at + 1);
            return false;
        }
        directory = spec.substr(0, at);
    }
    const Expected<usize, Error> opened = out.store.open(directory.c_str());
    if (!opened || !out.archive.open(out.store)) {
        std::fprintf(stderr, "cy_save_inspect: cannot open the save directory '%s'\n",
                     directory.c_str());
        return false;
    }
    return true;
}

/// Inspect one opened save, printing why when it cannot be. A Status rather than a flag, like every
/// load in the engine: `forbidden save pattern boolean-load` holds this file to it too.
Status inspect_one(OpenSave& save, const InspectOptions& options, SaveInspection& out) {
    Status inspected = inspect_save(save.archive, save.generation, options, out);
    if (!inspected) {
        std::fprintf(stderr, "cy_save_inspect: %s: %s\n", save.store.root(),
                     inspected.error().message);
    }
    return inspected;
}

struct ShowArguments {
    std::string directory;
    bool why = false;
    bool exact = false;
    const char* build = "";
    u32 generation = 0;
};

bool parse_show(int argc, char** argv, ShowArguments& out) {
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (argument == "--why") {
            out.why = true;
        } else if (argument == "--exact-build") {
            out.exact = true;
        } else if (argument == "--build" && has_value) {
            out.build = argv[++index];
        } else if (argument == "--generation" && has_value) {
            if (!parse_generation(argv[++index], out.generation)) {
                return false;
            }
        } else if (out.directory.empty() && !argument.starts_with("--")) {
            out.directory = argument;
        } else {
            return false;
        }
    }
    return !out.directory.empty();
}

int show(int argc, char** argv, const InspectOptions& schema) {
    ShowArguments arguments;
    if (!parse_show(argc, argv, arguments)) {
        usage();
        return kUsageExit;
    }
    OpenSave save;
    if (!open_save(arguments.directory, save)) {
        return kReadFailure;
    }
    if (arguments.generation != 0) {
        save.generation = arguments.generation;
    }
    InspectOptions options = schema;
    options.policy.build_id = arguments.build;
    options.policy.compatibility =
        arguments.exact ? Compatibility::ExactBuild : Compatibility::Migratable;

    SaveInspection inspection(tool_allocator());
    if (!inspect_one(save, options, inspection)) {
        return kReadFailure;
    }
    Array<char> text(tool_allocator());
    if (!render_inspection(inspection, options, text)) {
        return kReadFailure;
    }
    if (arguments.why) {
        Array<FieldOrigin> origins(tool_allocator());
        if (!explain_fields(save.archive, inspection, options, origins) ||
            !render_origins(origins.span(), text)) {
            return kReadFailure;
        }
    }
    print(text);
    return inspection.contents.failed() ? kReadFailure : 0;
}

int diff(int argc, char** argv, const InspectOptions& options) {
    if (argc != 4) {
        usage();
        return kUsageExit;
    }
    OpenSave before;
    OpenSave after;
    if (!open_save(argv[2], before) || !open_save(argv[3], after)) {
        return kReadFailure;
    }
    SaveInspection a(tool_allocator());
    SaveInspection b(tool_allocator());
    if (!inspect_one(before, options, a) || !inspect_one(after, options, b)) {
        return kReadFailure;
    }
    if (a.contents.failed() || b.contents.failed()) {
        std::fprintf(stderr, "cy_save_inspect: a save's contents could not be read; see `show`\n");
        return kReadFailure;
    }
    SaveDiff difference(tool_allocator());
    Array<char> text(tool_allocator());
    if (!diff_saves(a, b, difference) || !render_diff(difference, options.types, text)) {
        return kReadFailure;
    }
    std::printf("diff from generation=%u to generation=%u\n", a.generation, b.generation);
    print(text);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return kUsageExit;
    }
    reflect::TypeRegistry types;
    if (!reflect::register_generated_types(types)) {
        std::fprintf(stderr, "cy_save_inspect: this build's schema could not be registered\n");
        return kReadFailure;
    }
    InspectOptions options;
    options.types = &types;

    const std::string command = argv[1];
    if (command == "show") {
        return show(argc, argv, options);
    }
    if (command == "diff") {
        return diff(argc, argv, options);
    }
    usage();
    return kUsageExit;
}
