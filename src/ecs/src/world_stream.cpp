// The world byte stream: serialize, deserialize, and the entity reference remapping between them.
// Task 2.10.
//
// The format is self-describing. It carries a component table naming every component it uses —
// kind, manifest identifier when there is one, name, sizes — so a reader can bind the stream's
// numbering to its own world's without the two having registered in the same order. What it
// deliberately does NOT carry is a field layout: component bytes are written as the chunk holds
// them, which is why a stream is a cooked artefact and the text form (`serialization-and-prefabs`)
// is the authoring one.
//
// ENTITY REFERENCES ARE THE ONLY THING REWRITTEN. Every reference site is a declared byte offset
// within a component (component.h), so remapping is a strided pass over known columns: no
// reflection lookup, no per-row decision about what a field means. A reference to an entity outside
// the written set becomes the null entity at write time — a dangling reference is made explicit
// rather than left to resolve against whatever occupies that index later.
//
// WHY THE TWO SIDES ARE CLASSES RATHER THAN TWO LONG FUNCTIONS. Writing a block is six steps that
// all need the same four things — the world, the writer, the local-id table and a scratch buffer —
// and threading those through free functions produced parameter lists nobody could read. The state
// lives in `WorldWriter` and `WorldReader`, one step per member function, and the format is stated
// once in each direction in the same order.

#include <cy/ecs/snapshot.h>

#include <cstring>

namespace cy::ecs {
namespace {

constexpr u32 kMagic = 0x3057'5943u;  // "CyW0"
constexpr u32 kNoLocal = 0xFFFF'FFFFu;
/// A block whose component count is zero terminates the block list.
constexpr u32 kEndOfBlocks = 0;

/// Appending writer over a growable byte array. Every write is fallible because the array's growth
/// is: under -fno-exceptions there is no other way for it to report that the allocator refused.
class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(&out) {}

    [[nodiscard]] Status u32_value(u32 value) noexcept { return raw(&value, sizeof(value)); }
    [[nodiscard]] Status u8_value(u8 value) noexcept { return raw(&value, sizeof(value)); }

    [[nodiscard]] Status text(const char* value) noexcept {
        u32 length = 0;
        while (value != nullptr && value[length] != '\0') {
            ++length;
        }
        if (Status written = u32_value(length); !written) {
            return written;
        }
        return (length == 0) ? ok() : raw(value, length);
    }

    [[nodiscard]] Status raw(const void* data, usize size) noexcept {
        return out_->append(Span<const u8>(static_cast<const u8*>(data), size));
    }

private:
    Array<u8>* out_;
};

/// Bounds-checked reader. Every read can fail, and a truncated stream is reported at the read that
/// ran off the end rather than by producing a plausible value.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] Expected<u32, Error> u32_value() noexcept {
        u32 value = 0;
        if (Status read = raw(&value, sizeof(value)); !read) {
            return make_unexpected(read.error());
        }
        return value;
    }
    [[nodiscard]] Expected<u8, Error> u8_value() noexcept {
        u8 value = 0;
        if (Status read = raw(&value, sizeof(value)); !read) {
            return make_unexpected(read.error());
        }
        return value;
    }

    [[nodiscard]] Status raw(void* data, usize size) noexcept {
        if (cursor_ + size > bytes_.size()) {
            return fail(ErrorCode::OutOfRange, "the world stream ends inside a record");
        }
        std::memcpy(data, static_cast<const void*>(bytes_.data() + cursor_), size);
        cursor_ += size;
        return ok();
    }

    /// Step over `size` bytes without copying them.
    [[nodiscard]] Status skip(usize size) noexcept {
        if (cursor_ + size > bytes_.size()) {
            return fail(ErrorCode::OutOfRange, "the world stream ends inside a block");
        }
        cursor_ += size;
        return ok();
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
};

/// Rewrite every declared entity reference in one component value.
///
/// `translate` maps an entity to what should be written or read in its place. This is the whole of
/// the remapping: a loop over a component's declared offsets, and a load and a store at each.
template <class Translate>
void remap_value(const ComponentInfo& info, u8* value, Translate&& translate) noexcept {
    for (u32 index = 0; index < info.entity_offset_count; ++index) {
        u8* site = value + info.entity_offsets[index];
        Entity reference;
        std::memcpy(static_cast<void*>(&reference), static_cast<const void*>(site), sizeof(Entity));
        reference = translate(reference);
        std::memcpy(static_cast<void*>(site), static_cast<const void*>(&reference), sizeof(Entity));
    }
}

/// The first element of a buffer component's entry, inline or on the heap.
[[nodiscard]] const u8* buffer_elements(const u8* entry, const ComponentInfo& info) noexcept {
    // NOLINTBEGIN(bugprone-casting-through-void) — through void* on purpose; see the note in
    // buffer.h. A direct reinterpret_cast is what -Wcast-align reports under -Werror.
    const auto* header = static_cast<const BufferHeader*>(static_cast<const void*>(entry));
    // NOLINTEND(bugprone-casting-through-void)
    return (header->heap != nullptr)
               ? static_cast<const u8*>(header->heap)
               : (entry + align_up(sizeof(BufferHeader), info.element_alignment));
}

[[nodiscard]] const BufferHeader* buffer_header(const u8* entry) noexcept {
    // NOLINTBEGIN(bugprone-casting-through-void) — as above.
    return static_cast<const BufferHeader*>(static_cast<const void*>(entry));
    // NOLINTEND(bugprone-casting-through-void)
}

// --- Writing
// -----------------------------------------------------------------------------------

class WorldWriter {
public:
    WorldWriter(World& world, Array<u8>& out) noexcept
        : world_(&world),
          writer_(out),
          local_of_(world.allocator()),
          order_(world.allocator()),
          rows_(world.allocator()),
          scratch_(world.allocator()) {}

    [[nodiscard]] Status run(Span<const Entity> subset) noexcept {
        if (Status assigned = assign_local_ids(subset); !assigned) {
            return assigned;
        }
        if (Status written = write_header(); !written) {
            return written;
        }
        if (Status written = write_component_table(); !written) {
            return written;
        }
        if (Status written = write_blocks(); !written) {
            return written;
        }
        return writer_.u32_value(kEndOfBlocks);
    }

private:
    /// The stream's only entity vocabulary: dense indices in write order.
    [[nodiscard]] Status assign_local_ids(Span<const Entity> subset) noexcept {
        if (Status resized = local_of_.resize(world_->entities().capacity()); !resized) {
            return resized;
        }
        for (u32& local : local_of_) {
            local = kNoLocal;
        }
        if (Status collected = subset.empty() ? collect_all() : collect_subset(subset);
            !collected) {
            return collected;
        }
        for (usize index = 0; index < order_.size(); ++index) {
            local_of_[order_[index].index()] = static_cast<u32>(index);
        }
        return ok();
    }

    [[nodiscard]] Status collect_all() noexcept {
        for (u32 index = 0; index < world_->archetypes().size(); ++index) {
            Archetype& archetype = world_->archetypes().at(index);
            for (u32 chunk = 0; chunk < archetype.chunk_count(); ++chunk) {
                ChunkView view = archetype.chunk(chunk);
                const auto* keys = static_cast<const Entity*>(view.keys());
                if (Status appended = order_.append(Span<const Entity>(keys, view.count()));
                    !appended) {
                    return appended;
                }
            }
        }
        return ok();
    }

    [[nodiscard]] Status collect_subset(Span<const Entity> subset) noexcept {
        for (const Entity entity : subset) {
            if (!world_->is_alive(entity)) {
                continue;
            }
            if (Status pushed = order_.push_back(entity); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    /// A reference to an entity outside the written set becomes null. See the header.
    [[nodiscard]] Entity to_local(Entity reference) const noexcept {
        if (!reference.valid() || reference.index() >= local_of_.size() ||
            !world_->is_alive(reference)) {
            return kNoEntity;
        }
        const u32 local = local_of_[reference.index()];
        return (local == kNoLocal) ? kNoEntity : Entity::make(local, 1);
    }

    [[nodiscard]] Status write_header() noexcept {
        if (Status written = writer_.u32_value(kMagic); !written) {
            return written;
        }
        if (Status written = writer_.u32_value(kWorldStreamVersion); !written) {
            return written;
        }
        if (Status written = writer_.u32_value(world_->components().size()); !written) {
            return written;
        }
        return writer_.u32_value(static_cast<u32>(order_.size()));
    }

    [[nodiscard]] Status write_component_table() noexcept {
        const ComponentRegistry& registry = world_->components();
        for (ComponentTypeId component = 0; component < registry.size(); ++component) {
            const ComponentInfo& info = registry.info(component);
            if (Status written = writer_.u8_value(static_cast<u8>(info.kind)); !written) {
                return written;
            }
            if (Status written = writer_.u32_value(info.type_id.value()); !written) {
                return written;
            }
            if (Status written = writer_.text(info.name); !written) {
                return written;
            }
            const u32 sizes[] = {info.size, info.element_size, info.value_size};
            for (const u32 size : sizes) {
                if (Status written = writer_.u32_value(size); !written) {
                    return written;
                }
            }
        }
        return ok();
    }

    [[nodiscard]] Status write_blocks() noexcept {
        for (u32 index = 0; index < world_->archetypes().size(); ++index) {
            Archetype& archetype = world_->archetypes().at(index);
            if (Status gathered = gather_rows(index); !gathered) {
                return gathered;
            }
            if (rows_.empty()) {
                continue;
            }
            if (Status written = write_block(archetype); !written) {
                return written;
            }
        }
        return ok();
    }

    /// The local ids, in write order, of the selected entities that live in this archetype. Rows
    /// are gathered so that a filtered write is the same code path as a whole-world one.
    [[nodiscard]] Status gather_rows(u32 archetype_id) noexcept {
        rows_.clear();
        for (usize position = 0; position < order_.size(); ++position) {
            const EntityLocation* location = world_->location(order_[position]);
            if (location == nullptr || location->archetype != archetype_id) {
                continue;
            }
            if (Status pushed = rows_.push_back(static_cast<u32>(position)); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    [[nodiscard]] Status write_block(Archetype& archetype) noexcept {
        if (Status written = write_block_header(archetype); !written) {
            return written;
        }
        const ComponentRegistry& registry = world_->components();
        for (const ComponentTypeId component : archetype.components()) {
            const ComponentInfo& info = registry.info(component);
            if (!kind_has_column(info.kind)) {
                continue;
            }
            if (Status written = write_column(archetype, info); !written) {
                return written;
            }
        }
        return ok();
    }

    [[nodiscard]] Status write_block_header(Archetype& archetype) noexcept {
        if (Status written = writer_.u32_value(static_cast<u32>(archetype.components().size()));
            !written) {
            return written;
        }
        for (const ComponentTypeId component : archetype.components()) {
            if (Status written = writer_.u32_value(component); !written) {
                return written;
            }
        }
        if (Status written = writer_.u32_value(static_cast<u32>(archetype.shared().size()));
            !written) {
            return written;
        }
        for (const SharedValue& shared : archetype.shared()) {
            if (Status written = writer_.u32_value(shared.component); !written) {
                return written;
            }
            const u32 size = world_->components().info(shared.component).value_size;
            if (Status written =
                    writer_.raw(world_->shared_value(shared.component, shared.value), size);
                !written) {
                return written;
            }
        }
        return writer_.u32_value(static_cast<u32>(rows_.size()));
    }

    [[nodiscard]] Status write_column(Archetype& archetype, const ComponentInfo& info) noexcept {
        for (const u32 position : rows_) {
            const EntityLocation* location = world_->location(order_[position]);
            if (location == nullptr) {
                // The row list was built from the same table a moment ago and nothing structural
                // has run since, so this cannot happen; reported rather than asserted because an
                // assertion is compiled out of two of the four profiles.
                return fail(ErrorCode::Internal, "an entity left its archetype mid-write");
            }
            const auto* source =
                static_cast<const u8*>(archetype.value_at(location->chunk, location->row, info.id));
            Status written = (info.kind == ComponentKind::Buffer) ? write_buffer_row(info, source)
                                                                  : write_value(info, source);
            if (!written) {
                return written;
            }
        }
        return ok();
    }

    [[nodiscard]] Status write_value(const ComponentInfo& info, const u8* source) noexcept {
        scratch_.clear();
        if (Status appended = scratch_.append(Span<const u8>(source, info.size)); !appended) {
            return appended;
        }
        remap_value(info, scratch_.data(), [this](Entity entity) { return to_local(entity); });
        return writer_.raw(scratch_.data(), info.size);
    }

    [[nodiscard]] Status write_buffer_row(const ComponentInfo& info, const u8* entry) noexcept {
        const u32 size = buffer_header(entry)->size;
        if (Status written = writer_.u32_value(size); !written) {
            return written;
        }
        const u8* elements = buffer_elements(entry, info);
        if (!info.elements_are_entities) {
            return writer_.raw(elements, usize{size} * info.element_size);
        }
        for (u32 element = 0; element < size; ++element) {
            Entity reference;
            std::memcpy(static_cast<void*>(&reference),
                        static_cast<const void*>(elements + (usize{element} * info.element_size)),
                        sizeof(Entity));
            reference = to_local(reference);
            if (Status written = writer_.raw(&reference, sizeof(Entity)); !written) {
                return written;
            }
        }
        return ok();
    }

    World* world_;
    Writer writer_;
    Array<u32> local_of_;
    Array<Entity> order_;
    Array<u32> rows_;
    Array<u8> scratch_;
};

// --- Reading
// -----------------------------------------------------------------------------------

class WorldReader {
public:
    WorldReader(World& world, Span<const u8> bytes) noexcept
        : world_(&world),
          bytes_(bytes),
          bindings_(world.allocator()),
          created_(world.allocator()),
          plans_(world.allocator()),
          components_(world.allocator()),
          shared_(world.allocator()),
          scratch_(world.allocator()) {}

    [[nodiscard]] Status run(Array<Entity>& out) noexcept {
        Reader reader(bytes_);
        if (Status read = read_header(reader); !read) {
            return read;
        }
        if (Status read = bind_components(reader); !read) {
            return read;
        }
        // Two passes over the entity data, because a reference may name an entity in a later block.
        // The first creates every entity; the second fills the rows in and resolves the references
        // against the now-complete table.
        Reader creating = reader;
        if (Status made = create_entities(creating); !made) {
            return made;
        }
        if (Status filled = fill_entities(reader); !filled) {
            return filled;
        }
        return out.append(created_.span());
    }

private:
    struct Binding {
        ComponentTypeId component = kInvalidComponent;
    };

    struct BlockPlan {
        u32 first_local = 0;
        u32 rows = 0;
    };

    [[nodiscard]] Status read_header(Reader& reader) noexcept {
        Expected<u32, Error> magic = reader.u32_value();
        if (!magic || *magic != kMagic) {
            return fail(ErrorCode::InvalidArgument, "this is not a world stream");
        }
        Expected<u32, Error> version = reader.u32_value();
        if (!version) {
            return make_unexpected(version.error());
        }
        if (*version != kWorldStreamVersion) {
            return fail(ErrorCode::Unsupported, "this world stream was written by another version");
        }
        Expected<u32, Error> components = reader.u32_value();
        if (!components) {
            return make_unexpected(components.error());
        }
        component_count_ = *components;
        Expected<u32, Error> entities = reader.u32_value();
        if (!entities) {
            return make_unexpected(entities.error());
        }
        return created_.reserve(*entities);
    }

    /// Bind the stream's component numbering to this world's, by manifest identifier where the
    /// component has one and by name where it does not.
    [[nodiscard]] Status bind_components(Reader& reader) noexcept {
        char name[256];
        for (u32 index = 0; index < component_count_; ++index) {
            Expected<u8, Error> kind = reader.u8_value();
            Expected<u32, Error> type_id = reader.u32_value();
            Expected<u32, Error> length = reader.u32_value();
            if (!kind || !type_id || !length) {
                return fail(ErrorCode::OutOfRange,
                            "the world stream ends inside its component "
                            "table");
            }
            if (*length >= sizeof(name)) {
                return fail(ErrorCode::OutOfRange, "a component name in the stream is too long");
            }
            if (Status read = reader.raw(name, *length); !read) {
                return read;
            }
            name[*length] = '\0';
            // The three size fields; this world's registry is the authority on them, so they are
            // read past rather than trusted.
            if (Status read = reader.skip(3 * sizeof(u32)); !read) {
                return read;
            }
            if (Status bound = bind_one(*type_id, static_cast<const char*>(name)); !bound) {
                return bound;
            }
        }
        return ok();
    }

    [[nodiscard]] Status bind_one(u32 type_id, const char* name) noexcept {
        const ComponentRegistry& registry = world_->components();
        const ComponentInfo* info =
            (type_id != 0) ? registry.find(reflect::TypeId(type_id)) : registry.find(name);
        if (info == nullptr) {
            // Named rather than skipped: a stream naming a component the world has not registered
            // would otherwise load with a hole in it, and the hole is silent.
            return fail(ErrorCode::NotFound,
                        "the stream names a component this world has not registered");
        }
        return bindings_.push_back(Binding{info->id});
    }

    /// Pass one: every block's entities, created empty, with the payload stepped over.
    [[nodiscard]] Status create_entities(Reader& reader) noexcept {
        while (true) {
            Expected<u32, Error> count = reader.u32_value();
            if (!count) {
                return make_unexpected(count.error());
            }
            if (*count == kEndOfBlocks) {
                return ok();
            }
            if (Status read = read_component_list(reader, *count); !read) {
                return read;
            }
            if (Status read = read_shared_values(reader); !read) {
                return read;
            }
            Expected<u32, Error> rows = reader.u32_value();
            if (!rows) {
                return make_unexpected(rows.error());
            }
            if (Status made = instantiate_block(*rows); !made) {
                return made;
            }
            if (Status skipped = skip_payload(reader, *rows); !skipped) {
                return skipped;
            }
        }
    }

    [[nodiscard]] Status read_component_list(Reader& reader, u32 count) noexcept {
        components_.clear();
        for (u32 index = 0; index < count; ++index) {
            Expected<u32, Error> stream_component = reader.u32_value();
            if (!stream_component) {
                return make_unexpected(stream_component.error());
            }
            if (*stream_component >= bindings_.size()) {
                return fail(ErrorCode::OutOfRange, "a block names a component the table lacks");
            }
            if (Status pushed = components_.push_back(bindings_[*stream_component].component);
                !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    /// Read a block's shared values and intern them into this world, which is what turns the
    /// stream's payload into an archetype identity here.
    [[nodiscard]] Status read_shared_values(Reader& reader) noexcept {
        shared_.clear();
        Expected<u32, Error> count = reader.u32_value();
        if (!count) {
            return make_unexpected(count.error());
        }
        for (u32 index = 0; index < *count; ++index) {
            Expected<u32, Error> stream_component = reader.u32_value();
            if (!stream_component || *stream_component >= bindings_.size()) {
                return fail(ErrorCode::OutOfRange, "a shared value names an unknown component");
            }
            const ComponentTypeId component = bindings_[*stream_component].component;
            const u32 size = world_->components().info(component).value_size;
            scratch_.clear();
            if (Status resized = scratch_.resize(size); !resized) {
                return resized;
            }
            if (Status read = reader.raw(scratch_.data(), size); !read) {
                return read;
            }
            Expected<u32, Error> interned =
                world_->intern_shared(component, static_cast<const void*>(scratch_.data()));
            if (!interned) {
                return make_unexpected(interned.error());
            }
            if (Status pushed = shared_.push_back(SharedValue{component, *interned}); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    [[nodiscard]] Status instantiate_block(u32 rows) noexcept {
        BlockPlan plan;
        plan.first_local = static_cast<u32>(created_.size());
        plan.rows = rows;

        // Null columns: pass one only needs the entities and the rows, which `instantiate` zeroes.
        Array<const void*> columns(world_->allocator());
        if (Status resized = columns.resize(components_.size()); !resized) {
            return resized;
        }
        for (const void*& column : columns) {
            column = nullptr;
        }

        World::ArchetypeBlock block;
        block.components = components_.span();
        block.shared = shared_.span();
        block.columns = Span<const void* const>(columns.data(), columns.size());
        block.count = rows;
        if (Status made = world_->instantiate(block, created_); !made) {
            return made;
        }
        return plans_.push_back(plan);
    }

    /// The payload's length is not fixed — a buffer component's row is as long as the buffer — so
    /// it is measured by reading it in the order pass two will.
    [[nodiscard]] Status skip_payload(Reader& reader, u32 rows) noexcept {
        const ComponentRegistry& registry = world_->components();
        for (const ComponentTypeId component : components_) {
            const ComponentInfo& info = registry.info(component);
            if (!kind_has_column(info.kind)) {
                continue;
            }
            if (info.kind != ComponentKind::Buffer) {
                if (Status skipped = reader.skip(usize{info.size} * rows); !skipped) {
                    return skipped;
                }
                continue;
            }
            for (u32 row = 0; row < rows; ++row) {
                Expected<u32, Error> size = reader.u32_value();
                if (!size) {
                    return make_unexpected(size.error());
                }
                if (Status skipped = reader.skip(usize{*size} * info.element_size); !skipped) {
                    return skipped;
                }
            }
        }
        return ok();
    }

    /// Pass two: the same walk, filling the rows in.
    [[nodiscard]] Status fill_entities(Reader& reader) noexcept {
        usize plan_index = 0;
        while (true) {
            Expected<u32, Error> count = reader.u32_value();
            if (!count) {
                return make_unexpected(count.error());
            }
            if (*count == kEndOfBlocks) {
                return ok();
            }
            if (Status read = read_component_list(reader, *count); !read) {
                return read;
            }
            if (Status skipped = skip_shared_values(reader); !skipped) {
                return skipped;
            }
            Expected<u32, Error> rows = reader.u32_value();
            if (!rows) {
                return make_unexpected(rows.error());
            }
            if (plan_index >= plans_.size()) {
                return fail(ErrorCode::Internal, "the two passes disagree about the block count");
            }
            const BlockPlan plan = plans_[plan_index++];
            if (Status read = read_payload(reader, plan, *rows); !read) {
                return read;
            }
        }
    }

    [[nodiscard]] Status skip_shared_values(Reader& reader) noexcept {
        Expected<u32, Error> count = reader.u32_value();
        if (!count) {
            return make_unexpected(count.error());
        }
        for (u32 index = 0; index < *count; ++index) {
            Expected<u32, Error> stream_component = reader.u32_value();
            if (!stream_component || *stream_component >= bindings_.size()) {
                return fail(ErrorCode::OutOfRange, "a shared value names an unknown component");
            }
            const u32 size =
                world_->components().info(bindings_[*stream_component].component).value_size;
            if (Status skipped = reader.skip(size); !skipped) {
                return skipped;
            }
        }
        return ok();
    }

    [[nodiscard]] Entity to_entity(Entity reference) const noexcept {
        if (!reference.valid()) {
            return kNoEntity;
        }
        const u32 local = reference.index();
        return (local < created_.size()) ? created_[local] : kNoEntity;
    }

    [[nodiscard]] Status read_payload(Reader& reader, const BlockPlan& plan, u32 rows) noexcept {
        const ComponentRegistry& registry = world_->components();
        for (const ComponentTypeId component : components_) {
            const ComponentInfo& info = registry.info(component);
            if (!kind_has_column(info.kind)) {
                continue;
            }
            for (u32 row = 0; row < rows; ++row) {
                void* slot = world_->get_mut(created_[plan.first_local + row], component);
                if (slot == nullptr) {
                    return fail(ErrorCode::Internal,
                                "a stream row has no storage in its archetype");
                }
                Status read = (info.kind == ComponentKind::Buffer)
                                  ? read_buffer_row(reader, info, slot)
                                  : read_value(reader, info, slot);
                if (!read) {
                    return read;
                }
            }
        }
        return ok();
    }

    [[nodiscard]] Status read_value(Reader& reader, const ComponentInfo& info,
                                    void* slot) noexcept {
        if (Status read = reader.raw(slot, info.size); !read) {
            return read;
        }
        remap_value(info, static_cast<u8*>(slot),
                    [this](Entity entity) { return to_entity(entity); });
        return ok();
    }

    [[nodiscard]] Status read_buffer_row(Reader& reader, const ComponentInfo& info,
                                         void* slot) noexcept {
        Expected<u32, Error> size = reader.u32_value();
        if (!size) {
            return make_unexpected(size.error());
        }
        auto* header = static_cast<BufferHeader*>(slot);
        header->size = 0;
        header->heap = nullptr;
        header->heap_capacity = 0;
        if (*size > info.inline_capacity) {
            void* block = world_->allocator().allocate(usize{*size} * info.element_size,
                                                       info.element_alignment);
            if (block == nullptr) {
                return fail(ErrorCode::OutOfMemory, "could not allocate a buffer's spill");
            }
            header->heap = block;
            header->heap_capacity = *size;
        }
        auto* elements =
            (header->heap != nullptr)
                ? static_cast<u8*>(header->heap)
                : (static_cast<u8*>(slot) + align_up(sizeof(BufferHeader), info.element_alignment));
        if (Status read =
                reader.raw(static_cast<void*>(elements), usize{*size} * info.element_size);
            !read) {
            return read;
        }
        if (info.elements_are_entities) {
            resolve_entity_elements(elements, info, *size);
        }
        header->size = *size;
        return ok();
    }

    void resolve_entity_elements(u8* elements, const ComponentInfo& info, u32 count) noexcept {
        for (u32 index = 0; index < count; ++index) {
            u8* site = elements + (usize{index} * info.element_size);
            Entity reference;
            std::memcpy(static_cast<void*>(&reference), static_cast<const void*>(site),
                        sizeof(Entity));
            reference = to_entity(reference);
            std::memcpy(static_cast<void*>(site), static_cast<const void*>(&reference),
                        sizeof(Entity));
        }
    }

    World* world_;
    Span<const u8> bytes_;
    u32 component_count_ = 0;
    Array<Binding> bindings_;
    Array<Entity> created_;
    Array<BlockPlan> plans_;
    Array<ComponentTypeId> components_;
    Array<SharedValue> shared_;
    Array<u8> scratch_;
};

}  // namespace

Status serialize(World& world, Array<u8>& out, Span<const Entity> subset) noexcept {
    if (world.iterating()) {
        return fail(ErrorCode::Unavailable, "serializing a world mid-iteration");
    }
    WorldWriter writer(world, out);
    return writer.run(subset);
}

Status deserialize(World& world, Span<const u8> bytes, Array<Entity>& out) noexcept {
    if (world.iterating()) {
        return fail(ErrorCode::Unavailable, "deserializing into a world mid-iteration");
    }
    WorldReader reader(world, bytes);
    return reader.run(out);
}

}  // namespace cy::ecs
