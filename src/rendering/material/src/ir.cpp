// The material IR: the tables, the builder, the content hash, the serialiser. M7 task 6.1.
//
// See ir.h for the seven decisions this file implements. The two that shape the code most:
// `Builder::make` is the only route to a node, and every hash closes over `Name::text()` rather
// than over `Name::index()`.

#include <cy/rendering/material/ir.h>

#include <cstring>
#include <utility>

namespace cy::rendering::material {
namespace {

struct OpInfo {
    const char* name;
    u32 arity;
    bool commutative;
    bool closure;
    bool leaf_closure;
};

// Indexed by `Op`. The order must match the enumeration exactly; the static_assert below is what
// makes adding an op without describing it a compile error rather than a wrong arity at run time.
constexpr OpInfo kOps[] = {
    {"const", 0, false, false, false},
    {"param", 0, false, false, false},
    {"attr", 0, false, false, false},
    {"field", 0, false, false, false},
    {"sample", 1, false, false, false},
    {"add", 2, true, false, false},
    {"sub", 2, false, false, false},
    {"mul", 2, true, false, false},
    {"div", 2, false, false, false},
    {"min", 2, true, false, false},
    {"max", 2, true, false, false},
    {"dot", 2, false, false, false},
    {"pow", 2, false, false, false},
    {"saturate", 1, false, false, false},
    {"one_minus", 1, false, false, false},
    {"normalize", 1, false, false, false},
    {"lerp", 3, false, false, false},
    {"select", 3, false, false, false},
    {"swizzle", 1, false, false, false},
    {"combine", kVariadic, false, false, false},
    {"custom", kVariadic, false, false, false},
    {"diffuse", 1, false, true, true},
    {"specular", 2, false, true, true},
    {"coat", 1, false, true, true},
    {"transmission", 1, false, true, true},
    {"subsurface", 1, false, true, true},
    {"sheen", 1, false, true, true},
    {"emission", 1, false, true, true},
    {"closure_scale", 2, false, true, false},
    {"closure_add", kVariadic, true, true, false},
    {"closure_layer", 2, false, true, false},
};

static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<usize>(Op::Count),
              "every Op needs a row in kOps: the table carries arity, commutativity and whether "
              "the op is a closure, and all three are read by the builder");

[[nodiscard]] const OpInfo& info_of(Op op) noexcept {
    return kOps[static_cast<usize>(op)];
}

[[nodiscard]] bool is_numeric(ValueType type) noexcept {
    return type != ValueType::Closure && type != ValueType::Count;
}

[[nodiscard]] bool is_vector(ValueType type) noexcept {
    return type == ValueType::Vec2 || type == ValueType::Vec3 || type == ValueType::Vec4;
}

[[nodiscard]] ValueType vector_of(u32 components) noexcept {
    switch (components) {
        case 1:
            return ValueType::Float;
        case 2:
            return ValueType::Vec2;
        case 3:
            return ValueType::Vec3;
        default:
            return ValueType::Vec4;
    }
}

/// -0.0 and 0.0 are the same material and different bytes, and the hash is over bytes.
[[nodiscard]] f32 canonical_scalar(f32 value) noexcept {
    return value == 0.0F ? 0.0F : value;
}

[[nodiscard]] Immediate canonical_immediate(const Immediate& value) noexcept {
    return Immediate{canonical_scalar(value.x), canonical_scalar(value.y),
                     canonical_scalar(value.z), canonical_scalar(value.w), value.mask};
}

/// A constant whose every component is one, at its own width. Half of decision 7's rewrite.
[[nodiscard]] bool is_splat_one(const Module& module, NodeId id) noexcept {
    const Node& node = module.node(id);
    if (node.op != Op::Constant) {
        return false;
    }
    const f32 component[4] = {node.value.x, node.value.y, node.value.z, node.value.w};
    for (u32 index = 0; index < value_type_components(node.type); ++index) {
        if (component[index] != 1.0F) {
            return false;
        }
    }
    return node.type != ValueType::Bool && node.type != ValueType::Int;
}

/// True when `left` does not widen `right`: `1 - float3` is a one-minus, and `float3(1,1,1) -
/// float` is a subtraction that produces a float3 and must stay one.
[[nodiscard]] bool promotes_to(const Module& module, NodeId left, NodeId right) noexcept {
    const ValueType wide = module.node(left).type;
    return wide == ValueType::Float || wide == module.node(right).type;
}

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
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

constexpr u32 kMagic = 0x494D5943U;  // 'CYMI'

}  // namespace

// --- Names and tables ---------------------------------------------------------------------------

const char* value_type_name(ValueType type) noexcept {
    switch (type) {
        case ValueType::Float:
            return "float";
        case ValueType::Vec2:
            return "float2";
        case ValueType::Vec3:
            return "float3";
        case ValueType::Vec4:
            return "float4";
        case ValueType::Int:
            return "int";
        case ValueType::Bool:
            return "bool";
        case ValueType::Closure:
            return "closure";
        case ValueType::Count:
            break;
    }
    return "?";
}

u32 value_type_components(ValueType type) noexcept {
    switch (type) {
        case ValueType::Float:
        case ValueType::Int:
        case ValueType::Bool:
            return 1;
        case ValueType::Vec2:
            return 2;
        case ValueType::Vec3:
            return 3;
        case ValueType::Vec4:
            return 4;
        case ValueType::Closure:
        case ValueType::Count:
            break;
    }
    return 0;
}

const char* op_name(Op op) noexcept {
    return op < Op::Count ? info_of(op).name : "?";
}

u32 op_arity(Op op) noexcept {
    return info_of(op).arity;
}

bool op_is_commutative(Op op) noexcept {
    return info_of(op).commutative;
}

bool op_is_closure(Op op) noexcept {
    return info_of(op).closure;
}

bool op_is_leaf_closure(Op op) noexcept {
    return info_of(op).leaf_closure;
}

bool operator==(const Immediate& a, const Immediate& b) noexcept {
    // Field by field over the BIT PATTERNS, not `memcmp` over the object: a float has no unique
    // object representation, and comparing the padding-free struct as bytes is a rule
    // `bugprone-suspicious-memory-comparison` is right to object to even where the layout happens
    // to be tight. Bitwise equality is what interning wants — two immediates are one value when
    // they are the same bits — and `Builder` normalises the one bit pattern that would otherwise
    // split a value in two, a negative zero.
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

// --- Module -------------------------------------------------------------------------------------

Module::Module(Allocator& allocator) noexcept
    : nodes_(allocator),
      operand_pool_(allocator),
      parameters_(allocator),
      textures_(allocator),
      flags_(allocator),
      origin_begin_(allocator),
      origin_pool_(allocator) {}

Span<const NodeId> Module::operands(NodeId id) const noexcept {
    const Node& value = nodes_[id];
    return {operand_pool_.data() + value.operand_begin, value.operand_count};
}

const TextureDecl* Module::find_texture(Name texture) const noexcept {
    for (const TextureDecl& decl : textures_) {
        if (decl.name == texture) {
            return &decl;
        }
    }
    return nullptr;
}

NodeFlags Module::flags(NodeId id) const noexcept {
    return id < flags_.size() ? static_cast<NodeFlags>(flags_[id]) : NodeFlags::None;
}

Span<const u32> Module::origins(NodeId id) const noexcept {
    if (id + 1 >= origin_begin_.size()) {
        return {};
    }
    const u32 begin = origin_begin_[id];
    const u32 end = origin_begin_[id + 1];
    return {origin_pool_.data() + begin, end - begin};
}

// --- Builder ------------------------------------------------------------------------------------

Builder::Builder(Allocator& allocator, Name material_name) noexcept
    : module_(allocator), interned_(allocator) {
    module_.name_ = material_name;
}

Status Builder::declare_parameter(const ParameterDecl& decl) noexcept {
    for (const ParameterDecl& existing : module_.parameters_) {
        if (existing.name == decl.name) {
            return make_unexpected(
                Error{ErrorCode::AlreadyExists, "a parameter of this name is already declared", 0});
        }
    }
    ParameterDecl stored = decl;
    stored.default_value = canonical_immediate(decl.default_value);
    return module_.parameters_.push_back(stored);
}

Status Builder::declare_texture(const TextureDecl& decl) noexcept {
    for (const TextureDecl& existing : module_.textures_) {
        if (existing.name == decl.name) {
            return make_unexpected(
                Error{ErrorCode::AlreadyExists, "a texture of this name is already declared", 0});
        }
    }
    TextureDecl stored = decl;
    stored.average = canonical_immediate(decl.average);
    return module_.textures_.push_back(stored);
}

Expected<NodeId, Error> Builder::constant(ValueType type, const Immediate& value) noexcept {
    if (!is_numeric(type)) {
        return make_unexpected(invalid("a constant closure is not a value the IR can express"));
    }
    return make(Op::Constant, type, Name{}, value, {});
}

Expected<NodeId, Error> Builder::constant_float(f32 value) noexcept {
    return constant(ValueType::Float, Immediate::scalar(value));
}

Expected<NodeId, Error> Builder::constant_vec3(f32 x, f32 y, f32 z) noexcept {
    return constant(ValueType::Vec3, Immediate{x, y, z, 0.0F, 0});
}

Expected<NodeId, Error> Builder::parameter(Name name) noexcept {
    for (const ParameterDecl& decl : module_.parameters_) {
        if (decl.name == name) {
            return make(Op::Parameter, decl.type, name, Immediate{}, {});
        }
    }
    return make_unexpected(
        Error{ErrorCode::NotFound,
              "a parameter must be declared before it is used, so that a misspelt name is a "
              "diagnostic rather than a silent zero",
              0});
}

Expected<NodeId, Error> Builder::attribute(Name semantic, ValueType type) noexcept {
    return make(Op::Attribute, type, semantic, Immediate{}, {});
}

Expected<NodeId, Error> Builder::field(Name field_name, ValueType type) noexcept {
    return make(Op::Field, type, field_name, Immediate{}, {});
}

Expected<NodeId, Error> Builder::texture_sample(Name texture, NodeId uv) noexcept {
    if (module_.find_texture(texture) == nullptr) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "a texture must be declared before it is sampled", 0});
    }
    const NodeId operands[] = {uv};
    return make(Op::TextureSample, ValueType::Vec4, texture, Immediate{},
                Span<const NodeId>(operands, 1));
}

u32 Builder::swizzle_mask(Span<const u8> components) noexcept {
    u32 mask = 0;
    const u32 count = static_cast<u32>(components.size() > 4 ? 4 : components.size());
    for (u32 index = 0; index < count; ++index) {
        mask |= static_cast<u32>(components[index] & 0x3U) << (index * 4U);
    }
    return mask | (count << 28U);
}

Expected<NodeId, Error> Builder::make(Op op, Span<const NodeId> operands) noexcept {
    return make(op, ValueType::Count, Name{}, Immediate{}, operands);
}

Expected<NodeId, Error> Builder::make(Op op, ValueType hint, Name symbol, const Immediate& value,
                                      Span<const NodeId> operands) noexcept {
    if (op >= Op::Count) {
        return make_unexpected(invalid("no such operation"));
    }
    const OpInfo& described = info_of(op);
    if (described.arity == kVariadic) {
        // `Combine` and `ClosureAdd` need two — a vector of one component is a scalar and a sum of
        // one closure is that closure — but `Custom` is an author's own Slang and one argument is a
        // perfectly ordinary thing to write.
        const usize minimum = op == Op::Custom ? 1U : 2U;
        if (operands.size() < minimum || operands.size() > 4) {
            return make_unexpected(invalid("this operation takes one to four operands"));
        }
    } else if (operands.size() != described.arity) {
        return make_unexpected(invalid("wrong number of operands for this operation"));
    }
    for (const NodeId operand : operands) {
        if (operand >= module_.nodes_.size()) {
            return make_unexpected(invalid("an operand names no node in this module"));
        }
    }

    // DECISION 7 — one spelling per operation (design.md §1.2). An editor emits a "one minus"
    // node and a text definition writes `1 - x`; unless one of them becomes the other here, the two
    // front-ends produce different values for the same material and the exit criterion fails. The
    // builder is where such a choice belongs, because it is the only constructor.
    NodeId rewritten[1] = {};
    if (op == Op::Sub && operands.size() == 2 && is_splat_one(module_, operands[0]) &&
        promotes_to(module_, operands[0], operands[1])) {
        op = Op::OneMinus;
        rewritten[0] = operands[1];
        operands = Span<const NodeId>(rewritten, 1);
    }

    auto typed = result_type(op, hint, value, operands);
    if (!typed) {
        return make_unexpected(typed.error());
    }

    Node node;
    node.op = op;
    node.type = typed.value();
    node.symbol = symbol;
    node.value = canonical_immediate(value);
    return place(node, operands);
}

void Builder::canonicalise(Op op, Span<const NodeId> operands, NodeId* ordered) const noexcept {
    // DECISION 3: a commutative operand list is ordered by CONTENT HASH. Never by node id, which is
    // construction order wearing a disguise, and never by `Name::index()`, which is interning order
    // and is not stable across runs — see the note at the top of ir.h.
    const u32 count = static_cast<u32>(operands.size());
    for (u32 index = 0; index < count; ++index) {
        ordered[index] = operands[index];
    }
    if (!policy_.canonical_commutative || !op_is_commutative(op)) {
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

/// Whether this node may join an existing value. `intern_expressions` and `intern_texture_samples`
/// are the two finer switches: with hash consing on, they decide per node rather than per module.
bool Builder::mergeable(Op op) const noexcept {
    if (!policy_.intern) {
        return false;
    }
    if (op == Op::TextureSample) {
        return policy_.intern_texture_samples && policy_.intern_expressions;
    }
    return op_arity(op) == 0 || policy_.intern_expressions;
}

Expected<NodeId, Error> Builder::place(Node node, Span<const NodeId> operands) noexcept {
    NodeId ordered[4] = {};
    const u32 count = static_cast<u32>(operands.size());
    canonicalise(node.op, operands, ordered);
    const Span<const NodeId> canonical(ordered, count);

    node.hash = hash_of(node, canonical);
    if (mergeable(node.op)) {
        if (const NodeId* found = interned_.find(node.hash); found != nullptr) {
            if (same(node, canonical, *found)) {
                ++module_.merged_values_;
                module_.merged_samples_ += node.op == Op::TextureSample ? 1U : 0U;
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
    u64 hash = hash_u64(kHashSeed, static_cast<u64>(node.op));
    hash = hash_u64(hash, static_cast<u64>(node.type));
    hash = hash_text(hash, node.symbol.text());
    const Immediate canonical = canonical_immediate(node.value);
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
    if (!(other.value == canonical_immediate(node.value))) {
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

Status Builder::annotate(NodeId id, NodeFlags flags) noexcept {
    if (id >= module_.flags_.size()) {
        return make_unexpected(invalid("no such node"));
    }
    module_.flags_[id] = static_cast<u8>(module_.flags_[id] | static_cast<u8>(flags));
    return ok();
}

Status Builder::add_origin(NodeId id, u32 authoring_node) noexcept {
    if (id >= module_.nodes_.size()) {
        return make_unexpected(invalid("no such node"));
    }
    // Held as (node, origin) pairs until `finish`, where they are sorted into the flat side table.
    // Sorting at the end rather than on every call is what keeps a front-end's attribution calls
    // linear in the graph rather than quadratic.
    if (Status pushed = module_.origin_pool_.push_back(id); !pushed) {
        return pushed;
    }
    return module_.origin_pool_.push_back(authoring_node);
}

Status Builder::set_surface(NodeId id) noexcept {
    if (id >= module_.nodes_.size() || module_.nodes_[id].type != ValueType::Closure) {
        return make_unexpected(invalid("the surface root must be a closure"));
    }
    module_.surface_ = id;
    return ok();
}

Status Builder::set_opacity(NodeId id) noexcept {
    if (id >= module_.nodes_.size() || module_.nodes_[id].type != ValueType::Float) {
        return make_unexpected(invalid("the opacity root must be a float"));
    }
    module_.opacity_ = id;
    return ok();
}

Expected<Module, Error> Builder::finish() noexcept {
    if (finished_) {
        return make_unexpected(invalid("a builder produces one module"));
    }
    finished_ = true;

    // The provenance side table: (node, origin) pairs become a sorted, deduplicated flat set.
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

    Array<u32> flattened(module_.origin_pool_.allocator());
    Array<u32> begin(module_.origin_pool_.allocator());
    const u32 node_count = static_cast<u32>(module_.nodes_.size());
    usize cursor = 0;
    for (u32 id = 0; id <= node_count; ++id) {
        if (Status pushed = begin.push_back(static_cast<u32>(flattened.size())); !pushed) {
            return make_unexpected(pushed.error());
        }
        u32 previous = 0xFFFFFFFFU;
        while (cursor < pair_count && pairs[cursor * 2] == id) {
            const u32 origin = pairs[(cursor * 2) + 1];
            if (origin != previous) {
                if (Status pushed = flattened.push_back(origin); !pushed) {
                    return make_unexpected(pushed.error());
                }
                previous = origin;
            }
            ++cursor;
        }
    }
    module_.origin_pool_ = std::move(flattened);
    module_.origin_begin_ = std::move(begin);

    u64 digest = hash_u64(kHashSeed, kIrVersion);
    digest = hash_text(digest, module_.name_.text());
    digest = hash_u64(
        digest, module_.surface_ == kInvalidNode ? 0ULL : module_.nodes_[module_.surface_].hash);
    digest = hash_u64(
        digest, module_.opacity_ == kInvalidNode ? 0ULL : module_.nodes_[module_.opacity_].hash);
    module_.digest_ = digest;
    return std::move(module_);
}

// --- Typing ---------------------------------------------------------------------------------

namespace {

/// The arithmetic rule, stated once: a scalar promotes against a vector, and anything else must
/// agree. Every binary numeric op shares it, which is what keeps `float3 * float` legal and
/// `float3 * float2` a diagnostic in one place rather than in nine.
[[nodiscard]] Expected<ValueType, Error> combine_numeric(ValueType left, ValueType right) noexcept {
    if (!is_numeric(left) || !is_numeric(right)) {
        return make_unexpected(invalid("an arithmetic operand may not be a closure"));
    }
    if (left == right) {
        return left;
    }
    if (left == ValueType::Float && is_vector(right)) {
        return right;
    }
    if (right == ValueType::Float && is_vector(left)) {
        return left;
    }
    return make_unexpected(invalid("these operand types do not combine"));
}

/// A typing rule, as a condition and the sentence to say when it does not hold.
[[nodiscard]] Status require(bool condition, const char* message) noexcept {
    return condition ? Status{} : Status(make_unexpected(invalid(message)));
}

/// The ops whose result type comes from their operands' arithmetic.
[[nodiscard]] Expected<ValueType, Error> arithmetic_type(Op op, const Immediate& value,
                                                         Span<const ValueType> types) noexcept {
    switch (op) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Min:
        case Op::Max:
        case Op::Pow:
            return combine_numeric(types[0], types[1]);
        case Op::Dot:
            if (Status checked = require(types[0] == types[1] && is_vector(types[0]),
                                         "a dot product takes two vectors of one type");
                !checked) {
                return make_unexpected(checked.error());
            }
            return ValueType::Float;
        case Op::Saturate:
        case Op::OneMinus:
        case Op::Normalize:
            if (Status checked = require(is_numeric(types[0]), "this operand may not be a closure");
                !checked) {
                return make_unexpected(checked.error());
            }
            return types[0];
        case Op::Lerp: {
            auto blended = combine_numeric(types[0], types[1]);
            if (!blended) {
                return blended;
            }
            if (Status checked =
                    require(types[2] == ValueType::Float || types[2] == blended.value(),
                            "a lerp factor is a float or matches the blended type");
                !checked) {
                return make_unexpected(checked.error());
            }
            return blended;
        }
        case Op::Select:
            if (Status checked = require(types[0] == ValueType::Bool && types[1] == types[2],
                                         "select takes a bool and two values of one type");
                !checked) {
                return make_unexpected(checked.error());
            }
            return types[1];
        case Op::Swizzle: {
            const u32 count = (value.mask >> 28U) & 0xFU;
            if (Status checked = require(count >= 1 && count <= 4 && is_numeric(types[0]),
                                         "a swizzle selects one to four components");
                !checked) {
                return make_unexpected(checked.error());
            }
            return vector_of(count);
        }
        case Op::Combine:
            for (const ValueType type : types) {
                if (Status checked =
                        require(type == ValueType::Float, "combine assembles a vector from floats");
                    !checked) {
                    return make_unexpected(checked.error());
                }
            }
            return vector_of(static_cast<u32>(types.size()));
        default:
            break;
    }
    return make_unexpected(invalid("no such operation"));
}

/// The closures. Every one of them produces a closure; what differs is what it demands of its
/// operands, and saying that here keeps the arithmetic rules above free of it.
[[nodiscard]] Expected<ValueType, Error> closure_type(Op op, Span<const ValueType> types) noexcept {
    switch (op) {
        case Op::Diffuse:
        case Op::Transmission:
        case Op::Subsurface:
        case Op::Sheen:
        case Op::Emission:
            if (Status checked =
                    require(types[0] == ValueType::Vec3, "a closure's colour is a float3");
                !checked) {
                return make_unexpected(checked.error());
            }
            break;
        case Op::Specular:
            if (Status checked =
                    require(types[0] == ValueType::Vec3 && types[1] == ValueType::Float,
                            "specular takes a colour and a roughness");
                !checked) {
                return make_unexpected(checked.error());
            }
            break;
        case Op::Coat:
            if (Status checked = require(types[0] == ValueType::Float, "a coat takes a roughness");
                !checked) {
                return make_unexpected(checked.error());
            }
            break;
        case Op::ClosureScale:
            if (Status checked =
                    require(types[0] == ValueType::Closure && types[1] == ValueType::Float,
                            "a closure scale takes a closure and a weight");
                !checked) {
                return make_unexpected(checked.error());
            }
            break;
        default:
            for (const ValueType type : types) {
                if (Status checked =
                        require(type == ValueType::Closure, "this operation combines closures");
                    !checked) {
                    return make_unexpected(checked.error());
                }
            }
            break;
    }
    return ValueType::Closure;
}

}  // namespace

Expected<ValueType, Error> Builder::result_type(Op op, ValueType hint, const Immediate& value,
                                                Span<const NodeId> operands) const noexcept {
    // The leaves first: their type cannot be derived and the caller states it.
    switch (op) {
        case Op::Constant:
        case Op::Attribute:
        case Op::Field:
        case Op::Custom:
            if (hint == ValueType::Count) {
                return make_unexpected(invalid("this operation's result type must be stated"));
            }
            return hint;
        case Op::Parameter:
            return hint;
        default:
            break;
    }

    ValueType types[4] = {};
    for (usize index = 0; index < operands.size(); ++index) {
        types[index] = module_.nodes_[operands[index]].type;
    }
    const Span<const ValueType> operand_types(types, operands.size());

    if (op == Op::TextureSample) {
        if (Status checked =
                require(types[0] == ValueType::Vec2, "a texture coordinate is a float2");
            !checked) {
            return make_unexpected(checked.error());
        }
        return ValueType::Vec4;
    }
    if (op_is_closure(op)) {
        return closure_type(op, operand_types);
    }
    return arithmetic_type(op, value, operand_types);
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
    Status status = ok();
    put_u32(out, kMagic, status);
    put_u32(out, kModuleFormatVersion, status);
    put_u32(out, kIrVersion, status);
    put_text(out, module.name().text(), status);

    put_u32(out, static_cast<u32>(module.parameters().size()), status);
    for (const ParameterDecl& decl : module.parameters()) {
        put_text(out, decl.name.text(), status);
        put_u32(out, static_cast<u32>(decl.type), status);
        put_immediate(out, decl.default_value, status);
        put_u32(out, decl.requested_static ? 1U : 0U, status);
    }

    put_u32(out, static_cast<u32>(module.textures().size()), status);
    for (const TextureDecl& decl : module.textures()) {
        put_text(out, decl.name.text(), status);
        put_immediate(out, decl.average, status);
        put_u32(out, decl.shadow_critical ? 1U : 0U, status);
    }

    put_u32(out, module.size(), status);
    for (NodeId id = 0; id < module.size(); ++id) {
        const Node& node = module.node(id);
        put_u32(out, static_cast<u32>(node.op), status);
        put_u32(out, static_cast<u32>(node.type), status);
        put_text(out, node.symbol.text(), status);
        put_immediate(out, node.value, status);
        const Span<const NodeId> operands = module.operands(id);
        put_u32(out, static_cast<u32>(operands.size()), status);
        for (const NodeId operand : operands) {
            put_u32(out, operand, status);
        }
        put_u32(out, static_cast<u32>(module.flags(id)), status);
        const Span<const u32> origins = module.origins(id);
        put_u32(out, static_cast<u32>(origins.size()), status);
        for (const u32 origin : origins) {
            put_u32(out, origin, status);
        }
    }

    put_u32(out, module.surface(), status);
    put_u32(out, module.opacity(), status);
    return status;
}

namespace {

[[nodiscard]] Status decode_declarations(Reader& reader, Builder& builder) noexcept {
    const u32 parameters = reader.u32_value();
    for (u32 index = 0; index < parameters && reader.ok(); ++index) {
        ParameterDecl decl;
        decl.name = Name::intern(reader.text());
        decl.type = static_cast<ValueType>(reader.u32_value());
        decl.default_value = reader.immediate();
        decl.requested_static = reader.u32_value() != 0;
        if (Status declared = builder.declare_parameter(decl); !declared) {
            return declared;
        }
    }
    const u32 textures = reader.u32_value();
    for (u32 index = 0; index < textures && reader.ok(); ++index) {
        TextureDecl decl;
        decl.name = Name::intern(reader.text());
        decl.average = reader.immediate();
        decl.shadow_critical = reader.u32_value() != 0;
        if (Status declared = builder.declare_texture(decl); !declared) {
            return declared;
        }
    }
    return ok();
}

/// One node, rebuilt THROUGH THE BUILDER rather than pushed into an array.
///
/// That is the stronger round trip: a decoded module is re-typed, re-canonicalised and re-interned,
/// so a stream that encodes something the builder would refuse is refused on the way in rather than
/// becoming a module no front-end could have produced.
[[nodiscard]] Status decode_node(Reader& reader, Builder& builder,
                                 Array<NodeId>& mapping) noexcept {
    const auto op = static_cast<Op>(reader.u32_value());
    const auto type = static_cast<ValueType>(reader.u32_value());
    const Name symbol = Name::intern(reader.text());
    const Immediate value = reader.immediate();
    const u32 operand_count = reader.u32_value();
    if (!reader.ok() || operand_count > 4) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "a malformed material IR", 0});
    }
    NodeId operands[4] = {};
    for (u32 index = 0; index < operand_count; ++index) {
        const u32 encoded = reader.u32_value();
        if (encoded >= mapping.size()) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "an operand names no earlier node", 0});
        }
        operands[index] = mapping[encoded];
    }
    auto made = builder.make(op, type, symbol, value, Span<const NodeId>(operands, operand_count));
    if (!made) {
        return make_unexpected(made.error());
    }
    if (Status pushed = mapping.push_back(made.value()); !pushed) {
        return pushed;
    }
    const auto flags = static_cast<NodeFlags>(reader.u32_value());
    if (Status annotated = builder.annotate(made.value(), flags); !annotated) {
        return annotated;
    }
    const u32 origins = reader.u32_value();
    for (u32 index = 0; index < origins && reader.ok(); ++index) {
        if (Status added = builder.add_origin(made.value(), reader.u32_value()); !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace

Expected<Module, Error> decode_module(Span<const u8> bytes, Allocator& allocator) noexcept {
    Reader reader(bytes);
    const u32 magic = reader.u32_value();
    const u32 format = reader.u32_value();
    const u32 ir_version = reader.u32_value();
    if (!reader.ok() || magic != kMagic) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "not a serialised material IR module", 0});
    }
    if (format != kModuleFormatVersion || ir_version != kIrVersion) {
        return make_unexpected(Error{ErrorCode::Unsupported,
                                     "this material IR was written by a different compiler version",
                                     0});
    }
    Builder builder(allocator, Name::intern(reader.text()));
    if (Status declared = decode_declarations(reader, builder); !declared) {
        return make_unexpected(declared.error());
    }

    const u32 node_count = reader.u32_value();
    Array<NodeId> mapping(allocator);
    if (Status reserved = mapping.reserve(node_count); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (u32 index = 0; index < node_count; ++index) {
        if (Status decoded = decode_node(reader, builder, mapping); !decoded) {
            return make_unexpected(decoded.error());
        }
    }

    const u32 surface = reader.u32_value();
    const u32 opacity = reader.u32_value();
    if (!reader.ok()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "a truncated material IR", 0});
    }
    if (surface != kInvalidNode) {
        if (surface >= mapping.size()) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "the surface root is not a node", 0});
        }
        if (Status set = builder.set_surface(mapping[surface]); !set) {
            return make_unexpected(set.error());
        }
    }
    if (opacity != kInvalidNode) {
        if (opacity >= mapping.size()) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "the opacity root is not a node", 0});
        }
        if (Status set = builder.set_opacity(mapping[opacity]); !set) {
            return make_unexpected(set.error());
        }
    }
    return builder.finish();
}

}  // namespace cy::rendering::material
