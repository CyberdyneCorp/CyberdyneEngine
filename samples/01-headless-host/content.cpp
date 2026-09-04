#include "content.h"

#include <cy/core/assets/file.h>
#include <cy/core/assets/package.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/memory/scope.h>
#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/serialize.h>
#include <cy/core/reflect/type_info.h>

#include <atomic>
#include <utility>

namespace sample {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

/// The high half of every asset id this sample uses. An arbitrary constant, chosen once, well clear
/// of the reserved placeholder namespace.
constexpr u64 kIdNamespace = 0x4359'4245'5244'594Eull;  // "CYBERDYN"

/// The entity's authored state, as a pure function of its index. This stands in for an importer:
/// the values are what a cooker would have read out of a source asset.
[[nodiscard]] cy::demo::Health authored_health(u32 index) noexcept {
    cy::demo::Health health;
    health.maximum = 100.0F + (static_cast<f32>(index % 13U) * 10.0F);
    health.current = health.maximum * (0.05F + (static_cast<f32>(index % 17U) * 0.05F));
    health.displayed = health.current;
    health.last_damage = static_cast<u8>(index % 4U);
    health.icon = index + 1U;
    return health;
}

[[nodiscard]] cy::demo::Placement authored_placement(u32 index) noexcept {
    cy::demo::Placement placement;
    // A 64-wide grid: the column is the remainder and the row is the quotient, both integers
    // before either becomes a coordinate.
    const u32 column = index % 64U;
    const u32 row = index / 64U;
    placement.x = static_cast<f32>(column) * 1.5F;
    placement.y = static_cast<f32>(row) * 1.5F;
    placement.rotation = (static_cast<f32>(index % 31U) - 15.0F) * 0.1F;
    placement.flags = index % 8U;
    placement.tile = index / 256U;
    return placement;
}

/// What one partition of the decode loop needs. Blocks write disjoint slices, so nothing here is
/// shared but the counters, which are atomic.
struct DecodeState {
    const cy::Array<cy::Ref<cy::assets::AssetData>>* assets = nullptr;
    const cy::reflect::TypeRegistry* registry = nullptr;
    const Layout* layout = nullptr;
    World* world = nullptr;
    std::atomic<u64> records{0};
    std::atomic<u64> failures{0};
};

/// Read one block's records into the component arrays.
///
/// A record names its type by TypeId; the registry answers what that type is; `read_record` applies
/// it by FieldId. No name and no byte offset crosses the boundary, which is what makes the package
/// survive an upstream rename.
[[nodiscard]] cy::Status decode_block(DecodeState& state, u32 block) noexcept {
    const cy::assets::AssetData& asset = *(*state.assets)[block];
    const u8* cursor = asset.bytes().data();
    usize remaining = asset.size();
    u32 entity = state.layout->first_of(block);
    const u32 last = entity + state.layout->count_of(block);
    u64 records = 0;

    while (remaining > 0 && entity < last) {
        for (int half = 0; half < 2; ++half) {
            auto header = cy::reflect::peek_record(cursor, remaining);
            if (!header) {
                return cy::make_unexpected(header.error());
            }
            const cy::reflect::TypeInfo* type = state.registry->find(header.value().type);
            if (type == nullptr) {
                return cy::fail(cy::ErrorCode::NotFound,
                                "the package holds a record whose type is not registered");
            }
            void* object = header.value().type == cy::reflect::type_id_of<cy::demo::Health>()
                               ? static_cast<void*>(&state.world->health[entity])
                               : static_cast<void*>(&state.world->placement[entity]);
            if (cy::Status read = cy::reflect::read_record(*type, cursor, remaining, object);
                !read) {
                return read;
            }
            cursor += header.value().total_size();
            remaining -= header.value().total_size();
            ++records;
        }
        // `displayed` is Transient: the writer skipped it and the reader left it at its default.
        // Rebuilding it here is what "derived, not loaded" means in practice.
        state.world->health[entity].displayed = state.world->health[entity].current;
        ++entity;
    }

    state.records.fetch_add(records, std::memory_order_relaxed);
    return cy::ok();
}

void decode_partition(const cy::jobs::TaskContext& context, u64 begin, u64 end,
                      void* user) noexcept {
    (void)context;
    auto& state = *static_cast<DecodeState*>(user);
    for (u64 block = begin; block < end; ++block) {
        if (!decode_block(state, static_cast<u32>(block))) {
            state.failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace

u32 Layout::first_of(u32 block) const noexcept {
    return block * (entities / blocks);
}

u32 Layout::count_of(u32 block) const noexcept {
    const u32 base = entities / blocks;
    return block + 1 == blocks ? entities - (base * block) : base;
}

cy::AssetId block_id(u32 index) noexcept {
    return cy::AssetId{kIdNamespace, index + 1U};
}

cy::Expected<PackageStats, cy::Error> cook(const char* native_path, const Layout& layout) noexcept {
    cy::assets::PackageWriter writer;
    cy::assets::PackageManifest manifest;
    if (cy::Status named = manifest.set_build_id("cyberdyne m1 sample 01-headless-host"); !named) {
        return cy::make_unexpected(named.error());
    }
    if (cy::Status set = writer.set_manifest(manifest); !set) {
        return cy::make_unexpected(set.error());
    }

    PackageStats stats;
    cy::reflect::ByteBuffer payload;
    for (u32 block = 0; block < layout.blocks; ++block) {
        payload.clear();
        const u32 first = layout.first_of(block);
        const u32 count = layout.count_of(block);
        for (u32 entity = first; entity < first + count; ++entity) {
            const cy::demo::Health health = authored_health(entity);
            const cy::demo::Placement placement = authored_placement(entity);
            if (cy::Status written = cy::reflect::write_record(
                    cy::reflect::type_of<cy::demo::Health>(), &health, payload);
                !written) {
                return cy::make_unexpected(written.error());
            }
            if (cy::Status written = cy::reflect::write_record(
                    cy::reflect::type_of<cy::demo::Placement>(), &placement, payload);
                !written) {
                return cy::make_unexpected(written.error());
            }
        }

        cy::assets::PackageWriter::EntryOptions options;
        options.kind = cy::assets::AssetKind::Binary;
        if (cy::Status added =
                writer.add(block_id(block), cy::assets::VariantKey::any(),
                           cy::Span<const u8>(payload.data(), payload.size()), options);
            !added) {
            return cy::make_unexpected(added.error());
        }
        stats.payload_bytes += payload.size();
    }

    if (cy::Status written = writer.write(native_path); !written) {
        return cy::make_unexpected(written.error());
    }

    stats.entries = static_cast<u32>(writer.entry_count());
    stats.chunks = static_cast<u32>(writer.chunks_written());
    stats.deduplicated_bytes = writer.bytes_deduplicated();
    if (auto size = cy::assets::fs::file_size(native_path)) {
        stats.file_bytes = size.value();
    }
    return stats;
}

cy::Status mount(cy::assets::VirtualFileSystem& files, const char* native_path) noexcept {
    cy::assets::PackageOpenOptions options;
    auto reader = cy::assets::PackageReader::open(native_path, options);
    if (!reader) {
        return cy::make_unexpected(reader.error());
    }
    auto package = cy::make_unique<cy::assets::PackageMount>(cy::current_allocator(),
                                                             std::move(reader.value()));
    if (!package) {
        return cy::make_unexpected(package.error());
    }
    // A package is a mount like any other, at a declared priority: patch masking and mount order
    // are the same machinery for packages as for a project directory, not a second one.
    auto mounted =
        files.mount_owned(std::move(package.value()), cy::assets::mount_priority::kBasePackage);
    if (!mounted) {
        return cy::make_unexpected(mounted.error());
    }
    return cy::ok();
}

cy::Expected<LoadStats, cy::Error> load_and_decode(cy::assets::AssetSystem& assets,
                                                   cy::jobs::JobSystem& jobs,
                                                   const cy::reflect::TypeRegistry& registry,
                                                   const Layout& layout, World& world) noexcept {
    cy::Array<cy::AssetId> ids;
    if (cy::Status reserved = ids.reserve(layout.blocks); !reserved) {
        return cy::make_unexpected(reserved.error());
    }
    for (u32 block = 0; block < layout.blocks; ++block) {
        (void)ids.push_back(block_id(block));
    }

    // One request for the whole set: the read stage runs on the async service, never on a worker,
    // and the stages of different blocks overlap.
    auto request = assets.load_batch(ids.span());
    if (!request) {
        return cy::make_unexpected(request.error());
    }
    if (cy::Status waited = assets.wait(request.value()); !waited) {
        return cy::make_unexpected(waited.error());
    }

    cy::Array<cy::Ref<cy::assets::AssetData>> loaded;
    if (cy::Status reserved = loaded.reserve(layout.blocks); !reserved) {
        return cy::make_unexpected(reserved.error());
    }
    LoadStats stats;
    for (u32 block = 0; block < layout.blocks; ++block) {
        auto asset = assets.result_at(request.value(), block);
        if (!asset) {
            return cy::make_unexpected(asset.error());
        }
        stats.bytes += asset.value()->size();
        (void)loaded.push_back(std::move(asset.value()));
    }
    stats.assets = layout.blocks;

    DecodeState state;
    state.assets = &loaded;
    state.registry = &registry;
    state.layout = &layout;
    state.world = &world;

    // One block per partition. The grain is what makes the partitioning a property of the data
    // rather than of the machine: the same count and grain give the same partitions everywhere.
    constexpr u64 kGrain = 1;
    stats.partitions = cy::jobs::JobSystem::partition_count(layout.blocks, kGrain);
    auto decode = jobs.submit_parallel_for(layout.blocks, kGrain, &decode_partition, &state,
                                           "headless-host.decode");
    if (!decode) {
        return cy::make_unexpected(decode.error());
    }
    jobs.wait(decode.value());

    if (state.failures.load(std::memory_order_relaxed) != 0) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "a package entry did not decode into the records it declared");
    }
    stats.records = state.records.load(std::memory_order_relaxed);

    if (cy::Status forgotten = assets.forget(request.value()); !forgotten) {
        return cy::make_unexpected(forgotten.error());
    }
    return stats;
}

}  // namespace sample
