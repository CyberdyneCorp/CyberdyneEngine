// SPDX-License-Identifier: MIT
// map_load.cpp — the map read back through the asset system. See map_load.h.

#include "map_load.h"

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/cooked.h>
#include <cy/core/assets/package.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>

#include <chrono>
#include <vector>

namespace sample::rts::map {
namespace {

using cy::Status;
using cy::assets::AssetKind;

/// What each id is, and what decodes it.
struct Expectation {
    AssetKind kind;
    Status (*decode)(cy::Span<const u8> payload, cy::AssetId id, MapContent& out);
};

[[nodiscard]] Expectation expectation_of(cy::AssetId id) noexcept {
    if (id == kLayoutAsset) {
        return {AssetKind::Binary, [](cy::Span<const u8> p, cy::AssetId, MapContent& o) {
                    return decode_layout(p, o.layout);
                }};
    }
    if (id == kTerrainAsset) {
        return {AssetKind::Terrain, [](cy::Span<const u8> p, cy::AssetId, MapContent& o) {
                    return decode_terrain(p, o.terrain);
                }};
    }
    if (id == kGroundAsset) {
        return {AssetKind::Mesh, [](cy::Span<const u8> p, cy::AssetId, MapContent& o) {
                    return decode_meshes(p, o.ground);
                }};
    }
    if (id == kAlbedoAsset) {
        return {AssetKind::Texture, [](cy::Span<const u8> p, cy::AssetId, MapContent& o) {
                    return decode_texture(p, o.albedo);
                }};
    }
    if (id == kPropsAsset) {
        return {AssetKind::Mesh, [](cy::Span<const u8> p, cy::AssetId, MapContent& o) {
                    return decode_meshes(p, o.props);
                }};
    }
    if (id == kWorkerAsset) {
        return {AssetKind::Mesh, [](cy::Span<const u8> p, cy::AssetId, MapContent& o) {
                    return decode_skinned(p, o.worker);
                }};
    }
    if (id == kFoliageAsset) {
        return {AssetKind::Binary, [](cy::Span<const u8> p, cy::AssetId, MapContent& o) {
                    return decode_foliage(p, o.foliage);
                }};
    }
    if (id.high() == kMapNamespace && id.low() >= kFirstContentTexture &&
        id.low() < kFirstContentTexture + kMaxContentTextures) {
        return {AssetKind::Texture, [](cy::Span<const u8> p, cy::AssetId at, MapContent& o) {
                    const auto index = static_cast<std::size_t>(at.low() - kFirstContentTexture);
                    if (o.textures.size() <= index) {
                        o.textures.resize(index + 1);
                    }
                    return decode_texture(p, o.textures[index]);
                }};
    }
    return {AssetKind::Unknown, nullptr};
}

/// The services a load runs on, brought up and torn down in the one safe order.
class Services {
public:
    explicit Services(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}
    ~Services() {
        if (assets_started_) {
            assets_.shutdown();
        }
        if (async_started_) {
            async_.stop();
        }
        if (workers_started_) {
            workers_.shutdown();
        }
    }
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

    [[nodiscard]] Status start(const std::string& package) noexcept {
        cy::jobs::JobSystemConfig config;
        config.worker_count = 1;
        if (Status started = workers_.start(config); !started) {
            return started;
        }
        workers_started_ = true;
        if (Status started = async_.start(workers_); !started) {
            return started;
        }
        async_started_ = true;
        cy::Expected<cy::UniquePtr<cy::assets::PackageReader>, cy::Error> reader =
            cy::assets::PackageReader::open(package.c_str(), cy::assets::PackageOpenOptions{});
        if (!reader) {
            return Status{cy::make_unexpected(reader.error())};
        }
        cy::Expected<cy::UniquePtr<cy::assets::PackageMount>, cy::Error> mount =
            cy::make_unique<cy::assets::PackageMount>(*allocator_, std::move(*reader));
        if (!mount) {
            return Status{cy::make_unexpected(mount.error())};
        }
        if (cy::Expected<cy::assets::MountId, cy::Error> mounted =
                files_.mount_owned(std::move(*mount), cy::assets::mount_priority::kBasePackage);
            !mounted) {
            return Status{cy::make_unexpected(mounted.error())};
        }
        if (Status started =
                assets_.start(workers_, async_, files_, cy::assets::AssetSystemConfig{});
            !started) {
            return started;
        }
        assets_started_ = true;
        return cy::ok();
    }

    [[nodiscard]] cy::assets::AssetSystem& assets() noexcept { return assets_; }

private:
    cy::Allocator* allocator_;
    cy::jobs::JobSystem workers_;
    cy::jobs::AsyncService async_;
    cy::assets::VirtualFileSystem files_;
    cy::assets::AssetSystem assets_;
    bool workers_started_ = false;
    bool async_started_ = false;
    bool assets_started_ = false;
};

[[nodiscard]] Status load_one(cy::assets::AssetSystem& assets, cy::AssetId id, MapContent& out,
                              LoadReport& report) {
    cy::Expected<cy::Ref<cy::assets::AssetData>, cy::Error> loaded = assets.load(id);
    if (!loaded) {
        return Status{cy::make_unexpected(loaded.error())};
    }
    const cy::Span<const u8> bytes = (*loaded)->bytes();
    report.assets += 1;
    report.bytes += bytes.size();
    return decode_asset(id, bytes, out);
}

}  // namespace

Status decode_asset(cy::AssetId id, cy::Span<const u8> cooked, MapContent& out) {
    const Expectation expected = expectation_of(id);
    if (expected.decode == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound, "an asset id the map does not have");
    }
    cy::Expected<cy::assets::CookedAssetHeader, cy::Error> header =
        cy::assets::read_cooked_header(cooked.data(), cooked.size());
    if (!header) {
        return Status{cy::make_unexpected(header.error())};
    }
    if (header->kind != expected.kind) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "a map asset's cooked kind is not the kind its id is loaded as");
    }
    // THE HASH IS CHECKED: a truncated or damaged payload is a refusal, not a parse of rubbish.
    cy::Expected<cy::Span<const u8>, cy::Error> payload =
        cy::assets::read_cooked_payload(cooked.data(), cooked.size(), true);
    if (!payload) {
        return Status{cy::make_unexpected(payload.error())};
    }
    return expected.decode(*payload, id, out);
}

Status load_map(cy::Allocator& allocator, const std::string& package, MapContent& out,
                LoadReport& report, std::string& detail) {
    const auto started = std::chrono::steady_clock::now();
    out = MapContent{};
    report = LoadReport{};
    Services services(allocator);
    if (Status up = services.start(package); !up) {
        detail = package;
        return up;
    }
    // THE LAYOUT FIRST: it says how many content textures the package holds.
    const cy::AssetId fixed[] = {kLayoutAsset, kTerrainAsset, kGroundAsset, kAlbedoAsset,
                                 kPropsAsset,  kWorkerAsset,  kFoliageAsset};
    for (const cy::AssetId id : fixed) {
        if (Status loaded = load_one(services.assets(), id, out, report); !loaded) {
            detail = "asset " + std::to_string(id.low());
            return loaded;
        }
    }
    for (std::size_t index = 0; index < out.layout.textures.size(); ++index) {
        const cy::AssetId id{kMapNamespace, kFirstContentTexture + index};
        if (Status loaded = load_one(services.assets(), id, out, report); !loaded) {
            detail = "texture " + out.layout.textures[index];
            return loaded;
        }
    }
    if (out.textures.size() != out.layout.textures.size()) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "the package's textures disagree with its layout");
    }
    report.digest = content_digest(out);
    report.load_ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    return cy::ok();
}

}  // namespace sample::rts::map
