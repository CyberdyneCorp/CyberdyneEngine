// Wind response: one preparation per cluster, one pure function per vertex. See wind.h for why the
// CPU count is the requirement and why nothing here models wind.

#include <cy/foliage/wind.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::foliage {

const char* wind_term_name(WindTerm term) noexcept {
    switch (term) {
        case WindTerm::Trunk:
            return "trunk";
        case WindTerm::Branch:
            return "branch";
        case WindTerm::Twig:
            return "twig";
        case WindTerm::Leaf:
            return "leaf";
        case WindTerm::kCount:
            break;
    }
    return "unknown";
}

u32 terms_for_detail(WindDetail detail) noexcept {
    switch (detail) {
        case WindDetail::None:
            return 0;
        case WindDetail::Sway:
            return 1;
        case WindDetail::Branch:
            return 2;
        case WindDetail::Leaf:
            return kWindTermCount;
    }
    return 1;
}

WindDetail resolve_wind_detail(WindDetail declared, f32 distance_metres,
                               const WindTuning& tuning) noexcept {
    // Distance first, then the two ceilings. The species' declaration is a ceiling too: a blade of
    // grass declares `Sway` because it has no branches, and no budget setting can make it grow
    // some.
    WindDetail banded = WindDetail::Sway;
    if (distance_metres <= tuning.hierarchical_metres) {
        banded = WindDetail::Leaf;
    } else if (distance_metres <= tuning.sway_metres) {
        banded = WindDetail::Branch;
    }
    const u32 lowest = math::min(math::min(static_cast<u32>(banded), static_cast<u32>(declared)),
                                 static_cast<u32>(tuning.ceiling));
    return static_cast<WindDetail>(lowest);
}

namespace {

/// The per-term frequencies and amplitude ratios. Fixed numbers rather than declarations, because
/// they describe how a PLANT moves rather than how a project looks: a trunk that swayed at a twig's
/// frequency would not read as a trunk at any amplitude. `SpeciesDeclaration::stiffness` is the
/// per-species lever over them.
constexpr f32 kTermFrequency[kWindTermCount] = {0.27F, 0.81F, 2.3F, 5.1F};
constexpr f32 kTermAmplitude[kWindTermCount] = {1.0F, 0.42F, 0.16F, 0.07F};

}  // namespace

WindDisplacement evaluate_response(const SpeciesDeclaration& species, const ClusterWind& wind,
                                   const FoliageInstance& instance, const VertexParameters& vertex,
                                   f32 time_seconds) noexcept {
    WindDisplacement result;
    const u32 terms = terms_for_detail(wind.detail);
    if (terms == 0) {
        return result;
    }
    const f32 speed = std::sqrt((wind.wind.x * wind.wind.x) + (wind.wind.z * wind.wind.z));
    if (!(speed > 0.0F)) {
        result.term_count = terms;
        return result;
    }
    const f32 dir_x = wind.wind.x / speed;
    const f32 dir_z = wind.wind.z / speed;

    // The stiffness scales the whole response down and the phase lag up: a stiff trunk moves less
    // and its leaves lag further behind it, which is the visual difference between an oak and a
    // willow in one number.
    const f32 compliance = 1.0F - math::clamp(species.stiffness, 0.0F, 0.95F);
    const f32 amplitude = species.wind_amplitude * speed * compliance;

    // The instance's own phase comes from its stored yaw, and the cluster's from its identity.
    // Both are DERIVED, so two machines agree without exchanging anything — the same reason
    // `water`'s wave phases are drawn rather than authored.
    const f32 instance_phase = static_cast<f32>(instance.yaw) * (1.0F / 65536.0F);

    f32 magnitude = 0.0F;
    for (u32 index = 0; index < terms; ++index) {
        // Each term reaches further up the plant than the last: the trunk term moves the whole
        // plant, the leaf term only the canopy. `height_fraction` raised to the term's own power is
        // the cheapest curve with that shape.
        const f32 reach =
            index == 0 ? vertex.height_fraction
                       : std::pow(vertex.height_fraction, 1.0F + (static_cast<f32>(index) * 0.75F));
        const f32 radial = index == 0 ? 1.0F : (0.35F + (0.65F * vertex.radial_fraction));
        const f32 phase =
            (wind.phase + instance_phase + (vertex.vertex_phase * static_cast<f32>(index + 1))) *
            math::kTwoPi;
        const f32 wave = std::sin(
            (time_seconds * kTermFrequency[index] * math::kTwoPi * (1.0F + (speed * 0.05F))) +
            phase);
        const f32 term = amplitude * kTermAmplitude[index] * reach * radial * wave;
        result.terms[index] = term;
        magnitude += term;
    }
    result.term_count = terms;
    result.offset.x = dir_x * magnitude;
    result.offset.z = dir_z * magnitude;
    // A bending plant shortens. Without it a tall tree's canopy stretches away from its trunk at
    // full deflection, which is the one artefact a purely horizontal sway always has.
    result.offset.y = -std::abs(magnitude) * 0.25F * vertex.height_fraction;
    return result;
}

Expected<WindSampler, Error> WindSampler::open(const environment::FieldStore& store,
                                               environment::FieldId wind_field,
                                               determinism::SimulationClass reader_class) noexcept {
    // The firewall decision is made ONCE here, not per sample. `FieldReader::open()` refuses an
    // authoritative reader of a presentation field, which is where a foliage system that read wind
    // as authoritative is caught — before a frame runs rather than at a divergence.
    Expected<environment::FieldReader, Error> reader =
        environment::FieldReader::open(store, wind_field, reader_class);
    if (!reader) {
        return make_unexpected(reader.error());
    }
    return WindSampler(static_cast<environment::FieldReader&&>(reader.value()));
}

Vec3 WindSampler::wind_at(const world::WorldVec3d& at) const noexcept {
    const environment::FieldSample sample = reader_.sample(at);
    return Vec3{sample.value.components[0], sample.value.components[1], sample.value.components[2]};
}

Expected<WindPrepareReport, Error> WindSampler::prepare(const SpeciesLibrary& library,
                                                        Span<FoliageCluster> clusters,
                                                        const world::WorldVec3d& eye,
                                                        const WindTuning& tuning) const noexcept {
    WindPrepareReport report;
    for (FoliageCluster& cluster : clusters) {
        const world::WorldVec3d centre = cluster.bounds().centre();
        // ONE field sample per cluster. A forest does not need a wind sample per tree, and taking
        // one would be exactly the per-instance CPU work the requirement forbids.
        const environment::FieldSample sample = reader_.sample(centre);
        ++report.field_samples;

        const f64 dx = centre.x - eye.x;
        const f64 dy = centre.y - eye.y;
        const f64 dz = centre.z - eye.z;
        const auto distance = static_cast<f32>(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)));

        // The finest detail any species in this cluster declares. Per cluster and not per instance,
        // for the same reason the sample is.
        WindDetail declared = WindDetail::None;
        for (const SpeciesBlock& block : cluster.blocks()) {
            const SpeciesDeclaration* species = library.find(block.species);
            if (species != nullptr &&
                static_cast<u32>(species->wind) > static_cast<u32>(declared)) {
                declared = species->wind;
            }
        }

        ClusterWind& wind = cluster.wind();
        wind.wind = Vec3{sample.value.components[0] * tuning.amplitude_scale,
                         sample.value.components[1] * tuning.amplitude_scale,
                         sample.value.components[2] * tuning.amplitude_scale};
        // Derived from the cluster's own identity, so two adjacent clusters do not sway in lockstep
        // and two machines agree about which way each one is out of step.
        wind.phase = static_cast<f32>(cluster.id().value >> 40U) * 0x1.0p-24F;
        wind.detail = resolve_wind_detail(declared, distance, tuning);
        wind.field_version = sample.version;

        ++report.clusters_prepared;
        report.instances_covered += cluster.size();
        if (static_cast<u32>(wind.detail) > static_cast<u32>(WindDetail::Sway)) {
            ++report.hierarchical;
        } else {
            ++report.sway;
        }
    }
    return report;
}

environment::FieldDeclaration wind_field_declaration(f32 cell_metres) noexcept {
    environment::FieldDeclaration declaration;
    declaration.name = environment::fields::kWind;
    declaration.unit = "m/s";
    declaration.semantics =
        "air velocity, world axes; produced by weather-and-wind and read by foliage, particles and "
        "water so that all three agree about one gust";
    declaration.type = environment::FieldType::Vec3;
    declaration.encoding = environment::FieldEncoding::F32;
    declaration.interpolation = environment::FieldInterpolation::Linear;
    declaration.cadence = environment::FieldCadence::PerFrame;
    declaration.production = environment::FieldProduction::Cpu;
    declaration.range_min = -60.0F;
    declaration.range_max = 60.0F;
    declaration.levels[static_cast<u32>(environment::FieldResidency::Macro)] =
        environment::FieldLevel{cell_metres * 8.0F, true};
    declaration.levels[static_cast<u32>(environment::FieldResidency::Regional)] =
        environment::FieldLevel{cell_metres, false};
    // Volumetric: wind above a treeline is not wind inside it. `environment-fields` names wind as
    // the one standard field that is not planar, and this is that declaration.
    declaration.vertical_cells = 4;
    declaration.vertical_metres = 25.0F;
    declaration.vertical_origin_metres = 0.0F;
    declaration.layer_rule = environment::FieldLayerRule::Add;
    // Presentation: foliage deformation, particle advection and water ripple are appearance.
    // Gameplay that wants wind reads a gameplay-visible field a project declares; this one is
    // firewalled by `determinism::may_read()` and that is deliberate.
    declaration.classification = determinism::SimulationClass::Presentation;
    declaration.gameplay_level = environment::FieldResidency::Macro;
    return declaration;
}

}  // namespace cy::foliage
