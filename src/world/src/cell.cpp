#include <cy/world/cell.h>

#include <algorithm>
#include <cstring>
#include <ranges>

namespace cy::world {
namespace {

/// Two component sets are the same archetype when they name the same components in the same order.
/// The builder always appends in the caller's order, so "the same order" is not a restriction on
/// the cook, it is what makes the comparison an integer loop rather than a set operation.
[[nodiscard]] bool same_components(Span<const ecs::ComponentTypeId> a,
                                   Span<const ecs::ComponentTypeId> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize index = 0; index < a.size(); ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* cell_state_name(CellState state) noexcept {
    switch (state) {
        case CellState::Unloaded:
            return "Unloaded";
        case CellState::Metadata:
            return "Metadata";
        case CellState::Prefetching:
            return "Prefetching";
        case CellState::Resident:
            return "Resident";
        case CellState::Activated:
            return "Activated";
        case CellState::Deactivating:
            return "Deactivating";
        case CellState::Evictable:
            return "Evictable";
    }
    return "unknown";
}

const char* channel_name(Channel channel) noexcept {
    switch (channel) {
        case Channel::Entities:
            return "entities";
        case Channel::Geometry:
            return "geometry";
        case Channel::Textures:
            return "textures";
        case Channel::Physics:
            return "physics";
        case Channel::Navigation:
            return "navigation";
        case Channel::Ai:
            return "ai";
        case Channel::Audio:
            return "audio";
        case Channel::Illumination:
            return "illumination";
        case Channel::kCount:
            break;
    }
    return "unknown";
}

const char* world_profile_name(WorldProfile profile) noexcept {
    switch (profile) {
        case WorldProfile::Client:
            return "client";
        case WorldProfile::DedicatedServer:
            return "dedicated-server";
        case WorldProfile::Editor:
            return "editor";
    }
    return "unknown";
}

ChannelMask profile_channels(WorldProfile profile) noexcept {
    ChannelMask mask;
    switch (profile) {
        case WorldProfile::DedicatedServer:
            // "A server profile SHALL typically require entities, physics, navigation, AI and
            // network metadata, and omit geometry, textures, audio and illumination."
            mask.set(Channel::Entities);
            mask.set(Channel::Physics);
            mask.set(Channel::Navigation);
            mask.set(Channel::Ai);
            return mask;
        case WorldProfile::Client:
        case WorldProfile::Editor:
            break;
    }
    return ChannelMask::all();
}

u32 CookedCell::row_count() const noexcept {
    u32 total = 0;
    for (const CookedBlock& block : blocks.span()) {
        total += block.count;
    }
    return total;
}

CellBuilder::CellBuilder(Allocator& allocator, CellId id, CellCoord coord) noexcept
    : allocator_(&allocator), cell_(allocator) {
    cell_.id = id;
    cell_.coord = coord;
}

namespace {

/// The block for one (archetype, layer) inside a cell, created if it is not there yet.
[[nodiscard]] Expected<CookedBlock*, Error> block_for(
    Allocator& allocator, CookedCell& cell, LayerId layer,
    Span<const ecs::ComponentTypeId> components) noexcept {
    for (CookedBlock& block : cell.blocks.span()) {
        if (block.layer == layer && same_components(block.components.span(), components)) {
            return &block;
        }
    }

    CookedBlock block(allocator);
    block.layer = layer;
    if (Status appended = block.components.append(components); !appended) {
        return make_unexpected(appended.error());
    }
    // emplace_back rather than resize: a default-constructed `Array<u8>` would take the ambient
    // allocator, and every allocation this module makes belongs to the allocator the caller handed
    // it. M1's budget tree is only useful if nothing quietly opts out of it.
    for (usize index = 0; index < components.size(); ++index) {
        if (Expected<Array<u8>*, Error> column = block.columns.emplace_back(allocator); !column) {
            return make_unexpected(column.error());
        }
    }
    if (Status pushed = cell.blocks.push_back(std::move(block)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return &cell.blocks[cell.blocks.size() - 1];
}

}  // namespace

Status append_cooked_row(Allocator& allocator, CookedCell& cell, PersistentId id, LayerId layer,
                         Span<const ecs::ComponentTypeId> components,
                         Span<const void* const> values, Span<const u32> sizes) noexcept {
    if (!id.is_valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a persistent identifier is assigned at authoring time and is never zero");
    }
    if (components.size() != values.size() || components.size() != sizes.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked row needs one value and one size per named component");
    }

    Expected<CookedBlock*, Error> block = block_for(allocator, cell, layer, components);
    if (!block) {
        return make_unexpected(block.error());
    }

    // Appending to every column keeps the block rectangular: `count` rows in each. A partial
    // failure here would leave columns of different lengths, so the row is appended to the ids
    // array LAST, and a failure part-way is reported before `count` is advanced.
    for (usize index = 0; index < components.size(); ++index) {
        Array<u8>& column = (*block)->columns[index];
        const usize before = column.size();
        if (Status sized = column.resize(before + sizes[index]); !sized) {
            return sized;
        }
        if (sizes[index] != 0 && values[index] != nullptr) {
            std::memcpy(column.data() + before, values[index], sizes[index]);
        } else if (sizes[index] != 0) {
            std::memset(column.data() + before, 0, sizes[index]);
        }
    }
    if (Status pushed = (*block)->ids.push_back(id); !pushed) {
        return pushed;
    }
    ++(*block)->count;
    return ok();
}

Status CellBuilder::add_entity(PersistentId id, LayerId layer,
                               Span<const ecs::ComponentTypeId> components,
                               Span<const void* const> values, Span<const u32> sizes) noexcept {
    return append_cooked_row(*allocator_, cell_, id, layer, components, values, sizes);
}

Status CellBuilder::add_payload(const CellPayload& payload) noexcept {
    return cell_.payloads.push_back(payload);
}

Status CellBuilder::add_asset(AssetId asset) noexcept {
    return cell_.assets.push_back(asset);
}

Status CellBuilder::add_reference(const PersistentReference& reference) noexcept {
    if (Status pushed = cell_.references.push_back(reference); !pushed) {
        return pushed;
    }
    // The hard dependency closure is the set of cells a `RequireLoaded` reference forces resident.
    // Accumulated here so that the cooker never has to walk the references a second time, and so
    // that the dependency-explosion report has its input by construction.
    if (reference.policy != ReferencePolicy::RequireLoaded || !reference.target_cell.is_valid() ||
        reference.target_cell == cell_.id) {
        return ok();
    }
    for (const CellId existing : cell_.hard_dependencies.span()) {
        if (existing == reference.target_cell) {
            return ok();
        }
    }
    return cell_.hard_dependencies.push_back(reference.target_cell);
}

CookedCell CellBuilder::finish() noexcept {
    // The cost model, computed at cook time so that the planner compares candidates before
    // requesting either. The activation estimate is a linear model in rows and bytes; it is
    // deliberately crude, because `world-partition-and-streaming` requires it to be VALIDATED
    // against measured cost and reported when it diverges, and a crude model that is checked is
    // worth more than a clever one that is not.
    u64 bytes = 0;
    u32 rows = 0;
    for (const CookedBlock& block : cell_.blocks.span()) {
        rows += block.count;
        for (const Array<u8>& column : block.columns.span()) {
            bytes += column.size();
        }
        bytes += block.ids.size() * sizeof(PersistentId);
    }
    for (const CellPayload& payload : cell_.payloads.span()) {
        bytes += payload.size_bytes;
        if (payload.channel == Channel::Physics) {
            cell_.cost.physics_bodies += 1;
        }
        if (payload.channel == Channel::Navigation) {
            cell_.cost.navigation_tiles += 1;
        }
        if (payload.channel == Channel::Geometry || payload.channel == Channel::Textures) {
            cell_.cost.gpu_memory_bytes += payload.size_bytes;
        }
    }
    cell_.cost.entities = rows;
    cell_.cost.io_bytes = bytes;
    cell_.cost.cpu_memory_bytes = bytes;
    cell_.cost.activation_time = estimate_activation_time(rows, bytes);

    CookedCell finished = std::move(cell_);
    cell_ = CookedCell(*allocator_);
    return finished;
}

namespace {

/// The first identifier that appears twice anywhere in the cell, or an invalid one.
///
/// Quadratic in the cell's row count on purpose: a cell holds thousands of rows, not millions, and
/// a hash table here would have to be allocated, which would make validation fallible for no
/// benefit.
[[nodiscard]] PersistentId first_duplicate_id(const CookedCell& cell) noexcept {
    for (const CookedBlock& block : cell.blocks.span()) {
        for (usize outer = 0; outer < block.ids.size(); ++outer) {
            for (const CookedBlock& other : cell.blocks.span()) {
                const usize from = (&other == &block) ? outer + 1 : 0;
                for (usize inner = from; inner < other.ids.size(); ++inner) {
                    if (other.ids[inner] == block.ids[outer]) {
                        return block.ids[outer];
                    }
                }
            }
        }
    }
    return PersistentId{};
}

/// True when every block is rectangular: one identifier per row, one column per named component.
[[nodiscard]] bool blocks_are_rectangular(const CookedCell& cell) noexcept {
    return std::ranges::all_of(cell.blocks.span(), [](const CookedBlock& block) {
        return block.ids.size() == block.count && block.components.size() == block.columns.size();
    });
}

/// True when every payload names a channel the cell was cooked with.
[[nodiscard]] bool payloads_match_channels(const CookedCell& cell) noexcept {
    return std::ranges::all_of(cell.payloads.span(), [&cell](const CellPayload& payload) {
        return cell.channels.has(payload.channel);
    });
}

}  // namespace

ValidationResult validate_cell(const CookedCell& cell) noexcept {
    ValidationResult result;

    if (!blocks_are_rectangular(cell)) {
        result.ok = false;
        result.message =
            "a cooked block is not rectangular: its identifier array and its column "
            "count disagree with its row count";
        return result;
    }

    // "Duplicate persistent identifiers SHALL be a cook error."
    if (const PersistentId duplicate = first_duplicate_id(cell); duplicate.is_valid()) {
        result.ok = false;
        result.duplicate = duplicate;
        result.message = "duplicate persistent identifier in one cell";
        return result;
    }

    if (!payloads_match_channels(cell)) {
        result.ok = false;
        result.message = "the cell carries a payload for a channel it was not cooked with";
        return result;
    }
    return result;
}

}  // namespace cy::world
