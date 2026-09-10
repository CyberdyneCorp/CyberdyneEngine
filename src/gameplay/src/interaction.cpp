// The interaction framework. M8.b task 3.3.

#include <cy/gameplay/interaction.h>

#include <utility>

namespace cy::gameplay {

InteractionRegistry::InteractionRegistry(Allocator& allocator, f32 cell_size) noexcept
    : allocator_(&allocator),
      interactables_(allocator),
      by_entity_(allocator),
      options_(allocator),
      cells_(allocator),
      by_cell_(allocator),
      cell_size_(cell_size > 0.0F ? cell_size : 1.0F) {}

u64 InteractionRegistry::cell_key(i32 x, i32 y, i32 z) noexcept {
    constexpr u64 kMask = 0x1FFFFFULL;
    const u64 packed_x = static_cast<u64>(static_cast<u32>(x)) & kMask;
    const u64 packed_y = static_cast<u64>(static_cast<u32>(y)) & kMask;
    const u64 packed_z = static_cast<u64>(static_cast<u32>(z)) & kMask;
    return (packed_x << 42U) | (packed_y << 21U) | packed_z;
}

i32 InteractionRegistry::cell_of(f32 coordinate) const noexcept {
    const f32 scaled = coordinate / cell_size_;
    const auto truncated = static_cast<i32>(scaled);
    return scaled < 0.0F && static_cast<f32>(truncated) != scaled ? truncated - 1 : truncated;
}

InteractionRegistry::Cell* InteractionRegistry::find_cell(u64 key) noexcept {
    const u32* slot = by_cell_.find(key);
    return slot != nullptr && *slot < cells_.size() ? &cells_[*slot] : nullptr;
}

const InteractionRegistry::Cell* InteractionRegistry::find_cell(u64 key) const noexcept {
    const u32* slot = by_cell_.find(key);
    return slot != nullptr && *slot < cells_.size() ? &cells_[*slot] : nullptr;
}

u32 InteractionRegistry::find_interactable(ecs::Entity entity) const noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    return slot != nullptr ? *slot : static_cast<u32>(interactables_.size());
}

Status InteractionRegistry::insert_into_cell(u32 index, const Vec3& position) noexcept {
    const u64 key = cell_key(cell_of(position.x), cell_of(position.y), cell_of(position.z));
    Cell* cell = find_cell(key);
    if (cell == nullptr) {
        if (Status pushed = cells_.push_back(Cell{key, Array<u32>(*allocator_)}); !pushed) {
            return pushed;
        }
        if (auto placed = by_cell_.insert(key, static_cast<u32>(cells_.size() - 1)); !placed) {
            cells_.pop_back();
            return make_unexpected(placed.error());
        }
        cell = &cells_[cells_.size() - 1];
    }
    interactables_[index].cell = key;
    return cell->members.push_back(index);
}

void InteractionRegistry::remove_from_cell(u32 index) noexcept {
    Cell* cell = find_cell(interactables_[index].cell);
    if (cell == nullptr) {
        return;
    }
    for (usize slot = 0; slot < cell->members.size(); ++slot) {
        if (cell->members[slot] == index) {
            cell->members.erase(slot);
            return;
        }
    }
}

Status InteractionRegistry::add_interactable(ecs::Entity entity, const Vec3& position) noexcept {
    if (find_interactable(entity) < interactables_.size()) {
        return move_interactable(entity, position);
    }
    Interactable added;
    added.entity = entity;
    added.position = position;
    added.first_option = static_cast<u32>(options_.size());
    if (Status pushed = interactables_.push_back(added); !pushed) {
        return pushed;
    }
    const auto index = static_cast<u32>(interactables_.size() - 1);
    if (auto placed = by_entity_.insert(entity.bits(), index); !placed) {
        interactables_.pop_back();
        return make_unexpected(placed.error());
    }
    return insert_into_cell(index, position);
}

Status InteractionRegistry::move_interactable(ecs::Entity entity, const Vec3& position) noexcept {
    const u32 index = find_interactable(entity);
    if (index >= interactables_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such interactable", 0});
    }
    remove_from_cell(index);
    interactables_[index].position = position;
    return insert_into_cell(index, position);
}

void InteractionRegistry::remove_interactable(ecs::Entity entity) noexcept {
    const u32 index = find_interactable(entity);
    if (index >= interactables_.size()) {
        return;
    }
    remove_from_cell(index);
    // The entity is dropped from the grid and its row is emptied rather than erased: erasing would
    // shift every index the cells hold, and a rebuild per removal is a cost a streaming world pays
    // constantly. An emptied row is skipped by every query.
    (void)by_entity_.remove(entity.bits());
    interactables_[index].entity = ecs::Entity{};
    interactables_[index].option_count = 0;
}

Status InteractionRegistry::add_option(ecs::Entity entity,
                                       const InteractionOption& option) noexcept {
    const u32 index = find_interactable(entity);
    if (index >= interactables_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such interactable", 0});
    }
    Interactable& target = interactables_[index];
    if (target.option_count != 0 && target.first_option + target.option_count != options_.size()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "an interactable's options are added together", 0});
    }
    if (target.option_count == 0) {
        target.first_option = static_cast<u32>(options_.size());
    }
    if (Status pushed = options_.push_back(OptionRow{entity, option}); !pushed) {
        return pushed;
    }
    ++target.option_count;
    return ok();
}

Status InteractionRegistry::query_batch(Span<InteractionQuery> queries, const EntityTagStore& tags,
                                        const TagRegistry& registry,
                                        Array<InteractionCandidate>& results,
                                        InteractionQueryReport& report) const noexcept {
    report = InteractionQueryReport{};
    report.queries = static_cast<u32>(queries.size());
    for (InteractionQuery& query : queries) {
        query.first_result = static_cast<u32>(results.size());
        query.result_count = 0;
        const f32 radius = query.radius;
        const i32 low_x = cell_of(query.origin.x - radius);
        const i32 high_x = cell_of(query.origin.x + radius);
        const i32 low_y = cell_of(query.origin.y - radius);
        const i32 high_y = cell_of(query.origin.y + radius);
        const i32 low_z = cell_of(query.origin.z - radius);
        const i32 high_z = cell_of(query.origin.z + radius);
        for (i32 x = low_x; x <= high_x; ++x) {
            for (i32 y = low_y; y <= high_y; ++y) {
                for (i32 z = low_z; z <= high_z; ++z) {
                    const Cell* cell = find_cell(cell_key(x, y, z));
                    if (cell == nullptr) {
                        continue;
                    }
                    ++report.cells_visited;
                    for (const u32 member : cell->members) {
                        const Interactable& subject = interactables_[member];
                        if (!subject.entity.valid()) {
                            continue;
                        }
                        const f32 separation = distance(subject.position, query.origin);
                        for (u32 slot = 0; slot < subject.option_count; ++slot) {
                            const OptionRow& row = options_[subject.first_option + slot];
                            ++report.candidates_tested;
                            if (query.precision == InteractionPrecision::Precise &&
                                separation > row.option.range) {
                                continue;
                            }
                            if (row.option.required_tag != kInvalidTag &&
                                !tags.has(registry, query.interactor, row.option.required_tag)) {
                                continue;
                            }
                            if (row.option.forbidden_tag != kInvalidTag &&
                                tags.has(registry, query.interactor, row.option.forbidden_tag)) {
                                continue;
                            }
                            if (Status pushed = results.push_back(InteractionCandidate{
                                    subject.entity, subject.first_option + slot, separation});
                                !pushed) {
                                return pushed;
                            }
                            ++query.result_count;
                            ++report.results;
                        }
                    }
                }
            }
        }
    }
    return ok();
}

Expected<Command, Error> InteractionRegistry::select(const InteractionCandidate& candidate,
                                                     ParticipantId participant,
                                                     ControlSourceId source) const noexcept {
    if (candidate.option >= options_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such interaction option", 0});
    }
    const OptionRow& row = options_[candidate.option];
    if (row.option.command == kInvalidCommandType) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "this option declares no command", 0});
    }
    Command command;
    command.type = row.option.command;
    command.participant = participant;
    command.source = source;
    command.target = candidate.interactable;
    return command;
}

}  // namespace cy::gameplay
