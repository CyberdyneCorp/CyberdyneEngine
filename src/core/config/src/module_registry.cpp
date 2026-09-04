#include <cy/core/config/module_registry.h>

#include <cy/core/config/project.h>

#include <cstring>

namespace cy::config {
namespace {

constexpr const char* kLevelNames[kModuleLevelCount] = {"Core", "Servers", "Scene", "Editor"};

bool level_is_known(ModuleLevel level) {
    return static_cast<usize>(level) < kModuleLevelCount;
}

usize index_of(ModuleLevel level) {
    return static_cast<usize>(level);
}

// The whole of the ordering rule, in one place. Level first, then the name's bytes: a byte
// comparison rather than a locale-aware one, so two platforms agree.
bool sorts_before(const ModuleRegistration& left, const ModuleRegistration& right) {
    if (left.level != right.level) {
        return index_of(left.level) < index_of(right.level);
    }
    return std::strcmp(left.name, right.name) < 0;
}

}  // namespace

const char* module_level_name(ModuleLevel level) noexcept {
    return level_is_known(level) ? kLevelNames[index_of(level)] : "unknown";
}

Expected<ModuleLevel, Error> module_level_from_name(const char* name) {
    if (name == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a registration level was named by a null pointer");
    }
    for (usize index = 0; index < kModuleLevelCount; ++index) {
        if (std::strcmp(name, kLevelNames[index]) == 0) {
            return static_cast<ModuleLevel>(index);
        }
    }
    return fail(ErrorCode::NotFound,
                "unknown registration level; it is one of Core, Servers, Scene, Editor");
}

// --- Membership ----------------------------------------------------------------------------------

Status ModuleRegistry::add(const ModuleRegistration& registration) {
    if (registration.name == nullptr || registration.name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a module registration has no name");
    }
    if (!level_is_known(registration.level)) {
        return fail(ErrorCode::InvalidArgument,
                    "a module registration names a level outside Core, Servers, Scene, Editor");
    }
    if (find(registration.name) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "a module of that name is already registered; module names are the project "
                    "graph's identifiers and are unique");
    }
    if (count_ == kMaxRegisteredModules) {
        return fail(ErrorCode::OutOfRange,
                    "the module registry is full; raise cy::config::kMaxRegisteredModules");
    }
    for (const bool running : started_) {
        if (running) {
            return fail(ErrorCode::Unavailable,
                        "a module cannot be added while a registration level is running: the order "
                        "is decided before startup, not during it");
        }
    }

    // Insertion sort into the ordered array. The registry is a handful of entries and this runs
    // once at startup, so the order being obvious is worth more than the comparison count.
    usize position = 0;
    while (position < count_ && sorts_before(modules_[position], registration)) {
        ++position;
    }
    for (usize index = count_; index > position; --index) {
        modules_[index] = modules_[index - 1];
    }
    modules_[position] = registration;
    ++count_;
    return ok();
}

Status ModuleRegistry::add_project_modules() {
    for (const ProjectModule& module : project().modules) {
        ModuleRegistration registration;
        registration.name = module.name;
        registration.level = module.level;
        const Status added = add(registration);
        if (!added) {
            return added;
        }
    }
    return ok();
}

Status ModuleRegistry::bind(const char* name, ModuleRegisterFn on_register,
                            ModuleUnregisterFn on_unregister, void* user) {
    if (name == nullptr) {
        return fail(ErrorCode::InvalidArgument, "bind() was given a null module name");
    }
    for (usize index = 0; index < count_; ++index) {
        if (std::strcmp(modules_[index].name, name) == 0) {
            modules_[index].on_register = on_register;
            modules_[index].on_unregister = on_unregister;
            modules_[index].user = user;
            return ok();
        }
    }
    return fail(ErrorCode::NotFound,
                "no module of that name is in the registry; the project graph decides what is, so "
                "either the manifest does not declare it or the name is misspelled");
}

std::span<const ModuleRegistration> ModuleRegistry::modules() const {
    return {modules_, count_};
}

const ModuleRegistration* ModuleRegistry::find(const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }
    for (usize index = 0; index < count_; ++index) {
        if (std::strcmp(modules_[index].name, name) == 0) {
            return &modules_[index];
        }
    }
    return nullptr;
}

// --- Bringing a level up and down
// --------------------------------------------------------------------

Status ModuleRegistry::start(ModuleLevel level) {
    if (!level_is_known(level)) {
        return fail(ErrorCode::InvalidArgument, "start() names a level outside the four");
    }
    if (started_[index_of(level)]) {
        return fail(ErrorCode::AlreadyExists, "that registration level is already running");
    }
    for (usize lower = 0; lower < index_of(level); ++lower) {
        if (!started_[lower]) {
            return fail(ErrorCode::Unavailable,
                        "a registration level cannot start before the levels below it: they come "
                        "up in the order Core, Servers, Scene, Editor");
        }
    }

    for (usize index = 0; index < count_; ++index) {
        const ModuleRegistration& registration = modules_[index];
        if (registration.level != level) {
            continue;
        }
        if (registration.on_register != nullptr) {
            const Status registered = registration.on_register(registration.user);
            if (!registered) {
                // Leave the level as it was found: unregister, in reverse, exactly what this call
                // registered. `engine-architecture`: a failing stage unwinds what it built.
                for (usize done = index; done > 0; --done) {
                    const ModuleRegistration& earlier = modules_[done - 1];
                    if (earlier.level == level && earlier.on_unregister != nullptr) {
                        earlier.on_unregister(earlier.user);
                    }
                    if (earlier.level == level) {
                        record_stop(earlier.name);
                    }
                }
                return registered;
            }
        }
        record_start(registration.name);
    }

    started_[index_of(level)] = true;
    return ok();
}

void ModuleRegistry::stop(ModuleLevel level) {
    if (!level_is_known(level) || !started_[index_of(level)]) {
        return;
    }
    for (usize higher = index_of(level) + 1; higher < kModuleLevelCount; ++higher) {
        if (started_[higher]) {
            // A higher level is still running, so this one is not free to go. Silent rather than
            // fatal: shutdown is the path where nothing is left to report to, and the runtime's
            // own journal check is what catches an order that has drifted.
            return;
        }
    }

    for (usize index = count_; index > 0; --index) {
        const ModuleRegistration& registration = modules_[index - 1];
        if (registration.level != level) {
            continue;
        }
        if (registration.on_unregister != nullptr) {
            registration.on_unregister(registration.user);
        }
        record_stop(registration.name);
    }
    started_[index_of(level)] = false;
}

bool ModuleRegistry::level_started(ModuleLevel level) const {
    return level_is_known(level) && started_[index_of(level)];
}

// --- The journals --------------------------------------------------------------------------------

void ModuleRegistry::record_start(const char* name) {
    if (start_count_ == kJournalCapacity) {
        overflowed_ = true;
        return;
    }
    start_journal_[start_count_] = name;
    ++start_count_;
}

void ModuleRegistry::record_stop(const char* name) {
    if (stop_count_ == kJournalCapacity) {
        overflowed_ = true;
        return;
    }
    stop_journal_[stop_count_] = name;
    ++stop_count_;
}

std::span<const char* const> ModuleRegistry::start_journal() const {
    return {start_journal_, start_count_};
}

std::span<const char* const> ModuleRegistry::stop_journal() const {
    return {stop_journal_, stop_count_};
}

bool ModuleRegistry::journal_is_reversed() const {
    if (overflowed_ || stop_count_ != start_count_) {
        return false;
    }
    for (usize index = 0; index < stop_count_; ++index) {
        if (std::strcmp(stop_journal_[index], start_journal_[start_count_ - 1 - index]) != 0) {
            return false;
        }
    }
    return true;
}

void ModuleRegistry::clear_journals() {
    start_count_ = 0;
    stop_count_ = 0;
    overflowed_ = false;
}

}  // namespace cy::config
