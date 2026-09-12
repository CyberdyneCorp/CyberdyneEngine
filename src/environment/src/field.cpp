// The declaration's own rules, and the registration that refuses a second producer. Tasks 1.1,
// 1.2 and 1.4.

#include <cy/core/base/diagnostic_sink.h>
#include <cy/environment/field.h>

#include <cstdio>

namespace cy::environment {

const char* field_type_name(FieldType type) noexcept {
    switch (type) {
        case FieldType::Scalar:
            return "scalar";
        case FieldType::Vec2:
            return "vec2";
        case FieldType::Vec3:
            return "vec3";
        case FieldType::Vec4:
            return "vec4";
        case FieldType::Category:
            return "category";
    }
    return "unknown";
}

const char* field_encoding_name(FieldEncoding encoding) noexcept {
    switch (encoding) {
        case FieldEncoding::F32:
            return "f32";
        case FieldEncoding::UNorm8:
            return "unorm8";
        case FieldEncoding::UNorm16:
            return "unorm16";
        case FieldEncoding::Uint8:
            return "uint8";
        case FieldEncoding::Uint16:
            return "uint16";
    }
    return "unknown";
}

const char* field_layer_name(FieldLayer layer) noexcept {
    switch (layer) {
        case FieldLayer::Base:
            return "base";
        case FieldLayer::Delta:
            return "delta";
        case FieldLayer::kCount:
            break;
    }
    return "unknown";
}

const char* field_layer_rule_name(FieldLayerRule rule) noexcept {
    switch (rule) {
        case FieldLayerRule::Replace:
            return "replace";
        case FieldLayerRule::Add:
            return "add";
        case FieldLayerRule::Multiply:
            return "multiply";
        case FieldLayerRule::Max:
            return "max";
        case FieldLayerRule::Min:
            return "min";
    }
    return "unknown";
}

const char* field_residency_name(FieldResidency level) noexcept {
    switch (level) {
        case FieldResidency::Local:
            return "local";
        case FieldResidency::Regional:
            return "regional";
        case FieldResidency::Macro:
            return "macro";
        case FieldResidency::kCount:
            break;
    }
    return "unknown";
}

const char* field_production_name(FieldProduction production) noexcept {
    switch (production) {
        case FieldProduction::Cpu:
            return "cpu";
        case FieldProduction::Gpu:
            return "gpu";
    }
    return "unknown";
}

const char* producer_kind_name(ProducerKind kind) noexcept {
    switch (kind) {
        case ProducerKind::Baked:
            return "baked";
        case ProducerKind::System:
            return "system";
        case ProducerKind::Generator:
            return "generator";
    }
    return "unknown";
}

const char* declaration_problem_name(DeclarationProblem problem) noexcept {
    switch (problem) {
        case DeclarationProblem::None:
            return "none";
        case DeclarationProblem::NoName:
            return "a field must have a non-empty name";
        case DeclarationProblem::NoLevel:
            return "a field must declare at least one residency level";
        case DeclarationProblem::LevelsNotOrdered:
            return "declared levels must grow coarser from local to macro";
        case DeclarationProblem::CategoryInterpolated:
            return "a category field must be sampled nearest: an averaged category names nothing";
        case DeclarationProblem::EncodingMismatch:
            return "a category field stores raw integers and a continuous field does not";
        case DeclarationProblem::EmptyRange:
            return "a normalised encoding needs a non-empty range to map onto";
        case DeclarationProblem::NoVerticalCells:
            return "a field is at least one cell thick";
        case DeclarationProblem::GpuAuthoritative:
            return "a gameplay-visible field may not be produced by a GPU pass; declare it visual "
                   "or produce it on the CPU";
        case DeclarationProblem::GameplayLevelNotGuaranteed:
            return "a gameplay-visible field's gameplay level must be resident everywhere, or its "
                   "value would depend on what streamed";
        case DeclarationProblem::Redeclared:
            return "this field is already declared, differently";
    }
    return "unknown";
}

FieldValue combine_layers(FieldLayerRule rule, const FieldValue& base, const FieldValue& delta,
                          u32 components) noexcept {
    FieldValue result = base;
    for (u32 index = 0; index < components; ++index) {
        const f32 a = base.components[index];
        const f32 b = delta.components[index];
        switch (rule) {
            case FieldLayerRule::Replace:
                result.components[index] = b;
                break;
            case FieldLayerRule::Add:
                result.components[index] = a + b;
                break;
            case FieldLayerRule::Multiply:
                result.components[index] = a * b;
                break;
            case FieldLayerRule::Max:
                result.components[index] = (a > b) ? a : b;
                break;
            case FieldLayerRule::Min:
                result.components[index] = (a < b) ? a : b;
                break;
        }
    }
    return result;
}

f32 FieldDeclaration::resolved_precision() const noexcept {
    if (precision > 0.0F) {
        return precision;
    }
    // The encoding's own quantum. Two readers of a quantised field cannot meaningfully disagree by
    // less than one step of it, and claiming a tighter agreement than the storage can represent
    // would be a claim about the arithmetic rather than about the field.
    const f32 span = range_max - range_min;
    switch (encoding) {
        case FieldEncoding::UNorm8:
            return span / 255.0F;
        case FieldEncoding::UNorm16:
            return span / 65535.0F;
        case FieldEncoding::Uint8:
        case FieldEncoding::Uint16:
            // Integers are exact on both sides: nearest sampling reads one stored number and
            // neither reader does arithmetic on it.
            return 0.0F;
        case FieldEncoding::F32:
            // Two evaluations of the same blend in f32 differ in the last bits of the mantissa.
            // A relative allowance over the declared range, floored so a field whose range is zero
            // still has one.
            return (span > 0.0F) ? (span * 1.0e-5F) : 1.0e-5F;
    }
    return 1.0e-5F;
}

namespace {

/// The rules about how a field's values are STORED: the value's shape, its encoding, and the range
/// a quantised encoding maps onto. Split from `validate_declaration()` because these three are one
/// subject and the two below are others — and because a reader checking "why was my declaration
/// refused" should find the rule, not a sequence of twelve guards.
[[nodiscard]] DeclarationProblem storage_problem(const FieldDeclaration& declaration) noexcept {
    const bool category = declaration.type == FieldType::Category;
    const bool raw_integer = declaration.encoding == FieldEncoding::Uint8 ||
                             declaration.encoding == FieldEncoding::Uint16;
    if (category != raw_integer) {
        // Both directions: a category stored as a normalised float loses its identity, and a
        // continuous quantity stored as a raw integer has no declared mapping to read it back by.
        return DeclarationProblem::EncodingMismatch;
    }
    if (category && declaration.interpolation != FieldInterpolation::Nearest) {
        return DeclarationProblem::CategoryInterpolated;
    }
    const bool normalised = declaration.encoding == FieldEncoding::UNorm8 ||
                            declaration.encoding == FieldEncoding::UNorm16;
    if (normalised && !(declaration.range_max > declaration.range_min)) {
        return DeclarationProblem::EmptyRange;
    }
    return DeclarationProblem::None;
}

/// At least one level, and the declared ones coarsening outward. An unordered set of levels would
/// make "the finest resident level" a property of the enumeration's order rather than of the
/// resolutions, and `FieldStore::sample()` walks it in order.
[[nodiscard]] DeclarationProblem level_problem(const FieldDeclaration& declaration) noexcept {
    f32 previous = 0.0F;
    bool any = false;
    for (u32 level = 0; level < kFieldResidencyCount; ++level) {
        const FieldLevel& entry = declaration.levels[level];
        if (!entry.declared()) {
            continue;
        }
        if (any && !(entry.cell_metres > previous)) {
            return DeclarationProblem::LevelsNotOrdered;
        }
        previous = entry.cell_metres;
        any = true;
    }
    return any ? DeclarationProblem::None : DeclarationProblem::NoLevel;
}

/// The two rules a GAMEPLAY-VISIBLE field carries and a visual one does not. M10 tasks.md 1.4, and
/// the reason both are refusals here rather than checks at every sample: a value that has already
/// differed between two machines has already shipped.
[[nodiscard]] DeclarationProblem gameplay_problem(const FieldDeclaration& declaration) noexcept {
    if (!declaration.gameplay_visible()) {
        return DeclarationProblem::None;
    }
    // `environment-fields`: "A deterministic field SHALL NOT be produced by a GPU pass whose result
    // is not read back deterministically; such a field SHALL be either CPU-produced or declared
    // visual."
    if (declaration.production != FieldProduction::Cpu) {
        return DeclarationProblem::GpuAuthoritative;
    }
    // And the level gameplay reads must exist everywhere, or the value gameplay sees is a function
    // of what streamed. `store.h` explains why this is the refusal rather than a runtime check.
    const FieldLevel& gameplay = declaration.levels[static_cast<u32>(declaration.gameplay_level)];
    if (!gameplay.declared() || !gameplay.resident_everywhere) {
        return DeclarationProblem::GameplayLevelNotGuaranteed;
    }
    return DeclarationProblem::None;
}

}  // namespace

DeclarationProblem validate_declaration(const FieldDeclaration& declaration) noexcept {
    if (declaration.name == nullptr || declaration.name[0] == '\0') {
        return DeclarationProblem::NoName;
    }
    if (declaration.vertical_cells == 0) {
        return DeclarationProblem::NoVerticalCells;
    }
    if (const DeclarationProblem problem = storage_problem(declaration);
        problem != DeclarationProblem::None) {
        return problem;
    }
    if (const DeclarationProblem problem = level_problem(declaration);
        problem != DeclarationProblem::None) {
        return problem;
    }
    return gameplay_problem(declaration);
}

namespace {

/// Two declarations are the same declaration when every byte a stored tile's meaning depends on
/// agrees. Compared field by field rather than with `memcmp`, because the struct has padding and
/// comparing padding would make a re-declaration fail for a reason nobody can see.
[[nodiscard]] bool same_declaration(const FieldDeclaration& a, const FieldDeclaration& b) noexcept {
    if (a.type != b.type || a.encoding != b.encoding || a.interpolation != b.interpolation ||
        a.cadence != b.cadence || a.production != b.production || a.layer_rule != b.layer_rule ||
        a.classification != b.classification || a.gameplay_level != b.gameplay_level ||
        a.persistent != b.persistent) {
        return false;
    }
    if (a.range_min != b.range_min || a.range_max != b.range_max) {
        return false;
    }
    if (a.vertical_cells != b.vertical_cells || a.vertical_metres != b.vertical_metres ||
        a.vertical_origin_metres != b.vertical_origin_metres) {
        return false;
    }
    if (!(a.default_value == b.default_value)) {
        return false;
    }
    for (u32 level = 0; level < kFieldResidencyCount; ++level) {
        if (a.levels[level].cell_metres != b.levels[level].cell_metres ||
            a.levels[level].resident_everywhere != b.levels[level].resident_everywhere) {
            return false;
        }
    }
    return !(a.potential.value != b.potential.value ||
             a.recovery_per_second != b.recovery_per_second);
}

}  // namespace

FieldRegistry::FieldRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator), records_(allocator), index_(allocator), consumers_(allocator) {}

Status FieldRegistry::declare(const FieldDeclaration& declaration) noexcept {
    const DeclarationProblem problem = validate_declaration(declaration);
    if (problem != DeclarationProblem::None) {
        return fail(ErrorCode::InvalidArgument, declaration_problem_name(problem));
    }

    const FieldId id = declaration.id();
    if (FieldRecord* existing = find_mutable(id); existing != nullptr) {
        // Two modules that both need `wetness` to exist should not have to agree on which of them
        // declares it, so an identical re-declaration is accepted. A DIFFERENT one is refused:
        // widening a range or changing an encoding changes what every already-stored byte means.
        if (!same_declaration(existing->declaration, declaration)) {
            return fail(ErrorCode::AlreadyExists,
                        declaration_problem_name(DeclarationProblem::Redeclared));
        }
        return ok();
    }

    FieldRecord record;
    record.declaration = declaration;
    record.id = id;
    if (Status pushed = records_.push_back(record); !pushed) {
        return pushed;
    }
    if (Expected<usize*, Error> inserted = index_.insert(id.value, records_.size() - 1);
        !inserted) {
        records_.pop_back();
        return make_unexpected(inserted.error());
    }
    return ok();
}

Expected<ProducerToken, Error> FieldRegistry::claim(FieldId field, const char* producer_name,
                                                    ProducerKind kind) noexcept {
    if (producer_name == nullptr || producer_name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "environment: a producer must name itself");
    }
    FieldRecord* record = find_mutable(field);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }

    if (record->claimed) {
        // THE EXIT CRITERION. "A second producer for the same field fails, naming both producers."
        // Both names are in the structured conflict AND in the error's own message, because a
        // caller that only prints the error must still be told who the other producer was.
        conflict_.field = field;
        conflict_.field_name = record->declaration.name;
        conflict_.incumbent = record->producer_name;
        conflict_.challenger = producer_name;
        ++conflicts_;
        std::snprintf(conflict_message_, sizeof(conflict_message_),
                      "field '%s' already has producer '%s'; '%s' is refused — one producer per "
                      "field is a configuration error, not a last-writer-wins race",
                      record->declaration.name, record->producer_name, producer_name);
        emit_diagnostic(DiagnosticSeverity::Error, "environment", conflict_message_);
        return make_unexpected(Error{ErrorCode::AlreadyExists, conflict_message_, 0});
    }

    record->claimed = true;
    record->producer = producer_id(producer_name);
    record->producer_name = producer_name;
    record->producer_kind = kind;
    return ProducerToken(field, record->producer, producer_name);
}

Status FieldRegistry::declare_consumer(const char* consumer, determinism::SimulationClass klass,
                                       FieldId field) noexcept {
    if (consumer == nullptr || consumer[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "environment: a consumer must name itself");
    }
    if (find(field) == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    ConsumerDeclaration declaration;
    declaration.name = consumer;
    declaration.reader_class = klass;
    declaration.field = field;
    return consumers_.push_back(declaration);
}

Status FieldRegistry::validate(Array<FirewallViolation>& out) const noexcept {
    for (const ConsumerDeclaration& consumer : consumers_.span()) {
        const FieldRecord* record = find(consumer.field);
        if (record == nullptr) {
            continue;
        }
        // `determinism::may_read()`, and nothing else. The firewall's direction is
        // `simulation-and-determinism`'s answer and M10 does not get a second opinion about it.
        if (determinism::may_read(consumer.reader_class, record->declaration.classification)) {
            continue;
        }
        FirewallViolation violation;
        violation.consumer = consumer.name;
        violation.field_name = record->declaration.name;
        violation.field = consumer.field;
        violation.reader_class = consumer.reader_class;
        violation.field_class = record->declaration.classification;
        if (Status pushed = out.push_back(violation); !pushed) {
            return pushed;
        }
    }
    return ok();
}

const FieldRecord* FieldRegistry::find(FieldId field) const noexcept {
    const usize* slot = index_.find(field.value);
    return (slot == nullptr) ? nullptr : &records_[*slot];
}

FieldRecord* FieldRegistry::find_mutable(FieldId field) noexcept {
    usize* slot = index_.find(field.value);
    return (slot == nullptr) ? nullptr : &records_[*slot];
}

const FieldDeclaration* FieldRegistry::declaration(FieldId field) const noexcept {
    const FieldRecord* record = find(field);
    return (record == nullptr) ? nullptr : &record->declaration;
}

Status FieldRegistry::unclaimed(Array<FieldId>& out) const noexcept {
    for (const FieldRecord& record : records_.span()) {
        if (record.claimed) {
            continue;
        }
        if (Status pushed = out.push_back(record.id); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::environment
