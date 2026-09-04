// samples/01-headless-host — M1's closing artefact. Tasks 5.1 and 5.2.
//
// One program that loads a package from the virtual filesystem, runs a parallel job graph over
// reflected data, reports its memory budget tree, and shuts down deterministically. Every service
// M1 built is exercised here TOGETHER, because the seams between them are the thing this milestone
// has that its parts do not:
//
//   identity      the package holds records addressed by TypeId and FieldId, and the stage binds
//                 the same identifiers to accessors. A rename upstream touches neither.
//   memory        every allocation is attributed to a domain, and the budget tree at the end is the
//                 apportionment compared against what was actually taken.
//   jobs          the load reads on the async service, the decode is an indexed parallel loop, and
//                 the frame's parallelism is DERIVED from what its systems declared they touch.
//   assets        a real `.cypak`, mounted at a priority, resolved through one namespace.
//   project       the run's settings come from a typed, layered configuration, and the command line
//                 is the highest layer rather than a special case.
//
//   just run-sample headless-host                      the defaults: 4096 entities, 8 frames
//   just run-sample headless-host --host.frames=64     any declared setting, from the command line
//   just run-sample headless-host --content <dir>      keep the package where you can look at it
//   just run-sample headless-host --help               the switches and every setting
//
// THE HOST OWNS THE ORDER. Six services come up in dependency order and go down in the exact
// reverse, and `Services` below records both so that the claim is evidence rather than a comment —
// tests/smoke/test_headless_host.cpp compares the two lines across a hundred processes. It is a
// stack, not a table: the teardown order cannot be edited out of agreement with the startup order,
// because there is only one order written down.

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/file.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/config/project.h>
#include <cy/core/config/settings.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/diagnostics.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/budget.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/control_plane.h>
#include <cy/core/reflect/reflect.h>

#include "content.h"
#include "simulation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using cy::u32;
using cy::u64;
using cy::usize;

constexpr const char* kTag = "01-headless-host";

// --- The settings this sample declares -----------------------------------------------------------
//
// Typed and schema-declared, not a string map. A dotted key on the command line is a setting and an
// undeclared one is an error naming it, which is what separates `--host.frames 4` from `--content`.

const char* const kProfileNames[] = {"desktop", "handheld", "server"};

cy::Status declare_settings(cy::config::ConfigStore& settings) noexcept {
    using cy::config::SettingSchema;
    using cy::config::SettingType;
    using cy::config::SettingValue;

    SettingSchema entities;
    entities.key = "host.entities";
    entities.type = SettingType::Int;
    entities.default_value = SettingValue::from_int(4096);
    entities.description = "how many entities the package carries";
    entities.minimum_int = 1;
    entities.maximum_int = 1 << 20;

    SettingSchema blocks;
    blocks.key = "host.blocks";
    blocks.type = SettingType::Int;
    blocks.default_value = SettingValue::from_int(8);
    blocks.description = "how many package entries the entities are divided into";
    blocks.minimum_int = 1;
    blocks.maximum_int = 256;

    SettingSchema frames;
    frames.key = "host.frames";
    frames.type = SettingType::Int;
    frames.default_value = SettingValue::from_int(8);
    frames.description = "how many frames the stage runs";
    frames.minimum_int = 1;
    frames.maximum_int = 100000;

    SettingSchema profile;
    profile.key = "host.memory-profile";
    profile.type = SettingType::Enum;
    profile.default_value = SettingValue::from_string("desktop");
    profile.description = "which platform apportionment the budget tree is built from";
    profile.enumerators = kProfileNames;
    profile.enumerator_count = sizeof(kProfileNames) / sizeof(kProfileNames[0]);

    SettingSchema verify;
    verify.key = "host.verify-hashes";
    verify.type = SettingType::Bool;
    verify.default_value = SettingValue::from_bool(false);
    verify.description = "check every payload against its recorded content hash as it loads";

    for (const SettingSchema& schema : {entities, blocks, frames, profile, verify}) {
        if (cy::Status declared = settings.declare(schema); !declared) {
            return declared;
        }
    }
    return cy::ok();
}

// --- The engine switches -------------------------------------------------------------------------

struct Options {
    const char* content = nullptr;  // null: a directory under the system temporary directory
    bool keep_content = false;
    bool help = false;
};

void print_usage() {
    std::fputs(
        "samples/01-headless-host — loads a package, runs a job graph over reflected data,\n"
        "reports its memory budgets, and shuts down in the exact reverse of its startup.\n"
        "\n"
        "  --content <dir>   cook the package here (default: under the temporary directory)\n"
        "  --keep-content    do not remove the content directory on exit\n"
        "  --help            this text, and the settings below\n"
        "\n"
        "Settings are typed and take the highest configuration layer from the command line:\n"
        "  --host.entities <n>          entities in the package        (default 4096)\n"
        "  --host.blocks <n>            package entries to divide them into (default 8)\n"
        "  --host.frames <n>            frames to run                  (default 8)\n"
        "  --host.memory-profile <p>    desktop | handheld | server    (default desktop)\n"
        "  --host.verify-hashes         check content hashes on load\n",
        stderr);
}

/// Returns false when the command line is not one this sample understands, having said so. Dotted
/// keys are left alone: the configuration store consumes those.
bool parse_options(int argument_count, char** arguments, Options& options) {
    for (int i = 1; i < argument_count; ++i) {
        const std::string_view argument{arguments[i]};
        const bool has_value = i + 1 < argument_count;

        if (argument == "--help") {
            print_usage();
            options.help = true;
            return true;
        }
        if (argument == "--keep-content") {
            options.keep_content = true;
        } else if (argument == "--content" && has_value) {
            options.content = arguments[++i];
        } else if (argument.starts_with("--") && argument.find('.') != std::string_view::npos) {
            // A setting. `ConfigStore::apply_command_line` has already been given the whole vector;
            // skip the value that a `--key value` spelling puts here.
            if (argument.find('=') == std::string_view::npos && has_value) {
                ++i;
            }
        } else {
            std::fprintf(stderr, "%s: unrecognised argument '%s'\n\n", kTag, arguments[i]);
            print_usage();
            return false;
        }
    }
    return true;
}

/// Where the package is cooked when the caller named no directory. Under the system temporary
/// directory, because the sample writes it and removes it again.
const char* default_content_directory(char* buffer, usize capacity) {
    const char* base = std::getenv("TMPDIR");
    if (base == nullptr) {
        base = std::getenv("TEMP");
    }
    if (base == nullptr) {
        base = "/tmp";
    }
    const usize length = std::strlen(base);
    const bool trailing = length > 0 && (base[length - 1] == '/' || base[length - 1] == '\\');
    std::snprintf(buffer, capacity, "%s%scyberdyne-01-headless-host", base, trailing ? "" : "/");
    return buffer;
}

void report(const char* label, const cy::Error& error) {
    std::fprintf(stderr, "%s: %s failed: %s (%s)\n", kTag, label, error.message,
                 cy::error_code_name(error.code));
}

// --- The service stack ---------------------------------------------------------------------------

/// Services brought up in dependency order and taken down in the exact reverse.
///
/// One order, written once. A stack rather than two lists, because two lists can disagree and the
/// disagreement is exactly the defect `engine-architecture`'s deterministic-shutdown requirement
/// exists to prevent. Both journals are kept so the property can be printed and compared.
class Services {
public:
    static constexpr usize kCapacity = 8;

    using Down = void (*)(void* user) noexcept;

    /// Record `name` as started, with how to stop it. Only called after the service really is up.
    void started(const char* name, Down down, void* user) noexcept {
        if (depth_ >= kCapacity) {
            overflowed_ = true;
            return;
        }
        entries_[depth_] = Entry{name, down, user};
        ++depth_;
    }

    /// Stop everything, innermost first.
    void tear_down() noexcept {
        while (depth_ > 0) {
            --depth_;
            const Entry& entry = entries_[depth_];
            if (entry.down != nullptr) {
                entry.down(entry.user);
            }
            stopped_[stopped_count_] = entry.name;
            ++stopped_count_;
        }
    }

    /// "memory reflect jobs ...", in the order given. `stopped` asks for the teardown journal.
    void format(bool stopped, char* buffer, usize capacity) const noexcept {
        const usize count = stopped ? stopped_count_ : depth_;
        usize used = 0;
        for (usize index = 0; index < count && used + 1 < capacity; ++index) {
            const char* name = stopped ? stopped_[index] : entries_[index].name;
            used += static_cast<usize>(
                std::snprintf(buffer + used, capacity - used, "%s%s", index == 0 ? "" : " ", name));
        }
        buffer[used < capacity ? used : capacity - 1] = '\0';
    }

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

private:
    struct Entry {
        const char* name = "";
        Down down = nullptr;
        void* user = nullptr;
    };

    Entry entries_[kCapacity] = {};
    const char* stopped_[kCapacity] = {};
    usize depth_ = 0;
    usize stopped_count_ = 0;
    bool overflowed_ = false;
};

// --- The workload --------------------------------------------------------------------------------

/// The objects the workload builds and the report afterwards reads.
///
/// Declared in `main` so that they outlive the services torn down after the report: the checksum is
/// printed from the stage, and the stage's world must still exist when it is.
struct Workload {
    explicit Workload(cy::Allocator& allocator) noexcept : world(allocator) {}

    sample::World world;
    sample::Bindings bindings;
    sample::Stage stage;
};

/// Everything between the services being up and the report: size the world, resolve the field
/// bindings, load and decode the package, build the schedule, run the frames.
///
/// One function rather than five `if (exit_code == 0)` blocks threading a flag through `main`. Each
/// phase reports its own failure, because the phase is the useful half of the message and a single
/// caller could only say "the workload failed". Returns false having already said which phase and
/// why; `main` prints its report either way, since a partial run is exactly when the budget tree
/// and the counters are worth reading.
bool run_workload(Workload& work, cy::assets::AssetSystem& assets, cy::jobs::JobSystem& jobs,
                  const cy::reflect::TypeRegistry& types, const sample::Layout& layout,
                  u64 frame_count) {
    if (cy::Status sized = work.world.resize(layout.entities); !sized) {
        report("world", sized.error());
        return false;
    }

    // The last reflected lookups this program makes. Everything per entity after them is an offset,
    // which is the control-plane rule kept structurally rather than by discipline.
    auto bindings = sample::Bindings::resolve(types);
    if (!bindings) {
        report("field bindings", bindings.error());
        return false;
    }
    work.bindings = bindings.value();

    const auto load = sample::load_and_decode(assets, jobs, types, layout, work.world);
    if (!load) {
        report("load", load.error());
        return false;
    }
    std::fprintf(stdout, "%s: loaded   assets=%u bytes=%llu records=%llu partitions=%llu\n", kTag,
                 load.value().assets, static_cast<unsigned long long>(load.value().bytes),
                 static_cast<unsigned long long>(load.value().records),
                 static_cast<unsigned long long>(load.value().partitions));

    if (cy::Status built = work.stage.build(work.world, work.bindings); !built) {
        report("schedule", built.error());
        return false;
    }
    char plan[256];
    work.stage.format_plan(plan, sizeof(plan));
    std::fprintf(stdout, "%s: schedule systems=%u batches=%u  %s\n", kTag,
                 work.stage.system_count(), work.stage.batch_count(), plan);

    u64 retired = 0;
    for (u64 frame = 0; frame < frame_count; ++frame) {
        jobs.begin_frame(frame);
        const auto result = work.stage.run(jobs);
        if (!result) {
            report("frame", result.error());
            return false;
        }
        retired = result.value().retired;
        assets.update();
    }
    std::fprintf(
        stdout, "%s: frames   %llu run, %llu entities retired through the command buffer\n", kTag,
        static_cast<unsigned long long>(frame_count), static_cast<unsigned long long>(retired));
    return true;
}

// --- The report ----------------------------------------------------------------------------------

void report_budgets(const cy::BudgetTree& budgets, const char* profile_name) {
    cy::MemoryDomain worst = cy::MemoryDomain::Engine;
    const cy::f64 peak = budgets.peak_utilisation(worst);
    std::fprintf(stdout, "%s: budget   profile=%s worst=%s at %.2f%% of its budget\n", kTag,
                 profile_name, cy::domain_name(worst), peak * 100.0);

    cy::BudgetRow rows[cy::kMemoryDomainCount];
    const u32 count = budgets.report(rows, cy::kMemoryDomainCount);
    for (u32 index = 0; index < count; ++index) {
        const cy::BudgetRow& row = rows[index];
        if (row.live_bytes == 0 && row.peak_bytes == 0) {
            continue;  // budgeted and untouched: a row of zeroes teaches nothing
        }
        std::fprintf(
            stdout,
            "%s: budget   %-10s %-4s target %6llu MiB  live %8llu B  peak %8llu B  %5.2f%%%s\n",
            kTag, cy::domain_name(row.domain), cy::budget_kind_name(row.kind),
            static_cast<unsigned long long>(row.budget / (usize{1024} * 1024)),
            static_cast<unsigned long long>(row.live_bytes),
            static_cast<unsigned long long>(row.peak_bytes), row.utilisation * 100.0,
            row.over_budget ? "  OVER BUDGET" : "");
    }
}

}  // namespace

int main(int argument_count, char** arguments) {
    Options options;
    if (!parse_options(argument_count, arguments, options)) {
        return 2;
    }
    if (options.help) {
        return 0;
    }

    // The configuration, lowest layer to highest. Nothing below reads argv again.
    cy::config::ConfigStore settings;
    if (cy::Status declared = declare_settings(settings); !declared) {
        report("settings", declared.error());
        return 1;
    }
    if (cy::Status loaded = settings.load_project_settings(); !loaded) {
        report("project settings", loaded.error());
        return 1;
    }
    if (cy::Status applied = settings.apply_command_line(argument_count, arguments); !applied) {
        report("command line", applied.error());
        return 2;
    }

    const auto entities = settings.get_int("host.entities");
    const auto blocks = settings.get_int("host.blocks");
    const auto frames = settings.get_int("host.frames");
    const auto profile_name = settings.get_string("host.memory-profile");
    const auto verify = settings.get_bool("host.verify-hashes");
    if (!entities || !blocks || !frames || !profile_name || !verify) {
        report("settings", entities ? blocks.error() : entities.error());
        return 1;
    }

    const u64 frame_count = static_cast<u64>(frames.value());
    sample::Layout layout;
    layout.entities = static_cast<u32>(entities.value());
    layout.blocks = static_cast<u32>(blocks.value());
    layout.blocks = std::min(layout.blocks, layout.entities);

    char content_storage[512];
    const char* content = options.content != nullptr
                              ? options.content
                              : default_content_directory(content_storage, sizeof(content_storage));
    char package_path[640];
    std::snprintf(package_path, sizeof(package_path), "%s/content.cypak", content);

    // The engine's own services. Each is constructed here and started below, so the objects outlive
    // the stack that orders them and nothing is destroyed while another service still holds it.
    cy::BudgetTree& budgets = cy::default_budget_tree();
    cy::reflect::TypeRegistry& types = cy::reflect::default_registry();
    cy::jobs::JobSystem jobs;
    cy::jobs::AsyncService async;
    cy::assets::VirtualFileSystem files;
    cy::assets::AssetSystem assets;
    Services services;

    // MEMORY. The apportionment first, because it is what everything after it is measured against,
    // and because a tree whose children out-subscribe their parent is a configuration error that
    // must be found at startup rather than at the moment an allocation fails.
    const cy::MemoryProfile* profile = cy::find_memory_profile(profile_name.value());
    if (profile == nullptr) {
        std::fprintf(stderr, "%s: no memory profile named '%s'\n", kTag, profile_name.value());
        return 1;
    }
    if (cy::Status applied = budgets.apply(*profile); !applied) {
        report("memory budgets", applied.error());
        return 1;
    }
    services.started(
        "memory", [](void* user) noexcept { static_cast<cy::BudgetTree*>(user)->clear(); },
        &budgets);

    // REFLECT. An explicit call, never a static initialiser: static initialisation order is link
    // order, which is the non-determinism this sample's last line claims to have removed.
    if (cy::Status registered = cy::reflect::register_generated_types(types); !registered) {
        report("type registry", registered.error());
        services.tear_down();
        return 1;
    }
    services.started(
        "reflect",
        [](void* user) noexcept { static_cast<cy::reflect::TypeRegistry*>(user)->clear(); },
        &types);

    cy::jobs::JobSystemConfig job_config;
    if (cy::Status started = jobs.start(job_config); !started) {
        report("job system", started.error());
        services.tear_down();
        return 1;
    }
    services.started(
        "jobs", [](void* user) noexcept { static_cast<cy::jobs::JobSystem*>(user)->shutdown(); },
        &jobs);

    if (cy::Status started = async.start(jobs); !started) {
        report("async service", started.error());
        services.tear_down();
        return 1;
    }
    services.started(
        "async", [](void* user) noexcept { static_cast<cy::jobs::AsyncService*>(user)->stop(); },
        &async);

    // VFS. The package is cooked here because the cooker is M2; everything after this line reaches
    // it through the namespace and knows nothing about where the bytes came from.
    if (cy::Status made = cy::assets::fs::create_directories(content); !made) {
        report("content directory", made.error());
        services.tear_down();
        return 1;
    }
    const auto package = sample::cook(package_path, layout);
    if (!package) {
        report("cook", package.error());
        services.tear_down();
        return 1;
    }
    if (cy::Status mounted = sample::mount(files, package_path); !mounted) {
        report("mount", mounted.error());
        services.tear_down();
        return 1;
    }
    services.started(
        "vfs",
        [](void* user) noexcept {
            static_cast<cy::assets::VirtualFileSystem*>(user)->unmount_all();
        },
        &files);

    cy::assets::AssetSystemConfig asset_config;
    asset_config.verify_content_hashes = verify.value();
    if (cy::Status started = assets.start(jobs, async, files, asset_config); !started) {
        report("asset system", started.error());
        services.tear_down();
        return 1;
    }
    services.started(
        "assets",
        [](void* user) noexcept { static_cast<cy::assets::AssetSystem*>(user)->shutdown(); },
        &assets);

    char journal[256];
    services.format(false, journal, sizeof(journal));
    std::fprintf(stdout, "%s: startup  %s\n", kTag, journal);

    const auto profile_layer = settings.resolve("host.memory-profile");
    std::fprintf(stdout,
                 "%s: settings entities=%u blocks=%u frames=%llu memory-profile=%s (from %s)\n",
                 kTag, layout.entities, layout.blocks, static_cast<unsigned long long>(frame_count),
                 profile_name.value(),
                 profile_layer ? cy::config::config_layer_name(profile_layer.value().layer) : "?");
    std::fprintf(stdout, "%s: project  %s %s, %zu modules declared, manifest %s\n", kTag,
                 cy::config::project().name, cy::config::project().version,
                 cy::config::project().modules.size(),
                 cy::config::project().manifest_present ? "present" : "absent");
    std::fprintf(
        stdout,
        "%s: package  entries=%u chunks=%u records=%llu B  file=%llu B  deduplicated=%llu B\n",
        kTag, package.value().entries, package.value().chunks,
        static_cast<unsigned long long>(package.value().payload_bytes),
        static_cast<unsigned long long>(package.value().file_bytes),
        static_cast<unsigned long long>(package.value().deduplicated_bytes));

    // The entity arrays are the ECS domain's, which is what makes the budget row below say
    // something rather than being a table of zeroes.
    Workload work{cy::system_allocator(cy::MemoryDomain::Ecs)};
    const int exit_code = run_workload(work, assets, jobs, types, layout, frame_count) ? 0 : 1;

    const cy::assets::AssetSystemStats asset_stats = assets.stats();
    const cy::jobs::JobSystemStats job_stats = jobs.stats();
    std::fprintf(stdout,
                 "%s: assets   loaded=%llu coalesced=%llu placeholders=%llu resident=%zu (%zu B)\n",
                 kTag, static_cast<unsigned long long>(asset_stats.loads_completed),
                 static_cast<unsigned long long>(asset_stats.loads_coalesced),
                 static_cast<unsigned long long>(asset_stats.placeholders_served),
                 asset_stats.resident_assets, asset_stats.resident_bytes);
    // SUBMITTED, NOT EXECUTED, and the difference is the whole reason this line is reproducible.
    // `tasks_executed` is incremented AFTER the release store that unblocks the wait, so a host
    // that waits for the last task and reads the counter in the next statement can legitimately
    // miss it — `JobSystemStats` says so at its declaration ("internally consistent to within a
    // task or two"), and it is an undercount of exactly one about once in two hundred runs on a
    // loaded machine. `tasks_submitted` is incremented before the body runs, so it is ordered
    // before that release store and is exact for every task this program waited on. Every task
    // here is waited on, so the two figures name the same work and only this one can be compared
    // across a hundred processes.
    std::fprintf(stdout, "%s: jobs     workers=%u tasks=%llu\n", kTag, jobs.worker_count(),
                 static_cast<unsigned long long>(job_stats.tasks_submitted));
    report_budgets(budgets, profile_name.value());
    std::fprintf(stdout, "%s: reflect  types=%zu, %llu reflected lookups inside a hot region\n",
                 kTag, types.size(),
                 static_cast<unsigned long long>(cy::reflect::control_plane_violations()));
    if (exit_code == 0) {
        std::fprintf(stdout, "%s: checksum %016llx\n", kTag,
                     static_cast<unsigned long long>(work.stage.checksum()));
    }

    // The teardown. Not a second list: the same stack, unwound.
    services.tear_down();
    services.format(true, journal, sizeof(journal));
    std::fprintf(stdout, "%s: shutdown %s\n", kTag, journal);

    if (!options.keep_content) {
        (void)cy::assets::fs::remove_directory_recursive(content);
    }
    std::fprintf(stdout, "%s: exit %d (%s)\n", kTag, exit_code,
                 exit_code == 0 ? "clean" : "failed");
    return exit_code;
}
