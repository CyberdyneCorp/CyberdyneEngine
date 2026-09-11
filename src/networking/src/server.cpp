#include <cy/networking/server.h>

#include <algorithm>

namespace cy::net {

const char* cook_asset_kind_name(CookAsset::Kind kind) noexcept {
    switch (kind) {
        case CookAsset::Kind::Texture:
            return "Texture";
        case CookAsset::Kind::Shader:
            return "Shader";
        case CookAsset::Kind::Audio:
            return "Audio";
        case CookAsset::Kind::VfxAsset:
            return "VfxAsset";
        case CookAsset::Kind::Mesh:
            return "Mesh";
        case CookAsset::Kind::Collision:
            return "Collision";
        case CookAsset::Kind::Navigation:
            return "Navigation";
        case CookAsset::Kind::GameplayData:
            return "GameplayData";
    }
    return "unknown";
}

namespace {

/// Is this asset kind excluded from a dedicated server cook? The mesh case is not here, because a
/// mesh is neither simply kept nor simply dropped — see `apply_cook_profile()`.
[[nodiscard]] bool excluded_kind(const CookExclusions& exclusions, CookAsset::Kind kind) noexcept {
    switch (kind) {
        case CookAsset::Kind::Texture:
            return exclusions.textures;
        case CookAsset::Kind::Shader:
            return exclusions.shaders;
        case CookAsset::Kind::Audio:
            return exclusions.audio;
        case CookAsset::Kind::VfxAsset:
            return exclusions.vfx_assets;
        case CookAsset::Kind::Collision:
            return false;
        case CookAsset::Kind::Navigation:
            return !exclusions.keep_navigation;
        case CookAsset::Kind::GameplayData:
            return !exclusions.keep_gameplay_data;
        case CookAsset::Kind::Mesh:
            return false;
    }
    return false;
}

}  // namespace

CookReport apply_cook_profile(const CookExclusions& exclusions,
                              Span<const CookAsset> assets) noexcept {
    CookReport report;
    for (const auto& asset : assets) {
        ++report.assets_examined;

        if (asset.kind == CookAsset::Kind::Mesh) {
            if (!exclusions.high_resolution_meshes) {
                ++report.retained;
                report.bytes_retained += asset.bytes;
                continue;
            }
            if (exclusions.keep_collision_from_meshes && asset.collision_bytes != 0) {
                // The subset case, and the reason it is counted separately: retaining the whole
                // mesh and retaining its collision representation both "keep the mesh" in a report
                // that does not distinguish them.
                ++report.collision_subsets;
                ++report.retained;
                report.bytes_retained += asset.collision_bytes;
                report.bytes_excluded += asset.bytes - asset.collision_bytes;
                continue;
            }
            ++report.excluded;
            report.bytes_excluded += asset.bytes;
            continue;
        }

        if (excluded_kind(exclusions, asset.kind)) {
            ++report.excluded;
            report.bytes_excluded += asset.bytes;
            continue;
        }
        ++report.retained;
        report.bytes_retained += asset.bytes;
    }
    return report;
}

const char* admission_refusal_name(AdmissionRefusal refusal) noexcept {
    switch (refusal) {
        case AdmissionRefusal::None:
            return "None";
        case AdmissionRefusal::SessionFull:
            return "SessionFull";
        case AdmissionRefusal::Incompatible:
            return "Incompatible";
        case AdmissionRefusal::AlreadyJoined:
            return "AlreadyJoined";
    }
    return "unknown";
}

DedicatedServer::DedicatedServer(Allocator& allocator, Transport& transport, NetworkMode mode,
                                 const CompatibilityScope& scope, u32 maximum_peers) noexcept
    : allocator_(&allocator),
      transport_(&transport),
      mode_(mode),
      scope_(scope),
      maximum_(maximum_peers),
      peers_(allocator),
      ready_(allocator) {}

ServerPeer* DedicatedServer::find(PeerId peer) noexcept {
    for (auto& record : peers_) {
        if (record.peer == peer) {
            return &record;
        }
    }
    return nullptr;
}

const ServerPeer* DedicatedServer::find(PeerId peer) const noexcept {
    for (const auto& record : peers_) {
        if (record.peer == peer) {
            return &record;
        }
    }
    return nullptr;
}

Expected<u32, AdmissionRejection> DedicatedServer::admit(PeerId peer,
                                                         const CompatibilityScope& scope,
                                                         u64 tick) noexcept {
    AdmissionRejection rejection;
    if (find(peer) != nullptr) {
        rejection.refusal = AdmissionRefusal::AlreadyJoined;
        ++refusals_;
        return make_unexpected(rejection);
    }
    if (peers_.size() >= maximum_) {
        rejection.refusal = AdmissionRefusal::SessionFull;
        ++refusals_;
        return make_unexpected(rejection);
    }
    const JoinRefusal compatibility = join_verdict(mode_, scope_, scope);
    if (compatibility != JoinRefusal::None) {
        rejection.refusal = AdmissionRefusal::Incompatible;
        rejection.compatibility = compatibility;
        ++refusals_;
        return make_unexpected(rejection);
    }

    ServerPeer admitted;
    admitted.peer = peer;
    admitted.state = ConnectionState::Connected;
    admitted.scope = scope;
    admitted.joined_tick = tick;
    admitted.admitted = true;
    if (!peers_.push_back(admitted) || !ready_.push_back(false)) {
        rejection.refusal = AdmissionRefusal::SessionFull;
        ++refusals_;
        return make_unexpected(rejection);
    }
    ++admissions_;
    return static_cast<u32>(peers_.size() - 1);
}

void DedicatedServer::drop(PeerId peer) noexcept {
    for (usize index = 0; index < peers_.size(); ++index) {
        if (!(peers_[index].peer == peer)) {
            continue;
        }
        transport_->disconnect(peer);
        peers_.remove_unordered(index);
        ready_.remove_unordered(index);
        return;
    }
}

void DedicatedServer::step(u64 tick, u64 now_ms) noexcept {
    (void)tick;
    transport_->advance(now_ms);
    ++steps_;
}

Status DedicatedServer::report_ready(PeerId peer) noexcept {
    for (usize index = 0; index < peers_.size(); ++index) {
        if (peers_[index].peer == peer) {
            ready_[index] = true;
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "networking: no such peer");
}

bool DedicatedServer::everyone_ready() const noexcept {
    if (peers_.empty()) {
        // Nobody has joined. "Everyone is ready" over an empty set would start a match with no
        // players in it, which is the vacuous-truth bug in its most literal form.
        return false;
    }
    return std::ranges::all_of(ready_, [](bool ready) noexcept { return ready; });
}

}  // namespace cy::net
