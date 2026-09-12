// Output adapters. See include/cy/pcg/adapters.h.

#include <cy/pcg/adapters.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>

#include <cmath>

namespace cy::pcg {

const char* output_target_name(OutputTarget target) noexcept {
    switch (target) {
        case OutputTarget::Foliage:
            return "foliage";
        case OutputTarget::Terrain:
            return "terrain";
        case OutputTarget::Fields:
            return "fields";
        case OutputTarget::Entities:
            return "entities";
        case OutputTarget::Splines:
            return "splines";
        case OutputTarget::Geometry:
            return "geometry";
        case OutputTarget::WorldMetadata:
            return "world-metadata";
        case OutputTarget::kCount:
            break;
    }
    return "unknown";
}

Status OutputRegistry::register_adapter(OutputAdapter& adapter) noexcept {
    if (slots_.size() < static_cast<usize>(OutputTarget::kCount)) {
        // `resize()` value-initialises, so every unbound slot is null — which is what makes an
        // unregistered target a refusal rather than an uninitialised read.
        if (Status sized = slots_.resize(static_cast<usize>(OutputTarget::kCount)); !sized) {
            return sized;
        }
    }
    const usize index = static_cast<usize>(adapter.target());
    if (slots_[index] != nullptr && slots_[index] != &adapter) {
        // Two adapters writing one representation is the same defect `environment-fields` refuses
        // for a second producer, one level up — and the refusal names both, for the same reason.
        return Status{make_unexpected(Error{
            ErrorCode::AlreadyExists,
            "pcg: an output adapter is already registered for this target; a second one would "
            "make the representation depend on which ran last"})};
    }
    slots_[index] = &adapter;
    return ok();
}

OutputAdapter* OutputRegistry::find(OutputTarget target) const noexcept {
    const usize index = static_cast<usize>(target);
    if (index >= slots_.size()) {
        return nullptr;
    }
    return slots_[index];
}

Status OutputRegistry::emit(OutputTarget target, const EmitContext& context,
                            const PointSet& points) const noexcept {
    OutputAdapter* adapter = find(target);
    if (adapter == nullptr) {
        // "A PROCEDURAL RESULT SHALL NOT BE AN ENTITY BY DEFAULT." Nothing is bound to `Entities`
        // unless an application binds it, so a generator that asks for one fails here rather than
        // spawning ten million of them — and the same sentence covers every other target, because a
        // representation nobody registered is a representation nobody chose.
        return Status{make_unexpected(Error{
            ErrorCode::NotFound,
            "pcg: no output adapter is registered for this target; register one, or emit to a "
            "representation that is bound"})};
    }
    return adapter->emit(context, points);
}

usize OutputRegistry::size() const noexcept {
    usize total = 0;
    for (OutputAdapter* slot : slots_) {
        total += slot != nullptr ? 1U : 0U;
    }
    return total;
}

// --- The field adapter ------------------------------------------------------------------------

FieldOutputAdapter::FieldOutputAdapter(environment::FieldStore& store,
                                       environment::ProducerToken& token, f32 radius_metres,
                                       f32 value) noexcept
    : store_(&store), token_(&token), radius_(radius_metres), value_(value) {}

Status FieldOutputAdapter::emit(const EmitContext& context, const PointSet& points) noexcept {
    written_ = 0;
    if (points.empty()) {
        return ok();
    }
    const environment::FieldDeclaration* declaration =
        store_->registry().declaration(token_->field());
    if (declaration == nullptr) {
        return Status{make_unexpected(
            Error{ErrorCode::NotFound, "pcg: the field this adapter writes is not declared"})};
    }
    Expected<environment::FieldWriter, Error> writer = store_->open_writer(*token_);
    if (!writer) {
        return Status{make_unexpected(writer.error())};
    }
    const f32 cell_metres =
        declaration->levels[0].declared()
            ? declaration->levels[0].cell_metres
            : declaration->levels[static_cast<u32>(environment::FieldResidency::Macro)].cell_metres;
    if (cell_metres <= 0.0F) {
        return Status{make_unexpected(
            Error{ErrorCode::InvalidArgument, "pcg: the field declares no usable resolution"})};
    }
    const f64 cell_span = static_cast<f64>(cell_metres);
    const f64 tile_metres = cell_span * static_cast<f64>(environment::kTileCells);
    for (usize index = 0; index < points.size(); ++index) {
        const f64 wx = context.origin_x + static_cast<f64>(points.x(index));
        const f64 wz = context.origin_z + static_cast<f64>(points.z(index));
        // A point's contribution is a splat over `radius_`, not a single lattice point: a road
        // generator writing "distance to road" is writing a field, and a field written one lattice
        // point at a time would be a field of spikes.
        const i32 span = static_cast<i32>(std::ceil(radius_ / cell_metres));
        for (i32 dz = -span; dz <= span; ++dz) {
            for (i32 dx = -span; dx <= span; ++dx) {
                const f64 lx = wx + static_cast<f64>(dx) * cell_span;
                const f64 lz = wz + static_cast<f64>(dz) * cell_span;
                const f32 distance = std::sqrt(static_cast<f32>(dx * dx + dz * dz)) * cell_metres;
                if (distance > radius_) {
                    continue;
                }
                environment::TileAddress address;
                address.field = token_->field();
                address.x = static_cast<i32>(std::floor(lx / tile_metres));
                address.z = static_cast<i32>(std::floor(lz / tile_metres));
                address.level = 0;
                address.layer = static_cast<u8>(environment::FieldLayer::Base);
                if (Status staged = writer->stage(address); !staged) {
                    return staged;
                }
                const f64 tile_x = static_cast<f64>(address.x) * tile_metres;
                const f64 tile_z = static_cast<f64>(address.z) * tile_metres;
                const u32 cx = static_cast<u32>((lx - tile_x) / cell_span);
                const u32 cz = static_cast<u32>((lz - tile_z) / cell_span);
                const f32 falloff = radius_ > 0.0F ? 1.0F - distance / radius_ : 1.0F;
                if (Status written = writer->set(address, cx % environment::kTileCells, 0,
                                                 cz % environment::kTileCells,
                                                 environment::FieldValue::scalar(value_ * falloff));
                    !written) {
                    return written;
                }
                ++written_;
            }
        }
    }
    return writer->publish();
}

// --- The recording adapter --------------------------------------------------------------------

Status RecordingAdapter::emit(const EmitContext& context, const PointSet& points) noexcept {
    Emitted record;
    record.region = context.region;
    record.count = static_cast<u32>(points.size());
    if (Status pushed = points_.push_back(record); !pushed) {
        return pushed;
    }
    for (usize index = 0; index < points.size(); ++index) {
        if (Status pushed = identities_.push_back(GeneratedId{points.identity(index)}); !pushed) {
            return pushed;
        }
    }
    emitted_ += points.size();
    ++regions_;
    return ok();
}

Status RecordingAdapter::demote(const EmitContext& context) noexcept {
    (void)context;
    ++demotions_;
    return ok();
}

void RecordingAdapter::reset() noexcept {
    points_.clear();
    identities_.clear();
    emitted_ = 0;
    regions_ = 0;
    demotions_ = 0;
}

}  // namespace cy::pcg
