#include "content.h"

#include <cy/core/reflect/attributes.h>

#include <cstddef>
#include <cstring>

namespace cy::sample::openworld {
namespace {

/// The identifiers this sample's descriptors carry.
///
/// NOT MANIFEST IDENTIFIERS, and the range says so. Nothing here is registered into
/// `reflect::default_registry()` and nothing here is written to a committed file; the 9600s are
/// visibly not numbers `identity/manifest.toml` issued, and do not collide with the ECS suite's
/// 9000s, the serialization suite's 9100s, the scene suite's 9300s or the save suite's 9400s.
constexpr u32 kIdentTypeId = 9601;
constexpr u32 kPlacementTypeId = 9602;
constexpr u32 kPropTypeId = 9603;
constexpr u32 kStructureTypeId = 9604;

/// A field descriptor of the shape generated code emits. The generator's annotated-header list is
/// not a sample's to edit, and a `reflect::TypeInfo` is plain constexpr data — the same seam
/// src/save/tests/fixtures.h and src/ecs/tests/fixtures.h record.
[[nodiscard]] reflect::FieldInfo make_field(const char* name, u32 id, reflect::FieldKind kind,
                                            u32 offset, u32 size,
                                            reflect::PersistenceKind persistence) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes.declared = reflect::AttributeKind::Persistence;
    field.attributes.persistence = persistence;
    return field;
}

[[nodiscard]] Error content_error(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

/// SplitMix64, seeded by the content and by nothing else.
///
/// Deliberately local rather than `cy::hash_bytes()`: that one is seeded per process in development
/// builds, and a cook whose output depended on it would produce a different world on every run. The
/// cell identifiers next door are fixed-seed for exactly the same reason (world/coordinates.h).
[[nodiscard]] constexpr u64 mix(u64 value) noexcept {
    value += 0x9e37'79b9'7f4a'7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr u64 fold(u64 accumulator, u64 value) noexcept {
    return mix(accumulator ^ mix(value));
}

void copy_name(char (&destination)[32], std::string_view text) noexcept {
    const usize count = text.size() < 31 ? text.size() : 31;
    std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
}

/// The words of one line, quotes honoured. At most `kMaxWords`; a longer line is a content error
/// rather than a silently truncated record.
constexpr usize kMaxWords = 12;

struct Words {
    std::string_view items[kMaxWords];
    usize count = 0;

    [[nodiscard]] std::string_view at(usize index) const noexcept {
        return index < count ? items[index] : std::string_view{};
    }
};

[[nodiscard]] Words split(std::string_view line) noexcept {
    Words words;
    usize index = 0;
    while (index < line.size() && words.count < kMaxWords) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= line.size()) {
            break;
        }
        const bool quoted = line[index] == '"';
        index += quoted ? 1 : 0;
        const usize start = index;
        while (index < line.size() &&
               (quoted ? line[index] != '"' : line[index] != ' ' && line[index] != '\t')) {
            ++index;
        }
        words.items[words.count++] = line.substr(start, index - start);
        index += (quoted && index < line.size()) ? 1 : 0;
    }
    return words;
}

[[nodiscard]] u64 to_number(std::string_view text) noexcept {
    u64 value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return value;
        }
        value = (value * 10) + static_cast<u64>(character - '0');
    }
    return value;
}

/// The value following `key` in a `key value key value` tail, or `fallback`.
[[nodiscard]] u64 tagged(const Words& words, usize first, std::string_view key,
                         u64 fallback) noexcept {
    for (usize index = first; index + 1 < words.count; index += 2) {
        if (words.items[index] == key) {
            return to_number(words.items[index + 1]);
        }
    }
    return fallback;
}

}  // namespace

const reflect::TypeInfo& ident_type() noexcept {
    static const reflect::FieldInfo fields[] = {
        make_field("value", 1, reflect::FieldKind::U64, static_cast<u32>(offsetof(Ident, value)),
                   sizeof(u64), reflect::PersistenceKind::Authoring),
    };
    static reflect::TypeInfo info;
    info.name = "cy::sample::openworld::Ident";
    info.id = reflect::TypeId(kIdentTypeId);
    info.size = static_cast<u32>(sizeof(Ident));
    info.alignment = static_cast<u32>(alignof(Ident));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

const reflect::TypeInfo& placement_type() noexcept {
    using reflect::FieldKind;
    using reflect::PersistenceKind;
    static const reflect::FieldInfo fields[] = {
        make_field("x", 1, FieldKind::F32, static_cast<u32>(offsetof(Placement, x)), sizeof(f32),
                   PersistenceKind::Authoring),
        make_field("y", 2, FieldKind::F32, static_cast<u32>(offsetof(Placement, y)), sizeof(f32),
                   PersistenceKind::Authoring),
        make_field("z", 3, FieldKind::F32, static_cast<u32>(offsetof(Placement, z)), sizeof(f32),
                   PersistenceKind::Authoring),
    };
    static reflect::TypeInfo info;
    info.name = "cy::sample::openworld::Placement";
    info.id = reflect::TypeId(kPlacementTypeId);
    info.size = static_cast<u32>(sizeof(Placement));
    info.alignment = static_cast<u32>(alignof(Placement));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

const reflect::TypeInfo& prop_type() noexcept {
    using reflect::FieldKind;
    using reflect::PersistenceKind;
    static const reflect::FieldInfo fields[] = {
        make_field("kind", 1, FieldKind::U32, static_cast<u32>(offsetof(Prop, kind)), sizeof(u32),
                   PersistenceKind::Authoring),
        make_field("variant", 2, FieldKind::U32, static_cast<u32>(offsetof(Prop, variant)),
                   sizeof(u32), PersistenceKind::Authoring),
    };
    static reflect::TypeInfo info;
    info.name = "cy::sample::openworld::Prop";
    info.id = reflect::TypeId(kPropTypeId);
    info.size = static_cast<u32>(sizeof(Prop));
    info.alignment = static_cast<u32>(alignof(Prop));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

const reflect::TypeInfo& structure_type() noexcept {
    using reflect::FieldKind;
    using reflect::PersistenceKind;
    // BOTH FIELDS ARE PersistentState, and that is what a save carries. `Prop` and `Placement`
    // above are Authoring: the asset defines them, and a save that wrote them would be shipping the
    // world's authored content inside the player's save file.
    static const reflect::FieldInfo fields[] = {
        make_field("material", 1, FieldKind::U32, static_cast<u32>(offsetof(Structure, material)),
                   sizeof(u32), PersistenceKind::PersistentState),
        make_field("integrity", 2, FieldKind::U32, static_cast<u32>(offsetof(Structure, integrity)),
                   sizeof(u32), PersistenceKind::PersistentState),
    };
    static reflect::TypeInfo info;
    info.name = "cy::sample::openworld::Structure";
    info.id = reflect::TypeId(kStructureTypeId);
    info.size = static_cast<u32>(sizeof(Structure));
    info.alignment = static_cast<u32>(alignof(Structure));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

Expected<Components, Error> register_components(ecs::World& world) noexcept {
    struct Registration {
        const reflect::TypeInfo& (*type)() noexcept;
        ecs::ComponentTypeId Components::* field;
    };
    const Registration registrations[] = {
        {ident_type, &Components::ident},
        {placement_type, &Components::placement},
        {prop_type, &Components::prop},
        {structure_type, &Components::structure},
    };

    Components components;
    for (const Registration& registration : registrations) {
        const Expected<ecs::ComponentTypeId, Error> registered =
            world.components().register_reflected(registration.type());
        if (!registered) {
            return make_unexpected(registered.error());
        }
        components.*registration.field = *registered;
    }
    return components;
}

Status WorldContent::parse(std::string_view text) noexcept {
    u32 number = 0;
    usize start = 0;
    while (start <= text.size()) {
        const usize newline = text.find('\n', start);
        const usize end = newline == std::string_view::npos ? text.size() : newline;
        ++number;
        if (Status parsed = parse_line(text.substr(start, end - start), number); !parsed) {
            return parsed;
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    if (extent_ <= 0 || props_per_cell_ == 0 || palette_.empty()) {
        return make_unexpected(
            content_error("the stream declares no world, no props per cell, or no palette"));
    }
    return ok();
}

Status WorldContent::parse_line(std::string_view line, u32 number) noexcept {
    (void)number;
    const Words words = split(line);
    if (words.count == 0 || words.at(0).starts_with("#")) {
        return ok();
    }
    const std::string_view head = words.at(0);

    // A REPEATED HEADER IS NOT AN ERROR. The shipped stream is the concatenation of the sources the
    // build graph cooked, so it carries one header per source — see
    // project/build/openworld.cybuild.
    if (head == "cyopenworld") {
        return words.at(1) == "1" ? ok()
                                  : make_unexpected(content_error("unsupported stream version"));
    }
    if (head == "world") {
        copy_name(name_, words.at(1));
        return ok();
    }
    if (head == "extent") {
        extent_ = static_cast<i32>(to_number(words.at(1)));
        return ok();
    }
    if (head == "cell-size") {
        cell_size_ = static_cast<f32>(to_number(words.at(1)));
        return ok();
    }
    if (head == "seed") {
        seed_ = to_number(words.at(1));
        return ok();
    }
    if (head == "props-per-cell") {
        props_per_cell_ = static_cast<u32>(to_number(words.at(1)));
        return ok();
    }
    if (head == "prop") {
        PropKind entry;
        entry.kind = static_cast<u32>(to_number(words.at(1)));
        copy_name(entry.name, words.at(2));
        entry.weight = static_cast<u32>(tagged(words, 3, "weight", 1));
        entry.material = static_cast<u32>(tagged(words, 3, "material", entry.kind));
        return palette_.push_back(entry);
    }
    if (head == "landmark") {
        Landmark entry;
        entry.x = static_cast<i32>(to_number(words.at(1)));
        entry.z = static_cast<i32>(to_number(words.at(2)));
        copy_name(entry.name, words.at(3));
        entry.kind = static_cast<u32>(tagged(words, 4, "kind", 0));
        return landmarks_.push_back(entry);
    }
    return make_unexpected(content_error("unrecognised record"));
}

u32 WorldContent::cell_count() const noexcept {
    return static_cast<u32>(extent_ * extent_);
}

u64 WorldContent::entity_count() const noexcept {
    return (static_cast<u64>(cell_count()) * props_per_cell_) + landmarks_.size();
}

f64 WorldContent::span_metres() const noexcept {
    return static_cast<f64>(extent_) * static_cast<f64>(cell_size_);
}

u64 WorldContent::digest() const noexcept {
    u64 accumulator = mix(0x6379'6265'726e'6774ULL);
    accumulator = fold(accumulator, static_cast<u64>(extent_));
    accumulator = fold(accumulator, static_cast<u64>(cell_size_));
    accumulator = fold(accumulator, seed_);
    accumulator = fold(accumulator, props_per_cell_);
    for (const PropKind& entry : palette_.span()) {
        accumulator = fold(accumulator, entry.kind);
        accumulator = fold(accumulator, entry.weight);
        accumulator = fold(accumulator, entry.material);
    }
    for (const Landmark& entry : landmarks_.span()) {
        accumulator = fold(accumulator, static_cast<u64>(entry.x));
        accumulator = fold(accumulator, static_cast<u64>(entry.z));
        accumulator = fold(accumulator, entry.kind);
    }
    return accumulator;
}

const Landmark* WorldContent::landmark_at(i32 x, i32 z) const noexcept {
    for (const Landmark& entry : landmarks_.span()) {
        if (entry.x == x && entry.z == z) {
            return &entry;
        }
    }
    return nullptr;
}

world::PartitionConfig WorldContent::partition() const noexcept {
    // A uniform grid is a hierarchy of one level, which is the spelling world/partition.h asks for.
    return world::uniform_grid_config(cell_size_);
}

world::PersistentId identity_of(const WorldContent& content, i32 x, i32 z, u32 row) noexcept {
    const u64 ordinal =
        (static_cast<u64>(z) * static_cast<u64>(content.extent())) + static_cast<u64>(x);
    return world::PersistentId{(ordinal * 1000ULL) + row + 1ULL};
}

world::CookedCell cook_cell(Allocator& allocator, const WorldContent& content,
                            const world::Partitioner& partitioner, world::CellCoord coord,
                            const Components& components) noexcept {
    world::CellBuilder builder(allocator, partitioner.id_of(coord), coord);
    const ecs::ComponentTypeId types[] = {components.ident, components.placement, components.prop,
                                          components.structure};
    const u32 sizes[] = {static_cast<u32>(sizeof(Ident)), static_cast<u32>(sizeof(Placement)),
                         static_cast<u32>(sizeof(Prop)), static_cast<u32>(sizeof(Structure))};
    const Span<const PropKind> palette = content.palette();
    u32 total_weight = 0;
    for (const PropKind& entry : palette) {
        total_weight += entry.weight;
    }

    const u64 cell_seed =
        fold(fold(content.seed(), static_cast<u64>(coord.x)), static_cast<u64>(coord.z));
    for (u32 row = 0; row < content.props_per_cell(); ++row) {
        const u64 noise = fold(cell_seed, row);
        u32 choice = static_cast<u32>(noise % (total_weight == 0 ? 1 : total_weight));
        const PropKind* chosen = palette.data();
        for (const PropKind& entry : palette) {
            if (choice < entry.weight) {
                chosen = &entry;
                break;
            }
            choice -= entry.weight;
        }

        const Ident ident{identity_of(content, coord.x, coord.z, row).value};
        Placement placement;
        placement.x = static_cast<f32>((noise >> 8U) % 128U);
        placement.z = static_cast<f32>((noise >> 24U) % 128U);
        const Prop prop{chosen->kind, static_cast<u32>((noise >> 40U) % 4U)};
        const Structure structure{chosen->material, 100U};
        const void* values[] = {&ident, &placement, &prop, &structure};
        // A failed append shows up as a row count that does not match what the content declared,
        // which the report prints; a cook that stopped halfway would be worse than one that says
        // so.
        (void)builder.add_entity(world::PersistentId{ident.value}, world::kDefaultLayer, types,
                                 values, sizes);
    }

    if (const Landmark* landmark = content.landmark_at(coord.x, coord.z); landmark != nullptr) {
        const world::PersistentId id =
            identity_of(content, coord.x, coord.z, content.props_per_cell());
        const Ident ident{id.value};
        const Placement placement{64.0F, 0.0F, 64.0F};
        const Prop prop{landmark->kind, 0};
        const Structure structure{landmark->kind, 100U};
        const void* values[] = {&ident, &placement, &prop, &structure};
        (void)builder.add_entity(id, world::kDefaultLayer, types, values, sizes);
    }

    // The channels a cell carries, and what each costs to bring resident. The numbers are the
    // sample's own and are what the streaming budget is spent against — see traversal.cpp.
    const struct {
        world::Channel channel;
        u64 bytes;
    } payloads[] = {
        {world::Channel::Geometry, 96ULL * 1024},
        {world::Channel::Textures, 64ULL * 1024},
        {world::Channel::Physics, 24ULL * 1024},
        {world::Channel::Navigation, 8ULL * 1024},
    };
    for (const auto& entry : payloads) {
        world::CellPayload payload;
        payload.channel = entry.channel;
        payload.size_bytes = entry.bytes;
        (void)builder.add_payload(payload);
    }
    return builder.finish();
}

}  // namespace cy::sample::openworld
