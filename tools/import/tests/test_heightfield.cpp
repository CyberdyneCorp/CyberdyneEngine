#include <cy/import/heightfield.h>
#include <cy/test/test.h>

#include <algorithm>
#include <ranges>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::u8;

namespace {

bool reported(const ImportResult& result, std::string_view code) {
    return std::ranges::any_of(result.diagnostics(), [code](const ImportDiagnostic& diagnostic) {
        return std::string_view(diagnostic.code) == code;
    });
}

ImportOptions declared_options() {
    ImportOptions options;
    const OptionsSchema schema = heightfield_options();
    CY_REQUIRE(options.set(schema, "resolution-x", OptionValue::of_int(65)).has_value());
    CY_REQUIRE(options.set(schema, "resolution-z", OptionValue::of_int(65)).has_value());
    CY_REQUIRE(
        options.set(schema, "sample-format", OptionValue::of_enumeration("u16-le")).has_value());
    CY_REQUIRE(options.set(schema, "sample-metres", OptionValue::of_float(2.0)).has_value());
    CY_REQUIRE(options.set(schema, "height-min-metres", OptionValue::of_float(-100.0)).has_value());
    CY_REQUIRE(options.set(schema, "height-max-metres", OptionValue::of_float(900.0)).has_value());
    return options;
}

ImportResult run(const std::vector<u8>& bytes, const ImportOptions* options) {
    HeightfieldImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("terrain/island.r16").value();
    request.bytes = cy::Span<const u8>(bytes.data(), bytes.size());
    request.options = options;
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

}  // namespace

CY_TEST_CASE("heightfield: raw input refuses to guess dimensions format units or range") {
    std::vector<u8> bytes(65U * 65U * 2U, 0);
    ImportResult result = run(bytes, nullptr);
    CY_CHECK(result.has_errors());
    CY_CHECK(reported(result, "heightfield-metadata-required"));
    CY_CHECK(result.assets().empty());
}

CY_TEST_CASE("heightfield: explicit metadata produces a versioned tiled terrain asset") {
    std::vector<u8> bytes(65U * 65U * 2U, 0);
    bytes[2] = 0xFF;
    bytes[3] = 0xFF;
    ImportOptions options = declared_options();
    ImportResult result = run(bytes, &options);
    CY_CHECK(!result.has_errors());
    CY_REQUIRE(result.assets().size() == 1);
    CY_CHECK(result.assets()[0].kind == cy::assets::AssetKind::Terrain);
    CY_CHECK(result.assets()[0].view() == "terrain");
    CY_CHECK(sub_asset_kind_from_name(result.assets()[0].view()) == cy::assets::AssetKind::Terrain);
    auto header = read_cooked_heightfield(result.assets()[0].payload.span());
    CY_REQUIRE(header.has_value());
    CY_CHECK(header.value().width == 65);
    CY_CHECK(header.value().height == 65);
    CY_CHECK(header.value().tile_quads == 64);
    CY_CHECK(header.value().tiles_x == 1);
    CY_CHECK(header.value().tiles_z == 1);
    CY_CHECK(header.value().sample_metres == 2.0F);
    CY_CHECK(header.value().height_min_metres == -100.0F);
    CY_CHECK(header.value().height_max_metres == 900.0F);
}

CY_TEST_CASE("heightfield: inconsistent range resolution tiling and bytes are structured errors") {
    std::vector<u8> bytes(65U * 65U * 2U, 0);
    const OptionsSchema schema = heightfield_options();

    ImportOptions range = declared_options();
    CY_REQUIRE(range.set(schema, "height-max-metres", OptionValue::of_float(-100.0)).has_value());
    CY_CHECK(reported(run(bytes, &range), "heightfield-range"));

    ImportOptions tiling = declared_options();
    CY_REQUIRE(tiling.set(schema, "resolution-x", OptionValue::of_int(66)).has_value());
    CY_CHECK(reported(run(bytes, &tiling), "heightfield-tiling"));

    ImportOptions size = declared_options();
    bytes.pop_back();
    CY_CHECK(reported(run(bytes, &size), "heightfield-size"));
}
