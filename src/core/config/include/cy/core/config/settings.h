// Layered typed configuration.
//
// Task 4.3. `project-and-plugins`, "Layered typed configuration": settings are schema-defined and
// typed rather than an arbitrary string map, they resolve through declared layers each overriding
// the previous, the layers a shipping build permits are declared, and the effective value of any
// setting is inspectable **together with which layer supplied it**.
//
// FOUR DECISIONS WORTH KNOWING, because each is a place the obvious shortcut is wrong.
//
// 1. A SETTING MUST BE DECLARED BEFORE IT CAN BE SET. Writing an undeclared key is an error naming
//    it, not a new entry. A configuration system that accepts anything cannot validate anything,
//    cannot present anything in an editor, and cannot tell a typo from a feature.
//
// 2. THE LAYER IS PART OF THE ANSWER, NOT PART OF THE IMPLEMENTATION. resolve() returns the value
//    *and* the layer that supplied it, because "where did this value come from" is a scenario the
//    specification names, and a store that overwrote as it loaded could not answer it. Every
//    layer's value is kept; resolution picks.
//
// 3. THE SHIPPING POLICY IS DATA. Which layers a shipping build honours is a declared table, not a
//    condition scattered through the readers, so a developer-local override cannot alter shipping
//    behaviour and the rule is inspectable rather than inferred. `User` is excluded by default; a
//    setting may opt back in, one setting at a time, through its schema.
//
// 4. NO ALLOCATION AND NO STANDARD-LIBRARY CONTAINER. A ConfigStore is a fixed array plus a text
//    pool, so it is usable in the Core startup stage, before an allocator has been chosen — which
//    is exactly when the configuration that chooses one is read.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::config {

enum class SettingType : u8 {
    Bool,
    Int,
    Float,
    String,
    /// A string constrained to a declared set. Distinct from String because the set is what the
    /// editor presents and what validation rejects against.
    Enum,
};

const char* setting_type_name(SettingType type) noexcept;

/// The layers, lowest first. Each overrides the ones before it — `project-and-plugins` fixes both
/// the set and the order.
enum class ConfigLayer : u8 {
    EngineDefault = 0,
    Project,
    Platform,
    BuildConfiguration,
    /// Developer-local. Excluded from a shipping build unless the setting opts in.
    User,
    CommandLine,
};

inline constexpr usize kConfigLayerCount = 6;

const char* config_layer_name(ConfigLayer layer) noexcept;
Expected<ConfigLayer, Error> config_layer_from_name(const char* name);

/// A typed value. Strings are not owned: a value handed to ConfigStore is copied into its text
/// pool, and a value handed back points into that pool, which lives as long as the store.
class SettingValue {
public:
    SettingValue() = default;

    static SettingValue from_bool(bool value);
    static SettingValue from_int(i64 value);
    static SettingValue from_float(f64 value);
    static SettingValue from_string(const char* value);

    [[nodiscard]] SettingType type() const { return type_; }

    /// Each reader fails rather than converting: an Int read as a Bool is a defect in the caller,
    /// and a silent coercion is how a configuration system stops being typed. Deliberate
    /// conversions are the caller's, at the call site, where they are visible.
    [[nodiscard]] Expected<bool, Error> as_bool() const;
    [[nodiscard]] Expected<i64, Error> as_int() const;
    [[nodiscard]] Expected<f64, Error> as_float() const;
    /// Both String and Enum answer here: an enumerator is a string from a declared set.
    [[nodiscard]] Expected<const char*, Error> as_string() const;

    /// The value as text, into `buffer`. Always terminated. Used by the report and by the trace.
    void format(char* buffer, usize capacity) const;

private:
    SettingType type_ = SettingType::Bool;
    bool bool_ = false;
    i64 int_ = 0;
    f64 float_ = 0.0;
    const char* string_ = "";
};

/// What a setting is. Declared once, in the subsystem that owns the setting.
struct SettingSchema {
    /// Dotted, lower case: "renderer.profile". The dot is what tells a setting on the command line
    /// from an engine switch — see ConfigStore::apply_command_line().
    const char* key = nullptr;
    SettingType type = SettingType::Bool;
    SettingValue default_value;
    const char* description = "";

    /// For SettingType::Enum: the permitted spellings. A value outside them is rejected naming both
    /// the value and the set.
    const char* const* enumerators = nullptr;
    usize enumerator_count = 0;

    /// Inclusive bounds for Int and Float. The defaults admit everything.
    i64 minimum_int = -0x7FFFFFFFFFFFFFFF - 1;
    i64 maximum_int = 0x7FFFFFFFFFFFFFFF;
    f64 minimum_float = -1e308;
    f64 maximum_float = 1e308;

    /// Opt this one setting back into the User layer in a shipping build. Off by default:
    /// "a local developer override SHALL NOT be able to alter shipping behaviour unless explicitly
    /// allowed", and this is the explicit allowance.
    bool user_override_in_shipping = false;
};

/// Sized to be an ordinary member rather than an allocation, for the same reason the module
/// registry is: this is read in the Core startup stage, before an allocator exists.
inline constexpr usize kMaxSettings = 64;
inline constexpr usize kConfigTextBytes = 8192;

/// The effective value of a setting, and where it came from.
struct ResolvedSetting {
    SettingValue value;
    ConfigLayer layer = ConfigLayer::EngineDefault;
    /// True when no layer supplied a value and the schema's default is the answer.
    bool from_default = true;
    /// True when a layer *did* supply a value that the shipping policy refused. The value below is
    /// the one that won; this says a lower one was preferred deliberately.
    bool suppressed_by_shipping_policy = false;
};

/// What ConfigStore::report() hands to its caller, one setting at a time.
using ConfigReportFn = void (*)(const char* key, const char* value, ConfigLayer layer,
                                bool from_default, void* user);

class ConfigStore {
public:
    ConfigStore();

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    /// Declare a setting. Rejects a null or duplicate key, a default whose type is not the declared
    /// one, an Enum with no enumerators, and a default outside the declared bounds.
    Status declare(const SettingSchema& schema);

    [[nodiscard]] usize size() const { return count_; }
    [[nodiscard]] const SettingSchema* schema(const char* key) const;
    /// The declared settings, in declaration order, by index. Null past the end.
    [[nodiscard]] const SettingSchema* schema_at(usize index) const;

    /// Set a typed value in a layer. Fails naming the key when it is not declared, when the type
    /// does not match, or when the value is outside the schema's bounds or enumerator set.
    Status set(ConfigLayer layer, const char* key, const SettingValue& value);

    /// Set from text, parsed against the setting's declared type. This is the manifest's, the
    /// command line's and a configuration file's entry point: they all carry text and none of them
    /// carries a type.
    Status set_from_text(ConfigLayer layer, const char* key, const char* text);

    /// Apply the settings the project graph supplied — the manifest's own `settings`, and the
    /// per-platform override that applied to this build — into the Project and Platform layers.
    /// This is the seam that makes "the mobile platform layer overrides the renderer profile with
    /// no code change" true: nothing here knows which platform it is, the graph already resolved
    /// it.
    Status load_project_settings();

    /// Consume `--<dotted.key>=<value>` and `--<dotted.key> <value>` into the CommandLine layer.
    ///
    /// Only dotted keys are considered a setting, so `--headless` and `--fixed-step 120` pass
    /// through untouched: an engine switch and a setting are different things and the dot is what
    /// separates them. A dotted key that is *not* declared is an error naming it, because that is a
    /// typo the user wants to hear about rather than a switch for something else.
    Status apply_command_line(int argument_count, const char* const* arguments);

    void clear_layer(ConfigLayer layer);

    /// Whether this is a shipping build. Set it before loading any layer; it changes which layers
    /// resolve, not which are stored.
    void set_shipping(bool shipping) { shipping_ = shipping; }
    [[nodiscard]] bool shipping() const { return shipping_; }

    [[nodiscard]] bool layer_permitted_in_shipping(ConfigLayer layer) const;
    void set_layer_permitted_in_shipping(ConfigLayer layer, bool permitted);

    /// The effective value and the layer that supplied it.
    [[nodiscard]] Expected<ResolvedSetting, Error> resolve(const char* key) const;

    /// Typed reads, for call sites that know what they asked for. Each fails when the key is not
    /// declared or the type does not match.
    [[nodiscard]] Expected<bool, Error> get_bool(const char* key) const;
    [[nodiscard]] Expected<i64, Error> get_int(const char* key) const;
    [[nodiscard]] Expected<f64, Error> get_float(const char* key) const;
    [[nodiscard]] Expected<const char*, Error> get_string(const char* key) const;

    /// Every setting, in declaration order, with its effective value and its layer. Returns how
    /// many were reported. The sink is a function pointer rather than a container because this is
    /// read in the Core stage, and because the caller decides where it goes — a log line, a trace
    /// field, the editor's inspector.
    usize report(ConfigReportFn sink, void* user) const;

private:
    struct Entry {
        SettingSchema schema;
        SettingValue values[kConfigLayerCount];
        bool present[kConfigLayerCount];
    };

    [[nodiscard]] Entry* find(const char* key);
    [[nodiscard]] const Entry* find(const char* key) const;

    // Static because neither reads the store: both answer a question about one entry, and saying
    // so keeps them callable from declare(), which runs before the entry is part of the store.
    static Status validate(const Entry& entry, const SettingValue& value);
    static Expected<SettingValue, Error> parse(const Entry& entry, const char* text);
    /// Copy `text` into the pool and return the copy. Null when the pool is full.
    const char* intern(const char* text);
    [[nodiscard]] bool layer_resolves(const Entry& entry, ConfigLayer layer) const;

    Entry entries_[kMaxSettings] = {};
    usize count_ = 0;

    char text_[kConfigTextBytes] = {};
    usize text_used_ = 0;

    bool shipping_ = false;
    bool shipping_permits_[kConfigLayerCount] = {};
};

}  // namespace cy::config
