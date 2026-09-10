// Gameplay features. M8.b task 3.3.

#include <cy/gameplay/features.h>

#include <utility>

namespace cy::gameplay {

const char* feature_state_name(FeatureState state) noexcept {
    switch (state) {
        case FeatureState::Installed:
            return "Installed";
        case FeatureState::Loaded:
            return "Loaded";
        case FeatureState::Registered:
            return "Registered";
        case FeatureState::Activated:
            return "Activated";
        case FeatureState::Deactivated:
            return "Deactivated";
        case FeatureState::Count:
            break;
    }
    return "Installed";
}

const char* contribution_kind_name(ContributionKind kind) noexcept {
    switch (kind) {
        case ContributionKind::Component:
            return "Component";
        case ContributionKind::System:
            return "System";
        case ContributionKind::Rule:
            return "Rule";
        case ContributionKind::Tag:
            return "Tag";
        case ContributionKind::InputContext:
            return "InputContext";
        case ContributionKind::Asset:
            return "Asset";
        case ContributionKind::Interface:
            return "Interface";
        case ContributionKind::WorldLayer:
            return "WorldLayer";
        case ContributionKind::Count:
            break;
    }
    return "Component";
}

FeatureRegistry::FeatureRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator), features_(allocator) {}

Expected<FeatureId, Error> FeatureRegistry::register_feature(Name name,
                                                             FeatureScopeKind scope) noexcept {
    if (find(name) != kInvalidFeature) {
        return make_unexpected(Error{ErrorCode::AlreadyExists, "that feature is registered", 0});
    }
    if (features_.size() >= kMaxFeatures) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "more features than one session composes", 0});
    }
    Feature feature{name,
                    scope,
                    FeatureState::Installed,
                    kInvalidTag,
                    Array<Name>(*allocator_),
                    Array<Contribution>(*allocator_)};
    if (Status pushed = features_.push_back(std::move(feature)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<FeatureId>(features_.size() - 1);
}

FeatureId FeatureRegistry::find(Name name) const noexcept {
    for (usize index = 0; index < features_.size(); ++index) {
        if (features_[index].name == name) {
            return static_cast<FeatureId>(index);
        }
    }
    return kInvalidFeature;
}

Status FeatureRegistry::declare_dependency(FeatureId feature, Name dependency) noexcept {
    if (feature >= features_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such feature", 0});
    }
    return features_[feature].dependencies.push_back(dependency);
}

Status FeatureRegistry::contribute(FeatureId feature, ContributionKind kind, Name name,
                                   TagId tag) noexcept {
    if (feature >= features_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such feature", 0});
    }
    return features_[feature].contributions.push_back(Contribution{kind, name, tag});
}

Status FeatureRegistry::set_active_phase(FeatureId feature, TagId phase) noexcept {
    if (feature >= features_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such feature", 0});
    }
    features_[feature].active_phase = phase;
    return ok();
}

TagId FeatureRegistry::active_phase(FeatureId feature) const noexcept {
    return feature < features_.size() ? features_[feature].active_phase : kInvalidTag;
}

Status FeatureRegistry::load(FeatureId feature) noexcept {
    if (feature >= features_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such feature", 0});
    }
    if (features_[feature].state != FeatureState::Installed &&
        features_[feature].state != FeatureState::Deactivated) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a feature loads from Installed", 0});
    }
    features_[feature].state = FeatureState::Loaded;
    return ok();
}

Status FeatureRegistry::register_contributions(FeatureId feature) noexcept {
    if (feature >= features_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such feature", 0});
    }
    if (features_[feature].state != FeatureState::Loaded) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a feature registers from Loaded", 0});
    }
    features_[feature].state = FeatureState::Registered;
    return ok();
}

Status FeatureRegistry::visit(FeatureId feature, FeatureId* out, u32 capacity, u32& written,
                              u8* marks) const noexcept {
    if (marks[feature] == 1) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a cycle in the feature dependency graph", 0});
    }
    if (marks[feature] == 2) {
        return ok();
    }
    marks[feature] = 1;
    for (const Name dependency : features_[feature].dependencies) {
        const FeatureId resolved = find(dependency);
        if (resolved == kInvalidFeature) {
            return make_unexpected(
                Error{ErrorCode::NotFound, "a feature depends on one that is not registered", 0});
        }
        if (Status walked = visit(resolved, out, capacity, written, marks); !walked) {
            return walked;
        }
    }
    marks[feature] = 2;
    if (out != nullptr && written < capacity) {
        out[written] = feature;
    }
    ++written;
    return ok();
}

Expected<u32, Error> FeatureRegistry::resolve_order(FeatureId feature, FeatureId* out,
                                                    u32 capacity) const noexcept {
    if (feature >= features_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such feature", 0});
    }
    u8 marks[kMaxFeatures] = {};
    u32 written = 0;
    if (Status walked = visit(feature, out, capacity, written, marks); !walked) {
        return make_unexpected(walked.error());
    }
    return written;
}

Status FeatureRegistry::activate(FeatureId feature) noexcept {
    FeatureId order[kMaxFeatures] = {};
    auto resolved = resolve_order(feature, order, kMaxFeatures);
    if (!resolved) {
        // NOTHING IS ACTIVATED. A missing dependency or a cycle refuses the whole activation, and
        // it refuses before the first feature changes state — see the header comment.
        return make_unexpected(resolved.error());
    }
    for (u32 index = 0; index < resolved.value(); ++index) {
        Feature& target = features_[order[index]];
        if (target.state == FeatureState::Activated) {
            continue;
        }
        if (target.state == FeatureState::Installed || target.state == FeatureState::Deactivated) {
            target.state = FeatureState::Loaded;
        }
        if (target.state == FeatureState::Loaded) {
            target.state = FeatureState::Registered;
        }
        target.state = FeatureState::Activated;
    }
    return ok();
}

Status FeatureRegistry::deactivate(FeatureId feature) noexcept {
    if (feature >= features_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such feature", 0});
    }
    // A feature something else still depends on stays: deactivating it would leave the dependant
    // running against contributions that are gone.
    for (usize index = 0; index < features_.size(); ++index) {
        if (index == feature || features_[index].state != FeatureState::Activated) {
            continue;
        }
        for (const Name dependency : features_[index].dependencies) {
            if (dependency == features_[feature].name) {
                return make_unexpected(Error{ErrorCode::PermissionDenied,
                                             "an active feature still depends on this one", 0});
            }
        }
    }
    features_[feature].state = FeatureState::Deactivated;
    return ok();
}

FeatureState FeatureRegistry::state(FeatureId feature) const noexcept {
    return feature < features_.size() ? features_[feature].state : FeatureState::Installed;
}

Name FeatureRegistry::name_of(FeatureId feature) const noexcept {
    return feature < features_.size() ? features_[feature].name : Name{};
}

FeatureScopeKind FeatureRegistry::scope_of(FeatureId feature) const noexcept {
    return feature < features_.size() ? features_[feature].scope : FeatureScopeKind::Session;
}

u32 FeatureRegistry::contributions(FeatureId feature, Contribution* out,
                                   u32 capacity) const noexcept {
    if (feature >= features_.size()) {
        return 0;
    }
    const Feature& target = features_[feature];
    u32 found = 0;
    for (const Contribution& contribution : target.contributions) {
        if (out != nullptr && found < capacity) {
            out[found] = contribution;
        }
        ++found;
    }
    return found;
}

u32 FeatureRegistry::active_contributions(ContributionKind kind, Contribution* out,
                                          u32 capacity) const noexcept {
    u32 found = 0;
    for (const Feature& feature : features_) {
        if (feature.state != FeatureState::Activated) {
            continue;
        }
        for (const Contribution& contribution : feature.contributions) {
            if (contribution.kind != kind) {
                continue;
            }
            if (out != nullptr && found < capacity) {
                out[found] = contribution;
            }
            ++found;
        }
    }
    return found;
}

}  // namespace cy::gameplay
