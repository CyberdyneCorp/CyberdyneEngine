// `cy_cook` — the command-line front end over the cook pipeline. M2 task 3.2.13.
//
// Deliberately thin: it parses arguments, builds a world whose component registry the cook reads,
// runs the pipeline, and prints the report. Everything worth testing is in `cy::cook::run`, which
// is a library function with no `main` around it.
//
//   cy_cook --source <dir> --output <file.cypak> [--variant <key>] [--shipping]
//           [--fail-on-conflicts]
//
// WHAT IT CANNOT DO YET, AND WHY THAT IS SAID HERE RATHER THAN DISCOVERED. A cook needs to know
// each component's size, alignment and entity-reference offsets, and the only authority on that is
// a world that has registered them — which means a game's own registration code. Until the project
// system can load a game's module and call it (M4's ABI work), this front end registers the types
// the reflection registry already holds, with no entity-reference offsets, and says so in its
// report. A project with entity references in its components should drive `cy::cook::run` from its
// own tool until then; the pipeline takes the world, so that is a five-line program.

#include <cy/cook/pipeline.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/reflect.h>
#include <cy/core/reflect/registry.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace {

using namespace cy;

struct Arguments {
    const char* source = nullptr;
    const char* output = nullptr;
    const char* variant = "";
    bool shipping = false;
    bool fail_on_conflicts = false;
};

void print_usage() noexcept {
    std::fputs(
        "cy_cook — cook authoring documents into a package\n"
        "\n"
        "  cy_cook --source <dir> --output <file.cypak> [--variant <key>] [--shipping]\n"
        "          [--fail-on-conflicts]\n"
        "\n"
        "  --source            directory of .cyscene and .cyprefab documents, read recursively\n"
        "  --output            the .cypak to write\n"
        "  --variant           the platform/feature key every cooked asset is stamped with\n"
        "  --shipping          strip prefab provenance; a shipping build carries no prefab link\n"
        "  --fail-on-conflicts fail the run on unresolved override conflicts\n",
        stderr);
}

[[nodiscard]] bool parse(int argc, char** argv, Arguments& out) noexcept {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag(argv[index]);
        const auto value = [&]() noexcept -> const char* {
            return (index + 1 < argc) ? argv[++index] : nullptr;
        };
        if (flag == "--source") {
            out.source = value();
        } else if (flag == "--output") {
            out.output = value();
        } else if (flag == "--variant") {
            out.variant = value();
        } else if (flag == "--shipping") {
            out.shipping = true;
        } else if (flag == "--fail-on-conflicts") {
            out.fail_on_conflicts = true;
        } else {
            std::fprintf(stderr, "cy_cook: unknown argument '%s'\n", argv[index]);
            return false;
        }
    }
    return out.source != nullptr && out.output != nullptr && out.variant != nullptr;
}

/// Register every reflected type the process knows as a component of `world`.
///
/// The registry is filled first: reflection is opt-in by an explicit call, deliberately, so that
/// what a process has reflected does not depend on link order — which means a tool that never makes
/// the call sees an empty registry and cooks nothing, silently.
///
/// No entity-reference offsets are declared, because reflection does not record them — see the
/// header comment. A type reflection cannot describe as a component (not trivially relocatable) is
/// skipped rather than failing the run.
[[nodiscard]] u32 register_reflected_components(ecs::World& world) noexcept {
    if (const Status registered = reflect::register_generated_types(); !registered) {
        std::fprintf(stderr, "cy_cook: could not fill the reflection registry: %s\n",
                     registered.error().message);
        return 0;
    }
    u32 registered = 0;
    for (const reflect::TypeInfo* type : reflect::default_registry()) {
        if (world.components().register_reflected(*type)) {
            ++registered;
        }
    }
    return registered;
}

}  // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (!parse(argc, argv, arguments)) {
        print_usage();
        return 2;
    }

    Allocator& allocator = system_allocator(MemoryDomain::Assets);

    ecs::World world(allocator, ecs::WorldConfig{"cook", 16384});
    if (const Status started = world.initialize(); !started) {
        std::fprintf(stderr, "cy_cook: could not initialise the cook world: %s\n",
                     started.error().message);
        return 1;
    }
    const u32 components = register_reflected_components(world);

    assets::VirtualFileSystem vfs;
    Expected<UniquePtr<assets::DirectoryMount>, Error> mount = assets::DirectoryMount::create(
        arguments.source, assets::MountKind::Project, /*writable=*/false);
    if (!mount) {
        std::fprintf(stderr, "cy_cook: could not mount '%s': %s\n", arguments.source,
                     mount.error().message);
        return 1;
    }
    if (const auto mounted = vfs.mount_owned(std::move(mount.value()), 0); !mounted) {
        std::fprintf(stderr, "cy_cook: could not mount '%s': %s\n", arguments.source,
                     mounted.error().message);
        return 1;
    }

    const Expected<assets::VirtualPath, Error> root = assets::VirtualPath::normalise("");
    const Expected<assets::VariantKey, Error> variant =
        assets::VariantKey::parse(arguments.variant);
    if (!root || !variant) {
        std::fprintf(stderr, "cy_cook: '%s' is not a valid variant key\n", arguments.variant);
        return 1;
    }

    cook::CookRequest request;
    request.source = root.value();
    request.world = &world;
    request.variant = variant.value();
    request.shipping = arguments.shipping;
    request.fail_on_conflicts = arguments.fail_on_conflicts;

    cook::CookRunReport report(allocator);
    if (const Status cooked = cook::run(vfs, request, arguments.output, report); !cooked) {
        std::fprintf(stderr, "cy_cook: %s\n", cooked.error().message);
        for (const AssetId id : report.cycle) {
            char text[AssetId::kTextLength + 1] = {};
            (void)id.format(text);
            std::fprintf(stderr, "  in the dependency chain: %s\n", text);
        }
        return 1;
    }

    std::printf("cy_cook: %u component type(s) registered from reflection\n", components);
    std::printf("cy_cook: %u document(s) read, %u cooked, %u entities\n", report.documents_read,
                report.documents_cooked, report.total_entities);
    std::printf("cy_cook: %u relationship(s) flattened, %u override conflict(s)\n",
                report.total_relationships_flattened, report.total_conflicts);
    std::printf("cy_cook: wrote %s\n", arguments.output);
    return 0;
}
