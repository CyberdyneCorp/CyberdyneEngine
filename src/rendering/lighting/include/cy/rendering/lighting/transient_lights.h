#pragma once
// Lights a game spawns for a moment: a muzzle flash, an explosion, a spark, a thrown torch.
//
// `rendering-lighting-and-shadows` — "Light types", for the lights that are not authored in a
// level. A muzzle flash lives for three frames and a battle fires hundreds of them; a light that
// cost an entity, a component and a scene update to create would cost more than the flash is worth.
// So a transient light is a slot in a fixed pool: spawning takes the next free slot and writes a
// description, nothing allocates, and the lights the frame draws are appended to its light list
// once a frame (`append_to`) — the same `render::LightDescription` every other light is, so the
// clustered lists, the stochastic path and the GI see a muzzle flash as they see a lamp.
//
// ================================================================================================
// A LIGHT'S LIFE, AND ITS ENVELOPE
// ================================================================================================
//
// A light with a lifetime fades over it and is freed when it ends; a light with none lives until it
// is killed (a torch carried by a character, moved each frame with `update`). The fade is an
// envelope on the intensity — `None` is a constant until the end, `Linear` falls to zero along its
// life and `Quadratic` falls as `(1 - t)^2`, the shape of a flash's afterglow — so its light fades
// rather than vanishing, and the cluster lists see it shrink rather than pop.
//
// ================================================================================================
// HANDLES ARE GENERATIONAL
// ================================================================================================
//
// A handle is the slot and the generation the slot had when the light was spawned. A flash that
// expired and whose slot was reused by another is a stale handle, and `kill`, `update` and `alive`
// answer for it as for a light that is gone — never for the light now in its slot.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/model.h>

namespace cy::rendering {

/// The intensity envelope over a transient light's life.
enum class TransientFade : u8 {
    None = 0,
    Linear,
    Quadratic,
    Count,
};

/// A transient light: its slot in the low 32 bits and the slot's generation in the high 32. Zero is
/// never a light.
using TransientLight = u64;
inline constexpr TransientLight kNoTransientLight = 0;

/// The stable id a transient light carries into the frame: this bit set over its handle, so it can
/// never be an entity's.
inline constexpr u64 kTransientLightIdBit = 0x8000000000000000ULL;

struct TransientLightDesc {
    render::LightDescription light{};
    /// Seconds. Zero lives until killed.
    f32 lifetime_seconds = 0.0F;
    TransientFade fade = TransientFade::Quadratic;
};

/// What the pool holds and has done.
struct TransientLightStatus {
    u32 live = 0;
    u32 capacity = 0;
    u64 spawned = 0;
    u64 expired = 0;
    u64 killed = 0;
    /// Spawns refused because every slot was taken.
    u64 refused = 0;
};

class TransientLights {
public:
    explicit TransientLights(Allocator& allocator) noexcept;

    /// The pool's size: the most lights alive at once. Spawning past it is refused and counted,
    /// never grown into, so a battle cannot allocate in a frame.
    [[nodiscard]] Status reserve(u32 capacity) noexcept;

    /// A light, from now. Refused (`OutOfRange`) with every slot taken, and (`InvalidArgument`) for
    /// a negative lifetime or a directional light — a light for a moment is local.
    [[nodiscard]] Expected<TransientLight, Error> spawn(const TransientLightDesc& desc) noexcept;
    /// Replace a live light's description — move a torch, recolour a flash — keeping its age.
    /// `NotFound` for a light that is gone.
    [[nodiscard]] Status update(TransientLight light,
                                const render::LightDescription& desc) noexcept;
    /// End a light now. `NotFound` for one that is already gone.
    [[nodiscard]] Status kill(TransientLight light) noexcept;
    [[nodiscard]] bool alive(TransientLight light) const noexcept;

    /// Age every light by `seconds`, freeing those whose life ended.
    void advance(f32 seconds) noexcept;

    /// Append every live light, its intensity under its envelope, to the frame's lights. Returns
    /// how many were appended. In slot order, so two runs of one frame append the same list.
    [[nodiscard]] Expected<u32, Error> append_to(
        Array<render::LightDescription>& out) const noexcept;

    [[nodiscard]] const TransientLightStatus& status() const noexcept { return status_; }

private:
    struct Slot {
        TransientLightDesc desc{};
        f32 age = 0.0F;
        u32 generation = 1;
        bool live = false;
    };

    static constexpr u32 kNoSlot = 0xFFFFFFFFU;
    /// The slot a live light is in, or `kNoSlot` for a light that is gone.
    [[nodiscard]] u32 index_of(TransientLight light) const noexcept;
    void release(u32 index) noexcept;

    Array<Slot> slots_;
    Array<u32> free_;
    TransientLightStatus status_{};
};

/// The envelope at `age` of a life of `lifetime` seconds: 1 at the start, 0 at the end.
[[nodiscard]] f32 transient_envelope(TransientFade fade, f32 age, f32 lifetime) noexcept;

}  // namespace cy::rendering
