// The sparse component side table. Part of task 2.2.

#include <cy/ecs/sparse_store.h>

#include <cstring>

namespace cy::ecs {

u32 SparseStore::position_of(Entity entity) const noexcept {
    const u32 index = entity.index();
    if (index >= sparse_.size()) {
        return kAbsent;
    }
    const u32 position = sparse_[index];
    if (position == kAbsent) {
        return kAbsent;
    }
    // The generation check is what stops a recycled index from reading the previous occupant's
    // value. The entry stays until it is overwritten or erased, so this table cannot rely on the
    // entity table having cleaned up after a destroy.
    return (generations_[position] == entity.generation()) ? position : kAbsent;
}

Status SparseStore::set(Entity entity, const void* value) noexcept {
    if (!entity.valid()) {
        return fail(ErrorCode::InvalidArgument, "a sparse component needs a valid entity");
    }
    const u32 index = entity.index();
    if (index >= sparse_.size()) {
        const usize wanted = usize{index} + 1;
        if (Status reserved = sparse_.reserve(wanted); !reserved) {
            return reserved;
        }
        while (sparse_.size() < wanted) {
            if (Status pushed = sparse_.push_back(kAbsent); !pushed) {
                return pushed;
            }
        }
    }

    u32 position = sparse_[index];
    if (position != kAbsent && generations_[position] != entity.generation()) {
        // A recycled index whose previous occupant's entry is still here. Reuse the slot rather
        // than growing a second one for the same index.
        generations_[position] = entity.generation();
    } else if (position == kAbsent) {
        position = static_cast<u32>(keys_.size());
        if (Status pushed = keys_.push_back(index); !pushed) {
            return pushed;
        }
        if (Status pushed = generations_.push_back(entity.generation()); !pushed) {
            keys_.pop_back();
            return pushed;
        }
        if (Status resized = values_.resize(values_.size() + value_size_); !resized) {
            keys_.pop_back();
            generations_.pop_back();
            return resized;
        }
        sparse_[index] = position;
    }

    u8* slot = values_.data() + (usize{position} * value_size_);
    if (value == nullptr) {
        std::memset(static_cast<void*>(slot), 0, value_size_);
    } else {
        std::memcpy(static_cast<void*>(slot), value, value_size_);
    }
    return ok();
}

void* SparseStore::find(Entity entity) noexcept {
    const u32 position = position_of(entity);
    return (position == kAbsent)
               ? nullptr
               : static_cast<void*>(values_.data() + (usize{position} * value_size_));
}

const void* SparseStore::find(Entity entity) const noexcept {
    const u32 position = position_of(entity);
    return (position == kAbsent)
               ? nullptr
               : static_cast<const void*>(values_.data() + (usize{position} * value_size_));
}

Status SparseStore::erase(Entity entity) noexcept {
    const u32 position = position_of(entity);
    if (position == kAbsent) {
        return fail(ErrorCode::NotFound, "this entity has no value for that sparse component");
    }
    const auto last = static_cast<u32>(keys_.size() - 1);
    if (position != last) {
        // The dense arrays stay dense: the last entry takes the removed one's place, and its own
        // sparse slot is repointed. O(1), and the order of the set is unspecified by design.
        keys_[position] = keys_[last];
        generations_[position] = generations_[last];
        std::memcpy(static_cast<void*>(values_.data() + (usize{position} * value_size_)),
                    static_cast<const void*>(values_.data() + (usize{last} * value_size_)),
                    value_size_);
        sparse_[keys_[position]] = position;
    }
    sparse_[entity.index()] = kAbsent;
    keys_.pop_back();
    generations_.pop_back();
    if (Status resized = values_.resize(values_.size() - value_size_); !resized) {
        return resized;
    }
    return ok();
}

void SparseStore::clear() noexcept {
    sparse_.clear();
    keys_.clear();
    generations_.clear();
    values_.clear();
}

}  // namespace cy::ecs
