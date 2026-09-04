// Permutation axes, the mixed-radix key, and the budget report. Task 3.2.

#include <cy/backends/shader/permutation.h>

#include <cstdio>

namespace cy::shader {
namespace {

/// The ceiling on a shader's total variant count.
///
/// It exists to keep the mixed-radix arithmetic in range — sixteen axes of sixty-four values would
/// overflow a `u64` several times over — and it is far above anything a project should reach. The
/// budget warning fires four orders of magnitude below it; this is the wall behind the warning.
constexpr u64 kMaxVariants = u64{1} << 24;

}  // namespace

const char* variation_kind_name(VariationKind kind) noexcept {
    switch (kind) {
        case VariationKind::Specialization:
            return "specialization";
        case VariationKind::Generic:
            return "generic";
        case VariationKind::Preprocessor:
            return "preprocessor";
    }
    return "unknown";
}

PermutationSet::PermutationSet(Allocator& allocator) noexcept
    : axes_(allocator), values_(allocator) {}

Status PermutationSet::add_axis(Name name, VariationKind kind, Span<const u32> values,
                                u32 specialization_id) noexcept {
    if (name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "a permutation axis must be named");
    }
    if (axes_.size() >= kMaxPermutationAxes) {
        return fail(ErrorCode::OutOfRange, "too many permutation axes on one shader");
    }
    if (values.size() < 2 || values.size() > kMaxAxisValues) {
        // One value is not a variation, and an axis with more than kMaxAxisValues declared values
        // is a parameter that wants to be a uniform.
        return fail(ErrorCode::InvalidArgument,
                    "a permutation axis declares between 2 and kMaxAxisValues values");
    }
    if (index_of(name) != axes_.size()) {
        return fail(ErrorCode::AlreadyExists, "a permutation axis with that name is declared");
    }
    if (kind == VariationKind::Specialization) {
        for (const Axis& axis : axes_) {
            if (axis.kind == VariationKind::Specialization &&
                axis.specialization_id == specialization_id) {
                return fail(ErrorCode::AlreadyExists,
                            "two permutation axes claim the same specialization constant id");
            }
        }
    }

    const u64 stride = pipeline_variants();
    if (stride * values.size() > kMaxVariants) {
        return fail(ErrorCode::OutOfRange,
                    "the shader's declared permutation count exceeds the engine's ceiling");
    }

    Axis axis;
    axis.name = name;
    axis.kind = kind;
    axis.specialization_id = specialization_id;
    axis.first_value = static_cast<u32>(values_.size());
    axis.value_count = static_cast<u32>(values.size());
    axis.stride = stride;

    if (Status appended = values_.append(values); !appended) {
        return appended;
    }
    if (Status pushed = axes_.push_back(axis); !pushed) {
        // Roll the values back so a failed declaration leaves the set exactly as it was. Shrinking
        // never allocates, so the only way this reports is a bug in Array, and swallowing it would
        // hide that.
        if (Status rolled_back = values_.resize(axis.first_value); !rolled_back) {
            return rolled_back;
        }
        return pushed;
    }
    return ok();
}

PermutationAxis PermutationSet::axis_at(usize index) const noexcept {
    CY_ASSERT_MSG(index < axes_.size(), "PermutationSet::axis_at() past the end");
    const Axis& axis = axes_[index];
    PermutationAxis out;
    out.name = axis.name;
    out.kind = axis.kind;
    out.specialization_id = axis.specialization_id;
    out.values = Span<const u32>(values_.data() + axis.first_value, axis.value_count);
    return out;
}

usize PermutationSet::index_of(Name name) const noexcept {
    for (usize index = 0; index < axes_.size(); ++index) {
        if (axes_[index].name == name) {
            return index;
        }
    }
    return axes_.size();
}

u64 PermutationSet::pipeline_variants() const noexcept {
    u64 total = 1;
    for (const Axis& axis : axes_) {
        total *= axis.value_count;
    }
    return total;
}

u64 PermutationSet::compiled_variants() const noexcept {
    u64 total = 1;
    for (const Axis& axis : axes_) {
        if (requires_compilation(axis.kind)) {
            total *= axis.value_count;
        }
    }
    return total;
}

Expected<PermutationKey, Error> PermutationSet::encode(Span<const u32> indices) const noexcept {
    if (indices.size() != axes_.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a permutation choice supplies one index per declared axis");
    }
    PermutationKey key;
    for (usize index = 0; index < axes_.size(); ++index) {
        if (indices[index] >= axes_[index].value_count) {
            return fail(ErrorCode::OutOfRange, "a permutation index names an undeclared value");
        }
        key.value += static_cast<u64>(indices[index]) * axes_[index].stride;
    }
    return key;
}

Status PermutationSet::decode(PermutationKey key, Span<u32> indices) const noexcept {
    if (indices.size() != axes_.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "decoding a permutation writes one index per declared axis");
    }
    if (key.value >= pipeline_variants()) {
        return fail(ErrorCode::OutOfRange, "the permutation key is outside the declared space");
    }
    for (usize index = 0; index < axes_.size(); ++index) {
        indices[index] =
            static_cast<u32>((key.value / axes_[index].stride) % axes_[index].value_count);
    }
    return ok();
}

Expected<PermutationKey, Error> PermutationSet::with_axis(PermutationKey key, usize axis,
                                                          u32 index) const noexcept {
    if (axis >= axes_.size()) {
        return fail(ErrorCode::OutOfRange, "no such permutation axis");
    }
    if (index >= axes_[axis].value_count) {
        return fail(ErrorCode::OutOfRange, "a permutation index names an undeclared value");
    }
    if (key.value >= pipeline_variants()) {
        return fail(ErrorCode::OutOfRange, "the permutation key is outside the declared space");
    }
    const u64 stride = axes_[axis].stride;
    const u64 current = (key.value / stride) % axes_[axis].value_count;
    PermutationKey out;
    out.value = key.value - (current * stride) + (static_cast<u64>(index) * stride);
    return out;
}

Expected<PermutationKey, Error> PermutationSet::compilation_key(PermutationKey key) const noexcept {
    if (key.value >= pipeline_variants()) {
        return fail(ErrorCode::OutOfRange, "the permutation key is outside the declared space");
    }
    PermutationKey out;
    for (const Axis& axis : axes_) {
        if (!requires_compilation(axis.kind)) {
            continue;
        }
        const u64 index = (key.value / axis.stride) % axis.value_count;
        out.value += index * axis.stride;
    }
    return out;
}

Status PermutationSet::specialization_constants(
    PermutationKey key, Array<rhi::SpecializationConstant>& out) const noexcept {
    if (key.value >= pipeline_variants()) {
        return fail(ErrorCode::OutOfRange, "the permutation key is outside the declared space");
    }
    for (const Axis& axis : axes_) {
        if (axis.kind != VariationKind::Specialization) {
            continue;
        }
        const u64 index = (key.value / axis.stride) % axis.value_count;
        rhi::SpecializationConstant constant;
        constant.id = axis.specialization_id;
        constant.value = values_[axis.first_value + index];
        if (Status pushed = out.push_back(constant); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status PermutationSet::preprocessor_defines(PermutationKey key, Array<Name>& names,
                                            Array<u32>& values) const noexcept {
    if (key.value >= pipeline_variants()) {
        return fail(ErrorCode::OutOfRange, "the permutation key is outside the declared space");
    }
    for (const Axis& axis : axes_) {
        if (axis.kind != VariationKind::Preprocessor) {
            continue;
        }
        const u64 index = (key.value / axis.stride) % axis.value_count;
        if (Status pushed = names.push_back(axis.name); !pushed) {
            return pushed;
        }
        if (Status pushed = values.push_back(values_[axis.first_value + index]); !pushed) {
            return pushed;
        }
    }
    return ok();
}

bool PermutationSet::check_budget(u64 budget, DiagnosticLog& diagnostics,
                                  std::string_view shader_name) const noexcept {
    const u64 compiled = compiled_variants();
    if (compiled <= budget) {
        return true;
    }

    char message[256] = {};
    (void)std::snprintf(
        message, sizeof(message), "shader '%.*s' compiles %llu variants, over the budget of %llu",
        static_cast<int>(shader_name.size()), shader_name.data(),
        static_cast<unsigned long long>(compiled), static_cast<unsigned long long>(budget));
    (void)diagnostics.add(Severity::Warning, message);

    // The breakdown is the point: "too many variants" is not actionable, "SHADOW_QUALITY has four
    // values and is a preprocessor axis" is.
    for (const Axis& axis : axes_) {
        char line[256] = {};
        (void)std::snprintf(line, sizeof(line), "  axis '%s': %u values, %s%s", axis.name.c_str(),
                            axis.value_count, variation_kind_name(axis.kind),
                            requires_compilation(axis.kind) ? " (costs a compilation)"
                                                            : " (one module, many pipelines)");
        (void)diagnostics.add(Severity::Note, line);
    }
    return false;
}

bool PermutationSet::validate_specialization_ids(Span<const u32> declared_ids,
                                                 DiagnosticLog& diagnostics,
                                                 std::string_view shader_name) const noexcept {
    bool valid = true;
    for (const Axis& axis : axes_) {
        if (axis.kind != VariationKind::Specialization) {
            continue;
        }
        bool found = false;
        for (const u32 id : declared_ids) {
            if (id == axis.specialization_id) {
                found = true;
                break;
            }
        }
        if (found) {
            continue;
        }
        valid = false;
        char message[256] = {};
        (void)std::snprintf(
            message, sizeof(message),
            "shader '%.*s': axis '%s' feeds specialization constant %u, which the compiled module "
            "does not declare — the axis would silently do nothing",
            static_cast<int>(shader_name.size()), shader_name.data(), axis.name.c_str(),
            axis.specialization_id);
        (void)diagnostics.add(Severity::Error, message);
    }
    return valid;
}

}  // namespace cy::shader
