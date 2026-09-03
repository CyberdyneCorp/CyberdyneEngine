// The metadata table: names, categories and classified fields, registered once.
//
// Fixed capacity on purpose. Registration happens at a declaration site the first time it is
// reached, never in the emission path, and a table that cannot grow is a table that cannot allocate
// while a producer is running. An overflow is counted and reported through registry_stats() rather
// than resized into; the capacities below are an order of magnitude above what M0 through M6 will
// register, and raising one is a one-line change with a visible cost.

#include <cy/core/diagnostics/field.h>

#include <cstring>
#include <mutex>

namespace cy::diag {
namespace {

constexpr u32 kMaxNames = 4096;
constexpr u32 kMaxCategories = 256;
constexpr u32 kMaxFields = 1024;

struct FieldEntry {
    const char* name = nullptr;
    FieldType type = FieldType::UnsignedInteger;
    Privacy privacy = Privacy::Secret;  // the safe default: an entry never filled exports nothing
};

struct Registry {
    std::mutex mutex;
    const char* names[kMaxNames] = {};
    const char* categories[kMaxCategories] = {};
    FieldEntry fields[kMaxFields] = {};
    u32 name_count = 0;
    u32 category_count = 0;
    u32 field_count = 0;
    u32 rejected = 0;
};

/// A function-local static, so no other translation unit's static initialiser can reach a registry
/// that has not been constructed.
Registry& registry() noexcept {
    static Registry instance;
    return instance;
}

bool same_text(const char* a, const char* b) noexcept {
    return a == b || (a != nullptr && b != nullptr && std::strcmp(a, b) == 0);
}

/// Find `name` in a dense table of `count` entries, or append it. Ids are one-based; zero is the
/// invalid id, which is what an overflow returns.
u32 intern(const char** table, u32 capacity, u32& count, u32& rejected, const char* name) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (same_text(table[index], name)) {
            return index + 1;
        }
    }
    if (count == capacity) {
        ++rejected;
        return 0;
    }
    table[count] = name;
    ++count;
    return count;
}

}  // namespace

NameId register_name(const char* name) noexcept {
    if (name == nullptr) {
        return kInvalidName;
    }
    Registry& table = registry();
    const std::lock_guard<std::mutex> guard(table.mutex);
    return intern(table.names, kMaxNames, table.name_count, table.rejected, name);
}

CategoryId register_category(const char* name) noexcept {
    if (name == nullptr) {
        return kInvalidCategory;
    }
    Registry& table = registry();
    const std::lock_guard<std::mutex> guard(table.mutex);
    return intern(table.categories, kMaxCategories, table.category_count, table.rejected, name);
}

FieldId register_field(const char* name, FieldType type, Privacy privacy) noexcept {
    if (name == nullptr) {
        return kInvalidField;
    }
    Registry& table = registry();
    const std::lock_guard<std::mutex> guard(table.mutex);
    for (u32 index = 0; index < table.field_count; ++index) {
        if (same_text(table.fields[index].name, name)) {
            return index + 1;
        }
    }
    if (table.field_count == kMaxFields) {
        ++table.rejected;
        return kInvalidField;
    }
    table.fields[table.field_count] = FieldEntry{name, type, privacy};
    ++table.field_count;
    return table.field_count;
}

bool lookup_field(FieldId id, FieldInfo& out) noexcept {
    Registry& table = registry();
    const std::lock_guard<std::mutex> guard(table.mutex);
    if (id == kInvalidField || id > table.field_count) {
        return false;
    }
    const FieldEntry& entry = table.fields[id - 1];
    out = FieldInfo{entry.name, entry.type, entry.privacy};
    return true;
}

const char* lookup_name(NameId id) noexcept {
    Registry& table = registry();
    const std::lock_guard<std::mutex> guard(table.mutex);
    return (id == kInvalidName || id > table.name_count) ? nullptr : table.names[id - 1];
}

const char* lookup_category(CategoryId id) noexcept {
    Registry& table = registry();
    const std::lock_guard<std::mutex> guard(table.mutex);
    return (id == kInvalidCategory || id > table.category_count) ? nullptr
                                                                 : table.categories[id - 1];
}

RegistryStats registry_stats() noexcept {
    Registry& table = registry();
    const std::lock_guard<std::mutex> guard(table.mutex);
    return RegistryStats{table.name_count, table.category_count, table.field_count, table.rejected};
}

}  // namespace cy::diag
