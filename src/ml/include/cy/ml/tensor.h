#pragma once
// CyberML's tensors. M8.c task 4.1.
//
// `ml-inference`: "The engine SHALL provide a `Tensor` type — shape, element type, and a data
// view". Three properties the specification asks for and that decide the shape of this file:
//
//   * "Tensor memory SHALL be allocatable from engine allocators" — so a tensor holds an
//     `Allocator&` and never touches the global heap.
//   * "SHALL support zero-copy where the backend permits it" — so `Tensor::borrow` exists and a
//     borrowed tensor is indistinguishable from an owned one to everything except its destructor.
//   * "it SHALL reuse its allocations rather than allocating per invocation" — so `reshape` keeps
//     the allocation and fails rather than growing it silently. A session that runs every tick
//     allocates on its first tick and never again, and `Tensor::allocations()` is how a test proves
//     that rather than asserting it.
//
// A dynamic dimension is `TensorShape::kDynamic` and appears only in a SPECIFICATION. A tensor's
// own shape is always concrete: "the batch is 8" is a fact about data, and "the batch is whatever
// you pass" is a fact about a model.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

#include <type_traits>

namespace cy::ml {

/// The element types CyberML carries between the engine and a backend.
///
/// Deliberately short. Every entry is one an engine-side caller can construct and read without a
/// conversion library: a game builds `F32` inputs and reads `F32`, `I32` or `I64` outputs. `F16`
/// and `I8` are here because a model asset DECLARES them — a quantised model's input specification
/// says `I8` — and a caller that cannot produce one gets a shape-and-type error at session setup
/// rather than a silent reinterpretation.
enum class ElementType : u8 {
    Unknown = 0,
    F32 = 1,
    F16 = 2,
    I8 = 3,
    U8 = 4,
    I32 = 5,
    I64 = 6,
    Bool = 7,
    Count = 8,
};

/// Bytes one element occupies. Zero for `Unknown`, which is what makes an undeclared type an
/// allocation failure rather than a zero-sized tensor.
[[nodiscard]] usize element_size(ElementType type) noexcept;

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* element_type_name(ElementType type) noexcept;

/// The largest rank CyberML carries. Eight covers every shape an inference model in a game uses —
/// NCHW is four, an attention tensor is five — and it makes `TensorShape` a value that fits in a
/// cache line and copies without an allocator.
inline constexpr u32 kMaxTensorRank = 8;

/// A shape: a rank and its dimensions.
struct TensorShape {
    /// A dimension a model leaves open. `ml-inference` requires a model asset to carry a "shape
    /// with optional dynamic dimensions", and the batch dimension is the one that is nearly always
    /// dynamic — which is what makes "their inputs SHALL be batchable into one session call"
    /// expressible at all.
    static constexpr i64 kDynamic = -1;

    i64 dimensions[kMaxTensorRank] = {};
    u32 rank = 0;

    /// Build a shape from its dimensions. Fails on a rank above `kMaxTensorRank` and on a negative
    /// dimension that is not `kDynamic` — a -3 is a defect, not a shape.
    [[nodiscard]] static Expected<TensorShape, Error> of(Span<const i64> dimensions) noexcept;

    /// True when any dimension is `kDynamic`. A shape that has one is a specification, not a
    /// tensor's own shape.
    [[nodiscard]] bool has_dynamic() const noexcept;

    /// The product of the dimensions. Zero when the shape has a dynamic dimension, because the
    /// answer is not a number — and zero is a safe answer only because an allocation of zero
    /// elements is refused.
    [[nodiscard]] usize element_count() const noexcept;

    /// Does `concrete` satisfy this specification? Same rank, and every dimension either equal or
    /// declared dynamic here.
    [[nodiscard]] bool accepts(const TensorShape& concrete) const noexcept;

    friend bool operator==(const TensorShape& a, const TensorShape& b) noexcept;
    friend bool operator!=(const TensorShape& a, const TensorShape& b) noexcept {
        return !(a == b);
    }
};

/// One declared input or output of a model: "name, shape with optional dynamic dimensions, element
/// type", which is `ml-inference`'s own list.
struct TensorSpec {
    Name name;
    ElementType type = ElementType::Unknown;
    TensorShape shape;
};

/// A tensor: an element type, a concrete shape, and the bytes.
///
/// Owned or borrowed. A borrowed tensor points at memory somebody else owns — a backend's own
/// arena, a mapped device buffer, a row of an ECS chunk — and frees nothing; that is the zero-copy
/// path, and it is a constructor rather than a flag so a caller cannot forget which one it has.
class Tensor {
public:
    Tensor() noexcept = default;
    ~Tensor();

    Tensor(const Tensor&) = delete;
    Tensor& operator=(const Tensor&) = delete;
    Tensor(Tensor&& other) noexcept;
    Tensor& operator=(Tensor&& other) noexcept;

    /// Allocate from an engine allocator. Fails on a dynamic shape (a tensor is concrete), on an
    /// unknown element type, and on a zero element count.
    [[nodiscard]] static Expected<Tensor, Error> allocate(Allocator& allocator, ElementType type,
                                                          const TensorShape& shape) noexcept;

    /// View memory somebody else owns. `bytes` must cover the shape; nothing is copied and nothing
    /// is freed.
    [[nodiscard]] static Expected<Tensor, Error> borrow(void* data, usize bytes, ElementType type,
                                                        const TensorShape& shape) noexcept;

    /// Reuse this tensor's allocation for a different shape of the same element type — the batch
    /// changed from 8 to 5, and nothing should be reallocated.
    ///
    /// Fails with `BufferTooSmall` when the new shape needs more bytes than this tensor holds,
    /// rather than growing: a session that reallocates in the middle of a frame is the thing
    /// "SHALL reuse its allocations" forbids, and a caller that genuinely needs a bigger tensor
    /// should allocate one at the size it will need.
    [[nodiscard]] Status reshape(const TensorShape& shape) noexcept;

    [[nodiscard]] ElementType type() const noexcept { return type_; }
    [[nodiscard]] const TensorShape& shape() const noexcept { return shape_; }
    [[nodiscard]] usize element_count() const noexcept { return shape_.element_count(); }
    [[nodiscard]] usize byte_size() const noexcept { return bytes_; }
    [[nodiscard]] usize capacity_bytes() const noexcept { return capacity_; }
    [[nodiscard]] bool owns_memory() const noexcept { return allocator_ != nullptr; }
    [[nodiscard]] bool is_empty() const noexcept { return data_ == nullptr; }

    [[nodiscard]] void* data() noexcept { return data_; }
    [[nodiscard]] const void* data() const noexcept { return data_; }

    /// The typed view. Empty when the tensor's element type is not `T`'s, which is how a caller
    /// that guessed reads nothing rather than reinterpreting somebody's bytes.
    template <typename T>
    [[nodiscard]] Span<T> as() noexcept {
        return (element_type_of<T>() == type_ && data_ != nullptr)
                   ? Span<T>(static_cast<T*>(data_), element_count())
                   : Span<T>();
    }

    template <typename T>
    [[nodiscard]] Span<const T> as() const noexcept {
        return (element_type_of<T>() == type_ && data_ != nullptr)
                   ? Span<const T>(static_cast<const T*>(data_), element_count())
                   : Span<const T>();
    }

    /// How many times this tensor has allocated. A session's per-tick reuse is a claim, and this is
    /// the number that checks it.
    [[nodiscard]] u32 allocations() const noexcept { return allocations_; }

    /// The `ElementType` of a C++ type, or `Unknown`. Free rather than a trait class because the
    /// only thing anybody does with it is compare it.
    ///
    /// CV QUALIFIERS ARE STRIPPED, and that is not cosmetic. `as<const f32>()` on a non-const
    /// tensor is the natural way to ask for a read-only view, and without `remove_cv_t` it compares
    /// `const f32` against `f32`, fails, and returns an EMPTY span — a read that silently yields
    /// nothing rather than failing. That defect was found by `integration.ml_runtime` segfaulting
    /// on the empty span it got back, which is the only way it announces itself.
    template <typename T>
    [[nodiscard]] static constexpr ElementType element_type_of() noexcept {
        using Bare = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<Bare, f32>) {
            return ElementType::F32;
        } else if constexpr (std::is_same_v<Bare, i8>) {
            return ElementType::I8;
        } else if constexpr (std::is_same_v<Bare, u8>) {
            return ElementType::U8;
        } else if constexpr (std::is_same_v<Bare, i32>) {
            return ElementType::I32;
        } else if constexpr (std::is_same_v<Bare, i64>) {
            return ElementType::I64;
        } else if constexpr (std::is_same_v<Bare, bool>) {
            return ElementType::Bool;
        } else {
            return ElementType::Unknown;
        }
    }

private:
    void release() noexcept;

    Allocator* allocator_ = nullptr;  // null when borrowed
    void* data_ = nullptr;
    usize bytes_ = 0;     // what the current shape occupies
    usize capacity_ = 0;  // what was allocated or borrowed
    TensorShape shape_;
    ElementType type_ = ElementType::Unknown;
    u32 allocations_ = 0;
};

/// Does a tensor satisfy a specification? Reported as a reason rather than a boolean, because
/// `ml-inference` requires the diagnostic to name "both shapes" and a caller cannot name what it
/// was not told.
enum class SpecMismatch : u8 {
    None = 0,
    ElementType = 1,
    Rank = 2,
    Dimension = 3,
    Count = 4,
};

[[nodiscard]] const char* spec_mismatch_name(SpecMismatch mismatch) noexcept;

/// Compare a tensor against a specification. `dimension` receives the index of the first dimension
/// that disagreed when the result is `Dimension`.
[[nodiscard]] SpecMismatch match_spec(const TensorSpec& spec, const Tensor& tensor,
                                      u32* dimension = nullptr) noexcept;

}  // namespace cy::ml
