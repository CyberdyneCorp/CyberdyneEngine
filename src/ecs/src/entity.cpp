// The entity table. Task 2.1.

#include <cy/ecs/entity.h>

namespace cy::ecs {

Expected<Entity, Error> EntityTable::create() noexcept {
    if (!free_indices_.empty()) {
        const u32 index = free_indices_.back();
        free_indices_.pop_back();
        Record& record = records_[index];
        // The generation already moved on when the previous occupant died; reviving the index here
        // rather than there is what keeps a dead id detectable for as long as the index stays free.
        record.location = EntityLocation{};
        record.alive = true;
        ++live_;
        return Entity::make(index, record.generation);
    }

    const usize index = records_.size();
    if (index >= 0xFFFF'FFFFu) {
        return fail(ErrorCode::OutOfRange, "the entity table is full");
    }
    Record fresh;
    fresh.generation = 1;
    fresh.alive = true;
    if (Status pushed = records_.push_back(fresh); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++live_;
    return Entity::make(static_cast<u32>(index), 1);
}

Status EntityTable::destroy(Entity entity) noexcept {
    if (!is_alive(entity)) {
        return fail(ErrorCode::NotFound, "destroy() on an entity that is not alive");
    }
    Record& record = records_[entity.index()];
    record.generation = next_generation(record.generation);
    record.location = EntityLocation{};
    record.alive = false;
    --live_;
    if (Status pushed = free_indices_.push_back(entity.index()); !pushed) {
        // The free list could not grow. The entity is still destroyed — the generation has already
        // moved — and the index is simply never reissued, which costs one slot and stays correct.
        // Reporting it lets a caller under memory pressure see that the table is leaking indices.
        return pushed;
    }
    return ok();
}

void EntityTable::set_location(Entity entity, const EntityLocation& location) noexcept {
    const u32 index = entity.index();
    if (index >= records_.size() || records_[index].generation != entity.generation()) {
        return;
    }
    records_[index].location = location;
}

void EntityTable::clear() noexcept {
    records_.clear();
    free_indices_.clear();
    live_ = 0;
}

Status EntityTable::restore(Span<const Record> records, Span<const u32> free_indices,
                            u32 live) noexcept {
    records_.clear();
    free_indices_.clear();
    if (Status appended = records_.append(records); !appended) {
        return appended;
    }
    if (Status appended = free_indices_.append(free_indices); !appended) {
        return appended;
    }
    live_ = live;
    return ok();
}

}  // namespace cy::ecs
