#pragma once
// Generational handles, and the generation table that makes a stale one detectable. Task 1.3.2.
//
// `core-type-system` — "Generational handles": a runtime object owned by a server is addressed by
// `Handle<Tag>`, a 64-bit value packing a 32-bit slot index and a 32-bit generation counter.
// Freeing a handle increments its slot's generation, so a handle held across the free compares
// unequal to the slot that replaced it. The point of the counter is that the staleness is
// **detected**, not that the behaviour is undefined: a stale lookup answers "no" and the caller
// carries on.
//
// `Tag` is a phantom type and never has a definition anywhere. It is what makes `Handle<MeshTag>`
// and `Handle<TextureTag>` different types, so passing one where the other is expected is a
// compile error rather than a convention. See tests/compile_fail/ for the four confusions that are
// checked to stay errors.
//
// WHAT IS NOT HERE. A handle *pool* — slots that hold objects — is `core-memory-and-containers`
// task 2.5. This file owns the identity half: the slot indices, the generation counters, and the
// validity test. A pool composes a `GenerationTable` with storage; keeping the two apart is what
// lets a server validate a handle without owning the memory the object lives in.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/values/name.h>

#include <atomic>
#include <concepts>
#include <mutex>
#include <type_traits>
#include <vector>

namespace cy {

/// A tag's runtime identifier, used only by `AnyHandle` to keep an erased handle from being read
/// back as the wrong kind. Assigned by a counter on first use: handles are runtime-only and never
/// serialized, so this needs to be unique within a process and nothing more.
using HandleTag = u32;

inline constexpr HandleTag kInvalidHandleTag = 0;

namespace detail {

HandleTag register_handle_tag(const char* name) noexcept;

/// A tag declared through CY_HANDLE_TAG carries its own spelling; one declared as a bare `struct
/// Foo;` does not, and gets a placeholder. Detected rather than required, so an existing phantom
/// type does not have to change to be usable.
template <class Tag>
concept NamedHandleTag = requires {
    { Tag::handle_tag_name } -> std::convertible_to<const char*>;
};

}  // namespace detail

/// The runtime identifier for `Tag`, assigned on first call and stable for the process.
template <class Tag>
[[nodiscard]] HandleTag handle_tag_id() noexcept {
    // if constexpr rather than a ternary: the branch that names `Tag::handle_tag_name` must not be
    // instantiated for a tag that does not have one.
    static const HandleTag id = []() noexcept {
        if constexpr (detail::NamedHandleTag<Tag>) {
            return detail::register_handle_tag(Tag::handle_tag_name);
        } else {
            return detail::register_handle_tag("unnamed");
        }
    }();
    return id;
}

/// The name a tag was registered under, for a diagnostic. Empty when `tag` was never registered.
[[nodiscard]] Name handle_tag_name(HandleTag tag) noexcept;

/// Declare a handle tag and give it a spelling a diagnostic can print.
///
///   CY_HANDLE_TAG(Mesh);                 // declares struct MeshTag
///   using MeshHandle = cy::Handle<MeshTag>;
/// [[maybe_unused]] on the member: a tag declared in an anonymous namespace whose name is never
/// read is ordinary — the name exists for a diagnostic that may not be reached — and clang reports
/// an unused const variable, which the engine builds with -Werror.
#define CY_HANDLE_TAG(ident)                                                    \
    struct ident##Tag {                                                         \
        [[maybe_unused]] static constexpr const char* handle_tag_name = #ident; \
    }

/// A 64-bit reference to a slot in a `Tag`-specific pool.
///
/// Generation 0 means "no handle". A slot's first live generation is 1, so a zeroed component field
/// is the null handle rather than a reference to slot 0 — which is what makes a handle safe to
/// leave in trivially-relocatable, memset-initialised chunk storage.
template <class Tag>
class Handle {
public:
    using tag_type = Tag;

    constexpr Handle() noexcept = default;

    [[nodiscard]] static constexpr Handle from_slot(u32 index, u32 generation) noexcept {
        return Handle((static_cast<u64>(generation) << 32) | static_cast<u64>(index));
    }
    [[nodiscard]] static constexpr Handle from_bits(u64 bits) noexcept { return Handle(bits); }

    [[nodiscard]] constexpr u32 index() const noexcept { return static_cast<u32>(bits_); }
    [[nodiscard]] constexpr u32 generation() const noexcept {
        return static_cast<u32>(bits_ >> 32);
    }
    [[nodiscard]] constexpr u64 bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return generation() == 0; }
    explicit constexpr operator bool() const noexcept { return !is_null(); }

    friend constexpr bool operator==(Handle a, Handle b) noexcept { return a.bits_ == b.bits_; }
    friend constexpr bool operator!=(Handle a, Handle b) noexcept { return a.bits_ != b.bits_; }

private:
    explicit constexpr Handle(u64 bits) noexcept : bits_(bits) {}

    u64 bits_ = 0;
};

/// A handle whose tag is carried as data rather than as a type. This is what `Var` stores, because
/// a dynamic value cannot be templated on the tag; it is also what a signal connection stores for
/// its target. Reading one back as a typed handle checks the tag.
struct AnyHandle {
    u64 bits = 0;
    HandleTag tag = kInvalidHandleTag;

    [[nodiscard]] constexpr bool is_null() const noexcept { return (bits >> 32) == 0; }

    friend constexpr bool operator==(AnyHandle a, AnyHandle b) noexcept {
        return a.bits == b.bits && a.tag == b.tag;
    }
    friend constexpr bool operator!=(AnyHandle a, AnyHandle b) noexcept { return !(a == b); }
};

template <class Tag>
[[nodiscard]] AnyHandle to_any(Handle<Tag> handle) noexcept {
    return AnyHandle{handle.bits(), handle_tag_id<Tag>()};
}

/// Recover a typed handle. Fails when the erased handle belongs to a different pool — the confusion
/// a type system cannot catch once the type has been erased, caught here instead.
template <class Tag>
[[nodiscard]] Expected<Handle<Tag>, Error> from_any(AnyHandle handle) noexcept {
    if (handle.tag != handle_tag_id<Tag>()) {
        return fail(ErrorCode::InvalidArgument, "AnyHandle belongs to a different handle tag");
    }
    return Handle<Tag>::from_bits(handle.bits);
}

/// The identity of an entity. A distinct type from `Handle`, and deliberately not a handle tag:
/// entities are owned by the ECS world (M2), not by a server's handle pool, and the two are not
/// interchangeable even though both are 64-bit index-and-generation pairs.
class EntityId {
public:
    constexpr EntityId() noexcept = default;

    [[nodiscard]] static constexpr EntityId from_slot(u32 index, u32 generation) noexcept {
        return EntityId((static_cast<u64>(generation) << 32) | static_cast<u64>(index));
    }
    [[nodiscard]] static constexpr EntityId from_bits(u64 bits) noexcept { return EntityId(bits); }

    [[nodiscard]] constexpr u32 index() const noexcept { return static_cast<u32>(bits_); }
    [[nodiscard]] constexpr u32 generation() const noexcept {
        return static_cast<u32>(bits_ >> 32);
    }
    [[nodiscard]] constexpr u64 bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return generation() == 0; }

    friend constexpr bool operator==(EntityId a, EntityId b) noexcept { return a.bits_ == b.bits_; }
    friend constexpr bool operator!=(EntityId a, EntityId b) noexcept { return a.bits_ != b.bits_; }

private:
    explicit constexpr EntityId(u64 bits) noexcept : bits_(bits) {}

    u64 bits_ = 0;
};

/// The generation counters for one pool of slots, and nothing else.
///
/// CROSS-THREAD VALIDITY. `core-type-system` requires that resolving a handle concurrently with an
/// unrelated allocation in the same pool is safe, "with pool growth published atomically". Growth
/// here appends a chunk and then publishes the new chunk count with a release store; a reader loads
/// the count with acquire and therefore sees a fully constructed chunk or does not see it at all.
/// Chunks are never moved and never freed while the table lives, so a pointer a reader obtained
/// stays valid. Allocation and freeing take a mutex — they are rare, and the reader never does.
class GenerationTable {
public:
    /// Slots per chunk, rounded up to a power of two. The default is a page's worth of counters.
    explicit GenerationTable(u32 slots_per_chunk = 1024) noexcept;
    ~GenerationTable();

    GenerationTable(const GenerationTable&) = delete;
    GenerationTable& operator=(const GenerationTable&) = delete;

    /// Reserve a slot and return its index and its new generation. Reuses a freed slot when one is
    /// available, which is the case the generation counter exists for.
    [[nodiscard]] Expected<u32, Error> allocate() noexcept;

    /// Release a slot, incrementing its generation so every handle to it becomes stale. Fails on an
    /// index that was never allocated or is already free — that is a double free, and it is a
    /// caller error worth reporting rather than a silent no-op.
    Status release(u32 slot) noexcept;

    /// The slot's current generation, or 0 when it is free or out of range. Lock-free.
    [[nodiscard]] u32 generation_of(u32 slot) const noexcept;

    /// Whether `generation` still names the occupant of `slot`. Lock-free, and the whole of the
    /// staleness check: a mismatch is counted and answered false. This is a hot path — it is what a
    /// server calls before every dereference — so it emits nothing and takes no lock.
    [[nodiscard]] bool is_live(u32 slot, u32 generation) const noexcept;

    /// Typed sugar over the three above.
    template <class Tag>
    [[nodiscard]] Expected<Handle<Tag>, Error> allocate_handle() noexcept {
        const Expected<u32, Error> slot = allocate();
        if (!slot) {
            return make_unexpected(slot.error());
        }
        return Handle<Tag>::from_slot(*slot, generation_of(*slot));
    }

    template <class Tag>
    [[nodiscard]] bool is_live(Handle<Tag> handle) const noexcept {
        return is_live(handle.index(), handle.generation());
    }

    template <class Tag>
    Status release(Handle<Tag> handle) noexcept {
        if (!is_live(handle)) {
            return fail(ErrorCode::NotFound, "handle is stale or was never allocated");
        }
        return release(handle.index());
    }

    [[nodiscard]] u32 capacity() const noexcept;
    [[nodiscard]] u32 live() const noexcept;

    /// How many handles this table has rejected as stale. Per-table, unlike the process-wide figure
    /// in `values_diagnostics()`.
    [[nodiscard]] u64 stale_rejections() const noexcept;

    /// The largest table this implementation addresses: chunk pointers live in a fixed array so a
    /// reader never chases a pointer the writer is still moving.
    static constexpr u32 kMaxChunks = 4096;

private:
    struct Chunk {
        std::atomic<u32>* generations = nullptr;
    };

    [[nodiscard]] std::atomic<u32>* slot_ptr(u32 slot) const noexcept;

    mutable std::mutex mutex_;
    Chunk chunks_[kMaxChunks] = {};
    std::atomic<u32> chunk_count_{0};
    std::atomic<u32> slot_count_{0};
    std::atomic<u32> live_count_{0};
    mutable std::atomic<u64> stale_rejections_{0};
    std::vector<u32> free_slots_;
    u32 slots_per_chunk_;
    u32 chunk_shift_;
    u32 chunk_mask_;
};

// The three properties the specification asks of a handle, checked here rather than described.
// `ExampleTag` is a phantom type like every other tag: it is declared and never defined.
struct ExampleTag;
static_assert(sizeof(Handle<ExampleTag>) == 8,
              "a handle is a plain 64-bit value: no refcount, no indirection");
static_assert(std::is_trivially_copyable_v<Handle<ExampleTag>>,
              "a handle in a component must keep the component trivially relocatable");
static_assert(sizeof(EntityId) == 8, "an entity id is a plain 64-bit value");
static_assert(std::is_trivially_copyable_v<EntityId>);
static_assert(std::is_trivially_copyable_v<AnyHandle>);

}  // namespace cy
