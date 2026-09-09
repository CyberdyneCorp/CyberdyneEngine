// The shared pure-expression SSA core: identity, the four extensions, and the round trip.
// M8.b task 2.2.
//
// The anchor in test_anchor.cpp proves the core reproduces the material compiler's three digests.
// These cases prove the properties the anchor cannot see: that operation identity follows TEXT and
// not table position (E2's trap), that the type lattice, the operation table and the roots are the
// domain's (E1, E2, E3), and that a declared phase boundary splits one DAG into two dispatches
// (E4).

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/expr.h>
#include <cy/graph/passes.h>
#include <cy/test/test.h>

#include <utility>

using namespace cy;
using namespace cy::graph;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

constexpr TypeDesc kTypes[] = {
    {"scalar", 1, true, 0, false},
    {"vector", 3, true, 0, false},
};

constexpr RootDecl kRoots[] = {
    {"value", 0, false},
    {"carried", kInvalidType, false},
};

/// A domain whose operation table is supplied at construction, so two of them can differ in exactly
/// one respect: the ORDER of the rows, or the NAMES in them.
class TableDomain final : public Domain {
public:
    TableDomain(Span<const OpDesc> ops, u32 constant_index) noexcept
        : ops_(ops), constant_(static_cast<OpId>(constant_index)) {}

    [[nodiscard]] std::string_view domain_name() const noexcept override { return "test.table"; }
    [[nodiscard]] Span<const TypeDesc> types() const noexcept override { return {kTypes, 2}; }
    [[nodiscard]] Span<const OpDesc> ops() const noexcept override { return ops_; }
    [[nodiscard]] Span<const RootDecl> roots() const noexcept override { return {kRoots, 2}; }
    [[nodiscard]] OpId constant_op() const noexcept override { return constant_; }

    [[nodiscard]] Expected<TypeId, Error> result_type(
        const TypeQuery& query) const noexcept override {
        if (query.operands.empty()) {
            return query.hint == kInvalidType ? static_cast<TypeId>(0) : query.hint;
        }
        return query.operands[0];
    }

private:
    Span<const OpDesc> ops_;
    OpId constant_;
};

consteval OpDesc leaf(const char* name) {
    OpDesc desc;
    desc.name = name;
    desc.arity = 0;
    desc.inline_leaf = true;
    desc.uniform_leaf = true;
    desc.merge = MergeClass::Leaf;
    desc.category = OpCategory::Leaf;
    return desc;
}

consteval OpDesc binary(const char* name, bool commutative) {
    OpDesc desc;
    desc.name = name;
    desc.arity = 2;
    desc.commutative = commutative;
    return desc;
}

consteval OpDesc boundary(const char* name) {
    OpDesc desc;
    desc.name = name;
    desc.arity = 1;
    desc.phase_boundary = true;
    desc.varying = true;
    return desc;
}

/// `const`, `add`, `mul`, `batched` — in that order.
constexpr OpDesc kOrderA[] = {leaf("const"), binary("add", true), binary("mul", true),
                              boundary("batched")};
/// The SAME NAMES at different positions. A core whose identity is text produces the same digest.
constexpr OpDesc kOrderB[] = {binary("mul", true), leaf("const"), boundary("batched"),
                              binary("add", true)};
/// The same POSITIONS with a name changed. A core whose identity is text produces a different one.
constexpr OpDesc kRenamed[] = {leaf("const"), binary("plus", true), binary("mul", true),
                               boundary("batched")};

/// `add(const 2, const 3)` as the first root, built through whichever table it is handed.
[[nodiscard]] Expected<Module, Error> two_plus_three(const Domain& domain, OpId constant, OpId add,
                                                     bool reversed) noexcept {
    Builder builder(allocator(), domain, Name::intern("case"));
    auto two = builder.make(constant, 0, Name{}, Immediate::scalar(2.0F), {});
    auto three = builder.make(constant, 0, Name{}, Immediate::scalar(3.0F), {});
    if (!two || !three) {
        return make_unexpected(Error{ErrorCode::Internal, "constant", 0});
    }
    const NodeId operands[] = {reversed ? three.value() : two.value(),
                               reversed ? two.value() : three.value()};
    auto sum = builder.make(add, Span<const NodeId>(operands, 2));
    if (!sum) {
        return make_unexpected(sum.error());
    }
    if (Status set = builder.set_root(0, sum.value()); !set) {
        return make_unexpected(set.error());
    }
    return builder.finish();
}

}  // namespace

CY_TEST_CASE(
    "graph_expr: operation identity is TEXT, so the table's ORDER does not reach a digest") {
    // E2's trap, stated as `ir.h` states it: "the enumerator VALUES are part of every content hash
    // and therefore of every cook key". A table two domains extend cannot have positional identity.
    const TableDomain first(Span<const OpDesc>(kOrderA, 4), 0);
    const TableDomain second(Span<const OpDesc>(kOrderB, 4), 1);

    auto a = two_plus_three(first, 0, 1, false);
    auto b = two_plus_three(second, 1, 3, false);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_CHECK_EQ(a.value().digest(), b.value().digest());
}

CY_TEST_CASE(
    "graph_expr: an operation RENAMED changes the digest, which is what text identity means") {
    const TableDomain original(Span<const OpDesc>(kOrderA, 4), 0);
    const TableDomain renamed(Span<const OpDesc>(kRenamed, 4), 0);

    auto a = two_plus_three(original, 0, 1, false);
    auto b = two_plus_three(renamed, 0, 1, false);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_CHECK_NE(a.value().digest(), b.value().digest());
}

CY_TEST_CASE("graph_expr: a commutative operand list is canonically ordered by content hash") {
    const TableDomain domain(Span<const OpDesc>(kOrderA, 4), 0);
    auto forward = two_plus_three(domain, 0, 1, false);
    auto reversed = two_plus_three(domain, 0, 1, true);
    CY_REQUIRE(forward.has_value());
    CY_REQUIRE(reversed.has_value());
    // The authored order does not survive. That is exactly why `gameplay-abilities-and-effects`
    // cannot express its ordered pipeline here — design.md §1.3, probe P5.
    CY_CHECK_EQ(forward.value().digest(), reversed.value().digest());
}

CY_TEST_CASE("graph_expr: there is no back edge, and the diagnostic says so") {
    const TableDomain domain(Span<const OpDesc>(kOrderA, 4), 0);
    Builder builder(allocator(), domain, Name::intern("cycle"));
    const NodeId dangling[] = {7, 8};
    auto refused = builder.make(1, Span<const NodeId>(dangling, 2));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("graph_expr: roots are declared by the domain, and their types are checked") {
    const TableDomain domain(Span<const OpDesc>(kOrderA, 4), 0);
    Builder builder(allocator(), domain, Name::intern("roots"));
    auto vector_value = builder.make(0, 1, Name{}, Immediate::scalar(1.0F), {});
    CY_REQUIRE(vector_value.has_value());
    // Root 0 is declared `scalar`; a vector is refused. Root 1 declares no type and accepts it.
    CY_CHECK_FALSE(builder.set_root(0, vector_value.value()).has_value());
    CY_CHECK(builder.set_root(1, vector_value.value()).has_value());
    // A domain declares two roots here; a third does not exist.
    CY_CHECK_FALSE(builder.set_root(2, vector_value.value()).has_value());
}

CY_TEST_CASE("graph_expr: a declared phase boundary splits one DAG into two dispatches") {
    const TableDomain domain(Span<const OpDesc>(kOrderA, 4), 0);
    Builder builder(allocator(), domain, Name::intern("phases"));
    auto origin = builder.make(0, 0, Name{}, Immediate::scalar(1.0F), {});
    CY_REQUIRE(origin.has_value());
    const NodeId query_operand[] = {origin.value()};
    auto queried = builder.make(3, Span<const NodeId>(query_operand, 1));
    CY_REQUIRE(queried.has_value());
    const NodeId sum_operands[] = {queried.value(), origin.value()};
    auto after = builder.make(1, Span<const NodeId>(sum_operands, 2));
    CY_REQUIRE(after.has_value());
    CY_REQUIRE(builder.set_root(0, after.value()).has_value());

    auto module = builder.finish();
    CY_REQUIRE(module.has_value());
    CY_CHECK_EQ(module.value().phase_count(), 2U);
    CY_CHECK_EQ(module.value().phase_of(origin.value()), 0U);
    CY_CHECK_EQ(module.value().phase_of(queried.value()), 1U);
    CY_CHECK_EQ(module.value().phase_of(after.value()), 1U);
}

CY_TEST_CASE("graph_expr: the encode/decode round trip is checked by identity") {
    const TableDomain domain(Span<const OpDesc>(kOrderA, 4), 0);
    auto module = two_plus_three(domain, 0, 1, false);
    CY_REQUIRE(module.has_value());

    Array<u8> bytes(allocator());
    CY_REQUIRE(encode_module(module.value(), bytes).has_value());
    auto decoded = decode_module(bytes.span(), domain, allocator());
    CY_REQUIRE(decoded.has_value());
    CY_CHECK_EQ(decoded.value().digest(), module.value().digest());
    CY_CHECK_EQ(decoded.value().size(), module.value().size());

    // Two encodes of one module produce identical bytes, which is what lets a cook key be a digest
    // over them.
    Array<u8> again(allocator());
    CY_REQUIRE(encode_module(module.value(), again).has_value());
    CY_REQUIRE_EQ(again.size(), bytes.size());
    bool identical = true;
    for (usize index = 0; index < bytes.size(); ++index) {
        identical = identical && bytes[index] == again[index];
    }
    CY_CHECK(identical);

    // A buffer from another domain is refused rather than half-read.
    const TableDomain other(Span<const OpDesc>(kRenamed, 4), 0);
    Array<u8> truncated(allocator());
    CY_REQUIRE(truncated.append(Span<const u8>(bytes.data(), bytes.size() / 2)).has_value());
    CY_CHECK_FALSE(decode_module(truncated.span(), other, allocator()).has_value());
}

CY_TEST_CASE("graph_expr: hash-consing merges structurally identical values") {
    const TableDomain domain(Span<const OpDesc>(kOrderA, 4), 0);
    Builder builder(allocator(), domain, Name::intern("interning"));
    auto first = builder.make(0, 0, Name{}, Immediate::scalar(4.0F), {});
    auto second = builder.make(0, 0, Name{}, Immediate::scalar(4.0F), {});
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first.value(), second.value());

    BuilderPolicy policy;
    policy.intern = false;
    Builder apart(allocator(), domain, Name::intern("interning"));
    apart.set_policy(policy);
    auto one = apart.make(0, 0, Name{}, Immediate::scalar(4.0F), {});
    auto two = apart.make(0, 0, Name{}, Immediate::scalar(4.0F), {});
    CY_REQUIRE(one.has_value());
    CY_REQUIRE(two.has_value());
    CY_CHECK_NE(one.value(), two.value());
}

CY_TEST_CASE(
    "graph_expr: a value nothing reads is deleted by the rebuild, and the switch is real") {
    const TableDomain domain(Span<const OpDesc>(kOrderA, 4), 0);
    Builder builder(allocator(), domain, Name::intern("dead"));
    auto kept = builder.make(0, 0, Name{}, Immediate::scalar(1.0F), {});
    auto orphan = builder.make(0, 0, Name{}, Immediate::scalar(9.0F), {});
    CY_REQUIRE(kept.has_value());
    CY_REQUIRE(orphan.has_value());
    CY_REQUIRE(builder.add_origin(orphan.value(), 4242).has_value());
    CY_REQUIRE(builder.set_root(0, kept.value()).has_value());
    auto module = builder.finish();
    CY_REQUIRE(module.has_value());
    CY_CHECK_EQ(module.value().size(), 2U);

    OptimiseReport report(allocator());
    PassSwitches switches;
    auto optimised = optimise(module.value(), switches, report);
    CY_REQUIRE(optimised.has_value());
    CY_CHECK_EQ(optimised.value().size(), 1U);
    CY_CHECK_EQ(report.dropped_nodes, 1U);
    CY_REQUIRE_EQ(report.dropped_origins.size(), 1U);
    CY_CHECK_EQ(report.dropped_origins[0], 4242U);

    // Off, the pipeline carries the orphan across explicitly. Without that the switch would be a
    // placebo, because a rebuild from the roots has already removed every unreachable node.
    switches.dead_node_elimination = false;
    OptimiseReport carried(allocator());
    auto with_orphans = optimise(module.value(), switches, carried);
    CY_REQUIRE(with_orphans.has_value());
    CY_CHECK_EQ(with_orphans.value().size(), 2U);
    CY_CHECK(carried.bisection_build);
}
