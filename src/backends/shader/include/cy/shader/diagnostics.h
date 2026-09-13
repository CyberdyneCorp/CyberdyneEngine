#ifndef CY_SHADER_DIAGNOSTICS_H
#define CY_SHADER_DIAGNOSTICS_H
// The shader build report, and the module's counters on the engine trace. Task 3.7.
//
// `shader-system` — "Shader diagnostics": the build reports per-shader compile time, permutation
// counts per axis, SPIR-V instruction counts, register and occupancy estimates where the toolchain
// provides them, and pipeline cache statistics. Its scenario is the one worth designing to:
// *when one material's pipeline exceeds an instruction-count threshold, the build report flags it
// with its permutation key.*
//
// Two consequences of that sentence:
//
//   * A report entry has to carry its **permutation key and the domain to print it against**, or the
//     flag says "some variant of this shader is expensive", which sends the reader to look at all of
//     them. `format_entry()` prints `axis=value` pairs.
//   * The threshold is **configured, not fixed**. A compute kernel with ten thousand instructions is
//     ordinary and a fragment shader with ten thousand is a bug; the threshold is per stage.
//
// Register and occupancy estimates are recorded as `0 == not provided`, because no toolchain on this
// platform provides them today: Slang does not, and the vendor tools that do are not integrated. A
// field that is present and honestly empty is better than one that is absent, because it is where
// the number goes when the tool arrives, and because a report that silently omits a column teaches
// its readers that the column does not exist.
//
// --- THE COUNTERS ----------------------------------------------------------------------------------
//
// Every counter below is a classified trace field. `diagnostics-profiling-and-crash` makes the
// privacy classification a **required argument** of `CY_TRACE_FIELD`, and design.md §2 makes it a
// compile error to omit — see `src/core/diagnostics/field.h`. Shader counters are all `Public`: a
// compile time and an instruction count carry nothing about the machine or the person using it. The
// one field that is not is the source path, which is `Developer`: an absolute path in a user's home
// directory is not something an uploaded artefact should carry.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/cache.h>
#include <cy/shader/permutation.h>
#include <cy/shader/pipeline.h>
#include <cy/shader/shader.h>

namespace cy::shader {

/// One compiled variant, as the build report holds it.
struct ReportEntry {
    ShortName shader;
    ShortName entry_point;
    Stage stage = Stage::Vertex;
    PermutationKey permutation;
    u64 compile_ns = 0;
    u32 instruction_count = 0;
    u32 code_bytes = 0;
    u32 binding_count = 0;
    /// Where the artefact came from. A report in which everything is a `Miss` is a report from a
    /// build whose cache is not configured, and that is worth seeing at a glance.
    CacheOutcome cache = CacheOutcome::Miss;
    /// `0` means the toolchain did not provide the estimate. See the header comment.
    u32 register_estimate = 0;
    u32 occupancy_estimate = 0;
    bool flagged = false;
};

/// Per-stage instruction-count ceilings. A variant above its stage's ceiling is flagged.
///
/// The defaults are order-of-magnitude judgements, not measurements, and they are here so that the
/// mechanism has a value rather than because these are the right numbers. A project overrides them;
/// the first real measurement should replace them.
struct InstructionBudget {
    u32 per_stage[kStageCount] = {};

    /// The engine's starting point: 4k for a vertex stage, 8k for a fragment stage, 16k for compute,
    /// and 8k for everything else.
    [[nodiscard]] static InstructionBudget defaults() noexcept;
};

/// What a whole build compiled, and what it cost.
struct ReportTotals {
    u32 variants = 0;
    u32 flagged = 0;
    u64 total_compile_ns = 0;
    u64 total_instructions = 0;
    u64 total_code_bytes = 0;
    u32 cache_local_hits = 0;
    u32 cache_remote_hits = 0;
    u32 cache_misses = 0;
    /// The slowest variant, by index into the report. Meaningful only when `variants` is non-zero.
    u32 slowest = 0;
};

/// The build report. Accumulated during a cook or a hot-reload session and formatted at the end.
class ShaderReport {
public:
    explicit ShaderReport(Allocator& allocator) noexcept;

    ShaderReport(const ShaderReport&) = delete;
    ShaderReport& operator=(const ShaderReport&) = delete;

    void set_budget(const InstructionBudget& budget) noexcept { budget_ = budget; }

    /// Record one variant. Sets `flagged` from the budget.
    [[nodiscard]] Status record(const ReportEntry& entry) noexcept;

    /// Record a permutation domain's breakdown for a shader. `shader-system`'s "Permutation
    /// explosion is visible" scenario writes through here.
    [[nodiscard]] Status record_domain(std::string_view shader, const PermutationDomain& domain,
                                       const PermutationBudget& budget) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entries_.size()); }
    [[nodiscard]] const ReportEntry& at(u32 index) const noexcept { return entries_[index]; }
    [[nodiscard]] ReportTotals totals() const noexcept;

    /// Format one entry as a report line, with its permutation spelled out against `domain`.
    /// `domain` may be null, in which case the raw key is printed.
    [[nodiscard]] usize format_entry(u32 index, const PermutationDomain* domain, char* out,
                                     usize capacity) const noexcept;

    /// Emit every counter onto the engine trace, classified. Called once at the end of a cook.
    void emit_counters(const CacheStats& cache, const PipelineStats& pipelines) const noexcept;

private:
    struct DomainRecord {
        ShortName shader;
        PermutationBudget budget;
        u32 axis_count = 0;
        /// The axes, flattened: name and cardinality per axis. Kept as text so the report can be
        /// formatted after the domain that produced it has gone.
        u32 first_axis = 0;
    };

    struct AxisRecord {
        ShortName name;
        PermutationKind kind = PermutationKind::Preprocessor;
        u16 cardinality = 0;
    };

    Array<ReportEntry> entries_;
    Array<DomainRecord> domains_;
    Array<AxisRecord> axes_;
    InstructionBudget budget_;
};

}  // namespace cy::shader

#endif  // CY_SHADER_DIAGNOSTICS_H
