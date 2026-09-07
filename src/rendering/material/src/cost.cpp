// Material cost analysis. M7 task 6.3. See cost.h for why attribution reads the provenance table.

#include <cy/rendering/material/cost.h>

namespace cy::rendering::material {
namespace {

/// Find or append the entry for an origin. Linear, and deliberately: a material has tens of
/// authoring nodes, and a hash map here would cost more than it saved and would iterate in an order
/// that is not the report's.
[[nodiscard]] Expected<NodeCost*, Error> entry_for(Array<NodeCost>& entries, u32 origin) noexcept {
    for (NodeCost& entry : entries) {
        if (entry.origin == origin) {
            return &entry;
        }
    }
    NodeCost fresh;
    fresh.origin = origin;
    if (Status pushed = entries.push_back(fresh); !pushed) {
        return make_unexpected(pushed.error());
    }
    return &entries[entries.size() - 1];
}

void add_node(NodeCost& entry, Op op) noexcept {
    if (op == Op::TextureSample) {
        ++entry.texture_samples;
    } else if (op_is_closure(op)) {
        ++entry.closures;
    } else {
        ++entry.arithmetic;
    }
}

void sort_by_weight(Array<NodeCost>& entries) noexcept {
    for (usize outer = 1; outer < entries.size(); ++outer) {
        for (usize inner = outer; inner > 0; --inner) {
            const NodeCost& left = entries[inner - 1];
            const NodeCost& right = entries[inner];
            // Descending weight, and by origin id within a tie so two runs report one order.
            const bool ordered = left.weight() > right.weight() ||
                                 (left.weight() == right.weight() && left.origin <= right.origin);
            if (ordered) {
                break;
            }
            const NodeCost swap = entries[inner - 1];
            entries[inner - 1] = entries[inner];
            entries[inner] = swap;
        }
    }
}

}  // namespace

u32 NodeCost::weight() const noexcept {
    // A texture sample dominates: it is a memory access with a filter behind it, and an author
    // deciding what to simplify wants the samples at the top of the list.
    return (texture_samples * 16U) + (closures * 4U) + arithmetic;
}

Status analyse_cost(const Module& module, const GeneratedSource& source, const Lowered& lowered,
                    const CostModel& model, CostReport& report) noexcept {
    report.texture_samples = source.texture_samples;
    report.arithmetic = source.arithmetic;
    report.branches = source.branches;
    report.closures = source.closures;
    report.statements = source.statements;
    report.estimated_registers = source.peak_live_values;
    report.generic_cost_multiple = lowered.generic_cost_multiple;
    report.model_name = model.name;

    const f32 pixels = static_cast<f32>(model.pixels);
    const f32 nanoseconds = ((static_cast<f32>(source.texture_samples) * model.sample_ns) +
                             (static_cast<f32>(source.arithmetic) * model.arithmetic_ns) +
                             (static_cast<f32>(source.branches) * model.branch_ns)) *
                            pixels * lowered.generic_cost_multiple;
    report.full_screen_ms = nanoseconds / 1000000.0F;

    report.per_node.clear();
    for (const NodeId id : source.value_nodes) {
        const Span<const u32> origins = module.origins(id);
        if (origins.empty()) {
            auto entry = entry_for(report.per_node, kUnattributed);
            if (!entry) {
                return make_unexpected(entry.error());
            }
            add_node(*entry.value(), module.node(id).op);
            continue;
        }
        // A value several authoring nodes produced is charged to each of them. An editor showing
        // "which nodes are expensive" must highlight both, and halving the cost between them would
        // make two identical nodes look cheap.
        for (const u32 origin : origins) {
            auto entry = entry_for(report.per_node, origin);
            if (!entry) {
                return make_unexpected(entry.error());
            }
            add_node(*entry.value(), module.node(id).op);
        }
    }
    sort_by_weight(report.per_node);
    return ok();
}

void count_permutations(u32 static_bool_parameters, u32 program_count, const Profile& profile,
                        u32 geometry_sources, CostReport& report) noexcept {
    // Under a visibility buffer one program serves every geometry source: "WHEN one material is
    // used on static meshes, skinned meshes, virtual geometry, and mesh particles under a
    // visibility buffer pipeline THEN one program SHALL serve all four."
    const u32 sources = geometry_sources == 0 ? 1U : geometry_sources;
    report.geometry_variants = profile.visibility_buffer ? 1U : sources;
    u32 statics = 1;
    for (u32 index = 0; index < static_bool_parameters && index < 16U; ++index) {
        statics *= 2U;
    }
    report.permutation_count = kQualityTierCount * statics * report.geometry_variants;
    report.program_count = program_count;
    report.total_programs = report.permutation_count * program_count;
}

}  // namespace cy::rendering::material
