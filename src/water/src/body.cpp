// The body registry, its refusals, and the declared order that resolves an overlap. M10 task 2.3.

#include <cy/water/body.h>

#include <algorithm>
#include <cstdio>

namespace cy::water {

const char* water_body_type_name(WaterBodyType type) noexcept {
    switch (type) {
        case WaterBodyType::Ocean:
            return "ocean";
        case WaterBodyType::Lake:
            return "lake";
        case WaterBodyType::River:
            return "river";
        case WaterBodyType::Pool:
            return "pool";
        case WaterBodyType::Waterfall:
            return "waterfall";
    }
    return "unknown";
}

const char* water_backend_name(WaterBackend backend) noexcept {
    switch (backend) {
        case WaterBackend::Flat:
            return "Flat";
        case WaterBackend::Spectral:
            return "Spectral";
        case WaterBackend::SplineFlow:
            return "SplineFlow";
        case WaterBackend::ShallowWater:
            return "ShallowWater";
        case WaterBackend::Particle:
            return "Particle";
    }
    return "unknown";
}

BackendStatus backend_status(WaterBackend backend) noexcept {
    // The specification's own Status column, transcribed. `ShallowWater` is Planned and `Particle`
    // is Deferred; the other three are Required and are built.
    switch (backend) {
        case WaterBackend::Flat:
        case WaterBackend::Spectral:
        case WaterBackend::SplineFlow:
            return BackendStatus::Required;
        case WaterBackend::ShallowWater:
            return BackendStatus::Planned;
        case WaterBackend::Particle:
            return BackendStatus::Deferred;
    }
    return BackendStatus::Deferred;
}

const char* backend_status_name(BackendStatus status) noexcept {
    switch (status) {
        case BackendStatus::Required:
            return "required";
        case BackendStatus::Planned:
            return "planned";
        case BackendStatus::Deferred:
            return "deferred";
    }
    return "unknown";
}

const char* body_problem_name(BodyProblem problem) noexcept {
    switch (problem) {
        case BodyProblem::None:
            return "none";
        case BodyProblem::NoName:
            return "a body must have a name, because its hash is its identity";
        case BodyProblem::EmptyBounds:
            return "a body's bounds must have positive extent on all three axes";
        case BodyProblem::LevelOutsideBounds:
            return "a body's mean level must lie within its own vertical bounds";
        case BodyProblem::NonPositiveDensity:
            return "a body's density must be positive, because buoyancy is computed from it";
        case BodyProblem::NonPositiveSegment:
            return "a body's streaming segment must have positive size";
        case BodyProblem::BackendPlanned:
            return "the backend is declared Planned by the specification and is not built";
        case BodyProblem::BackendDeferred:
            return "the backend is declared Deferred by the specification and is not built";
        case BodyProblem::DuplicateName:
            return "a body of this name is already registered";
    }
    return "unknown";
}

WaterOptics clear_sea_optics() noexcept {
    // Clear ocean water: red is absorbed roughly twenty-five times faster than blue, which is why
    // deep water is blue rather than merely dark, and why `water_transmittance()` shifts hue with
    // no gradient authored anywhere.
    WaterOptics optics;
    optics.absorption = Vec3{0.45F, 0.09F, 0.018F};
    optics.scattering = Vec3{0.002F, 0.004F, 0.008F};
    optics.scatter_colour = Vec3{0.03F, 0.24F, 0.42F};
    optics.roughness = 0.02F;
    return optics;
}

WaterOptics silty_lake_optics() noexcept {
    // A silty inland lake: everything is absorbed faster, the green channel least, and the strong
    // scattering is what makes it look milky rather than dark.
    WaterOptics optics;
    optics.absorption = Vec3{0.90F, 0.55F, 0.85F};
    optics.scattering = Vec3{0.35F, 0.45F, 0.25F};
    optics.scatter_colour = Vec3{0.28F, 0.30F, 0.16F};
    optics.roughness = 0.06F;
    return optics;
}

BodyProblem validate_body(const WaterBodyDesc& desc) noexcept {
    if (desc.name == nullptr || desc.name[0] == '\0') {
        return BodyProblem::NoName;
    }
    if (!desc.bounds.valid()) {
        return BodyProblem::EmptyBounds;
    }
    if (desc.mean_level < desc.bounds.min_y || desc.mean_level > desc.bounds.max_y) {
        return BodyProblem::LevelOutsideBounds;
    }
    if (desc.density <= 0.0F) {
        return BodyProblem::NonPositiveDensity;
    }
    if (desc.segment_metres <= 0.0F) {
        return BodyProblem::NonPositiveSegment;
    }
    switch (backend_status(desc.backend)) {
        case BackendStatus::Planned:
            return BodyProblem::BackendPlanned;
        case BackendStatus::Deferred:
            return BodyProblem::BackendDeferred;
        case BackendStatus::Required:
            break;
    }
    return BodyProblem::None;
}

WaterRegistry::WaterRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator), records_(allocator) {}

Expected<WaterBodyId, Error> WaterRegistry::add(const WaterBodyDesc& desc) noexcept {
    BodyProblem problem = validate_body(desc);
    if (problem == BodyProblem::None && find(desc.id()) != nullptr) {
        problem = BodyProblem::DuplicateName;
    }
    if (problem != BodyProblem::None) {
        problem_ = problem;
        // The refusal names the body, the backend and the backend's declared status — and, for the
        // deferred fluid tier, the seam it shares with `vfx-system`, because the specification's
        // requirement about it is that the two do not become separate fluid systems and a refusal
        // that did not say where the seam is would invite exactly that.
        const char* seam = (problem == BodyProblem::BackendDeferred)
                               ? " Its seam is the one `vfx-system` reserves for fluids; the two"
                                 " must not become separate fluid systems."
                               : "";
        std::snprintf(refusal_, sizeof(refusal_), "water: body '%s' (%s, backend %s, %s): %s.%s",
                      (desc.name == nullptr) ? "" : desc.name, water_body_type_name(desc.type),
                      water_backend_name(desc.backend),
                      backend_status_name(backend_status(desc.backend)), body_problem_name(problem),
                      seam);
        const ErrorCode code =
            (problem == BodyProblem::DuplicateName) ? ErrorCode::AlreadyExists
            : (problem == BodyProblem::BackendPlanned || problem == BodyProblem::BackendDeferred)
                ? ErrorCode::NotImplemented
                : ErrorCode::InvalidArgument;
        return make_unexpected(Error{code, refusal_, 0});
    }

    WaterBodyRecord record;
    record.desc = desc;
    record.id = desc.id();
    record.ordinal = static_cast<u32>(records_.size());
    if (Status pushed = records_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    problem_ = BodyProblem::None;
    return record.id;
}

Status WaterRegistry::set_mean_level(WaterBodyId body, f64 level,
                                     const WaterBounds& bounds) noexcept {
    for (WaterBodyRecord& record : records_) {
        if (!(record.id == body)) {
            continue;
        }
        if (!bounds.valid() || level < bounds.min_y || level > bounds.max_y) {
            return fail(ErrorCode::InvalidArgument,
                        "water: a body's new mean level must lie within the bounds given with it");
        }
        record.desc.mean_level = level;
        record.desc.bounds = bounds;
        return ok();
    }
    return fail(ErrorCode::NotFound, "water: no such body is registered");
}

const WaterBodyRecord* WaterRegistry::find(WaterBodyId id) const noexcept {
    for (const WaterBodyRecord& record : records_) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

const WaterBodyDesc* WaterRegistry::describe(WaterBodyId id) const noexcept {
    const WaterBodyRecord* record = find(id);
    return (record == nullptr) ? nullptr : &record->desc;
}

namespace {

/// The declared resolution order, as a strict comparison: higher priority first, declaration order
/// as the tie-break. One function so that `body_at()` and `bodies_at()` cannot disagree about which
/// body wins — "queries SHALL return ONE CONSISTENT ANSWER" is a property of there being one order,
/// not of the order being any particular rule.
[[nodiscard]] bool outranks(const WaterBodyRecord& a, const WaterBodyRecord& b) noexcept {
    if (a.desc.priority != b.desc.priority) {
        return a.desc.priority > b.desc.priority;
    }
    return a.ordinal < b.ordinal;
}

}  // namespace

WaterBodyId WaterRegistry::body_at(const world::WorldVec3d& at) const noexcept {
    const WaterBodyRecord* best = nullptr;
    for (const WaterBodyRecord& record : records_) {
        if (!record.desc.bounds.contains_horizontal(at)) {
            continue;
        }
        if (best == nullptr || outranks(record, *best)) {
            best = &record;
        }
    }
    return (best == nullptr) ? WaterBodyId{} : best->id;
}

Status WaterRegistry::bodies_at(const world::WorldVec3d& at,
                                Array<WaterBodyId>& out) const noexcept {
    Array<const WaterBodyRecord*> hits(*allocator_);
    for (const WaterBodyRecord& record : records_) {
        if (!record.desc.bounds.contains_horizontal(at)) {
            continue;
        }
        if (Status pushed = hits.push_back(&record); !pushed) {
            return pushed;
        }
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const WaterBodyRecord* a, const WaterBodyRecord* b) noexcept {
                         return outranks(*a, *b);
                     });
    for (const WaterBodyRecord* record : hits) {
        if (Status pushed = out.push_back(record->id); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::water
