#pragma once
// The shader library: the compiled artefact, and the runtime store it becomes. Tasks 3.4 and 3.7.
//
// `shader-system` — "Shader library and caching": "Compiled shaders SHALL be stored in shader
// libraries: content-addressed artefacts containing SPIR-V or backend-native code, reflection data,
// and the permutation key."
//
// ONE CLASS IS BOTH THE FILE AND THE TABLE IN MEMORY, and that is deliberate. A separate
// "serialized library" type and "resident library" type would be two descriptions of the same thing
// that have to agree, which is the arrangement this codebase keeps refusing (see reflection.h for
// the same argument about descriptor layouts). `serialize()` and `parse()` are the two ends of one
// encoding, and a library that round-trips is a library whose in-memory form is the artefact.
//
// --- VARIANTS AND PROGRAMS ARE NOT THE SAME COUNT ------------------------------------------------
//
// `shader-system`: "Generated shaders SHALL be deduplicated by content hash so materials producing
// identical source share one program and one pipeline", and its scenario is two materials that
// differ only in parameter values.
//
// So the library holds two tables. A VARIANT is a (module, entry point, permutation) somebody asked
// for. A PROGRAM is a distinct compiled module, identified by the content hash of its code. Two
// variants whose code is byte-identical map to ONE program, and a pipeline is created per program —
// which is what makes the deduplication observable rather than asserted: `variant_count()` and
// `program_count()` differ, and they differ by exactly the number of duplicates.
//
// --- IDS ARE STABLE ACROSS A HOT RELOAD ----------------------------------------------------------
//
// A consumer holds a `ShaderVariantId`, never a pointer. `replace()` swaps the code behind an id
// and bumps its generation; a failed rebuild does not call it at all. That is the mechanism behind
// `shader-system`'s "Broken shader does not break the frame": there is no moment at which a
// consumer holds a handle to something that is being rebuilt, because rebuilding produces a new
// artefact off to one side and the swap is one assignment.

#include <cy/backends/shader/compiler.h>
#include <cy/backends/shader/permutation.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::shader {

/// The library format's magic number and version. A parse that does not see both refuses rather
/// than reading a differently shaped record as this one.
inline constexpr u32 kLibraryMagic = 0x4C53'5943U;  // 'CYSL', little-endian
inline constexpr u32 kLibraryVersion = 1;

struct ShaderVariantId {
    u32 value = 0xFFFF'FFFFU;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
    friend constexpr bool operator==(ShaderVariantId a, ShaderVariantId b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(ShaderVariantId a, ShaderVariantId b) noexcept {
        return a.value != b.value;
    }
};

struct ShaderProgramId {
    u32 value = 0xFFFF'FFFFU;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
    friend constexpr bool operator==(ShaderProgramId a, ShaderProgramId b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(ShaderProgramId a, ShaderProgramId b) noexcept {
        return a.value != b.value;
    }
};

/// What a caller asks for, and what a variant records.
struct VariantKey {
    Name module_name;
    Name entry_point;
    rhi::ShaderStage stage = rhi::ShaderStage::None;
    PermutationKey permutation;

    friend bool operator==(const VariantKey& a, const VariantKey& b) noexcept {
        return a.module_name == b.module_name && a.entry_point == b.entry_point &&
               a.stage == b.stage && a.permutation == b.permutation;
    }
};

/// The numbers `shader-system`'s "Shader diagnostics" requirement asks a build to report.
struct LibraryReport {
    u32 variant_count = 0;
    u32 program_count = 0;
    /// variant_count - program_count: how many variants the content hash collapsed.
    u32 deduplicated = 0;
    u64 code_bytes = 0;
    u64 compile_ns = 0;
    u32 max_instruction_count = 0;
    /// The variant that reached `max_instruction_count`, so the report names it rather than only
    /// counting it — "Expensive shader is identified" wants the permutation key too.
    ShaderVariantId max_instruction_variant;
    /// Variants per shader stage, indexed by the bit position in `rhi::ShaderStage`.
    u32 stage_counts[8] = {};
};

class ShaderLibrary {
public:
    explicit ShaderLibrary(Allocator& allocator) noexcept;

    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;
    ShaderLibrary(ShaderLibrary&&) noexcept = default;
    ShaderLibrary& operator=(ShaderLibrary&&) noexcept = default;

    /// Add a compiled variant. Deduplicates by the artefact's content hash: an identical module
    /// already present becomes the same program, and only the variant row is new.
    ///
    /// Inserting a key that is already present replaces it, so a cook that compiles the same
    /// variant twice ends with one row rather than two.
    [[nodiscard]] Expected<ShaderVariantId, Error> insert(const VariantKey& key,
                                                          CompiledShader&& shader) noexcept;

    [[nodiscard]] ShaderVariantId find(const VariantKey& key) const noexcept;
    [[nodiscard]] const VariantKey* key_at(ShaderVariantId id) const noexcept;
    [[nodiscard]] ShaderProgramId program_of(ShaderVariantId id) const noexcept;
    /// The code and reflection behind a variant, or null for an invalid id.
    [[nodiscard]] const CompiledShader* shader_at(ShaderVariantId id) const noexcept;
    [[nodiscard]] const CompiledShader* program_at(ShaderProgramId id) const noexcept;

    /// Replace the artefact behind an existing variant and bump its generation. Hot reload's swap.
    [[nodiscard]] Status replace(ShaderVariantId id, CompiledShader&& shader) noexcept;
    /// How many times this variant's code has been replaced. A consumer that caches anything
    /// derived from the code — a pipeline — compares this and rebuilds when it moved.
    [[nodiscard]] u32 generation(ShaderVariantId id) const noexcept;

    [[nodiscard]] usize variant_count() const noexcept { return variants_.size(); }
    [[nodiscard]] usize program_count() const noexcept { return programs_.size(); }
    [[nodiscard]] LibraryReport report() const noexcept;

    void clear() noexcept;

    /// Encode the whole library. The bytes are what a cache tier stores and what ships beside a
    /// game; `content_hash` over them is the library's own content address.
    [[nodiscard]] Status serialize(Array<u8>& out) const noexcept;
    [[nodiscard]] static Expected<ShaderLibrary, Error> parse(Allocator& allocator,
                                                              Span<const u8> bytes) noexcept;

private:
    struct Variant {
        VariantKey key;
        ShaderProgramId program;
        u32 generation = 0;
    };

    /// A program with no live variant is kept rather than compacted: compaction would invalidate
    /// every `ShaderProgramId` a caller holds, and a hot-reload swap producing an orphan is exactly
    /// when a caller is most likely to be holding one. `clear()` is the only thing that frees them.
    struct Program {
        assets::ContentHash hash;
        CompiledShader shader;
        u32 references = 0;
    };

    [[nodiscard]] Expected<ShaderProgramId, Error> intern_program(CompiledShader&& shader) noexcept;
    [[nodiscard]] ShaderProgramId find_program(const assets::ContentHash& hash) const noexcept;

    Allocator* allocator_;
    Array<Variant> variants_;
    Array<Program> programs_;
};

}  // namespace cy::shader
