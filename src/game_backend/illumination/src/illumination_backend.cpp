// SPDX-License-Identifier: MIT
#include <cy/game_backend/illumination_backend.h>

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/rendering/gi/emissive.h>
#include <cy/rendering/gi/gi_volume.h>

#include <utility>

namespace cy::game_backend {
namespace {

namespace gi = rendering::gi;

// The ABI's enumerations are the engine's, value for value: the table cannot name CyberGI, so the
// adapter, which names both, is where a renumbering fails to compile.
static_assert(static_cast<u32>(gi::GiMode::None) == CY_GI_MODE_NONE);
static_assert(static_cast<u32>(gi::GiMode::Baked) == CY_GI_MODE_BAKED);
static_assert(static_cast<u32>(gi::GiMode::Probe) == CY_GI_MODE_PROBE);
static_assert(static_cast<u32>(gi::GiMode::Dynamic) == CY_GI_MODE_DYNAMIC);
static_assert(static_cast<u32>(gi::GiMode::Hybrid) == CY_GI_MODE_HYBRID);
static_assert(static_cast<u32>(gi::GiMode::Count) == CY_GI_MODE_HYBRID + 1U);
static_assert(static_cast<u32>(gi::GiQuality::Low) == CY_GI_QUALITY_LOW);
static_assert(static_cast<u32>(gi::GiQuality::Medium) == CY_GI_QUALITY_MEDIUM);
static_assert(static_cast<u32>(gi::GiQuality::High) == CY_GI_QUALITY_HIGH);
static_assert(static_cast<u32>(gi::GiQuality::Epic) == CY_GI_QUALITY_EPIC);
static_assert(static_cast<u32>(gi::GiQuality::Count) == CY_GI_QUALITY_EPIC + 1U);
static_assert(static_cast<u32>(gi::GiTierPreference::Automatic) == CY_GI_TIER_AUTOMATIC);
static_assert(static_cast<u32>(gi::GiTierPreference::SoftwareOnly) == CY_GI_TIER_SOFTWARE_ONLY);
static_assert(static_cast<u32>(gi::GiTierPreference::WorldOnly) == CY_GI_TIER_WORLD_ONLY);
static_assert(static_cast<u32>(gi::GiTierPreference::Count) == CY_GI_TIER_WORLD_ONLY + 1U);
static_assert(static_cast<u32>(gi::IlluminationClass::Static) == CY_GI_CLASS_STATIC);
static_assert(static_cast<u32>(gi::IlluminationClass::SlowDynamic) == CY_GI_CLASS_SLOW_DYNAMIC);
static_assert(static_cast<u32>(gi::IlluminationClass::FastDynamic) == CY_GI_CLASS_FAST_DYNAMIC);
static_assert(static_cast<u32>(gi::IlluminationClass::TransientIgnored) == CY_GI_CLASS_TRANSIENT);
static_assert(static_cast<u32>(gi::IlluminationClass::Count) == CY_GI_CLASS_AUTOMATIC);
static_assert(static_cast<u32>(gi::EmitterOverride::Automatic) == CY_GI_EMISSION_AUTOMATIC);
static_assert(static_cast<u32>(gi::EmitterOverride::CacheOnly) == CY_GI_EMISSION_CACHE_ONLY);
static_assert(static_cast<u32>(gi::EmitterOverride::Promote) == CY_GI_EMISSION_PROMOTE);
static_assert(static_cast<u32>(gi::EmitterOverride::Off) == CY_GI_EMISSION_OFF);
static_assert(static_cast<u32>(gi::EmitterOverride::Count) == CY_GI_EMISSION_OFF + 1U);
static_assert(static_cast<u32>(gi::RadianceSource::None) == CY_GI_SOURCE_NONE);
static_assert(static_cast<u32>(gi::RadianceSource::Lightmap) == CY_GI_SOURCE_LIGHTMAP);
static_assert(static_cast<u32>(gi::RadianceSource::IrradianceVolume) ==
              CY_GI_SOURCE_IRRADIANCE_VOLUME);
static_assert(static_cast<u32>(gi::RadianceSource::RadianceCache) == CY_GI_SOURCE_RADIANCE_CACHE);
static_assert(static_cast<u32>(gi::RadianceSource::SurfaceCache) == CY_GI_SOURCE_SURFACE_CACHE);
static_assert(static_cast<u32>(gi::RadianceSource::ScreenTrace) == CY_GI_SOURCE_SCREEN_TRACE);
static_assert(static_cast<u32>(gi::RadianceSource::SoftwareTrace) == CY_GI_SOURCE_SOFTWARE_TRACE);
static_assert(static_cast<u32>(gi::RadianceSource::HardwareTrace) == CY_GI_SOURCE_HARDWARE_TRACE);
static_assert(static_cast<u32>(gi::RadianceSource::ReflectionProbe) ==
              CY_GI_SOURCE_REFLECTION_PROBE);
static_assert(static_cast<u32>(gi::RadianceSource::Sky) == CY_GI_SOURCE_SKY);
static_assert(static_cast<u32>(gi::RadianceSource::Count) == CY_GI_SOURCE_SKY + 1U);
static_assert(gi::gi_volume_bit(gi::GiVolumeParameter::Quality) == CY_GI_VOLUME_SETS_QUALITY);
static_assert(gi::gi_volume_bit(gi::GiVolumeParameter::ProbeDensity) ==
              CY_GI_VOLUME_SETS_PROBE_DENSITY);
static_assert(gi::gi_volume_bit(gi::GiVolumeParameter::UpdateRate) ==
              CY_GI_VOLUME_SETS_UPDATE_RATE);
static_assert(gi::gi_volume_bit(gi::GiVolumeParameter::DiffuseIntensity) ==
              CY_GI_VOLUME_SETS_DIFFUSE_INTENSITY);
static_assert(gi::gi_volume_bit(gi::GiVolumeParameter::SpecularIntensity) ==
              CY_GI_VOLUME_SETS_SPECULAR_INTENSITY);
static_assert(gi::gi_volume_bit(gi::GiVolumeParameter::TierPreference) == CY_GI_VOLUME_SETS_TIER);
static_assert(gi::gi_volume_bit(gi::GiVolumeParameter::FarFieldDistance) ==
              CY_GI_VOLUME_SETS_FAR_FIELD);

/// An engine refusal, with its code and sentence: an argument the GI judged (InvalidArgument), an
/// id it does not hold (NotFound).
[[nodiscard]] CyResult refused(const Error& error) noexcept {
    return abi::report(error);
}

[[nodiscard]] CyResult out_of_memory() noexcept {
    return abi::report(CY_RESULT_OUT_OF_MEMORY, "the GI adapter could not grow a list");
}

[[nodiscard]] f32 luminance(Vec3 value) noexcept {
    return gi::emission_luminance(value);
}

/// The six axis directions a body standing at a point is lit from.
constexpr Vec3 kAxes[6] = {Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F},
                           Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F},
                           Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, -1.0F}};

}  // namespace

gi::GiVolume gi_volume_from_abi(const CyGiVolumeDesc& desc, u64 id) noexcept {
    gi::GiVolume volume;
    volume.bounds =
        Aabb::from_min_max(Vec3{desc.bounds_min[0], desc.bounds_min[1], desc.bounds_min[2]},
                           Vec3{desc.bounds_max[0], desc.bounds_max[1], desc.bounds_max[2]});
    volume.priority = desc.priority;
    volume.blend_distance_metres = desc.blend_distance;
    volume.weight = desc.weight;
    volume.sets = desc.sets;
    volume.id = id;
    const auto sets = [&desc](u32 bit) { return (desc.sets & bit) != 0U; };
    if (sets(CY_GI_VOLUME_SETS_QUALITY)) {
        volume.quality = static_cast<gi::GiQuality>(desc.quality);
    }
    if (sets(CY_GI_VOLUME_SETS_PROBE_DENSITY)) {
        volume.probe_density = desc.probe_density;
    }
    if (sets(CY_GI_VOLUME_SETS_UPDATE_RATE)) {
        volume.update_rate = desc.update_rate;
    }
    if (sets(CY_GI_VOLUME_SETS_DIFFUSE_INTENSITY)) {
        volume.diffuse_intensity = desc.diffuse_intensity;
    }
    if (sets(CY_GI_VOLUME_SETS_SPECULAR_INTENSITY)) {
        volume.specular_intensity = desc.specular_intensity;
    }
    if (sets(CY_GI_VOLUME_SETS_TIER)) {
        volume.tier = static_cast<gi::GiTierPreference>(desc.tier);
    }
    if (sets(CY_GI_VOLUME_SETS_FAR_FIELD)) {
        volume.far_field_begins_metres = desc.far_field_metres;
    }
    return volume;
}

IlluminationAdapter::IlluminationAdapter(Allocator& allocator,
                                         gi::IlluminationSystem& system) noexcept
    : allocator_(&allocator),
      system_(&system),
      objects_(allocator),
      materials_(allocator),
      volumes_(allocator),
      emissions_(allocator) {}

Status IlluminationAdapter::bind_object(CyEntity entity, u64 object, u32 instance) noexcept {
    if (entity == CY_ENTITY_NULL || bound(entity) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "IlluminationAdapter::bind_object: the entity is null or already bound");
    }
    return objects_.push_back(BoundObject{entity, object, instance});
}

void IlluminationAdapter::unbind_object(CyEntity entity) noexcept {
    for (usize index = objects_.size(); index > 0; --index) {
        if (objects_[index - 1].entity == entity) {
            objects_.erase(index - 1);
        }
    }
    Array<Emission> next(*allocator_);
    for (const Emission& emission : emissions_) {
        if (emission.entity != entity && !next.push_back(emission)) {
            return;
        }
    }
    if (next.size() != emissions_.size()) {
        (void)apply_emission(next);
    }
}

Status IlluminationAdapter::add_material(const char* name, u32 material_id) noexcept {
    const u64 hash = abi::game::name_hash(name);
    if (name == nullptr || *name == '\0' || material(hash) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "IlluminationAdapter::add_material: the name is empty or already registered");
    }
    return materials_.push_back(Material{hash, material_id});
}

const IlluminationAdapter::BoundObject* IlluminationAdapter::bound(CyEntity entity) const noexcept {
    for (const BoundObject& object : objects_) {
        if (object.entity == entity) {
            return &object;
        }
    }
    return nullptr;
}

const IlluminationAdapter::Material* IlluminationAdapter::material(u64 hash) const noexcept {
    for (const Material& entry : materials_) {
        if (entry.hash == hash) {
            return &entry;
        }
    }
    return nullptr;
}

IlluminationAdapter::Volume* IlluminationAdapter::find_volume(CyGiVolume handle) noexcept {
    for (Volume& entry : volumes_) {
        if (entry.handle == handle) {
            return &entry;
        }
    }
    return nullptr;
}

// --- Settings ------------------------------------------------------------------------------------

CyResult IlluminationAdapter::settings(CyGiSettings& out) const noexcept {
    const gi::GiRuntimeSettings current = system_->runtime_settings();
    out.mode = static_cast<u32>(current.mode);
    out.quality = static_cast<u32>(current.quality);
    out.tier = static_cast<u32>(current.tier);
    out.flags = current.hardware_ray_tracing ? CY_GI_HARDWARE_RAY_TRACING : 0U;
    out.budget_ms = current.budget_ms;
    return CY_RESULT_OK;
}

CyResult IlluminationAdapter::set_settings(const CyGiSettings& settings) noexcept {
    gi::GiRuntimeSettings next;
    next.mode = static_cast<gi::GiMode>(settings.mode);
    next.quality = static_cast<gi::GiQuality>(settings.quality);
    next.tier = static_cast<gi::GiTierPreference>(settings.tier);
    next.hardware_ray_tracing = (settings.flags & CY_GI_HARDWARE_RAY_TRACING) != 0U;
    next.budget_ms = settings.budget_ms;
    if (Status applied = system_->set_runtime_settings(next); !applied) {
        return refused(applied.error());
    }
    return CY_RESULT_OK;
}

// --- Volumes -------------------------------------------------------------------------------------

CyResult IlluminationAdapter::apply_volumes(Array<Volume>& next) noexcept {
    Array<gi::GiVolume> placed(*allocator_);
    for (const Volume& entry : next) {
        if (!placed.push_back(entry.volume)) {
            return out_of_memory();
        }
    }
    if (Status set = system_->set_volumes(placed.span()); !set) {
        return refused(set.error());
    }
    volumes_ = std::move(next);
    return CY_RESULT_OK;
}

CyResult IlluminationAdapter::create_volume(const CyGiVolumeDesc& desc, CyGiVolume& out) noexcept {
    Array<Volume> next(*allocator_);
    if (!next.append(volumes_.span()) ||
        !next.push_back(Volume{next_volume_, gi_volume_from_abi(desc, next_volume_)})) {
        return out_of_memory();
    }
    if (const CyResult result = apply_volumes(next); result != CY_RESULT_OK) {
        return result;
    }
    out = next_volume_;
    next_volume_ += 1;
    return CY_RESULT_OK;
}

CyResult IlluminationAdapter::update_volume(CyGiVolume volume,
                                            const CyGiVolumeDesc& desc) noexcept {
    if (find_volume(volume) == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "gi_volume_update: no such GI volume");
    }
    Array<Volume> next(*allocator_);
    if (!next.append(volumes_.span())) {
        return out_of_memory();
    }
    for (Volume& entry : next) {
        if (entry.handle == volume) {
            entry.volume = gi_volume_from_abi(desc, volume);
        }
    }
    return apply_volumes(next);
}

CyResult IlluminationAdapter::destroy_volume(CyGiVolume volume) noexcept {
    if (find_volume(volume) == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "gi_volume_destroy: no such GI volume");
    }
    Array<Volume> next(*allocator_);
    for (const Volume& entry : volumes_) {
        if (entry.handle != volume && !next.push_back(entry)) {
            return out_of_memory();
        }
    }
    return apply_volumes(next);
}

// --- Objects -------------------------------------------------------------------------------------

CyResult IlluminationAdapter::set_object_class(CyEntity entity, u32 klass) noexcept {
    const BoundObject* object = bound(entity);
    if (object == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "gi_object_set_class: the entity is not an object of the GI");
    }
    const bool automatic = klass == CY_GI_CLASS_AUTOMATIC;
    const auto chosen =
        automatic ? gi::IlluminationClass::Static : static_cast<gi::IlluminationClass>(klass);
    if (Status set = system_->set_object_class(object->object, !automatic, chosen, frame_); !set) {
        return refused(set.error());
    }
    return CY_RESULT_OK;
}

CyResult IlluminationAdapter::object_class(CyEntity entity, u32& out) const noexcept {
    const BoundObject* object = bound(entity);
    const gi::GiObject* registered = object == nullptr ? nullptr : system_->object(object->object);
    if (registered == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "gi_object_class: the entity is not an object of the GI");
    }
    out = static_cast<u32>(registered->klass);
    return CY_RESULT_OK;
}

// --- Emission ------------------------------------------------------------------------------------

CyResult IlluminationAdapter::apply_emission(Array<Emission>& next) noexcept {
    Array<gi::EmitterRule> rules(*allocator_);
    for (const Emission& emission : next) {
        gi::EmitterRule rule;
        rule.mode = emission.mode;
        if (emission.entity != CY_ENTITY_NULL) {
            const BoundObject* object = bound(emission.entity);
            if (object == nullptr) {
                continue;  // unbound since: the rule has nothing left to match
            }
            rule.instance_id = object->instance;
        }
        if (emission.material != 0U) {
            const Material* entry = material(emission.material);
            if (entry == nullptr) {
                continue;
            }
            rule.material_id = entry->id;
        }
        if (!rules.push_back(rule)) {
            return out_of_memory();
        }
    }
    if (Status set = system_->set_emitter_rules(rules.span(), frame_); !set) {
        return refused(set.error());
    }
    emissions_ = std::move(next);
    return CY_RESULT_OK;
}

CyResult IlluminationAdapter::set_emission(CyEntity entity, u64 material_hash, u32 mode) noexcept {
    if (entity != CY_ENTITY_NULL && bound(entity) == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "gi_emission_set: the entity is not an object of the GI");
    }
    if (material_hash != 0U && material(material_hash) == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "gi_emission_set: no material of that name is registered with the GI");
    }
    Array<Emission> next(*allocator_);
    for (const Emission& emission : emissions_) {
        const bool same = emission.entity == entity && emission.material == material_hash;
        if (!same && !next.push_back(emission)) {
            return out_of_memory();
        }
    }
    const auto override_mode = static_cast<gi::EmitterOverride>(mode);
    if (override_mode != gi::EmitterOverride::Automatic &&
        !next.push_back(Emission{entity, material_hash, override_mode})) {
        return out_of_memory();
    }
    return apply_emission(next);
}

// --- Queries -------------------------------------------------------------------------------------

CyResult IlluminationAdapter::sample_irradiance(const f32 (&position)[3], const f32* normal,
                                                CyGiIrradiance& out) const noexcept {
    const Vec3 at{position[0], position[1], position[2]};
    const gi::SurfaceProperties surface{};
    if (normal != nullptr) {
        const Vec3 facing = normalized_or(Vec3{normal[0], normal[1], normal[2]}, Vec3{});
        if (length(facing) == 0.0F) {
            return abi::report(CY_RESULT_INVALID_ARGUMENT,
                               "gi_sample_irradiance: the normal has no direction");
        }
        const gi::ResolveResult result = system_->indirect_diffuse(at, facing, surface);
        out.radiance[0] = result.radiance.x;
        out.radiance[1] = result.radiance.y;
        out.radiance[2] = result.radiance.z;
        out.luminance = luminance(result.radiance);
        out.confidence = result.confidence;
        out.source = static_cast<u32>(result.dominant);
        return CY_RESULT_OK;
    }
    // A body, not a surface: the mean over the six axis directions, which is exact for the
    // caches' SH L1 encoding and what a character standing there is lit by on average.
    Vec3 sum{};
    f32 confidence = 0.0F;
    f32 brightest = -1.0F;
    gi::RadianceSource dominant = gi::RadianceSource::None;
    for (const Vec3 axis : kAxes) {
        const gi::ResolveResult result = system_->indirect_diffuse(at, axis, surface);
        sum = sum + result.radiance;
        confidence += result.confidence;
        if (luminance(result.radiance) > brightest) {
            brightest = luminance(result.radiance);
            dominant = result.dominant;
        }
    }
    const Vec3 mean = sum * (1.0F / 6.0F);
    out.flags = CY_GI_SAMPLE_OMNIDIRECTIONAL;
    out.radiance[0] = mean.x;
    out.radiance[1] = mean.y;
    out.radiance[2] = mean.z;
    out.luminance = luminance(mean);
    out.confidence = confidence / 6.0F;
    out.source = static_cast<u32>(dominant);
    return CY_RESULT_OK;
}

CyResult IlluminationAdapter::status(CyGiStatus& out) const noexcept {
    const gi::GiProfile profile = system_->profile();
    const gi::EmissiveReport emitters = system_->emitters().report();
    out.world_tier = static_cast<u32>(system_->world_tracer().tier());
    out.convergence = system_->convergence().worst_convergence();
    out.measured_ms = profile.measured_ms;
    out.allocation_ms = system_->budget().settings().allocation_ms;
    out.volumes = static_cast<u32>(system_->volumes().size());
    out.objects = static_cast<u32>(system_->objects().size());
    out.promoted_lights = static_cast<u32>(system_->emitters().lights().size());
    out.emitters = emitters.emitters;
    out.disabled_emitters = emitters.disabled_emitters;
    out.frame = profile.frame;
    return CY_RESULT_OK;
}

void bind_illumination(cy::abi::Host& host, IlluminationAdapter* adapter) noexcept {
    host.game.illumination = adapter;
}

}  // namespace cy::game_backend
