// Session validation, and the per-tick divergence probe. Task 4.2.6.

#include <cy/servers/physics/determinism.h>

#include <cy/core/memory/allocator.h>

namespace cy::physics {

const char* session_determinism_name(SessionDeterminism value) noexcept {
    switch (value) {
        case SessionDeterminism::None:
            return "none";
        case SessionDeterminism::SamePlatform:
            return "same-platform";
        case SessionDeterminism::CrossPlatform:
            return "cross-platform";
        case SessionDeterminism::Lockstep:
            return "lockstep";
    }
    return "unknown";
}

Status validate_session(SessionDeterminism session, PhysicsAuthority authority,
                        DeterminismPolicy backend) noexcept {
    // Presentation physics is compatible with every session, which is `physics`' own escape hatch:
    // "physics MAY be classified NonAuthoritative and used for debris, ragdolls, and secondary
    // effects outside the deterministic core". Checked first, so the two rejections below read as
    // statements about AUTHORITATIVE physics only.
    if (authority == PhysicsAuthority::Presentation) {
        return ok();
    }

    switch (session) {
        case SessionDeterminism::None:
            return ok();
        case SessionDeterminism::SamePlatform:
            if (backend != DeterminismPolicy::SamePlatformDeterministic) {
                return fail(ErrorCode::Unsupported,
                            "a same-platform deterministic session treats physics as "
                            "authoritative, but the backend does not guarantee same-platform "
                            "determinism");
            }
            return ok();
        case SessionDeterminism::CrossPlatform:
        case SessionDeterminism::Lockstep:
            // `physics`: "A session declaring CrossPlatform or Lockstep while treating physics as
            // authoritative SHALL be rejected at configuration time." No backend in this engine
            // guarantees cross-platform determinism and none is expected to; the fix is either the
            // deterministic math path (M9) or PhysicsAuthority::Presentation.
            return fail(ErrorCode::Unsupported,
                        "a cross-platform or lockstep session cannot treat physics as "
                        "authoritative: cross-platform determinism is not guaranteed. Either "
                        "classify physics as presentation, or use the deterministic math path for "
                        "authoritative movement");
    }
    return fail(ErrorCode::InvalidArgument, "unknown session determinism");
}

DeterminismProbe::DeterminismProbe(Allocator& allocator, u32 capacity) noexcept
    : trees_(allocator), ticks_(allocator), allocator_(&allocator), capacity_(capacity) {}

DeterminismProbe::~DeterminismProbe() {
    for (determinism::StateHashTree* tree : trees_) {
        tree->~StateHashTree();
        allocator_->deallocate(tree, sizeof(determinism::StateHashTree),
                               alignof(determinism::StateHashTree));
    }
}

Status DeterminismProbe::record(const PhysicsServer& server, WorldHandle world, u64 tick) noexcept {
    if (static_cast<u32>(ticks_.size()) >= capacity_) {
        // Stops rather than growing. A probe that ran the machine out of memory would have replaced
        // the divergence it was looking for with a different failure — see the header.
        return fail(ErrorCode::OutOfRange,
                    "determinism probe: the recording capacity is full; raise it or record fewer "
                    "ticks");
    }
    void* storage = allocator_->allocate(sizeof(determinism::StateHashTree),
                                         alignof(determinism::StateHashTree));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "determinism probe: could not allocate a hash tree");
    }
    auto* tree = construct_at<determinism::StateHashTree>(storage, *allocator_);
    Status hashed = server.hash_state(world, *tree);
    if (!hashed) {
        tree->~StateHashTree();
        allocator_->deallocate(storage, sizeof(determinism::StateHashTree),
                               alignof(determinism::StateHashTree));
        return hashed;
    }
    if (Status pushed = trees_.push_back(tree); !pushed) {
        tree->~StateHashTree();
        allocator_->deallocate(storage, sizeof(determinism::StateHashTree),
                               alignof(determinism::StateHashTree));
        return pushed;
    }
    if (Status pushed = ticks_.push_back(tick); !pushed) {
        trees_.pop_back();
        tree->~StateHashTree();
        allocator_->deallocate(storage, sizeof(determinism::StateHashTree),
                               alignof(determinism::StateHashTree));
        return pushed;
    }
    return ok();
}

u64 DeterminismProbe::hash_at(u32 index) const noexcept {
    return index < trees_.size() ? trees_[index]->root_hash() : 0;
}

u64 DeterminismProbe::tick_at(u32 index) const noexcept {
    return index < ticks_.size() ? ticks_[index] : 0;
}

PhysicsDivergence DeterminismProbe::compare(const DeterminismProbe& left,
                                            const DeterminismProbe& right) noexcept {
    PhysicsDivergence out;
    const u32 count = left.recorded() < right.recorded() ? left.recorded() : right.recorded();
    for (u32 index = 0; index < count; ++index) {
        if (left.trees_[index]->root_hash() == right.trees_[index]->root_hash()) {
            continue;
        }
        determinism::Divergence node;
        determinism::StateHashTree::compare(*left.trees_[index], *right.trees_[index], node);
        out.diverged = true;
        out.tick = left.ticks_[index];
        out.shape_mismatch = node.shape_mismatch;
        out.left_hash = node.left;
        out.right_hash = node.right;
        // The deepest node on the reported path that is a body. `hash_state` opens one
        // `HashLevel::Entity` node per body with the handle's bits as the id, so this is what turns
        // "the trees differ" into `physics`' "the tick number and the diverging body SHALL be
        // reported".
        for (u32 depth = 0; depth < node.depth; ++depth) {
            if (node.levels[depth] == determinism::HashLevel::Entity) {
                out.body = BodyHandle::from_bits(node.ids[depth]);
            }
        }
        return out;
    }
    if (left.recorded() != right.recorded()) {
        // Same hashes as far as both ran, and one ran longer. That is a divergence in the SHAPE of
        // the session rather than of the state, and reporting it as agreement would be the most
        // misleading answer available.
        out.diverged = true;
        out.shape_mismatch = true;
        out.tick = count == 0 ? 0 : left.ticks_[count - 1];
    }
    return out;
}

}  // namespace cy::physics
