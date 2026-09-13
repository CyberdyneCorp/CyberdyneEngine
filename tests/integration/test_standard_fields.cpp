// EVERY FIELD EVERY PRODUCING MODULE DECLARES, INTO ONE `FieldRegistry`, IN BOTH ORDERS.
//
// The seam this suite is about is the one nothing owned. `environment-fields` lists the standard
// field names so that "two rows naming one quantity name one field", and `FieldRegistry::declare()`
// accepts an identical re-declaration and refuses a different one — so a disagreement between two
// modules about ONE name is a project that fails at startup, in either order, with a message about
// a field rather than about the two rows that disagree. Nothing in the tree composed the modules'
// declarations until this file, and so nothing noticed that two of them did disagree:
//
//   * `wind` — M10's own gate found it. `foliage::wind_field_declaration()` declared the standard
//     name `Presentation` while `weather::weather_field_declaration(WeatherField::Wind, …)`
//     declares it `Authoritative`. Three requirements settle it for weather — `weather-and-wind`'s
//     "wind affecting projectiles … SHALL be authoritative", `environment-fields`' "the producer of
//     the wind field is weather-and-wind", and weather's own `integration.weather_fields`, which
//     needs `FieldReader::open(wind, Authoritative)` to succeed. Foliage now READS; its declaration
//     is gone, and src/foliage/include/cy/foliage/wind.h records why in the space it left.
//   * `vegetation-potential` — FOUND BY THIS SUITE, still open, and reported by name below.
//
// **THE DECLARATIONS ARE NOT WRITTEN OUT HERE.** Each module is registered through the entry point
// a project calls, and what it declared is read back out of its own registry — so a field a module
// adds later joins this suite without anybody remembering to add it, which is the only version of
// this check that keeps working.

#include <cy/test/test.h>

#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/foliage/species.h>
#include <cy/foliage/system.h>
#include <cy/rendering/sky/cloud_shadows.h>
#include <cy/terrain/deform.h>
#include <cy/terrain/system.h>
#include <cy/terrain/tile.h>
#include <cy/water/shoreline.h>
#include <cy/weather/fields.h>

#include <cstring>

namespace {

using cy::environment::FieldDeclaration;
using cy::environment::FieldRecord;
using cy::environment::FieldRegistry;

[[nodiscard]] cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// The standard names `environment-fields` requires the engine to define. Spelled through the
/// substrate's own constants rather than as strings, so a rename moves this list with it.
constexpr const char* kStandardFields[] = {
    cy::environment::fields::kBiome,       cy::environment::fields::kMoisture,
    cy::environment::fields::kTemperature, cy::environment::fields::kSoil,
    cy::environment::fields::kSnowDepth,   cy::environment::fields::kWaterDistance,
    cy::environment::fields::kWaterDepth,  cy::environment::fields::kWaterFlow,
    cy::environment::fields::kWetness,     cy::environment::fields::kWind,
    cy::environment::fields::kBurnState,   cy::environment::fields::kHumanExclusion,
};

// --- The producing modules, each through the entry point a project calls ------------------------

using RegisterFn = cy::Status (*)(FieldRegistry&) noexcept;

/// Terrain: the registration a project calls, and then the public factory behind it.
///
/// BOTH, EVERYWHERE BELOW, and the second is not redundant. `declare()` accepts an identical
/// re-declaration, so a factory that still agrees with its own registration costs nothing here and
/// a factory that has DRIFTED from it is refused — which is the same defect as the cross-module
/// one, seen inside a single module. It is also the shape the `wind` defect had: a public factory a
/// project was invited to call, agreeing with nothing.
[[nodiscard]] cy::Status register_terrain(FieldRegistry& registry) noexcept {
    // The stores are the constructor's, not this check's: `register_producer()` declares and claims
    // `soil` and touches neither.
    cy::terrain::TileLayout layout;
    cy::terrain::TerrainStore store(allocator(), layout);
    cy::terrain::TerrainDeltaStore deltas(allocator(), layout);
    cy::terrain::TerrainSystem system(allocator(), store, deltas);
    if (cy::Status registered = system.register_producer(registry, "cy::terrain", 8.0F);
        !registered) {
        return registered;
    }
    return registry.declare(cy::terrain::soil_field_declaration(8.0F));
}

[[nodiscard]] cy::Status register_water(FieldRegistry& registry) noexcept {
    cy::water::WaterFieldOptions options;
    // THE COMPOSITION THE ENGINE SHIPS, and samples/10-world sets exactly this line. `wetness` has
    // one producer; water publishes the shore's contribution as `water-shore-wetness` for weather
    // to compose with precipitation. The third case below holds what happens when both rows claim
    // it instead, so that this line reads as a choice rather than as a way past a failure.
    options.wetness_owner = cy::water::WetnessOwner::External;
    cy::water::WaterFields fields(allocator());
    return fields.declare(registry, options);
}

[[nodiscard]] cy::Status register_weather(FieldRegistry& registry) noexcept {
    // The module's own defaults: the ecosystem half and the accumulation half both declared, which
    // is the widest set weather declares and therefore the one most likely to collide.
    const cy::weather::WeatherFieldOptions options;
    cy::weather::WeatherFields fields(allocator());
    if (cy::Status declared = fields.declare(registry, options); !declared) {
        return declared;
    }
    for (cy::u32 index = 0; index < cy::weather::kWeatherFieldCount; ++index) {
        const auto field = static_cast<cy::weather::WeatherField>(index);
        if (cy::Status declared =
                registry.declare(cy::weather::weather_field_declaration(field, options));
            !declared) {
            return declared;
        }
    }
    return cy::ok();
}

[[nodiscard]] cy::Status register_foliage(FieldRegistry& registry) noexcept {
    constexpr cy::f32 kMacroCellMetres = 512.0F;
    constexpr cy::f32 kRecoveryPerSecond = 1.0e-7F;
    cy::foliage::SpeciesLibrary library(allocator());
    cy::foliage::FoliageSystem system(allocator(), library);
    if (cy::Status registered =
            system.register_producer(registry, "cy::foliage", kMacroCellMetres, kRecoveryPerSecond);
        !registered) {
        return registered;
    }
    if (cy::Status declared =
            registry.declare(cy::foliage::vegetation_potential_declaration(kMacroCellMetres));
        !declared) {
        return declared;
    }
    return registry.declare(
        cy::foliage::vegetation_field_declaration(kMacroCellMetres, kRecoveryPerSecond));
}

[[nodiscard]] cy::Status register_sky(FieldRegistry& registry) noexcept {
    const cy::rendering::sky::CloudShadowQuality quality;
    cy::Expected<FieldDeclaration, cy::Error> declaration =
        cy::rendering::sky::cloud_shadow_declaration(quality);
    if (!declaration) {
        return cy::Status{cy::make_unexpected(declaration.error())};
    }
    return registry.declare(declaration.value());
}

struct Module {
    const char* name;
    RegisterFn register_fields;
};

/// Every module in the tree that declares a field into the substrate. Terrain, water, weather and
/// foliage are M10's producers; sky declares `cloud-shadow`. PCG writes fields through adapters and
/// declares none of its own, which is why it is absent.
constexpr Module kModules[] = {
    {"cy::terrain", register_terrain},   {"cy::water", register_water},
    {"cy::weather", register_weather},   {"cy::foliage", register_foliage},
    {"cy::rendering-sky", register_sky},
};

/// A field name in a form doctest will PRINT.
///
/// `CY_TEST_MESSAGE` and `CY_TEST_FAIL_CHECK` stringify a `const char*` through doctest's POINTER
/// path unless the build defines `DOCTEST_CONFIG_TREAT_CHAR_STAR_AS_STRING`, and this one does not:
/// the first run of this suite reported `the standard field '0x5eb8b68c3472'`. An array takes the
/// array path and prints its characters, so a name is copied into one before it is reported —
/// which matters most in the failure message, where the name is the whole of what a reader needs.
struct Printable {
    char text[64] = {};

    explicit Printable(const char* name) noexcept {
        cy::usize index = 0;
        for (; name[index] != '\0' && (index + 1) < sizeof(text); ++index) {
            text[index] = name[index];
        }
        text[index] = '\0';
    }
};

/// One declaration, and the module whose registration produced it.
struct Declared {
    const char* module;
    FieldDeclaration declaration;
};

/// Register every module into a registry of its own and copy out what each one declared.
///
/// A registry per module, because the whole question is what happens when they share one — asking
/// it of a shared registry would mean the second module's declarations were already filtered by the
/// first's. The copied `name` pointers are the modules' own literals and outlive everything here.
[[nodiscard]] cy::Status collect(cy::Array<Declared>& out) noexcept {
    for (const Module& module : kModules) {
        FieldRegistry registry(allocator());
        if (cy::Status registered = module.register_fields(registry); !registered) {
            return registered;
        }
        for (const FieldRecord& record : registry.records()) {
            if (cy::Status pushed = out.push_back(Declared{module.name, record.declaration});
                !pushed) {
                return pushed;
            }
        }
    }
    return cy::ok();
}

/// Declare `entries` into one fresh registry, forwards or backwards, and report the names refused.
///
/// BOTH DIRECTIONS MATTER. `declare()` keeps the first declaration and refuses the second, so a
/// check that only ran one way round would be a check about declaration order: it would report the
/// challenger's name today and the incumbent's tomorrow, and a reader would learn nothing about
/// which of the two is right.
[[nodiscard]] cy::Status declare_all(const cy::Array<Declared>& entries, bool forwards,
                                     cy::Array<const char*>& refused) noexcept {
    FieldRegistry registry(allocator());
    const auto count = static_cast<cy::usize>(entries.size());
    for (cy::usize step = 0; step < count; ++step) {
        const Declared& entry = entries[forwards ? step : (count - 1 - step)];
        if (cy::Status declared = registry.declare(entry.declaration); !declared) {
            if (cy::Status pushed = refused.push_back(entry.declaration.name); !pushed) {
                return pushed;
            }
        }
    }
    return cy::ok();
}

/// THE ONE NAME TWO MODULES STILL DISAGREE ABOUT, and the reason this suite names it rather than
/// asserting "no refusals" and going red.
///
/// `foliage::vegetation_potential_declaration()` declares it `UNorm8`, `Static`, one level,
/// `Persistent`; `weather::weather_field_declaration(WeatherField::VegetationPotential, …)`
/// declares it `UNorm16`, `SlowlyVarying`, three levels, `Authoritative`. No configuration
/// reconciles them, so a project that registers both rows fails at startup exactly as the `wind`
/// pair did. It is NOT the same fix: `wind` had a producer named by two specifications, while this
/// pair is two modules' answers to "what does vegetation recover toward" — weather's ecosystem
/// potential, and the potential of foliage's own realised instance density — and deciding whether
/// those are one quantity is a modelling decision across two rows rather than a repair.
///
/// Declared as `m10:fields-one-vegetation-potential`, closing at M11. **WHEN IT CLOSES THIS SUITE
/// GOES RED**, and the fix is to require no refusals at all rather than to move this string.
/// An ARRAY and not a `const char*`, which is not a style choice: this build does not define
/// `DOCTEST_CONFIG_TREAT_CHAR_STAR_AS_STRING`, so a pointer reaches `CY_TEST_MESSAGE` through
/// doctest's pointer stringifier and the summary line reports an ADDRESS. The first run of this
/// suite printed `0x62b70e186254` where the field's name belongs.
constexpr char kKnownDisagreement[] = "vegetation-potential";

}  // namespace

CY_TEST_CASE("the standard fields have one declaration between all the modules that declare them") {
    // `environment-fields` — the standard names exist "so that two rows naming one quantity name
    // one field". This is that sentence as a check: every declaration of a standard name, from
    // every module that makes one, into one registry, both ways round, all accepted.
    cy::Array<Declared> entries(allocator());
    CY_REQUIRE(collect(entries).has_value());
    CY_REQUIRE_FALSE(entries.empty());

    cy::u32 standard_names = 0;
    cy::u32 standard_declarations = 0;
    // COUNTED AND PRINTED, never spelled as a literal in the message. The M10 ledger records a
    // report that printed "four different producers" with `four` in the format string; a summary
    // line that cannot say a number it did not measure is the fix for that class of mistake.
    cy::u32 standard_refused = 0;
    for (const char* name : kStandardFields) {
        cy::Array<Declared> sharing(allocator());
        for (const Declared& entry : entries.span()) {
            if (std::strcmp(entry.declaration.name, name) == 0) {
                CY_REQUIRE(sharing.push_back(entry).has_value());
            }
        }
        if (sharing.empty()) {
            continue;
        }
        ++standard_names;
        standard_declarations += static_cast<cy::u32>(sharing.size());

        cy::Array<const char*> refused(allocator());
        CY_REQUIRE(declare_all(sharing, true, refused).has_value());
        CY_REQUIRE(declare_all(sharing, false, refused).has_value());
        standard_refused += static_cast<cy::u32>(refused.size());
        if (!refused.empty()) {
            // Named, because "a standard field was refused" is not actionable and "wind, declared
            // by cy::foliage and cy::weather" is.
            const Printable field(name);
            const Printable first(sharing[0].module);
            const Printable second(sharing[sharing.size() - 1].module);
            CY_TEST_FAIL_CHECK("the standard field '"
                               << field.text << "' is declared by " << sharing.size()
                               << " module(s) and they do not agree: " << first.text << " and "
                               << second.text);
        }
        CY_CHECK_EQ(refused.size(), 0u);
    }

    CY_TEST_MESSAGE("standard fields: ", standard_declarations, " declaration(s) of ",
                    standard_names, " standard name(s), refused ", standard_refused);
    // Twelve standard names exist; a tree that declared none of them would pass every check above
    // vacuously.
    CY_CHECK(standard_names >= 8u);
}

CY_TEST_CASE("every producing module's fields coexist in one registry, in either order") {
    // The whole composition, not just the standard half: a project registers all five of these and
    // must get a registry, not a refusal. Everything refused is named, and the one name that is
    // still refused is the declared gap `kKnownDisagreement` explains.
    cy::Array<Declared> entries(allocator());
    CY_REQUIRE(collect(entries).has_value());

    cy::Array<const char*> forwards(allocator());
    cy::Array<const char*> backwards(allocator());
    CY_REQUIRE(declare_all(entries, true, forwards).has_value());
    CY_REQUIRE(declare_all(entries, false, backwards).has_value());

    const auto unexpected_in = [](const cy::Array<const char*>& run) {
        cy::u32 unexpected = 0;
        for (const char* name : run.span()) {
            if (std::strcmp(name, kKnownDisagreement) != 0) {
                ++unexpected;
                const Printable field(name);
                CY_TEST_FAIL_CHECK("'" << field.text
                                       << "' is declared differently by two modules: a project "
                                          "using both rows fails at startup, in either order");
            }
        }
        return unexpected;
    };
    CY_CHECK_EQ(unexpected_in(forwards) + unexpected_in(backwards), 0u);

    // The known one, reported rather than hidden — and reported as a count so that a second field
    // joining it is a red test rather than a longer log line.
    CY_CHECK_EQ(forwards.size(), 1u);
    CY_CHECK_EQ(backwards.size(), 1u);
    CY_TEST_MESSAGE("composition: ", entries.size(), " declaration(s) from ",
                    sizeof(kModules) / sizeof(kModules[0]), " module(s); refused forwards ",
                    forwards.size(), ", backwards ", backwards.size(),
                    "; the only disagreement is '", kKnownDisagreement,
                    "' (m10:fields-one-vegetation-potential)");
}

CY_TEST_CASE(
    "two rows claiming one standard field is refused, which is why water declares an owner") {
    // `WetnessOwner` exists because `wetness` has two candidate producers, and the substrate is
    // what makes the choice compulsory. Held here so that the `External` line in `register_water()`
    // above reads as the composition it is: pick the other one and the engine refuses, which is the
    // design working rather than a bug to be smoothed over by making the two declarations match.
    cy::water::WaterFieldOptions owned;
    owned.wetness_owner = cy::water::WetnessOwner::Water;

    // Weather declaring `wetness` and water owning it: refused, by the substrate, because the two
    // declarations differ (UNorm16 against UNorm8). This is the failure the `wind` pair had, and
    // the difference is that here BOTH modules offer the option that resolves it.
    FieldRegistry contested(allocator());
    CY_REQUIRE(register_weather(contested).has_value());
    cy::water::WaterFields both(allocator());
    CY_CHECK_FALSE(both.declare(contested, owned).has_value());

    // The other supported composition, for symmetry: weather's accumulation half turned off, water
    // owning `wetness` outright. A project has two ways to answer the question and no way to leave
    // it unanswered, which is what makes the refusal above a design rather than a defect.
    FieldRegistry resolved(allocator());
    cy::weather::WeatherFieldOptions without_accumulation;
    without_accumulation.accumulation = false;
    cy::weather::WeatherFields weather(allocator());
    CY_REQUIRE(weather.declare(resolved, without_accumulation).has_value());
    cy::water::WaterFields water(allocator());
    CY_CHECK(water.declare(resolved, owned).has_value());
}
