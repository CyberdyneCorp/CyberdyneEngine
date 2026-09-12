#pragma once
// The world M10 built, assembled from the seven modules that build it. samples/10-world.
//
// ================================================================================================
// WHAT THIS CLASS IS, AND WHY IT IS ONE CLASS
// ================================================================================================
//
// M10 landed seven modules — `src/environment/`, `src/pcg/`, `src/terrain/`, `src/water/`,
// `src/foliage/`, `src/weather/` and the atmosphere half of `src/rendering/sky/` — and each was
// reached only from its own suite. This class is what removes that sentence from the tree: ONE
// object that names all seven, in one translation unit, and the seams between them either line up
// or this program does not run. samples/09b-animated-character's CMakeLists.txt makes the identical
// argument about the four bodies of work M8.d landed, and this file only follows it.
//
// The arrow between them is the SUBSTRATE, and that is the milestone's own claim:
//
//   cy::pcg        generates an elevation raster and a scattered point set, region by region, with
//                  the four properties M10's spike made binding (design.md §1.6).
//   cy::terrain    cooks that raster into tiles, and CLAIMS the `soil` field.
//   cy::water      floats an ocean on it and CLAIMS `water-depth`, `water-distance`, `water-flow`
//                  and `water-shore-wetness`.
//   cy::weather    runs a climate over it and CLAIMS `wind`, `temperature`, `wetness`,
//                  `snow-depth`, `precipitation-rate` and twelve more.
//   cy::foliage    READS `soil`, `water-distance`, `moisture`, `temperature` and
//                  `vegetation-density` back out of the substrate to decide where a tree stands,
//                  and reads `wind` to make it move.
//   cy::rendering::sky  takes weather's own `CloudDrive` as its cloud state and produces the light
//                  everything above is shaded by.
//
// NOTHING HERE TALKS TO ANOTHER MODULE DIRECTLY except through the substrate or through a declared
// seam (`water::BedSource` is a callback into terrain; `weather::TerrainProfile` is another). That
// is not an accident of this file: `cy::weather` does not link `cy::water`, `cy::water` does not
// link `cy::terrain`, and `cy::pcg` links neither `cy::foliage` nor `cy::ecs`. The wiring has to
// happen somewhere, and an artefact is where.
//
// ================================================================================================
// WHAT IT DOES NOT CLAIM
// ================================================================================================
//
// Every producer in this world runs on the PROCESSOR. M10 shipped no `.slang` module for a field,
// for a terrain material, for a water surface, for grass expansion or for a cloud march — six
// separate agents each recorded that gap against their own row, and this artefact does not close
// any of them. What the renderer beside this file draws is geometry and per-vertex colour that
// these modules produced on the CPU. README.md says so at length and so does the report the program
// prints, because a picture that implied otherwise would be the worst outcome available to it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/placement.h>
#include <cy/foliage/species.h>
#include <cy/foliage/wind.h>
#include <cy/pcg/adapters.h>
#include <cy/pcg/execute.h>
#include <cy/pcg/foliage_adapter.h>
#include <cy/pcg/program.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/tables.h>
#include <cy/terrain/meshing.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/system.h>
#include <cy/terrain/tile.h>
#include <cy/water/ocean.h>
#include <cy/water/system.h>
#include <cy/weather/climate.h>
#include <cy/weather/system.h>
#include <cy/world/cell.h>

namespace cy::sample::world {

using cy::world::WorldVec3d;

/// How big the world is and what it is made of. Everything a caller may change.
struct WorldOptions {
    /// The world seed. Every draw every module below makes is a substream of it, so two runs of
    /// this program produce the same world — which is what makes the video regenerable rather than
    /// a recording of one afternoon.
    u64 seed = 0x10'0D'5E'ED'C0DEULL;
    /// Regions per side of the generated world. A region is 64 m (`pcg::kRegionCells` cells of
    /// 4 m), so 24 is a 1 536 m square — the same 24x24 grid M10's spike measured its answer on.
    i32 region_edge = 24;
    /// Still-water level, absolute metres. The generated elevation is biased so that a real
    /// fraction of the world falls below it and there is a shore for water to have an opinion
    /// about.
    f64 sea_level = 0.0;
    /// Seconds of wall time one simulated day takes. The whole cycle is filmed, so this is the
    /// take's own length.
    f32 seconds_per_day = 24.0F;
    /// Where in the day the take starts, as a fraction. 0 is midnight, 0.5 noon.
    f32 day_start = 0.18F;
    /// Frames per second of the take. One frame is one simulated tick of everything in this world,
    /// so there is no wall clock anywhere in the loop and two runs produce the same world at the
    /// same frame index — the same reproducibility argument samples/09b-animated-character makes.
    f32 take_fps = 30.0F;
    /// How many foliage regions on a side are generated. Clamped to the world.
    i32 foliage_edge = 24;
    /// Cloud march steps. The quality lever, carried here because the sky's cost across a day is
    /// what task 7.3 asks to be measured as a curve.
    u32 cloud_steps = 24;
    /// Sky dome vertices are coloured by `sky::compose_sky()`, which marches clouds per direction.
    /// This is the dome's resolution in rings and segments.
    u32 sky_rings = 56;
    u32 sky_segments = 112;
};

/// What building the world produced. Counted, not claimed: every number here is read back off the
/// module that produced it, and `main.cpp` prints all of them.
struct BuildReport {
    // --- procedural content generation
    u32 pcg_regions = 0;
    u64 pcg_region_evaluations = 0;
    u32 pcg_stages = 0;
    u64 pcg_points = 0;
    /// What the output adapter turned those points into. `procedural-content-generation`'s own
    /// requirement, counted rather than described.
    u64 pcg_boulders = 0;
    u32 pcg_boulder_clusters = 0;
    f64 pcg_millis = 0.0;
    u64 program_digest = 0;
    /// Whether the compiled program may be cached. False for a budgeted program, which M10's spike
    /// showed is the one axis with no reproducing configuration anywhere.
    bool pcg_cacheable = false;

    // --- terrain
    u32 terrain_tiles = 0;
    u64 terrain_bytes = 0;
    u32 terrain_triangles = 0;
    u32 terrain_stitched_vertices = 0;
    f32 terrain_min_height = 0.0F;
    f32 terrain_max_height = 0.0F;
    f64 terrain_millis = 0.0;

    // --- water
    f32 sea_significant_height = 0.0F;
    f32 sea_peak_wavelength = 0.0F;
    u32 sea_cascades = 0;
    u32 sea_authoritative_bands = 0;
    u32 sea_visual_bands = 0;
    u64 shoreline_points = 0;

    // --- foliage
    u32 foliage_clusters = 0;
    u64 foliage_instances = 0;
    u64 foliage_bytes = 0;
    u32 foliage_species = 0;
    f64 foliage_millis = 0.0;

    // --- the substrate
    u32 fields_declared = 0;
    u32 fields_claimed = 0;
    u64 field_bytes = 0;

    // --- sky
    u64 sky_directions_integrated = 0;
    f64 sky_tables_millis = 0.0;
};

/// What one frame of the world cost, in milliseconds, per producer.
///
/// M10 tasks.md 7.3 asks for the environment demo's budget "measured as a CURVE across the cycle
/// rather than asserted at one time of day", and this struct is one point of that curve. It is
/// filled by `advance()` from a monotonic clock; `main.cpp` writes every frame's to a CSV and
/// `tools/docs/collect_world.py` plots it.
struct FrameCosts {
    f64 weather_ms = 0.0;
    /// Publishing weather's fields into the substrate. Separated from the simulation because
    /// `weather-and-wind` declares two different budgets over exactly these two numbers.
    f64 fields_ms = 0.0;
    /// The water simulation: the spectrum re-derived from the wind, the clock, and the foam
    /// field's advection and decay over its whole grid.
    f64 water_ms = 0.0;
    /// Regenerating the camera-relative ocean patch. Separated because it is the half that scales
    /// with the patch's vertex count and the other is the half that scales with the foam grid, and
    /// a budget curve that could not tell them apart would send a reader to the wrong lever.
    f64 ocean_ms = 0.0;
    f64 sky_ms = 0.0;
    /// Re-sampling the substrate for every terrain vertex's colour — the consumer side of wetness
    /// and snow, and the reason a terrain goes white when it snows.
    f64 terrain_shade_ms = 0.0;
    f64 foliage_ms = 0.0;

    [[nodiscard]] f64 total() const noexcept {
        return weather_ms + fields_ms + water_ms + ocean_ms + sky_ms + terrain_shade_ms +
               foliage_ms;
    }
};

/// One terrain tile, meshed once and re-shaded every frame.
///
/// `mesh.positions` are LOCAL to the tile's minimum corner (`terrain::TerrainMesh`'s own
/// convention); `origin` is that corner in absolute metres. The colours are recomputed each frame
/// from the environment fields, which is what makes snow and wetness visible.
struct TerrainPatch {
    terrain::TerrainMesh mesh;
    WorldVec3d origin;
    Array<Vec3> colours;

    TerrainPatch(Allocator& allocator) noexcept : mesh(allocator), colours(allocator) {}

    TerrainPatch(const TerrainPatch&) = delete;
    TerrainPatch& operator=(const TerrainPatch&) = delete;
    TerrainPatch(TerrainPatch&&) noexcept = default;
    TerrainPatch& operator=(TerrainPatch&&) noexcept = default;
    ~TerrainPatch() = default;
};

/// One plant, as the renderer needs it: where it stands, how big it is, which way it faces, what
/// colour it is, and how far the wind has pushed its crown this frame.
///
/// The wind offset is `foliage::evaluate_response()`'s own answer for a vertex at the top of the
/// canopy. `foliage` requires that response to be evaluated ON THE GPU as part of geometry
/// processing; this artefact evaluates it on the processor, which is the gap src/foliage/'s README
/// records against its own row. What is true here is that the function, the per-cluster
/// `ClusterWind` it reads and the wind field behind it are the shipped ones.
/// Which of the three shapes a plant proxy is drawn as. `foliage::SpeciesClass` decides, because a
/// class is what the species declaration already says and a fourth field would be a second place to
/// keep in step: a `Scatter` species is a boulder, an `Understory` one a bush, and everything else
/// a tree. Without this the generator's own boulders were drawn as conifers, which is what the
/// first version of this artefact's still actually showed.
enum class PlantKind : u8 { Tree = 0, Bush, Boulder };

struct PlantDraw {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 trunk_colour{0.0F, 0.0F, 0.0F};
    Vec3 crown_colour{0.0F, 0.0F, 0.0F};
    /// The wind displacement at the top of the crown, metres, world axes.
    Vec3 crown_offset{0.0F, 0.0F, 0.0F};
    f32 height = 1.0F;
    f32 radius = 1.0F;
    f32 yaw = 0.0F;
    PlantKind kind = PlantKind::Tree;
};

/// One vertex of the sky dome: a direction, and the radiance `sky::compose_sky()` answered for it.
struct SkyVertex {
    Vec3 direction{0.0F, 1.0F, 0.0F};
    Vec3 radiance{0.0F, 0.0F, 0.0F};
};

/// One star, as the renderer needs it.
///
/// A star is a POINT LIGHT AT INFINITY — `sky::StarField` stores exactly that, a direction and an
/// illuminance — and it is drawn here as a point rather than composed into the dome. The dome is
/// forty-four rings of eighty-eight segments, which is four degrees of sky per quad, so a dome
/// vertex that caught a star would smear it into a four-degree lozenge. Removing the stars from
/// `compose_sky()`'s inputs for the DOME and drawing them separately is the fix; the LIGHTING
/// integral still composes them, because starlight really is what lights the ground at midnight.
struct StarDraw {
    Vec3 direction{0.0F, 1.0F, 0.0F};
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    /// Angular half-size, in radians, scaled with brightness so a first-magnitude star is bigger
    /// than a sixth-magnitude one — which is what a camera does and what an eye believes.
    f32 angular_size = 0.0006F;
};

/// What the renderer is told about the light, once per frame.
struct Lighting {
    /// The direction light TRAVELS, which is `-sun.direction`.
    Vec3 sun_travel{0.0F, -1.0F, 0.0F};
    /// The sun's illuminance after the atmosphere AND after the clouds, normalised to a unit key
    /// light so the shading is exposure-independent.
    Vec3 sun_colour{1.0F, 1.0F, 1.0F};
    Vec3 ambient{0.05F, 0.06F, 0.08F};
    /// The exposure the tone map divides by: the mean sky radiance, so a night frame is dark and a
    /// noon frame is not blown out.
    f32 exposure = 1.0F;
    /// The fraction of the sun that survived the clouds. Reported so the video's caption can say
    /// whether a dark frame is dusk or a storm.
    f32 cloud_transmittance = 1.0F;
};

/// Everything the simulated state is, for the report and for the caption.
struct WorldState {
    f64 seconds = 0.0;
    /// The day as a fraction: 0 midnight, 0.5 noon.
    f32 day_fraction = 0.0F;
    f32 sun_elevation_degrees = 0.0F;
    f32 temperature_celsius = 0.0F;
    f32 wind_speed_mps = 0.0F;
    f32 precipitation_mm_per_hour = 0.0F;
    f32 cloud_coverage = 0.0F;
    f32 wetness = 0.0F;
    f32 snow_depth_metres = 0.0F;
    f32 snow_depth_west_metres = 0.0F;
    f32 significant_wave_height = 0.0F;
    /// The mean radiance of the whole sky dome this frame, in nits. What the exposure is derived
    /// from, reported so a dark frame can be attributed to dusk or to cloud rather than guessed at.
    f32 mean_sky_nits = 0.0F;
    /// The fraction of the sun that survived the cloud deck.
    f32 cloud_transmittance = 1.0F;
    /// The thickest cloud anywhere on the dome this frame, as the transmittance along that
    /// direction. One is a cloudless sky; a value near zero is a deck that hides what is behind it.
    /// Reported because "the clouds are volumetric" is a claim a reader should be able to check
    /// against a number rather than against a screenshot.
    f32 thickest_cloud = 1.0F;
    /// The divisor the tone map is fed through. See `Lighting::exposure`.
    f32 exposure = 1.0F;
    f32 star_visibility = 0.0F;
    const char* precipitation = "none";
};

/// The world. See the header note.
class World {
public:
    /// The options are taken HERE and not at `build()` because half of what this class owns —
    /// the generated world's extent, the terrain store's layout — is fixed at construction by
    /// types that are deliberately neither copyable nor movable (`pcg::GenerationWorld`,
    /// `terrain::TerrainStore`, `terrain::HeightfieldSource`). Re-seating them later would mean
    /// giving those types an assignment operator they are better off without.
    World(Allocator& allocator, const WorldOptions& options) noexcept;

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /// Generate, cook, claim and place. Everything that happens once.
    [[nodiscard]] Status build(BuildReport& report) noexcept;

    /// Advance the world by exactly one frame of the take and recompute everything the renderer
    /// reads. One frame is one simulated tick of every module in it, and there is no wall clock in
    /// the loop, so frame N of two runs is the same world.
    [[nodiscard]] Status advance(FrameCosts& costs) noexcept;

    /// A deformation and a save, for M10 tasks.md 7.2. Craters the terrain at a position, records
    /// the delta into `world::PersistenceOverlay`, drops the delta store, restores it from the
    /// overlay, and reports whether the restored surface equals the deformed one.
    struct PersistenceReport {
        u32 samples_compared = 0;
        u32 samples_disagreeing = 0;
        u64 overlay_bytes = 0;
        u32 overlay_cells = 0;
        u32 overlay_blobs = 0;
        f32 depth_before = 0.0F;
        f32 depth_after_restore = 0.0F;
        bool restored = false;
    };
    [[nodiscard]] Status deform_and_round_trip(const WorldVec3d& at, f32 radius, f32 depth,
                                               PersistenceReport& report) noexcept;

    // --- What the renderer reads ---------------------------------------------------------------

    [[nodiscard]] Span<const TerrainPatch> terrain_patches() const noexcept {
        return patches_.span();
    }
    [[nodiscard]] Span<const PlantDraw> plants() const noexcept { return plants_.span(); }
    [[nodiscard]] Span<const SkyVertex> sky_dome() const noexcept { return sky_dome_.span(); }
    [[nodiscard]] Span<const u32> sky_indices() const noexcept { return sky_indices_.span(); }
    [[nodiscard]] Span<const StarDraw> stars() const noexcept { return star_draws_.span(); }
    [[nodiscard]] const water::OceanSurface& ocean() const noexcept { return ocean_; }
    [[nodiscard]] const Lighting& lighting() const noexcept { return lighting_; }
    [[nodiscard]] const WorldState& state() const noexcept { return state_; }
    [[nodiscard]] const WorldOptions& options() const noexcept { return options_; }
    /// The centre of the world, which is what f32 rendering coordinates are relative to.
    [[nodiscard]] WorldVec3d centre() const noexcept;
    [[nodiscard]] f32 extent_metres() const noexcept;
    /// The terrain surface under a position — what the camera stands on.
    [[nodiscard]] f32 ground_height(f64 x, f64 z) const noexcept;
    /// Where the camera looks from and at, on one frame of the take.
    void camera_at(u64 frame, u64 frames, WorldVec3d& eye, WorldVec3d& target) const noexcept;

private:
    // Each of these is one module's share of `build()`, in the order the dependencies force.
    [[nodiscard]] Status generate(BuildReport& report) noexcept;
    [[nodiscard]] Status cook_terrain(BuildReport& report) noexcept;
    /// One level-0 tile: its heights from the generator, its material from its own heights.
    [[nodiscard]] Status cook_tile(const terrain::TileCoord& coord, BuildReport& report) noexcept;
    void write_material(terrain::TerrainTile& tile) const noexcept;
    [[nodiscard]] Status derive_coarse_levels(i32 tiles_per_side, BuildReport& report) noexcept;
    [[nodiscard]] Status declare_fields(BuildReport& report) noexcept;
    [[nodiscard]] Status configure_water(BuildReport& report) noexcept;
    [[nodiscard]] Status configure_weather(BuildReport& report) noexcept;
    [[nodiscard]] Status place_foliage(BuildReport& report) noexcept;
    [[nodiscard]] Status configure_sky(BuildReport& report) noexcept;
    [[nodiscard]] Status mesh_terrain(BuildReport& report) noexcept;
    [[nodiscard]] Status build_sky_dome() noexcept;

    [[nodiscard]] Status shade_terrain() noexcept;
    [[nodiscard]] Status shade_sky() noexcept;
    [[nodiscard]] Status update_plants() noexcept;
    [[nodiscard]] Status publish_shoreline() noexcept;

    /// Simulated seconds one frame of the take advances the world by. A day across the take.
    [[nodiscard]] f64 simulated_seconds_per_frame() const noexcept;
    /// Frames in one simulated day, which is the length of the take.
    [[nodiscard]] u64 frames_per_day() const noexcept;

    /// The generated elevation at a world position, bilinear over the PCG raster. The one function
    /// that reads the generator's output, so terrain cooking and the water bed cannot disagree.
    [[nodiscard]] f32 generated_height(f64 x, f64 z) const noexcept;
    /// `water::BedSource`'s signature. Forwards to the terrain query, which is what makes the
    /// shoreline read the actual cooked ground rather than the raster it came from.
    static f64 bed_at(void* user, f64 x, f64 z) noexcept;
    /// `weather::TerrainProfile::elevation_at`'s signature, over the same query, so the rain shadow
    /// is cast by the same mountains the water runs off.
    static f64 elevation_at(void* user, f64 x, f64 z) noexcept;

    Allocator* allocator_;
    WorldOptions options_;

    // --- generation
    pcg::GenerationWorld generated_;
    Array<pcg::Generator> generator_;  // one element; `Generator` has no default constructor
    pcg::GenerationContext generation_context_;
    /// The geometry the generator's own point stages are tested against.
    ///
    /// It answers from the ELEVATION RASTER THIS RUN IS PRODUCING, and that is sound for exactly
    /// one reason: `pcg::Generator` evaluates STAGE-MAJOR, so every region's elevation is complete
    /// before the first candidate is scattered — in a full run and in a partial one alike. It is
    /// the same property the spike's first condition rests on, used here rather than only obeyed.
    /// Binding `pcg::FlatSpatialQuery` instead would put every boulder at sea level.
    class GeneratedSurface final : public pcg::SpatialQuery {
    public:
        explicit GeneratedSurface(const World& world) noexcept : world_(&world) {}

        void height_batch(Span<const f64> x, Span<const f64> z,
                          Span<f32> out) const noexcept override;
        void slope_batch(Span<const f64> x, Span<const f64> z,
                         Span<f32> out) const noexcept override;
        void surface_batch(Span<const f64> x, Span<const f64> z,
                           Span<u8> out) const noexcept override;

    private:
        const World* world_;
    };

    GeneratedSurface surface_{*this};
    pcg::AttributeId elevation_channel_;
    pcg::AttributeId density_channel_;
    /// Where the generator's ACCEPTED POINTS go. `procedural-content-generation` — "A procedural
    /// result SHALL NOT be an entity by default. A generator producing ten million trees SHALL
    /// produce a FOLIAGE POPULATION, not ten million spawn operations" — so the graph's terminal
    /// emits through `pcg::FoliageOutputAdapter` into this store, and nothing in the path can spell
    /// `Entity`: neither `cy::pcg` nor `cy::pcg-adapters` links `cy::ecs`.
    foliage::ClusterStore generated_clusters_;
    pcg::OutputRegistry outputs_;
    /// A direct member rather than an `Array` element: `pcg::OutputAdapter` is an interface with a
    /// deleted copy and no move, which is correct for a type the registry holds by reference, and
    /// it means the adapter has to be constructed where it will live.
    pcg::FoliageOutputAdapter boulder_adapter_;

    // --- terrain
    terrain::TileLayout layout_;
    terrain::TerrainStore terrain_;
    terrain::TerrainDeltaStore deltas_;
    terrain::HeightfieldSource heights_;
    terrain::TerrainQuery query_;
    terrain::TerrainSystem terrain_system_;
    Array<TerrainPatch> patches_;

    // --- the substrate
    environment::FieldRegistry registry_;
    environment::FieldStore fields_;
    environment::FieldId wetness_field_;
    environment::FieldId snow_field_;
    environment::FieldId wind_field_;
    environment::FieldId vegetation_field_;
    environment::FieldId water_depth_field_;
    environment::FieldId water_distance_field_;

    // --- water
    water::WaterSystem water_;
    water::WaterBodyId sea_;
    water::OceanSurface ocean_;
    water::OceanParams ocean_params_;

    // --- weather
    weather::ClimateMap climate_;
    weather::WeatherSystem weather_;

    // --- foliage
    foliage::SpeciesLibrary species_;
    foliage::PlacementRuleSet rules_;
    foliage::FieldBindings bindings_;
    Array<foliage::PlacementSampler> sampler_;  // one element; holds references
    Array<foliage::FoliageCluster> clusters_;
    Array<foliage::WindSampler> wind_sampler_;  // one element; opened after weather claims `wind`
    Array<PlantDraw> plants_;

    // --- sky
    rendering::sky::Atmosphere atmosphere_;
    rendering::sky::AtmosphereTables tables_;
    rendering::sky::CelestialModel celestial_model_;
    rendering::sky::CelestialState celestial_;
    rendering::sky::StarField stars_;
    rendering::sky::CloudWeatherMap cloud_map_;
    rendering::sky::CloudLayerSet cloud_layers_;
    rendering::sky::CloudField cloud_field_;
    rendering::sky::CloudQuality cloud_quality_;
    Array<SkyVertex> sky_dome_;
    Array<u32> sky_indices_;
    Array<StarDraw> star_draws_;

    Lighting lighting_;
    WorldState state_;
    /// Where the camera stands this frame. The ocean patch, the wind preparation and the sky
    /// composition are all relative to it, and it is resolved once per frame.
    WorldVec3d camera_focus_;
    u64 tick_ = 0;
    f64 seconds_ = 0.0;
};

}  // namespace cy::sample::world
