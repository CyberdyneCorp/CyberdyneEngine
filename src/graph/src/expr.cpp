// The shared pure-expression SSA core: the builder, the content hash, the serialiser. Task 2.2.
//
// See expr.h for the four extensions this file implements and for the one trap E2 carries.

#include <cy/graph/expr.h>

#include <cstring>
#include <utility>

namespace cy::graph {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

[[nodiscard]] f32 canonical_scalar(f32 value) noexcept {
    return value == 0.0F ? 0.0F : value;
}

// --- Byte reading and writing ------------------------------------------------------------------
//
// Explicit little-endian, byte by byte. `encode_module`'s output is a cook key's input, so it may
// not depend on the host's byte order or on a struct's padding.

void put_u32(Array<u8>& out, u32 value, Status& status) noexcept {
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> shift) & 0xFFU)); !pushed) {
            status = pushed;
            return;
        }
    }
}

void put_f32(Array<u8>& out, f32 value, Status& status) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits, status);
}

void put_text(Array<u8>& out, std::string_view text, Status& status) noexcept {
    put_u32(out, static_cast<u32>(text.size()), status);
    for (const char character : text) {
        if (!status) {
            return;
        }
        if (Status pushed = out.push_back(static_cast<u8>(character)); !pushed) {
            status = pushed;
            return;
        }
    }
}

void put_immediate(Array<u8>& out, const Immediate& value, Status& status) noexcept {
    put_f32(out, value.x, status);
    put_f32(out, value.y, status);
    put_f32(out, value.z, status);
    put_f32(out, value.w, status);
    put_u32(out, value.mask, status);
}

class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    [[nodiscard]] u32 u32_value() noexcept {
        if (!ok_ || cursor_ + 4 > bytes_.size()) {
            ok_ = false;
            return 0;
        }
        u32 value = 0;
        for (u32 index = 0; index < 4U; ++index) {
            value |= static_cast<u32>(bytes_[cursor_ + index]) << (index * 8U);
        }
        cursor_ += 4;
        return value;
    }

    [[nodiscard]] f32 f32_value() noexcept {
        const u32 bits = u32_value();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] std::string_view text() noexcept {
        const u32 size = u32_value();
        if (!ok_ || cursor_ + size > bytes_.size()) {
            ok_ = false;
            return {};
        }
        const auto* data = reinterpret_cast<const char*>(bytes_.data() + cursor_);
        cursor_ += size;
        return {data, size};
    }

    [[nodiscard]] Immediate immediate() noexcept {
        Immediate value;
        value.x = f32_value();
        value.y = f32_value();
        value.z = f32_value();
        value.w = f32_value();
        value.mask = u32_value();
        return value;
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
    bool ok_ = true;
};

constexpr u32 kMagic = 0x47585943U;  // 'CYXG'

}  // namespace

// --- Hashing ------------------------------------------------------------------------------------

u64 hash_bytes(u64 seed, const void* data, usize size) noexcept {
    const auto* cursor = static_cast<const u8*>(data);
    u64 hash = seed;
    for (usize index = 0; index < size; ++index) {
        hash ^= static_cast<u64>(cursor[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

u64 hash_u64(u64 seed, u64 value) noexcept {
    return hash_bytes(seed, &value, sizeof(value));
}

u64 hash_text(u64 seed, std::string_view text) noexcept {
    const u64 sized = hash_u64(seed, static_cast<u64>(text.size()));
    return hash_bytes(sized, text.data(), text.size());
}

u64 text_identity(std::string_view name) noexcept {
    return hash_text(kHashSeed, name);
}

bool operator==(const Immediate& a, const Immediate& b) noexcept {
    // Field by field over the BIT PATTERNS, not `memcmp` over the object: a float has no unique
    // object representation. Bitwise equality is what interning wants, and the builder normalises
    // the one bit pattern that would otherwise split a value in two, a negative zero.
    const f32 left[4] = {a.x, a.y, a.z, a.w};
    const f32 right[4] = {b.x, b.y, b.z, b.w};
    for (u32 index = 0; index < 4U; ++index) {
        u32 left_bits = 0;
        u32 right_bits = 0;
        std::memcpy(&left_bits, &left[index], sizeof(left_bits));
        std::memcpy(&right_bits, &right[index], sizeof(right_bits));
        if (left_bits != right_bits) {
            return false;
        }
    }
    return a.mask == b.mask;
}

// --- Domain ---------------------------------------------------------------------------------

bool Domain::normalise(const Builder& /*builder*/, OpId& /*op*/, Span<const NodeId> /*operands*/,
                       NodeId* /*out*/, u32& /*out_count*/) const noexcept {
    return false;
}

OpId Domain::constant_op() const noexcept {
    return kInvalidOp;
}

FoldOutcome Domain::fold(const Builder& /*out*/, OpId /*op*/, TypeId /*type*/,
                         const Immediate& /*value*/,
                         Span<const NodeId> /*operands*/) const noexcept {
    return FoldOutcome{};
}

Expected<NodeId, Error> Domain::simplify_aggregate(const Node& node, Span<const NodeId> operands,
                                                   Builder& out,
                                                   u32& /*simplifications*/) const noexcept {
    for (const NodeId operand : operands) {
        if (operand == kInvalidNode) {
            return kInvalidNode;
        }
    }
    return out.make(node.op, node.type, node.symbol, node.value, operands);
}

Immediate Domain::canonical_immediate(const Immediate& value) const noexcept {
    return Immediate{canonical_scalar(value.x), canonical_scalar(value.y),
                     canonical_scalar(value.z), canonical_scalar(value.w), value.mask};
}

const TypeDesc& Domain::type_desc(TypeId type) const noexcept {
    static constexpr TypeDesc kUnknown{};
    const Span<const TypeDesc> table = types();
    return type < table.size() ? table[type] : kUnknown;
}

const OpDesc& Domain::op_desc(OpId op) const noexcept {
    static constexpr OpDesc kUnknown{};
    const Span<const OpDesc> table = ops();
    return op < table.size() ? table[op] : kUnknown;
}

u64 Domain::type_identity(TypeId type) const noexcept {
    const TypeDesc& desc = type_desc(type);
    return desc.pinned ? desc.identity : text_identity(desc.name);
}

u64 Domain::op_identity(OpId op) const noexcept {
    const OpDesc& desc = op_desc(op);
    return desc.pinned ? desc.identity : text_identity(desc.name);
}

TypeId Domain::find_type(std::string_view name) const noexcept {
    const Span<const TypeDesc> table = types();
    for (usize index = 0; index < table.size(); ++index) {
        if (name == table[index].name) {
            return static_cast<TypeId>(index);
        }
    }
    return kInvalidType;
}

OpId Domain::find_op(std::string_view name) const noexcept {
    const Span<const OpDesc> table = ops();
    for (usize index = 0; index < table.size(); ++index) {
        if (name == table[index].name) {
            return static_cast<OpId>(index);
        }
    }
    return kInvalidOp;
}

bool Domain::valid_type(TypeId type) const noexcept {
    return type < types().size();
}

bool Domain::valid_op(OpId op) const noexcept {
    return op < ops().size();
}

u32 Domain::components(TypeId type) const noexcept {
    return type_desc(type).components;
}

// --- Module -------------------------------------------------------------------------------------

Module::Module(Allocator& allocator, const Domain& domain) noexcept
    : domain_(&domain),
      nodes_(allocator),
      operand_pool_(allocator),
      decls_(allocator),
      flags_(allocator),
      phase_(allocator),
      origin_begin_(allocator),
      origin_pool_(allocator),
      roots_(allocator) {}

Span<const NodeId> Module::operands(NodeId id) const noexcept {
    const Node& value = nodes_[id];
    return {operand_pool_.data() + value.operand_begin, value.operand_count};
}

NodeId Module::root(u32 slot) const noexcept {
    return slot < roots_.size() ? roots_[slot] : kInvalidNode;
}

const Decl* Module::find_decl(u16 kind, Name name) const noexcept {
    for (const Decl& decl : decls_) {
        if (decl.kind == kind && decl.name == name) {
            return &decl;
        }
    }
    return nullptr;
}

u32 Module::flags(NodeId id) const noexcept {
    return id < flags_.size() ? flags_[id] : 0U;
}

Span<const u32> Module::origins(NodeId id) const noexcept {
    if (id + 1 >= origin_begin_.size()) {
        return {};
    }
    const u32 begin = origin_begin_[id];
    const u32 end = origin_begin_[id + 1];
    return {origin_pool_.data() + begin, end - begin};
}

u32 Module::phase_of(NodeId id) const noexcept {
    return id < phase_.size() ? phase_[id] : 0U;
}

// --- Builder ------------------------------------------------------------------------------------

Builder::Builder(Allocator& allocator, const Domain& domain, Name module_name) noexcept
    : domain_(&domain), module_(allocator, domain), interned_(allocator) {
    module_.name_ = module_name;
}

Status Builder::declare(const Decl& decl) noexcept {
    for (const Decl& existing : module_.decls_) {
        if (existing.kind == decl.kind && existing.name == decl.name) {
            return make_unexpected(
                Error{ErrorCode::AlreadyExists, "a declaration of this kind and name exists", 0});
        }
    }
    Decl stored = decl;
    stored.value = domain_->canonical_immediate(decl.value);
    return module_.decls_.push_back(stored);
}

Status Builder::check_arity(const OpDesc& desc, usize count) noexcept {
    if (desc.arity == kVariadic) {
        if (count < desc.min_operands || count > desc.max_operands ||
            count > static_cast<usize>(kMaxOperands)) {
            return make_unexpected(invalid("wrong number of operands for this operation"));
        }
        return ok();
    }
    if (count != desc.arity) {
        return make_unexpected(invalid("wrong number of operands for this operation"));
    }
    return ok();
}

Expected<NodeId, Error> Builder::make(OpId op, Span<const NodeId> operands) noexcept {
    return make(op, kInvalidType, Name{}, Immediate{}, operands);
}

Expected<NodeId, Error> Builder::make(OpId op, TypeId hint, Name symbol, const Immediate& value,
                                      Span<const NodeId> operands) noexcept {
    if (!domain_->valid_op(op)) {
        return make_unexpected(invalid("no such operation in this domain"));
    }
    if (Status checked = check_arity(domain_->op_desc(op), operands.size()); !checked) {
        return make_unexpected(checked.error());
    }
    for (const NodeId operand : operands) {
        if (operand >= module_.nodes_.size()) {
            // THE ABSENCE OF A BACK EDGE, stated as a diagnostic. An operand must already exist,
            // which is why no state machine, resumable tree, loop or basic block can be encoded
            // here — design.md §1.3, probe P3.
            return make_unexpected(invalid("an operand names no node in this module"));
        }
    }

    NodeId rewritten[kMaxOperands] = {};
    u32 rewritten_count = 0;
    if (domain_->normalise(*this, op, operands, rewritten, rewritten_count)) {
        operands = Span<const NodeId>(rewritten, rewritten_count);
        if (Status checked = check_arity(domain_->op_desc(op), operands.size()); !checked) {
            return make_unexpected(checked.error());
        }
    }

    TypeId operand_types[kMaxOperands] = {};
    for (usize index = 0; index < operands.size(); ++index) {
        operand_types[index] = module_.nodes_[operands[index]].type;
    }
    TypeQuery query;
    query.op = op;
    query.hint = hint;
    query.value = value;
    query.operands = Span<const TypeId>(operand_types, operands.size());
    auto typed = domain_->result_type(query);
    if (!typed) {
        return make_unexpected(typed.error());
    }
    if (!domain_->valid_type(typed.value())) {
        return make_unexpected(invalid("the domain typed this node with no type"));
    }

    Node node;
    node.op = op;
    node.type = typed.value();
    node.symbol = symbol;
    node.value = domain_->canonical_immediate(value);
    return place(node, operands);
}

void Builder::canonicalise(OpId op, Span<const NodeId> operands, NodeId* ordered) const noexcept {
    // A commutative operand list is ordered by CONTENT HASH. Never by node id, which is
    // construction order wearing a disguise, and never by `Name::index()`, which is interning order
    // and is not stable across runs.
    const u32 count = static_cast<u32>(operands.size());
    for (u32 index = 0; index < count; ++index) {
        ordered[index] = operands[index];
    }
    if (!policy_.canonical_commutative || !domain_->op_desc(op).commutative) {
        return;
    }
    for (u32 outer = 1; outer < count; ++outer) {
        for (u32 inner = outer; inner > 0 && module_.nodes_[ordered[inner - 1]].hash >
                                                 module_.nodes_[ordered[inner]].hash;
             --inner) {
            const NodeId swap = ordered[inner - 1];
            ordered[inner - 1] = ordered[inner];
            ordered[inner] = swap;
        }
    }
}

bool Builder::mergeable(OpId op) const noexcept {
    if (!policy_.intern) {
        return false;
    }
    switch (domain_->op_desc(op).merge) {
        case MergeClass::Leaf:
            return true;
        case MergeClass::Sample:
            return policy_.intern_samples && policy_.intern_expressions;
        case MergeClass::Expression:
            return policy_.intern_expressions;
        case MergeClass::Never:
            break;
    }
    return false;
}

Expected<NodeId, Error> Builder::place(Node node, Span<const NodeId> operands) noexcept {
    NodeId ordered[kMaxOperands] = {};
    const u32 count = static_cast<u32>(operands.size());
    canonicalise(node.op, operands, ordered);
    const Span<const NodeId> canonical(ordered, count);

    node.hash = hash_of(node, canonical);
    const bool sample = domain_->op_desc(node.op).merge == MergeClass::Sample;
    if (mergeable(node.op)) {
        if (const NodeId* found = interned_.find(node.hash); found != nullptr) {
            if (same(node, canonical, *found)) {
                ++module_.merged_values_;
                module_.merged_samples_ += sample ? 1U : 0U;
                return *found;
            }
            // A 64-bit collision between two different nodes. It costs a duplicate value and never
            // a wrong merge, which is the only outcome that matters.
        }
    }

    node.operand_begin = static_cast<u32>(module_.operand_pool_.size());
    node.operand_count = count;
    for (u32 index = 0; index < count; ++index) {
        if (Status pushed = module_.operand_pool_.push_back(ordered[index]); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    const auto id = static_cast<NodeId>(module_.nodes_.size());
    if (Status pushed = module_.nodes_.push_back(node); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = module_.flags_.push_back(0); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (policy_.intern && interned_.find(node.hash) == nullptr) {
        auto inserted = interned_.insert(node.hash, id);
        if (!inserted) {
            return make_unexpected(inserted.error());
        }
    }
    return id;
}

u64 Builder::hash_of(const Node& node, Span<const NodeId> operands) const noexcept {
    u64 hash = hash_u64(kHashSeed, domain_->op_identity(node.op));
    hash = hash_u64(hash, domain_->type_identity(node.type));
    hash = hash_text(hash, node.symbol.text());
    const Immediate canonical = domain_->canonical_immediate(node.value);
    hash = hash_bytes(hash, &canonical, sizeof(canonical));
    hash = hash_u64(hash, static_cast<u64>(operands.size()));
    for (const NodeId operand : operands) {
        hash = hash_u64(hash, module_.nodes_[operand].hash);
    }
    return hash;
}

bool Builder::same(const Node& node, Span<const NodeId> operands, NodeId candidate) const noexcept {
    const Node& other = module_.nodes_[candidate];
    if (other.op != node.op || other.type != node.type || other.symbol != node.symbol) {
        return false;
    }
    if (!(other.value == domain_->canonical_immediate(node.value))) {
        return false;
    }
    const Span<const NodeId> other_operands = module_.operands(candidate);
    if (other_operands.size() != operands.size()) {
        return false;
    }
    for (usize index = 0; index < operands.size(); ++index) {
        if (other_operands[index] != operands[index]) {
            return false;
        }
    }
    return true;
}

Status Builder::annotate(NodeId id, u32 flags) noexcept {
    if (id >= module_.flags_.size()) {
        return make_unexpected(invalid("no such node"));
    }
    module_.flags_[id] |= flags;
    return ok();
}

Status Builder::add_origin(NodeId id, u32 authoring_node) noexcept {
    if (id >= module_.nodes_.size()) {
        return make_unexpected(invalid("no such node"));
    }
    // Held as (node, origin) pairs until `finish`, where they become the flat side table. Sorting
    // at the end keeps a front-end's attribution calls linear in the graph rather than quadratic.
    if (Status pushed = module_.origin_pool_.push_back(id); !pushed) {
        return pushed;
    }
    return module_.origin_pool_.push_back(authoring_node);
}

Status Builder::set_root(u32 slot, NodeId id) noexcept {
    const Span<const RootDecl> declared = domain_->roots();
    if (slot >= declared.size()) {
        return make_unexpected(invalid("this domain declares no such root"));
    }
    if (id >= module_.nodes_.size()) {
        return make_unexpected(invalid("a root must name a value in this module"));
    }
    const RootDecl& decl = declared[slot];
    if (decl.type != kInvalidType && module_.nodes_[id].type != decl.type) {
        return make_unexpected(invalid("this root's type is not the one the domain declared"));
    }
    while (module_.roots_.size() <= slot) {
        if (Status pushed = module_.roots_.push_back(kInvalidNode); !pushed) {
            return pushed;
        }
    }
    module_.roots_[slot] = id;
    return ok();
}

Status Builder::finish_origins() noexcept {
    Array<u32>& pairs = module_.origin_pool_;
    const usize pair_count = pairs.size() / 2;
    for (usize outer = 1; outer < pair_count; ++outer) {
        for (usize inner = outer; inner > 0; --inner) {
            const bool ordered = pairs[(inner - 1) * 2] < pairs[inner * 2] ||
                                 (pairs[(inner - 1) * 2] == pairs[inner * 2] &&
                                  pairs[((inner - 1) * 2) + 1] <= pairs[(inner * 2) + 1]);
            if (ordered) {
                break;
            }
            for (usize half = 0; half < 2; ++half) {
                const u32 swap = pairs[((inner - 1) * 2) + half];
                pairs[((inner - 1) * 2) + half] = pairs[(inner * 2) + half];
                pairs[(inner * 2) + half] = swap;
            }
        }
    }

    Array<u32> flattened(pairs.allocator());
    Array<u32> begin(pairs.allocator());
    const u32 node_count = static_cast<u32>(module_.nodes_.size());
    usize cursor = 0;
    for (u32 id = 0; id <= node_count; ++id) {
        if (Status pushed = begin.push_back(static_cast<u32>(flattened.size())); !pushed) {
            return pushed;
        }
        u32 previous = 0xFFFFFFFFU;
        while (cursor < pair_count && pairs[cursor * 2] == id) {
            const u32 origin = pairs[(cursor * 2) + 1];
            if (origin != previous) {
                if (Status pushed = flattened.push_back(origin); !pushed) {
                    return pushed;
                }
                previous = origin;
            }
            ++cursor;
        }
    }
    module_.origin_pool_ = std::move(flattened);
    module_.origin_begin_ = std::move(begin);
    return ok();
}

Status Builder::finish_phases() noexcept {
    // E4. One DAG splits into as many dispatches as there are phase boundaries on the longest path
    // from a leaf: a boundary's value arrives from a batched external query, so everything above it
    // belongs to a later dispatch. A domain that declares no boundary op has one phase.
    if (Status sized = module_.phase_.resize(module_.nodes_.size()); !sized) {
        return sized;
    }
    u32 highest = 0;
    for (NodeId id = 0; id < module_.nodes_.size(); ++id) {
        u32 phase = 0;
        for (const NodeId operand : module_.operands(id)) {
            const u32 operand_phase = module_.phase_[operand];
            phase = operand_phase > phase ? operand_phase : phase;
        }
        phase += domain_->op_desc(module_.nodes_[id].op).phase_boundary ? 1U : 0U;
        module_.phase_[id] = static_cast<u8>(phase);
        highest = phase > highest ? phase : highest;
    }
    module_.phase_count_ = highest + 1;
    return ok();
}

Expected<Module, Error> Builder::finish() noexcept {
    if (finished_) {
        return make_unexpected(invalid("a builder produces one module"));
    }
    finished_ = true;

    const Span<const RootDecl> declared = domain_->roots();
    while (module_.roots_.size() < declared.size()) {
        if (Status pushed = module_.roots_.push_back(kInvalidNode); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (usize slot = 0; slot < declared.size(); ++slot) {
        if (declared[slot].required && module_.roots_[slot] == kInvalidNode) {
            return make_unexpected(invalid("a required root of this domain was never set"));
        }
    }

    if (Status done = finish_origins(); !done) {
        return make_unexpected(done.error());
    }
    if (Status done = finish_phases(); !done) {
        return make_unexpected(done.error());
    }

    u64 digest = hash_u64(kHashSeed, kIrVersion);
    digest = hash_text(digest, module_.name_.text());
    for (const NodeId root : module_.roots_) {
        digest = hash_u64(digest, root == kInvalidNode ? 0ULL : module_.nodes_[root].hash);
    }
    module_.digest_ = digest;
    return std::move(module_);
}

// --- Traversal ----------------------------------------------------------------------------------

Status canonical_order(const Module& module, Span<const NodeId> roots,
                       Array<NodeId>& out) noexcept {
    out.clear();
    Array<u8> state(module.allocator());
    if (Status sized = state.resize(module.size()); !sized) {
        return sized;
    }
    for (u8& mark : state) {
        mark = 0;
    }
    Array<NodeId> stack(module.allocator());
    for (const NodeId root : roots) {
        if (root == kInvalidNode) {
            continue;
        }
        if (Status pushed = stack.push_back(root); !pushed) {
            return pushed;
        }
        while (!stack.empty()) {
            const NodeId current = stack.back();
            if (state[current] == 2) {
                stack.pop_back();
                continue;
            }
            if (state[current] == 1) {
                state[current] = 2;
                stack.pop_back();
                if (Status pushed = out.push_back(current); !pushed) {
                    return pushed;
                }
                continue;
            }
            state[current] = 1;
            const Span<const NodeId> operands = module.operands(current);
            // Pushed in reverse so that operand 0 is visited first: the emitted order is then the
            // order an author reads, and it is the same for two modules with the same content.
            for (usize index = operands.size(); index > 0; --index) {
                if (state[operands[index - 1]] != 2) {
                    if (Status pushed = stack.push_back(operands[index - 1]); !pushed) {
                        return pushed;
                    }
                }
            }
        }
    }
    return ok();
}

// --- Serialisation --------------------------------------------------------------------------

Status encode_module(const Module& module, Array<u8>& out) noexcept {
    const Domain& domain = module.domain();
    Status status = ok();
    put_u32(out, kMagic, status);
    put_u32(out, kModuleFormatVersion, status);
    put_u32(out, kIrVersion, status);
    // THE DOMAIN'S NAME AND EVERY OP AND TYPE BY TEXT. E2's trap applies to the wire as much as to
    // the hash: an index into a table two domains extend is not an identity.
    put_text(out, domain.domain_name(), status);
    put_text(out, module.name().text(), status);

    put_u32(out, static_cast<u32>(module.decls().size()), status);
    for (const Decl& decl : module.decls()) {
        put_text(out, decl.name.text(), status);
        put_text(out, domain.type_desc(decl.type).name, status);
        put_immediate(out, decl.value, status);
        put_u32(out, decl.kind, status);
        put_u32(out, decl.flags, status);
    }

    put_u32(out, module.size(), status);
    for (NodeId id = 0; id < module.size(); ++id) {
        const Node& node = module.node(id);
        put_text(out, domain.op_desc(node.op).name, status);
        put_text(out, domain.type_desc(node.type).name, status);
        put_text(out, node.symbol.text(), status);
        put_immediate(out, node.value, status);
        put_u32(out, node.operand_count, status);
        for (const NodeId operand : module.operands(id)) {
            put_u32(out, operand, status);
        }
        put_u32(out, module.flags(id), status);
        put_u32(out, static_cast<u32>(module.origins(id).size()), status);
        for (const u32 origin : module.origins(id)) {
            put_u32(out, origin, status);
        }
    }

    put_u32(out, static_cast<u32>(module.roots().size()), status);
    for (const NodeId root : module.roots()) {
        put_u32(out, root, status);
    }
    return status;
}

namespace {

[[nodiscard]] Status decode_decls(Reader& reader, const Domain& domain, Builder& builder) noexcept {
    const u32 count = reader.u32_value();
    for (u32 index = 0; index < count && reader.ok(); ++index) {
        Decl decl;
        decl.name = Name::intern(reader.text());
        decl.type = domain.find_type(reader.text());
        decl.value = reader.immediate();
        decl.kind = static_cast<u16>(reader.u32_value());
        decl.flags = reader.u32_value();
        if (!reader.ok()) {
            break;
        }
        if (Status declared = builder.declare(decl); !declared) {
            return declared;
        }
    }
    return ok();
}

[[nodiscard]] Status decode_nodes(Reader& reader, const Domain& domain, Builder& builder,
                                  Array<NodeId>& mapping) noexcept {
    const u32 count = reader.u32_value();
    for (u32 index = 0; index < count && reader.ok(); ++index) {
        const OpId op = domain.find_op(reader.text());
        const TypeId type = domain.find_type(reader.text());
        const Name symbol = Name::intern(reader.text());
        const Immediate value = reader.immediate();
        const u32 operand_count = reader.u32_value();
        if (!reader.ok() || operand_count > kMaxOperands) {
            return make_unexpected(invalid("a node in this buffer names too many operands"));
        }
        NodeId operands[kMaxOperands] = {};
        for (u32 slot = 0; slot < operand_count; ++slot) {
            const u32 encoded = reader.u32_value();
            if (encoded >= mapping.size()) {
                return make_unexpected(invalid("a node names an operand it does not follow"));
            }
            operands[slot] = mapping[encoded];
        }
        auto made =
            builder.make(op, type, symbol, value, Span<const NodeId>(operands, operand_count));
        if (!made) {
            return make_unexpected(made.error());
        }
        if (Status pushed = mapping.push_back(made.value()); !pushed) {
            return pushed;
        }
        if (Status annotated = builder.annotate(made.value(), reader.u32_value()); !annotated) {
            return annotated;
        }
        const u32 origin_count = reader.u32_value();
        for (u32 slot = 0; slot < origin_count && reader.ok(); ++slot) {
            if (Status added = builder.add_origin(made.value(), reader.u32_value()); !added) {
                return added;
            }
        }
    }
    return ok();
}

}  // namespace

Expected<Module, Error> decode_module(Span<const u8> bytes, const Domain& domain,
                                      Allocator& allocator) noexcept {
    Reader reader(bytes);
    if (reader.u32_value() != kMagic) {
        return make_unexpected(invalid("this is not an encoded expression module"));
    }
    if (reader.u32_value() != kModuleFormatVersion || reader.u32_value() != kIrVersion) {
        return make_unexpected(invalid("this module was encoded by a different core version"));
    }
    if (reader.text() != domain.domain_name()) {
        return make_unexpected(invalid("this module belongs to a different domain"));
    }
    const Name module_name = Name::intern(reader.text());
    if (!reader.ok()) {
        return make_unexpected(invalid("this buffer is truncated"));
    }

    // REPLAYED THROUGH THE BUILDER rather than assigned into a module: decoding is the one place a
    // hand-written buffer could introduce a node that no builder would have made. Replaying types,
    // canonicalises and interns it exactly as the front-end did, so a decoded module is in the same
    // canonical form as an authored one or it is refused.
    Builder builder(allocator, domain, module_name);
    if (Status decoded = decode_decls(reader, domain, builder); !decoded) {
        return make_unexpected(decoded.error());
    }
    Array<NodeId> mapping(allocator);
    if (Status decoded = decode_nodes(reader, domain, builder, mapping); !decoded) {
        return make_unexpected(decoded.error());
    }

    const u32 root_count = reader.u32_value();
    for (u32 slot = 0; slot < root_count && reader.ok(); ++slot) {
        const u32 encoded = reader.u32_value();
        if (encoded == kInvalidNode) {
            continue;
        }
        if (encoded >= mapping.size()) {
            return make_unexpected(invalid("a root names no node in this buffer"));
        }
        if (Status set = builder.set_root(slot, mapping[encoded]); !set) {
            return make_unexpected(set.error());
        }
    }
    if (!reader.ok()) {
        return make_unexpected(invalid("this buffer is truncated"));
    }
    return builder.finish();
}

}  // namespace cy::graph
