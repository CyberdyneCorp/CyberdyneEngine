#include <cy/core/config/settings.h>

#include <cy/core/config/project.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cy::config {
namespace {

constexpr const char* kLayerNames[kConfigLayerCount] = {
    "EngineDefault", "Project", "Platform", "BuildConfiguration", "User", "CommandLine",
};

constexpr const char* kTypeNames[] = {"bool", "int", "float", "string", "enum"};

usize index_of(ConfigLayer layer) {
    return static_cast<usize>(layer);
}

bool layer_is_known(ConfigLayer layer) {
    return index_of(layer) < kConfigLayerCount;
}

bool equal(const char* left, const char* right) {
    return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

// "true"/"false", "on"/"off", "1"/"0", and "yes"/"no". Four spellings because configuration is
// written by hand and a build that rejects "on" teaches nothing except which word this parser
// wanted.
bool parse_bool(const char* text, bool& out) {
    static constexpr const char* kTrue[] = {"true", "on", "1", "yes"};
    static constexpr const char* kFalse[] = {"false", "off", "0", "no"};
    for (const char* candidate : kTrue) {
        if (equal(text, candidate)) {
            out = true;
            return true;
        }
    }
    for (const char* candidate : kFalse) {
        if (equal(text, candidate)) {
            out = false;
            return true;
        }
    }
    return false;
}

bool parse_int(const char* text, i64& out) {
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<i64>(value);
    return true;
}

bool parse_float(const char* text, f64& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<f64>(value);
    return true;
}

bool is_dotted_key(const char* text) {
    return std::strchr(text, '.') != nullptr;
}

}  // namespace

const char* setting_type_name(SettingType type) noexcept {
    const auto index = static_cast<usize>(type);
    return index < (sizeof(kTypeNames) / sizeof(kTypeNames[0])) ? kTypeNames[index] : "unknown";
}

const char* config_layer_name(ConfigLayer layer) noexcept {
    return layer_is_known(layer) ? kLayerNames[index_of(layer)] : "unknown";
}

Expected<ConfigLayer, Error> config_layer_from_name(const char* name) {
    if (name == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a configuration layer was named by a null pointer");
    }
    for (usize index = 0; index < kConfigLayerCount; ++index) {
        if (equal(name, kLayerNames[index])) {
            return static_cast<ConfigLayer>(index);
        }
    }
    return fail(ErrorCode::NotFound,
                "unknown configuration layer; they are EngineDefault, Project, Platform, "
                "BuildConfiguration, User, CommandLine");
}

// --- SettingValue --------------------------------------------------------------------------------

SettingValue SettingValue::from_bool(bool value) {
    SettingValue result;
    result.type_ = SettingType::Bool;
    result.bool_ = value;
    return result;
}

SettingValue SettingValue::from_int(i64 value) {
    SettingValue result;
    result.type_ = SettingType::Int;
    result.int_ = value;
    return result;
}

SettingValue SettingValue::from_float(f64 value) {
    SettingValue result;
    result.type_ = SettingType::Float;
    result.float_ = value;
    return result;
}

SettingValue SettingValue::from_string(const char* value) {
    SettingValue result;
    result.type_ = SettingType::String;
    result.string_ = value != nullptr ? value : "";
    return result;
}

Expected<bool, Error> SettingValue::as_bool() const {
    if (type_ != SettingType::Bool) {
        return fail(ErrorCode::InvalidArgument, "the setting is not a bool");
    }
    return bool_;
}

Expected<i64, Error> SettingValue::as_int() const {
    if (type_ != SettingType::Int) {
        return fail(ErrorCode::InvalidArgument, "the setting is not an int");
    }
    return int_;
}

Expected<f64, Error> SettingValue::as_float() const {
    if (type_ != SettingType::Float) {
        return fail(ErrorCode::InvalidArgument, "the setting is not a float");
    }
    return float_;
}

Expected<const char*, Error> SettingValue::as_string() const {
    if (type_ != SettingType::String && type_ != SettingType::Enum) {
        return fail(ErrorCode::InvalidArgument, "the setting is not a string or an enum");
    }
    return string_;
}

void SettingValue::format(char* buffer, usize capacity) const {
    if (buffer == nullptr || capacity == 0) {
        return;
    }
    switch (type_) {
        case SettingType::Bool:
            std::snprintf(buffer, capacity, "%s", bool_ ? "true" : "false");
            return;
        case SettingType::Int:
            std::snprintf(buffer, capacity, "%lld", static_cast<long long>(int_));
            return;
        case SettingType::Float:
            std::snprintf(buffer, capacity, "%g", static_cast<double>(float_));
            return;
        case SettingType::String:
        case SettingType::Enum:
            std::snprintf(buffer, capacity, "%s", string_);
            return;
    }
    buffer[0] = '\0';
}

// --- ConfigStore
// ----------------------------------------------------------------------------------

ConfigStore::ConfigStore() {
    // The shipping policy, as data. Everything the project and the build decide survives into a
    // shipping build; the developer's own layer does not.
    shipping_permits_[index_of(ConfigLayer::EngineDefault)] = true;
    shipping_permits_[index_of(ConfigLayer::Project)] = true;
    shipping_permits_[index_of(ConfigLayer::Platform)] = true;
    shipping_permits_[index_of(ConfigLayer::BuildConfiguration)] = true;
    shipping_permits_[index_of(ConfigLayer::User)] = false;
    shipping_permits_[index_of(ConfigLayer::CommandLine)] = true;
}

Status ConfigStore::declare(const SettingSchema& schema) {
    if (schema.key == nullptr || schema.key[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a setting was declared with no key");
    }
    if (find(schema.key) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "that setting key is already declared");
    }
    if (count_ == kMaxSettings) {
        return fail(ErrorCode::OutOfRange,
                    "the configuration store is full; raise cy::config::kMaxSettings");
    }
    if (schema.type == SettingType::Enum && schema.enumerator_count == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "an enum setting declares no enumerators, so no value could ever be valid");
    }

    Entry& entry = entries_[count_];
    entry.schema = schema;
    for (bool& present : entry.present) {
        present = false;
    }

    // The default is stored in the EngineDefault layer rather than kept beside it: resolution then
    // has one rule instead of a rule plus a fallback, and "which layer supplied it" answers
    // EngineDefault without a special case.
    SettingValue value = schema.default_value;
    if (value.type() == SettingType::String || value.type() == SettingType::Enum) {
        const auto text = value.as_string();
        const char* stored = intern(text.value());
        if (stored == nullptr) {
            return fail(ErrorCode::OutOfMemory, "the configuration text pool is full");
        }
        value = SettingValue::from_string(stored);
    }
    Status valid = validate(entry, value);
    if (!valid) {
        return valid;
    }
    entry.values[index_of(ConfigLayer::EngineDefault)] = value;
    entry.present[index_of(ConfigLayer::EngineDefault)] = true;
    ++count_;
    return ok();
}

const SettingSchema* ConfigStore::schema(const char* key) const {
    const Entry* entry = find(key);
    return entry != nullptr ? &entry->schema : nullptr;
}

const SettingSchema* ConfigStore::schema_at(usize index) const {
    return index < count_ ? &entries_[index].schema : nullptr;
}

ConfigStore::Entry* ConfigStore::find(const char* key) {
    return const_cast<Entry*>(static_cast<const ConfigStore*>(this)->find(key));
}

const ConfigStore::Entry* ConfigStore::find(const char* key) const {
    if (key == nullptr) {
        return nullptr;
    }
    for (usize index = 0; index < count_; ++index) {
        if (equal(entries_[index].schema.key, key)) {
            return &entries_[index];
        }
    }
    return nullptr;
}

// The type check and the constraint check, in the one place both a typed set and a parsed one
// reach.
Status ConfigStore::validate(const Entry& entry, const SettingValue& value) {
    const SettingSchema& schema = entry.schema;
    const bool textual = schema.type == SettingType::String || schema.type == SettingType::Enum;
    const bool value_is_textual =
        value.type() == SettingType::String || value.type() == SettingType::Enum;
    if (textual != value_is_textual || (!textual && value.type() != schema.type)) {
        return fail(ErrorCode::InvalidArgument,
                    "the value's type is not the setting's declared type");
    }

    if (schema.type == SettingType::Int) {
        const i64 number = value.as_int().value();
        if (number < schema.minimum_int || number > schema.maximum_int) {
            return fail(ErrorCode::OutOfRange, "the value is outside the setting's declared range");
        }
    }
    if (schema.type == SettingType::Float) {
        const f64 number = value.as_float().value();
        if (number < schema.minimum_float || number > schema.maximum_float) {
            return fail(ErrorCode::OutOfRange, "the value is outside the setting's declared range");
        }
    }
    if (schema.type == SettingType::Enum) {
        const char* text = value.as_string().value();
        for (usize index = 0; index < schema.enumerator_count; ++index) {
            if (equal(schema.enumerators[index], text)) {
                return ok();
            }
        }
        return fail(ErrorCode::InvalidArgument,
                    "the value is not one of the setting's declared enumerators");
    }
    return ok();
}

const char* ConfigStore::intern(const char* text) {
    const char* source = text != nullptr ? text : "";
    const usize length = std::strlen(source) + 1;
    if (text_used_ + length > kConfigTextBytes) {
        return nullptr;
    }
    char* destination = text_ + text_used_;
    std::memcpy(destination, source, length);
    text_used_ += length;
    return destination;
}

Status ConfigStore::set(ConfigLayer layer, const char* key, const SettingValue& value) {
    if (!layer_is_known(layer)) {
        return fail(ErrorCode::InvalidArgument, "the configuration layer is outside the six");
    }
    Entry* entry = find(key);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound,
                    "no setting of that key is declared; a configuration key is declared by the "
                    "subsystem that owns it before anything may set it");
    }

    SettingValue stored = value;
    if (value.type() == SettingType::String || value.type() == SettingType::Enum) {
        const char* text = intern(value.as_string().value());
        if (text == nullptr) {
            return fail(ErrorCode::OutOfMemory, "the configuration text pool is full");
        }
        stored = SettingValue::from_string(text);
    }

    Status valid = validate(*entry, stored);
    if (!valid) {
        return valid;
    }
    entry->values[index_of(layer)] = stored;
    entry->present[index_of(layer)] = true;
    return ok();
}

Expected<SettingValue, Error> ConfigStore::parse(const Entry& entry, const char* text) {
    if (text == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the value is a null pointer");
    }
    switch (entry.schema.type) {
        case SettingType::Bool: {
            bool value = false;
            if (!parse_bool(text, value)) {
                return fail(ErrorCode::InvalidArgument,
                            "the value is not a bool; write true, false, on, off, 1 or 0");
            }
            return SettingValue::from_bool(value);
        }
        case SettingType::Int: {
            i64 value = 0;
            if (!parse_int(text, value)) {
                return fail(ErrorCode::InvalidArgument, "the value is not a whole number");
            }
            return SettingValue::from_int(value);
        }
        case SettingType::Float: {
            f64 value = 0.0;
            if (!parse_float(text, value)) {
                return fail(ErrorCode::InvalidArgument, "the value is not a number");
            }
            return SettingValue::from_float(value);
        }
        case SettingType::String:
        case SettingType::Enum:
            return SettingValue::from_string(text);
    }
    return fail(ErrorCode::Internal, "the setting has no declared type");
}

Status ConfigStore::set_from_text(ConfigLayer layer, const char* key, const char* text) {
    const Entry* entry = find(key);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound,
                    "no setting of that key is declared; an unknown configuration entry is "
                    "reported rather than ignored");
    }
    const auto parsed = parse(*entry, text);
    if (!parsed) {
        return fail(parsed.error().code, parsed.error().message, parsed.error().system_code);
    }
    return set(layer, key, parsed.value());
}

Status ConfigStore::load_project_settings() {
    for (const ProjectSetting& setting : project().settings) {
        const auto layer = config_layer_from_name(setting.layer);
        if (!layer) {
            return fail(layer.error().code, layer.error().message);
        }
        const Status applied = set_from_text(layer.value(), setting.key, setting.value);
        if (!applied) {
            return applied;
        }
    }
    return ok();
}

Status ConfigStore::apply_command_line(int argument_count, const char* const* arguments) {
    if (argument_count > 0 && arguments == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the argument vector is null");
    }
    for (int index = 0; index < argument_count; ++index) {
        const char* argument = arguments[index];
        if (argument == nullptr || argument[0] != '-' || argument[1] != '-') {
            continue;
        }
        const char* body = argument + 2;

        // "--key=value": the key is everything before the '=' and must fit the scratch buffer. A
        // longer key than this is not a key anyone typed.
        const char* equals = std::strchr(body, '=');
        char key[128];
        const char* value = nullptr;
        if (equals != nullptr) {
            const auto length = static_cast<usize>(equals - body);
            if (length == 0 || length >= sizeof(key)) {
                continue;
            }
            std::memcpy(key, body, length);
            key[length] = '\0';
            value = equals + 1;
        } else {
            if (std::strlen(body) >= sizeof(key)) {
                continue;
            }
            std::snprintf(key, sizeof(key), "%s", body);
        }

        if (!is_dotted_key(key)) {
            continue;  // an engine switch, not a setting
        }
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return fail(ErrorCode::NotFound,
                        "the command line names a setting that is not declared; a dotted key on "
                        "the command line is a setting, and an unknown one is reported rather "
                        "than ignored");
        }
        if (value == nullptr) {
            // "--flag.enabled" with nothing after it is true for a bool and needs a value for
            // anything else, which is the only shape where omitting one is unambiguous.
            if (entry->schema.type == SettingType::Bool) {
                const Status applied =
                    set(ConfigLayer::CommandLine, key, SettingValue::from_bool(true));
                if (!applied) {
                    return applied;
                }
                continue;
            }
            if (index + 1 >= argument_count || arguments[index + 1] == nullptr) {
                return fail(ErrorCode::InvalidArgument,
                            "the command line names a setting with no value");
            }
            ++index;
            value = arguments[index];
        }
        const Status applied = set_from_text(ConfigLayer::CommandLine, key, value);
        if (!applied) {
            return applied;
        }
    }
    return ok();
}

void ConfigStore::clear_layer(ConfigLayer layer) {
    if (!layer_is_known(layer) || layer == ConfigLayer::EngineDefault) {
        // The default layer is the schema, not an override: clearing it would leave a declared
        // setting with no value at all.
        return;
    }
    for (usize index = 0; index < count_; ++index) {
        entries_[index].present[index_of(layer)] = false;
    }
}

bool ConfigStore::layer_permitted_in_shipping(ConfigLayer layer) const {
    return layer_is_known(layer) && shipping_permits_[index_of(layer)];
}

void ConfigStore::set_layer_permitted_in_shipping(ConfigLayer layer, bool permitted) {
    if (layer_is_known(layer)) {
        shipping_permits_[index_of(layer)] = permitted;
    }
}

bool ConfigStore::layer_resolves(const Entry& entry, ConfigLayer layer) const {
    if (!entry.present[index_of(layer)]) {
        return false;
    }
    if (!shipping_) {
        return true;
    }
    if (shipping_permits_[index_of(layer)]) {
        return true;
    }
    return layer == ConfigLayer::User && entry.schema.user_override_in_shipping;
}

Expected<ResolvedSetting, Error> ConfigStore::resolve(const char* key) const {
    const Entry* entry = find(key);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "no setting of that key is declared");
    }

    ResolvedSetting resolved;
    bool suppressed = false;
    for (usize index = kConfigLayerCount; index > 0; --index) {
        const auto layer = static_cast<ConfigLayer>(index - 1);
        if (layer_resolves(*entry, layer)) {
            resolved.value = entry->values[index - 1];
            resolved.layer = layer;
            resolved.from_default = layer == ConfigLayer::EngineDefault;
            resolved.suppressed_by_shipping_policy = suppressed;
            return resolved;
        }
        if (entry->present[index - 1]) {
            suppressed = true;  // a layer had a value and the shipping policy refused it
        }
    }
    return fail(ErrorCode::Internal,
                "a declared setting has no value in any layer, not even its default");
}

Expected<bool, Error> ConfigStore::get_bool(const char* key) const {
    const auto resolved = resolve(key);
    if (!resolved) {
        return fail(resolved.error().code, resolved.error().message);
    }
    return resolved.value().value.as_bool();
}

Expected<i64, Error> ConfigStore::get_int(const char* key) const {
    const auto resolved = resolve(key);
    if (!resolved) {
        return fail(resolved.error().code, resolved.error().message);
    }
    return resolved.value().value.as_int();
}

Expected<f64, Error> ConfigStore::get_float(const char* key) const {
    const auto resolved = resolve(key);
    if (!resolved) {
        return fail(resolved.error().code, resolved.error().message);
    }
    return resolved.value().value.as_float();
}

Expected<const char*, Error> ConfigStore::get_string(const char* key) const {
    const auto resolved = resolve(key);
    if (!resolved) {
        return fail(resolved.error().code, resolved.error().message);
    }
    return resolved.value().value.as_string();
}

usize ConfigStore::report(ConfigReportFn sink, void* user) const {
    if (sink == nullptr) {
        return 0;
    }
    usize reported = 0;
    for (usize index = 0; index < count_; ++index) {
        const auto resolved = resolve(entries_[index].schema.key);
        if (!resolved) {
            continue;
        }
        char text[128];
        resolved.value().value.format(text, sizeof(text));
        sink(entries_[index].schema.key, text, resolved.value().layer,
             resolved.value().from_default, user);
        ++reported;
    }
    return reported;
}

}  // namespace cy::config
