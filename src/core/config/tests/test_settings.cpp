// Layered typed configuration. Task 4.3.
//
// Each case is named after the scenario in `project-and-plugins` it exercises, or after the
// sentence in the requirement that has no scenario of its own.

#include <cy/core/config/settings.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using cy::config::ConfigLayer;
using cy::config::ConfigStore;
using cy::config::ResolvedSetting;
using cy::config::SettingSchema;
using cy::config::SettingType;
using cy::config::SettingValue;

constexpr const char* kProfiles[] = {"desktop", "mobile", "console"};

SettingSchema renderer_profile() {
    SettingSchema schema;
    schema.key = "renderer.profile";
    schema.type = SettingType::Enum;
    schema.default_value = SettingValue::from_string("desktop");
    schema.description = "Which renderer feature set to configure for";
    schema.enumerators = kProfiles;
    schema.enumerator_count = 3;
    return schema;
}

SettingSchema frames_in_flight() {
    SettingSchema schema;
    schema.key = "renderer.max_frames_in_flight";
    schema.type = SettingType::Int;
    schema.default_value = SettingValue::from_int(2);
    schema.minimum_int = 1;
    schema.maximum_int = 4;
    return schema;
}

SettingSchema validation_layers() {
    SettingSchema schema;
    schema.key = "renderer.validation";
    schema.type = SettingType::Bool;
    schema.default_value = SettingValue::from_bool(false);
    return schema;
}

struct Reported {
    std::string key;
    std::string value;
    ConfigLayer layer;
    bool from_default;
};

void collect(const char* key, const char* value, ConfigLayer layer, bool from_default, void* user) {
    static_cast<std::vector<Reported>*>(user)->push_back(Reported{key, value, layer, from_default});
}

}  // namespace

CY_TEST_CASE("configuration is schema-defined: an undeclared key cannot be read or written") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));

    const cy::Status unknown =
        store.set(ConfigLayer::Project, "renderer.pofile", SettingValue::from_string("mobile"));
    CY_CHECK_FALSE(unknown);
    CY_CHECK_EQ(unknown.error().code, cy::ErrorCode::NotFound);
    CY_CHECK_FALSE(store.resolve("renderer.pofile"));
    CY_CHECK_EQ(store.size(), 1U);
}

CY_TEST_CASE("configuration is typed: a value of the wrong type or outside its range is refused") {
    ConfigStore store;
    CY_REQUIRE(store.declare(frames_in_flight()));
    CY_REQUIRE(store.declare(renderer_profile()));

    CY_CHECK_EQ(store
                    .set(ConfigLayer::Project, "renderer.max_frames_in_flight",
                         SettingValue::from_string("three"))
                    .error()
                    .code,
                cy::ErrorCode::InvalidArgument);
    CY_CHECK_EQ(
        store.set(ConfigLayer::Project, "renderer.max_frames_in_flight", SettingValue::from_int(9))
            .error()
            .code,
        cy::ErrorCode::OutOfRange);
    CY_CHECK_EQ(store.set_from_text(ConfigLayer::Project, "renderer.max_frames_in_flight", "three")
                    .error()
                    .code,
                cy::ErrorCode::InvalidArgument);
    CY_CHECK_EQ(
        store.set_from_text(ConfigLayer::Project, "renderer.profile", "holographic").error().code,
        cy::ErrorCode::InvalidArgument);

    // The default survived every rejection: a refused write changes nothing.
    CY_CHECK_EQ(store.get_int("renderer.max_frames_in_flight").value(), 2);
    CY_CHECK(std::strcmp(store.get_string("renderer.profile").value(), "desktop") == 0);
}

CY_TEST_CASE("Platform override: the platform layer overrides the project's with no code change") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));

    CY_REQUIRE(store.set_from_text(ConfigLayer::Project, "renderer.profile", "desktop"));
    CY_CHECK(std::strcmp(store.get_string("renderer.profile").value(), "desktop") == 0);

    CY_REQUIRE(store.set_from_text(ConfigLayer::Platform, "renderer.profile", "mobile"));
    CY_CHECK(std::strcmp(store.get_string("renderer.profile").value(), "mobile") == 0);

    // The project's value is not gone, it is overridden: clearing the platform layer restores it.
    store.clear_layer(ConfigLayer::Platform);
    CY_CHECK(std::strcmp(store.get_string("renderer.profile").value(), "desktop") == 0);
}

CY_TEST_CASE("Where did this value come from: the effective value carries its layer") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));

    const ResolvedSetting untouched = store.resolve("renderer.profile").value();
    CY_CHECK_EQ(untouched.layer, ConfigLayer::EngineDefault);
    CY_CHECK(untouched.from_default);

    CY_REQUIRE(store.set_from_text(ConfigLayer::Project, "renderer.profile", "console"));
    CY_REQUIRE(store.set_from_text(ConfigLayer::User, "renderer.profile", "mobile"));
    const ResolvedSetting overridden = store.resolve("renderer.profile").value();
    CY_CHECK_EQ(overridden.layer, ConfigLayer::User);
    CY_CHECK_FALSE(overridden.from_default);
    CY_CHECK(std::strcmp(overridden.value.as_string().value(), "mobile") == 0);
}

CY_TEST_CASE("Local overrides do not ship: the User layer is not applied in a shipping build") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));
    CY_REQUIRE(store.set_from_text(ConfigLayer::Project, "renderer.profile", "console"));
    CY_REQUIRE(store.set_from_text(ConfigLayer::User, "renderer.profile", "mobile"));

    CY_CHECK(std::strcmp(store.get_string("renderer.profile").value(), "mobile") == 0);

    store.set_shipping(true);
    const ResolvedSetting shipped = store.resolve("renderer.profile").value();
    CY_CHECK(std::strcmp(shipped.value.as_string().value(), "console") == 0);
    CY_CHECK_EQ(shipped.layer, ConfigLayer::Project);
    // The refusal is reported rather than silent: a caller can see that a layer was suppressed.
    CY_CHECK(shipped.suppressed_by_shipping_policy);
    CY_CHECK_FALSE(store.layer_permitted_in_shipping(ConfigLayer::User));

    // The stored value is untouched — the policy decides resolution, not storage.
    store.set_shipping(false);
    CY_CHECK(std::strcmp(store.get_string("renderer.profile").value(), "mobile") == 0);
}

CY_TEST_CASE("a setting may declare that its user override does ship") {
    SettingSchema schema = validation_layers();
    schema.user_override_in_shipping = true;

    ConfigStore store;
    CY_REQUIRE(store.declare(schema));
    CY_REQUIRE(store.set(ConfigLayer::User, "renderer.validation", SettingValue::from_bool(true)));
    store.set_shipping(true);

    const ResolvedSetting resolved = store.resolve("renderer.validation").value();
    CY_CHECK(resolved.value.as_bool().value());
    CY_CHECK_EQ(resolved.layer, ConfigLayer::User);
    CY_CHECK_FALSE(resolved.suppressed_by_shipping_policy);
}

CY_TEST_CASE("the command line is the last layer, and only dotted keys are settings") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));
    CY_REQUIRE(store.declare(frames_in_flight()));
    CY_REQUIRE(store.declare(validation_layers()));
    CY_REQUIRE(store.set_from_text(ConfigLayer::User, "renderer.profile", "mobile"));

    const char* arguments[] = {
        "--headless",                  // an engine switch: untouched
        "--renderer.profile=console",  // --key=value
        "--renderer.max_frames_in_flight",
        "4",                      // --key value
        "--renderer.validation",  // a bare bool is true
        "--fixed-step",
        "120",  // an engine switch with its own value
    };
    CY_REQUIRE(store.apply_command_line(6, arguments));

    CY_CHECK(std::strcmp(store.get_string("renderer.profile").value(), "console") == 0);
    CY_CHECK_EQ(store.resolve("renderer.profile").value().layer, ConfigLayer::CommandLine);
    CY_CHECK_EQ(store.get_int("renderer.max_frames_in_flight").value(), 4);
    CY_CHECK(store.get_bool("renderer.validation").value());
}

CY_TEST_CASE("an unknown dotted key on the command line is reported, not ignored") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));

    const char* arguments[] = {"--renderer.porfile=mobile"};
    const cy::Status applied = store.apply_command_line(1, arguments);
    CY_CHECK_FALSE(applied);
    CY_CHECK_EQ(applied.error().code, cy::ErrorCode::NotFound);
}

CY_TEST_CASE("every setting reports its effective value and its layer") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));
    CY_REQUIRE(store.declare(frames_in_flight()));
    CY_REQUIRE(store.set_from_text(ConfigLayer::Platform, "renderer.profile", "mobile"));

    std::vector<Reported> reported;
    CY_CHECK_EQ(store.report(&collect, &reported), 2U);
    CY_REQUIRE_EQ(reported.size(), 2U);

    CY_CHECK_EQ(reported[0].key, std::string{"renderer.profile"});
    CY_CHECK_EQ(reported[0].value, std::string{"mobile"});
    CY_CHECK_EQ(reported[0].layer, ConfigLayer::Platform);
    CY_CHECK_FALSE(reported[0].from_default);

    CY_CHECK_EQ(reported[1].key, std::string{"renderer.max_frames_in_flight"});
    CY_CHECK_EQ(reported[1].value, std::string{"2"});
    CY_CHECK(reported[1].from_default);
}

CY_TEST_CASE("a setting is declared once, and an enum with no enumerators is refused") {
    ConfigStore store;
    CY_REQUIRE(store.declare(renderer_profile()));
    CY_CHECK_EQ(store.declare(renderer_profile()).error().code, cy::ErrorCode::AlreadyExists);

    SettingSchema empty;
    empty.key = "renderer.mode";
    empty.type = SettingType::Enum;
    empty.default_value = SettingValue::from_string("anything");
    CY_CHECK_EQ(store.declare(empty).error().code, cy::ErrorCode::InvalidArgument);

    SettingSchema unnamed;
    unnamed.type = SettingType::Bool;
    CY_CHECK_EQ(store.declare(unnamed).error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("layer names round-trip, and bool text is written the four ways people write it") {
    for (const ConfigLayer layer :
         {ConfigLayer::EngineDefault, ConfigLayer::Project, ConfigLayer::Platform,
          ConfigLayer::BuildConfiguration, ConfigLayer::User, ConfigLayer::CommandLine}) {
        const auto parsed =
            cy::config::config_layer_from_name(cy::config::config_layer_name(layer));
        CY_REQUIRE(parsed);
        CY_CHECK(parsed.value() == layer);
    }

    ConfigStore store;
    CY_REQUIRE(store.declare(validation_layers()));
    for (const char* text : {"true", "on", "1", "yes"}) {
        CY_REQUIRE(store.set_from_text(ConfigLayer::Project, "renderer.validation", text));
        CY_CHECK(store.get_bool("renderer.validation").value());
    }
    for (const char* text : {"false", "off", "0", "no"}) {
        CY_REQUIRE(store.set_from_text(ConfigLayer::Project, "renderer.validation", text));
        CY_CHECK_FALSE(store.get_bool("renderer.validation").value());
    }
    CY_CHECK_FALSE(store.set_from_text(ConfigLayer::Project, "renderer.validation", "maybe"));
}
