#include <cy/vfx/renderers.h>

#include <cmath>

namespace cy::vfx {
namespace {

/// One live particle's presentation attributes, read through the layout so a quantised attribute
/// arrives quantised.
///
/// The same four attributes `runtime.cpp`'s own `read_presentation` reads, plus the two the six
/// kinds here need that a sprite does not. It is a second function rather than a shared one because
/// the sprite path is the hot one and reading a velocity it does not use would cost every frame
/// that draws a sprite; `emission`'s handling is identical in both and is the part that must not
/// diverge, so it is written the same way and `test_vfx_renderers.cpp` compares the two.
struct Presentation {
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    f32 velocity[3] = {0.0F, 0.0F, 0.0F};
    f32 size = 0.0F;
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    f32 emission = 1.0F;
    bool has_velocity = false;
};

[[nodiscard]] const AttributeSlot* live_slot(const AttributeLayout& layout,
                                             const Name& name) noexcept {
    const AttributeSlot* slot = layout.find(name);
    return slot != nullptr && !slot->elided ? slot : nullptr;
}

void read_presentation(const SimulationWorld& world, const EffectInstance& instance, u32 emitter,
                       u32 particle, Presentation& out) noexcept {
    static const Name kPosition = Name::intern("position");
    static const Name kVelocity = Name::intern("velocity");
    static const Name kSize = Name::intern("size");
    static const Name kColor = Name::intern("color");
    static const Name kEmission = Name::intern("emission");

    const AttributeLayout& layout = instance.system->emitters()[emitter].layout();
    for (u32 component = 0; component < 3U; ++component) {
        out.position[component] =
            world.read_attribute(instance, emitter, particle, kPosition, component);
    }
    out.has_velocity = live_slot(layout, kVelocity) != nullptr;
    if (out.has_velocity) {
        for (u32 component = 0; component < 3U; ++component) {
            out.velocity[component] =
                world.read_attribute(instance, emitter, particle, kVelocity, component);
        }
    }
    out.size = world.read_attribute(instance, emitter, particle, kSize, 0);
    for (u32 component = 0; component < 4U; ++component) {
        out.color[component] = world.read_attribute(instance, emitter, particle, kColor, component);
    }
    out.emission = live_slot(layout, kEmission) != nullptr
                       ? world.read_attribute(instance, emitter, particle, kEmission, 0)
                       : 1.0F;
}

/// Camera-relative, and the radiance already multiplied out. Every kind's row starts here, so the
/// rebase and the emission fold are written once.
void fill_common(const Presentation& presentation, const EffectInstance& instance,
                 const Vec3& camera_position, const RendererDecl& decl, u32 particle,
                 RenderRowCommon& out) noexcept {
    out.position[0] = presentation.position[0] + instance.position.x - camera_position.x;
    out.position[1] = presentation.position[1] + instance.position.y - camera_position.y;
    out.position[2] = presentation.position[2] + instance.position.z - camera_position.z;
    for (u32 channel = 0; channel < 3U; ++channel) {
        out.color[channel] = presentation.color[channel] * presentation.emission;
    }
    out.color[3] = presentation.color[3];
    out.material = decl.material;
    out.particle = particle;
}

/// Fill `previous_position` from the history and say whether it is meaningful.
///
/// THE SUPPRESSION IS THE ABSENCE OF A RECORD, not a test for a large delta. A threshold would
/// misclassify a fast particle as a teleport and a slow re-use of a freed slot as continuous
/// motion; "did this slot publish last frame" is exactly the question spawn, kill and re-use each
/// answer no to.
[[nodiscard]] bool fill_motion(const PublicationHistory& history, u32 slot, bool wanted,
                               RenderRowCommon& out) noexcept {
    for (u32 component = 0; component < 3U; ++component) {
        out.previous_position[component] = out.position[component];
    }
    if (!wanted) {
        out.motion_valid = false;
        return false;
    }
    f32 previous[3] = {0.0F, 0.0F, 0.0F};
    if (!history.sample(slot, 1, previous)) {
        out.motion_valid = false;
        return false;
    }
    for (u32 component = 0; component < 3U; ++component) {
        out.previous_position[component] = previous[component];
    }
    out.motion_valid = true;
    return true;
}

/// A particle is drawn when it is alive and has a size. `size <= 0` is what a dead slot costs
/// rather than a branch, which is `publish_sprites`'s own rule.
[[nodiscard]] bool drawable(const Presentation& presentation) noexcept {
    return presentation.size > 0.0F;
}

/// The one deterministic wiggle a beam uses. Not `cyVfxNoise`: this is presentation-only and runs
/// on whichever machine draws, so it must be cheap and need not match a kernel. `simulation-and-
/// determinism`'s classification is what allows that — a beam's lateral wobble is not a field
/// gameplay can see.
[[nodiscard]] f32 beam_wobble(u32 beam, u32 segment) noexcept {
    u32 state = (beam * 2654435761U) + (segment * 40503U) + 1U;
    state ^= state >> 15U;
    state *= 2246822519U;
    state ^= state >> 13U;
    return (static_cast<f32>(state >> 8U) * (1.0F / 8388608.0F)) - 1.0F;
}

/// Every (instance, emitter) pair of a world, in the order publication walks them.
struct EmitterCursor {
    const EffectInstance* instance = nullptr;
    u32 emitter = 0;
    u32 block = 0;
    u32 count = 0;
};

/// Call `visit` for every active emitter of every active instance. Extracted because six
/// publications would otherwise each carry the same three nested loops, and a seventh would carry a
/// slightly different one.
template <typename Visit>
[[nodiscard]] Status for_each_emitter(const SimulationWorld& world, RenderPublishReport& report,
                                      Visit&& visit) noexcept {
    for (const EffectInstance& instance : world.instances()) {
        if (!instance.active || instance.system == nullptr) {
            continue;
        }
        const u32 emitters = static_cast<u32>(instance.system->emitters().size());
        for (u32 emitter = 0; emitter < emitters; ++emitter) {
            const u32 block = instance.first_block + emitter;
            if (block >= world.blocks().size()) {
                continue;
            }
            ++report.base.emitters;
            EmitterCursor cursor;
            cursor.instance = &instance;
            cursor.emitter = emitter;
            cursor.block = block;
            cursor.count = world.blocks()[block].particles;
            if (Status visited = visit(cursor); !visited) {
                return visited;
            }
        }
    }
    return ok();
}

/// The liveness of one slot, as the publication sees it: `SimulationWorld` keeps the flags outside
/// the pool and hands them out per block.
[[nodiscard]] bool slot_alive(const SimulationWorld& world, u32 block, u32 slot) noexcept {
    const Span<const u8> flags = world.alive_flags(block);
    return slot < flags.size() && flags[slot] != 0;
}

}  // namespace

const char* renderer_kind_name(RendererKind kind) noexcept {
    switch (kind) {
        case RendererKind::Sprite:
            return "Sprite";
        case RendererKind::Mesh:
            return "Mesh";
        case RendererKind::Ribbon:
            return "Ribbon";
        case RendererKind::Beam:
            return "Beam";
        case RendererKind::Trail:
            return "Trail";
        case RendererKind::Decal:
            return "Decal";
        case RendererKind::Light:
            return "Light";
        case RendererKind::Volume:
            return "Volume";
        case RendererKind::Count:
            break;
    }
    return "unknown";
}

const char* sorting_mode_name(SortingMode mode) noexcept {
    switch (mode) {
        case SortingMode::None:
            return "None";
        case SortingMode::Distance:
            return "Distance";
        case SortingMode::Age:
            return "Age";
        case SortingMode::CustomKey:
            return "CustomKey";
        case SortingMode::EmitterOrder:
            return "EmitterOrder";
    }
    return "unknown";
}

// --- The history
// ----------------------------------------------------------------------------------

Status PublicationHistory::resize(u32 slots, u32 history) noexcept {
    if (history == 0 || history > kMaxTrailHistory) {
        return fail(ErrorCode::OutOfRange,
                    "a publication history is between one and `kMaxTrailHistory` steps deep; it is "
                    "a ring the caller owns and an unbounded one would be an allocation a frame");
    }
    if (Status resized = entries_.resize(slots); !resized) {
        return resized;
    }
    slots_ = slots;
    history_ = history;
    reset();
    return ok();
}

void PublicationHistory::reset() noexcept {
    for (Entry& entry : entries_) {
        entry = Entry{};
    }
}

void PublicationHistory::begin_frame() noexcept {
    // SHIFT, then mark step 0 absent. A slot the publication does not record this frame therefore
    // has no step-0 entry next frame and its motion vector is suppressed — which is how a kill and
    // a re-use of a freed slot both become suppressions without either being detected as one.
    for (Entry& entry : entries_) {
        for (u32 step = kMaxTrailHistory - 1U; step > 0; --step) {
            for (u32 component = 0; component < 3U; ++component) {
                entry.positions[step][component] = entry.positions[step - 1U][component];
            }
        }
        entry.present = static_cast<u8>((entry.present << 1U) & 0xFFU);
    }
}

bool PublicationHistory::sample(u32 slot, u32 age, f32 out[3]) const noexcept {
    if (slot >= entries_.size() || age >= history_ || age >= kMaxTrailHistory) {
        return false;
    }
    const Entry& entry = entries_[slot];
    if ((entry.present & (1U << age)) == 0) {
        return false;
    }
    for (u32 component = 0; component < 3U; ++component) {
        out[component] = entry.positions[age][component];
    }
    return true;
}

void PublicationHistory::record(u32 slot, const f32 position[3]) noexcept {
    if (slot >= entries_.size()) {
        return;
    }
    Entry& entry = entries_[slot];
    for (u32 component = 0; component < 3U; ++component) {
        entry.positions[0][component] = position[component];
    }
    entry.present = static_cast<u8>(entry.present | 1U);
}

// --- Ribbon and trail -------------------------------------------------------------------------

Status publish_ribbons(const SimulationWorld& world, const RendererDecl& decl,
                       const Vec3& camera_position, u32 capacity, PublicationHistory& history,
                       Array<RibbonVertex>& out, RenderPublishReport& report) noexcept {
    if (decl.kind != RendererKind::Ribbon) {
        return fail(ErrorCode::InvalidArgument,
                    "publish_ribbons was given a RendererDecl whose kind is not Ribbon; the "
                    "renderer is a property the emitter declares and is not inferred from which "
                    "attributes it happens to have");
    }
    out.clear();
    report = RenderPublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }
    history.begin_frame();
    u32 strip = 0;

    return for_each_emitter(world, report, [&](const EmitterCursor& cursor) noexcept -> Status {
        // A CHAIN IS A RUN OF CONSECUTIVE LIVE SLOTS. The pool allocates ascending and the CPU
        // executor fills free slots ascending, so adjacent live slots are adjacent in spawn order —
        // which is what a ribbon threads through. A dead slot ENDS the run, and that is the
        // "terminate cleanly rather than connecting across the gap" requirement made structural.
        u32 run_start = 0;
        u32 run_length = 0;
        const auto flush = [&]() noexcept -> Status {
            if (run_length < 2) {
                // A one-particle chain is not a strip. Counted as a break only when something
                // preceded it, which `chain_breaks` below already covers.
                run_length = 0;
                return ok();
            }
            ++report.primitives;
            ++strip;
            for (u32 index = 0; index < run_length; ++index) {
                const u32 slot = run_start + index;
                Presentation presentation;
                read_presentation(world, *cursor.instance, cursor.emitter, slot, presentation);
                if (out.size() >= capacity) {
                    ++report.base.dropped;
                    continue;
                }
                RibbonVertex vertex;
                fill_common(presentation, *cursor.instance, camera_position, decl, slot,
                            vertex.row);
                report.motion_suppressed +=
                    fill_motion(history, slot, decl.motion_vectors, vertex.row) ? 0U : 1U;
                history.record(slot, vertex.row.position);
                const f32 along = run_length > 1
                                      ? static_cast<f32>(index) / static_cast<f32>(run_length - 1U)
                                      : 0.0F;
                vertex.along = along;
                // "with width and twist over length": the width tapers from the declared width at
                // the head to nothing at the tail, and the twist accumulates along the strip.
                vertex.width =
                    decl.width * presentation.size * cursor.instance->scale * (1.0F - along);
                vertex.twist = decl.twist * along;
                vertex.strip = strip;
                if (Status pushed = out.push_back(vertex); !pushed) {
                    return pushed;
                }
                ++report.base.particles;
            }
            run_length = 0;
            return ok();
        };

        for (u32 slot = 0; slot < cursor.count; ++slot) {
            Presentation presentation;
            read_presentation(world, *cursor.instance, cursor.emitter, slot, presentation);
            const bool alive = slot_alive(world, cursor.block, slot) && drawable(presentation);
            if (alive) {
                if (run_length == 0) {
                    run_start = slot;
                }
                ++run_length;
                continue;
            }
            if (run_length != 0) {
                ++report.chain_breaks;
            }
            if (Status flushed = flush(); !flushed) {
                return flushed;
            }
        }
        return flush();
    });
}

Status publish_trails(const SimulationWorld& world, const RendererDecl& decl,
                      const Vec3& camera_position, u32 capacity, PublicationHistory& history,
                      Array<RibbonVertex>& out, RenderPublishReport& report) noexcept {
    if (decl.kind != RendererKind::Trail) {
        return fail(ErrorCode::InvalidArgument,
                    "publish_trails was given a RendererDecl whose kind is not Trail");
    }
    if (decl.trail_history == 0 || decl.trail_history > kMaxTrailHistory) {
        return fail(ErrorCode::OutOfRange,
                    "RendererDecl::trail_history is between one and `kMaxTrailHistory`");
    }
    if (history.history_depth() < decl.trail_history) {
        return fail(ErrorCode::InvalidArgument,
                    "the PublicationHistory this trail was given is shallower than the trail it "
                    "declares; a trail longer than its history would draw to a position nobody "
                    "recorded");
    }
    out.clear();
    report = RenderPublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }
    history.begin_frame();
    u32 strip = 0;

    return for_each_emitter(world, report, [&](const EmitterCursor& cursor) noexcept -> Status {
        for (u32 slot = 0; slot < cursor.count; ++slot) {
            Presentation presentation;
            read_presentation(world, *cursor.instance, cursor.emitter, slot, presentation);
            if (!slot_alive(world, cursor.block, slot) || !drawable(presentation)) {
                continue;
            }
            RibbonVertex head;
            fill_common(presentation, *cursor.instance, camera_position, decl, slot, head.row);
            const bool had_motion = fill_motion(history, slot, decl.motion_vectors, head.row);
            report.motion_suppressed += had_motion ? 0U : 1U;
            history.record(slot, head.row.position);

            // THE TRAIL IS EXACTLY AS LONG AS THE HISTORY THAT EXISTS. A particle spawned this
            // frame has one recorded position and therefore no trail at all; one whose slot was
            // re-used has whatever the re-use recorded and nothing older, because `begin_frame`
            // shifted an absence into the steps its previous occupant filled. That is the same
            // suppression the motion vector uses, applied to a whole strip.
            u32 length = 1;
            f32 tail[kMaxTrailHistory][3] = {};
            for (u32 age = 1; age < decl.trail_history; ++age) {
                if (!history.sample(slot, age, tail[age])) {
                    break;
                }
                ++length;
            }
            if (length < 2) {
                continue;
            }
            ++report.primitives;
            ++strip;
            for (u32 age = 0; age < length; ++age) {
                if (out.size() >= capacity) {
                    ++report.base.dropped;
                    continue;
                }
                RibbonVertex vertex = head;
                if (age != 0) {
                    for (u32 component = 0; component < 3U; ++component) {
                        vertex.row.position[component] = tail[age][component];
                        vertex.row.previous_position[component] = tail[age][component];
                    }
                    // Only the HEAD of a trail carries a motion vector: the tail vertices are past
                    // positions and their "previous" is a position one step older still, which is
                    // the trail itself rather than a velocity.
                    vertex.row.motion_valid = false;
                }
                const f32 along = static_cast<f32>(age) / static_cast<f32>(length - 1U);
                vertex.along = along;
                vertex.width =
                    decl.width * presentation.size * cursor.instance->scale * (1.0F - along);
                vertex.twist = decl.twist * along;
                vertex.strip = strip;
                if (Status pushed = out.push_back(vertex); !pushed) {
                    return pushed;
                }
                ++report.base.particles;
            }
        }
        return ok();
    });
}

// --- Beam -------------------------------------------------------------------------------------

Status publish_beams(const SimulationWorld& world, const RendererDecl& decl,
                     const Vec3& camera_position, u32 capacity, PublicationHistory& history,
                     Array<BeamVertex>& out, RenderPublishReport& report) noexcept {
    if (decl.kind != RendererKind::Beam) {
        return fail(ErrorCode::InvalidArgument,
                    "publish_beams was given a RendererDecl whose kind is not Beam");
    }
    if (decl.segments < 1) {
        return fail(ErrorCode::OutOfRange, "a beam has at least one segment");
    }
    out.clear();
    report = RenderPublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }
    static const Name kBeamEnd = Name::intern("beam_end");
    history.begin_frame();
    u32 beam = 0;

    return for_each_emitter(world, report, [&](const EmitterCursor& cursor) noexcept -> Status {
        const AttributeLayout& layout =
            cursor.instance->system->emitters()[cursor.emitter].layout();
        const bool has_end = live_slot(layout, kBeamEnd) != nullptr;
        for (u32 slot = 0; slot < cursor.count; ++slot) {
            Presentation presentation;
            read_presentation(world, *cursor.instance, cursor.emitter, slot, presentation);
            if (!slot_alive(world, cursor.block, slot) || !drawable(presentation)) {
                continue;
            }
            BeamVertex head;
            fill_common(presentation, *cursor.instance, camera_position, decl, slot, head.row);
            report.motion_suppressed +=
                fill_motion(history, slot, decl.motion_vectors, head.row) ? 0U : 1U;
            history.record(slot, head.row.position);

            // THE FAR END, and an emitter without a `beam_end` attribute beams back to its own
            // origin rather than being refused. `vfx-system` describes a beam as "point to point";
            // the emitter's origin is a point, and a diagnostic here would make the commonest
            // authoring — a beam from the emitter to a particle — impossible to express.
            // The emitter's own origin, camera-relative — the same rebase `fill_common` performs,
            // named once because the far end is either it or an offset from it.
            const f32 origin[3] = {cursor.instance->position.x - camera_position.x,
                                   cursor.instance->position.y - camera_position.y,
                                   cursor.instance->position.z - camera_position.z};
            f32 far_end[3] = {origin[0], origin[1], origin[2]};
            if (has_end) {
                for (u32 component = 0; component < 3U; ++component) {
                    far_end[component] = world.read_attribute(*cursor.instance, cursor.emitter,
                                                              slot, kBeamEnd, component) +
                                         origin[component];
                }
            }

            ++report.primitives;
            ++beam;
            const u32 vertices = static_cast<u32>(decl.segments) + 1U;
            for (u32 index = 0; index < vertices; ++index) {
                if (out.size() >= capacity) {
                    ++report.base.dropped;
                    continue;
                }
                const f32 along = static_cast<f32>(index) / static_cast<f32>(decl.segments);
                BeamVertex vertex = head;
                for (u32 component = 0; component < 3U; ++component) {
                    vertex.row.position[component] =
                        head.row.position[component] +
                        ((far_end[component] - head.row.position[component]) * along);
                }
                // SAG is a parabola that is zero at both ends and `beam_sag` at the middle, so a
                // beam with sag still meets the two points it is defined by. A linear droop would
                // move the far end, which is the one thing a point-to-point primitive may not do.
                vertex.row.position[1] -= decl.beam_sag * 4.0F * along * (1.0F - along);
                const f32 wobble = beam_wobble(beam, index) * decl.beam_noise;
                vertex.row.position[0] += wobble * along * (1.0F - along) * 4.0F;
                vertex.row.position[2] -= wobble * along * (1.0F - along) * 4.0F;
                // Only the head vertex sits where a particle is, so only it has a motion vector.
                vertex.row.motion_valid = index == 0 && head.row.motion_valid;
                vertex.along = along;
                vertex.width = decl.width * presentation.size * cursor.instance->scale;
                vertex.beam = beam;
                if (Status pushed = out.push_back(vertex); !pushed) {
                    return pushed;
                }
                ++report.base.particles;
            }
        }
        return ok();
    });
}

// --- Decal, light and volume --------------------------------------------------------------------

Status publish_decals(const SimulationWorld& world, const RendererDecl& decl,
                      const Vec3& camera_position, u32 capacity, PublicationHistory& history,
                      Array<DecalInstance>& out, RenderPublishReport& report) noexcept {
    if (decl.kind != RendererKind::Decal) {
        return fail(ErrorCode::InvalidArgument,
                    "publish_decals was given a RendererDecl whose kind is not Decal");
    }
    out.clear();
    report = RenderPublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }
    history.begin_frame();

    return for_each_emitter(world, report, [&](const EmitterCursor& cursor) noexcept -> Status {
        for (u32 slot = 0; slot < cursor.count; ++slot) {
            Presentation presentation;
            read_presentation(world, *cursor.instance, cursor.emitter, slot, presentation);
            if (!slot_alive(world, cursor.block, slot) || !drawable(presentation)) {
                continue;
            }
            if (out.size() >= capacity) {
                ++report.base.dropped;
                continue;
            }
            DecalInstance record;
            fill_common(presentation, *cursor.instance, camera_position, decl, slot, record.row);
            report.motion_suppressed +=
                fill_motion(history, slot, decl.motion_vectors, record.row) ? 0U : 1U;
            history.record(slot, record.row.position);

            // THE PROJECTION AXIS IS THE VELOCITY WHERE THERE IS ONE. A decal from a particle that
            // is moving should project the way the particle is going — a bullet impact's scorch
            // faces along the shot. An emitter whose layout has no velocity gets world -Y, which is
            // the direction a decal on the ground projects.
            f32 length = 0.0F;
            if (presentation.has_velocity) {
                length = std::sqrt((presentation.velocity[0] * presentation.velocity[0]) +
                                   (presentation.velocity[1] * presentation.velocity[1]) +
                                   (presentation.velocity[2] * presentation.velocity[2]));
            }
            if (length > 1.0e-4F) {
                for (u32 component = 0; component < 3U; ++component) {
                    record.axis[component] = presentation.velocity[component] / length;
                }
            }
            record.radius = presentation.size * cursor.instance->scale;
            record.depth = decl.decal_depth * cursor.instance->scale;
            record.opacity = presentation.color[3];
            if (Status pushed = out.push_back(record); !pushed) {
                return pushed;
            }
            ++report.base.particles;
            ++report.primitives;
        }
        return ok();
    });
}

Status publish_lights(const SimulationWorld& world, const RendererDecl& decl,
                      const BudgetLevers& levers, const Vec3& camera_position, u32 capacity,
                      PublicationHistory& history, Array<LightInstance>& out,
                      RenderPublishReport& report) noexcept {
    if (decl.kind != RendererKind::Light) {
        return fail(ErrorCode::InvalidArgument,
                    "publish_lights was given a RendererDecl whose kind is not Light");
    }
    out.clear();
    report = RenderPublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }
    history.begin_frame();

    // THE HARD PER-FRAME COUNT BUDGET, degraded by the controller. `vfx-system`: particle lights
    // "SHALL participate in clustered light assignment subject to a hard per-frame count budget,
    // DEGRADED BY THE BUDGET CONTROLLER like any other VFX cost." The declaration is the hard
    // number and `count_cap_scale` is the degradation; the smaller of the two and the caller's own
    // capacity is what survives.
    const f32 scaled = static_cast<f32>(decl.max_lights) * levers.count_cap_scale;
    u32 budget = scaled > 0.0F ? static_cast<u32>(scaled) : 0U;
    budget = budget < capacity ? budget : capacity;

    const Status walked =
        for_each_emitter(world, report, [&](const EmitterCursor& cursor) noexcept -> Status {
            for (u32 slot = 0; slot < cursor.count; ++slot) {
                Presentation presentation;
                read_presentation(world, *cursor.instance, cursor.emitter, slot, presentation);
                if (!slot_alive(world, cursor.block, slot) || !drawable(presentation)) {
                    continue;
                }
                LightInstance record;
                fill_common(presentation, *cursor.instance, camera_position, decl, slot,
                            record.row);
                for (u32 channel = 0; channel < 3U; ++channel) {
                    record.intensity[channel] = record.row.color[channel];
                }
                const f32 peak = std::fmax(record.intensity[0],
                                           std::fmax(record.intensity[1], record.intensity[2]));
                // The radius at which this light falls below a visible threshold, from the inverse
                // square law: a dim particle must not claim a cluster it contributes nothing to.
                // `kLightCutoff` is a radiance, so the radius is in metres without a second unit.
                constexpr f32 kLightCutoff = 0.05F;
                record.radius = peak > kLightCutoff ? std::sqrt(peak / kLightCutoff) : 0.0F;
                if (record.radius <= 0.0F) {
                    continue;
                }
                const f32 distance_squared = (record.row.position[0] * record.row.position[0]) +
                                             (record.row.position[1] * record.row.position[1]) +
                                             (record.row.position[2] * record.row.position[2]);
                // WHAT THE BUDGET KEEPS, ranked rather than taken in slot order: the brightest
                // lights nearest the camera. A budget that kept the first N slots would drop a
                // frame's brightest light because a dim one was spawned earlier.
                record.rank = peak / (1.0F + distance_squared);

                report.motion_suppressed +=
                    fill_motion(history, slot, decl.motion_vectors, record.row) ? 0U : 1U;
                history.record(slot, record.row.position);

                if (out.size() < budget) {
                    if (Status pushed = out.push_back(record); !pushed) {
                        return pushed;
                    }
                    ++report.base.particles;
                    ++report.primitives;
                    continue;
                }
                // Over budget: replace the weakest survivor if this one beats it, and count the
                // one that lost either way.
                ++report.lights_over_budget;
                ++report.base.dropped;
                if (budget == 0) {
                    continue;
                }
                usize weakest = 0;
                for (usize index = 1; index < out.size(); ++index) {
                    weakest = out[index].rank < out[weakest].rank ? index : weakest;
                }
                if (record.rank > out[weakest].rank) {
                    out[weakest] = record;
                }
            }
            return ok();
        });
    return walked;
}

Status publish_volumes(const SimulationWorld& world, const RendererDecl& decl,
                       const Vec3& camera_position, u32 capacity, PublicationHistory& history,
                       Array<VolumeInstance>& out, RenderPublishReport& report) noexcept {
    if (decl.kind != RendererKind::Volume) {
        return fail(ErrorCode::InvalidArgument,
                    "publish_volumes was given a RendererDecl whose kind is not Volume");
    }
    out.clear();
    report = RenderPublishReport{};
    if (Status reserved = out.reserve(capacity); !reserved) {
        return reserved;
    }
    history.begin_frame();

    return for_each_emitter(world, report, [&](const EmitterCursor& cursor) noexcept -> Status {
        for (u32 slot = 0; slot < cursor.count; ++slot) {
            Presentation presentation;
            read_presentation(world, *cursor.instance, cursor.emitter, slot, presentation);
            if (!slot_alive(world, cursor.block, slot) || !drawable(presentation)) {
                continue;
            }
            if (out.size() >= capacity) {
                ++report.base.dropped;
                continue;
            }
            VolumeInstance record;
            fill_common(presentation, *cursor.instance, camera_position, decl, slot, record.row);
            report.motion_suppressed +=
                fill_motion(history, slot, decl.motion_vectors, record.row) ? 0U : 1U;
            history.record(slot, record.row.position);
            record.radius = presentation.size * cursor.instance->scale;
            // EXTINCTION SCALES WITH THE OPACITY the author gave the particle, so a fading volume
            // thins rather than shrinking — a shrinking one would pop out of a froxel grid.
            record.extinction = decl.volume_extinction * presentation.color[3];
            for (u32 channel = 0; channel < 3U; ++channel) {
                record.scattering[channel] = record.row.color[channel];
            }
            if (Status pushed = out.push_back(record); !pushed) {
                return pushed;
            }
            ++report.base.particles;
            ++report.primitives;
        }
        return ok();
    });
}

}  // namespace cy::vfx
