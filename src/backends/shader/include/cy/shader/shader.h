#ifndef CY_SHADER_SHADER_H
#define CY_SHADER_SHADER_H
// The shader system's vocabulary: stages, targets, and the identity of a compiled artefact.
// Section 3 of `openspec/changes/implement-m3-first-light/tasks.md`.
//
// `shader-system` fixes the shape of this milestone's work: **Slang is the authoring language and
// SPIR-V the interchange form**. The engine does not author a shading language and does not write a
// shader optimiser — it produces good input to somebody else's. Everything in this module is
// therefore about what happens *around* a third-party compiler: which source is fed to it, which
// variants exist, what comes back, where it is cached, and what a binding layout is derived from.
//
// WHY THIS MODULE IS AT LAYER 3. SPIR-V is a third-party interchange format and Slang is a
// third-party compiler, and `engine-architecture` puts every third-party type behind the backend
// layer. A `u32` word of SPIR-V is not a Vulkan type, but the moment reflection knows what a
// `SpvOpTypeImage` means it is speaking somebody else's vocabulary, and that vocabulary belongs
// here with volk and VMA rather than three layers above them. Nothing above `src/backends/` sees a
// SPIR-V word, a Slang handle, or a descriptor-set index that came from either.

#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy::shader {

using assets::ContentHash;

/// A programmable stage. Ray tracing and mesh shading are declared here and capability-gated at
/// use: `shader-system` requires them "as capability-gated additions, with SPIR-V remaining the
/// interchange form", and a stage the enum cannot name is a stage the cache key cannot distinguish.
enum class Stage : u8 {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
    Geometry = 3,
    TessellationControl = 4,
    TessellationEvaluation = 5,
    Task = 6,
    Mesh = 7,
    RayGeneration = 8,
    Intersection = 9,
    AnyHit = 10,
    ClosestHit = 11,
    Miss = 12,
    Callable = 13,
};

inline constexpr u32 kStageCount = 14;

/// The enumerator's own spelling, for a diagnostic or a report. Never null.
const char* stage_name(Stage stage) noexcept;

/// The Slang entry-point profile a stage compiles against — `vertex`, `fragment`, `compute` and so
/// on. Kept beside the enum rather than in the Slang backend so that the backend has no table of
/// its own to drift from this one.
const char* stage_profile(Stage stage) noexcept;

/// A set of stages, as a bitmask. Reflection reports which stages reference a binding, and a
/// descriptor-set layout needs the union across the program's entry points.
enum class StageMask : u32 {
    None = 0,
    Vertex = 1U << 0,
    Fragment = 1U << 1,
    Compute = 1U << 2,
    Geometry = 1U << 3,
    TessellationControl = 1U << 4,
    TessellationEvaluation = 1U << 5,
    Task = 1U << 6,
    Mesh = 1U << 7,
    RayGeneration = 1U << 8,
    Intersection = 1U << 9,
    AnyHit = 1U << 10,
    ClosestHit = 1U << 11,
    Miss = 1U << 12,
    Callable = 1U << 13,
};

[[nodiscard]] constexpr StageMask operator|(StageMask a, StageMask b) noexcept {
    return static_cast<StageMask>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr StageMask operator&(StageMask a, StageMask b) noexcept {
    return static_cast<StageMask>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr StageMask& operator|=(StageMask& a, StageMask b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool any(StageMask mask) noexcept {
    return static_cast<u32>(mask) != 0;
}

/// The single-stage mask for a stage.
[[nodiscard]] constexpr StageMask stage_bit(Stage stage) noexcept {
    return static_cast<StageMask>(1U << static_cast<u32>(stage));
}

/// The interchange form a compilation produces.
///
/// `shader-system`'s pipeline is SPIR-V first and backend-native afterwards: SPIR-V is retained for
/// Vulkan, cross-compiled to MSL for Metal, and lowered to DXIL for D3D12. M3 produces only SPIR-V
/// — there is no Metal and no D3D12 backend until M11 — but the enumerator exists now because it is
/// part of the **cache key**, and a key that cannot distinguish two targets is a key that serves
/// one target's artefact to the other.
enum class Target : u8 {
    SpirV = 0,
    Msl = 1,
    Dxil = 2,
};

const char* target_name(Target target) noexcept;

/// How hard the third-party compiler is asked to work.
///
/// Not an optimiser of the engine's own: this selects what Slang is told to do. `Debug` keeps the
/// SPIR-V close to the source so a stepping debugger has something to step through; `Size` is what
/// a shipping build asks for when the artefact is downloaded rather than installed.
enum class Optimisation : u8 {
    None = 0,
    Debug = 1,
    Default = 2,
    Size = 3,
};

const char* optimisation_name(Optimisation level) noexcept;

/// The renderer profile a variant was compiled for. `shader-system` requires the cache key to carry
/// it, because the same source compiled for a mobile feature set is not the artefact a desktop
/// build wants and the two must not collide in the cache.
///
/// One producer today (`Desktop`); the point is that the key has a field for the second.
enum class RendererProfile : u8 {
    Desktop = 0,
    Mobile = 1,
    Console = 2,
};

const char* renderer_profile_name(RendererProfile profile) noexcept;

/// Capability bits a compilation may assume. A feature the device does not have is a feature the
/// shader must not reference, so this is part of the cache key exactly as the profile is.
///
/// The set is small and grows with the RHI's capability model rather than ahead of it: a bit with
/// no backend behind it is a cache key that splits for no reason.
enum class FeatureSet : u32 {
    None = 0,
    /// `VK_EXT_descriptor_indexing` and its friends: unbounded descriptor arrays.
    Bindless = 1U << 0,
    /// Subgroup arithmetic and ballots.
    Subgroups = 1U << 1,
    /// 16-bit arithmetic and storage.
    Float16 = 1U << 2,
    /// Mesh and task shaders.
    MeshShading = 1U << 3,
    /// Ray query and ray pipelines.
    RayTracing = 1U << 4,
};

[[nodiscard]] constexpr FeatureSet operator|(FeatureSet a, FeatureSet b) noexcept {
    return static_cast<FeatureSet>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_feature(FeatureSet set, FeatureSet feature) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(feature)) == static_cast<u32>(feature);
}

/// The longest an entry-point name may be. Slang entry points are identifiers, and an identifier
/// this long is a generated name — which is exactly the case the material compiler will produce, so
/// the bound is generous rather than tight.
inline constexpr usize kMaxEntryPointLength = 127;

/// The longest a reflected binding, parameter or axis name may be. Reflection names come from the
/// source, and a name longer than this is truncated rather than rejected: a name is a diagnostic
/// aid, and losing the tail of one is not a reason to fail a compile that would otherwise work.
inline constexpr usize kMaxNameLength = 95;

/// A short, fixed-capacity name. Reflection produces hundreds of these per program and every one of
/// them outlives the SPIR-V blob it was parsed from, so they are copied rather than pointed at.
///
/// Truncation is silent by design — see `kMaxNameLength`. `was_truncated()` reports it for the one
/// caller that cares, the build report.
class ShortName {
public:
    ShortName() noexcept = default;
    explicit ShortName(std::string_view text) noexcept { assign(text); }

    void assign(std::string_view text) noexcept;

    [[nodiscard]] const char* c_str() const noexcept { return text_; }
    [[nodiscard]] std::string_view view() const noexcept { return {text_, size_}; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool was_truncated() const noexcept { return truncated_; }

    friend bool operator==(const ShortName& a, const ShortName& b) noexcept {
        return a.view() == b.view();
    }
    friend bool operator!=(const ShortName& a, const ShortName& b) noexcept { return !(a == b); }

private:
    char text_[kMaxNameLength + 1] = {};
    u8 size_ = 0;
    bool truncated_ = false;
};

}  // namespace cy::shader

#endif  // CY_SHADER_SHADER_H
