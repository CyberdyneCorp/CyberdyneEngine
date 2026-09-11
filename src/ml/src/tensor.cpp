// Shapes and tensors. M8.c task 4.1.

#include <cy/ml/tensor.h>

#include <cstddef>

namespace cy::ml {
namespace {

constexpr usize kElementSizes[static_cast<usize>(ElementType::Count)] = {
    0,  // Unknown
    4,  // F32
    2,  // F16
    1,  // I8
    1,  // U8
    4,  // I32
    8,  // I64
    1,  // Bool
};

constexpr const char* kElementNames[static_cast<usize>(ElementType::Count)] = {
    "unknown", "f32", "f16", "i8", "u8", "i32", "i64", "bool",
};

}  // namespace

usize element_size(ElementType type) noexcept {
    const auto index = static_cast<usize>(type);
    return (index < static_cast<usize>(ElementType::Count)) ? kElementSizes[index] : 0;
}

const char* element_type_name(ElementType type) noexcept {
    const auto index = static_cast<usize>(type);
    return (index < static_cast<usize>(ElementType::Count)) ? kElementNames[index] : "unknown";
}

// --- TensorShape --------------------------------------------------------------------------------

Expected<TensorShape, Error> TensorShape::of(Span<const i64> dimensions) noexcept {
    if (dimensions.size() > kMaxTensorRank) {
        return fail(ErrorCode::InvalidArgument,
                    "a tensor shape carries at most cy::ml::kMaxTensorRank dimensions",
                    static_cast<i64>(dimensions.size()));
    }
    TensorShape shape;
    shape.rank = static_cast<u32>(dimensions.size());
    for (usize index = 0; index < dimensions.size(); ++index) {
        const i64 dimension = dimensions[index];
        if (dimension < 0 && dimension != kDynamic) {
            return fail(ErrorCode::InvalidArgument,
                        "a dimension is a count or TensorShape::kDynamic, never another negative",
                        dimension);
        }
        shape.dimensions[index] = dimension;
    }
    return shape;
}

bool TensorShape::has_dynamic() const noexcept {
    for (u32 index = 0; index < rank; ++index) {
        if (dimensions[index] == kDynamic) {
            return true;
        }
    }
    return false;
}

usize TensorShape::element_count() const noexcept {
    if (rank == 0 || has_dynamic()) {
        return 0;
    }
    usize count = 1;
    for (u32 index = 0; index < rank; ++index) {
        count *= static_cast<usize>(dimensions[index]);
    }
    return count;
}

bool TensorShape::accepts(const TensorShape& concrete) const noexcept {
    if (rank != concrete.rank) {
        return false;
    }
    for (u32 index = 0; index < rank; ++index) {
        if (dimensions[index] != kDynamic && dimensions[index] != concrete.dimensions[index]) {
            return false;
        }
    }
    return true;
}

bool operator==(const TensorShape& a, const TensorShape& b) noexcept {
    if (a.rank != b.rank) {
        return false;
    }
    for (u32 index = 0; index < a.rank; ++index) {
        if (a.dimensions[index] != b.dimensions[index]) {
            return false;
        }
    }
    return true;
}

// --- Tensor -------------------------------------------------------------------------------------

Tensor::~Tensor() {
    release();
}

Tensor::Tensor(Tensor&& other) noexcept
    : allocator_(other.allocator_),
      data_(other.data_),
      bytes_(other.bytes_),
      capacity_(other.capacity_),
      shape_(other.shape_),
      type_(other.type_),
      allocations_(other.allocations_) {
    other.allocator_ = nullptr;
    other.data_ = nullptr;
    other.bytes_ = 0;
    other.capacity_ = 0;
}

Tensor& Tensor::operator=(Tensor&& other) noexcept {
    if (this != &other) {
        release();
        allocator_ = other.allocator_;
        data_ = other.data_;
        bytes_ = other.bytes_;
        capacity_ = other.capacity_;
        shape_ = other.shape_;
        type_ = other.type_;
        allocations_ = other.allocations_;
        other.allocator_ = nullptr;
        other.data_ = nullptr;
        other.bytes_ = 0;
        other.capacity_ = 0;
    }
    return *this;
}

void Tensor::release() noexcept {
    if (allocator_ != nullptr && data_ != nullptr) {
        allocator_->deallocate(data_, capacity_, alignof(std::max_align_t));
    }
    allocator_ = nullptr;
    data_ = nullptr;
    bytes_ = 0;
    capacity_ = 0;
}

Expected<Tensor, Error> Tensor::allocate(Allocator& allocator, ElementType type,
                                         const TensorShape& shape) noexcept {
    const usize stride = element_size(type);
    if (stride == 0) {
        return fail(ErrorCode::InvalidArgument, "a tensor of unknown element type has no size");
    }
    if (shape.has_dynamic()) {
        return fail(ErrorCode::InvalidArgument,
                    "a tensor's shape is concrete; a dynamic dimension belongs to a TensorSpec");
    }
    const usize count = shape.element_count();
    if (count == 0) {
        return fail(ErrorCode::InvalidArgument, "a tensor holds at least one element");
    }

    Tensor tensor;
    const usize bytes = count * stride;
    void* memory = allocator.allocate(bytes, alignof(std::max_align_t));
    if (memory == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no room for a tensor", static_cast<i64>(bytes));
    }
    tensor.allocator_ = &allocator;
    tensor.data_ = memory;
    tensor.bytes_ = bytes;
    tensor.capacity_ = bytes;
    tensor.shape_ = shape;
    tensor.type_ = type;
    tensor.allocations_ = 1;
    return tensor;
}

Expected<Tensor, Error> Tensor::borrow(void* data, usize bytes, ElementType type,
                                       const TensorShape& shape) noexcept {
    const usize stride = element_size(type);
    if (stride == 0 || data == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a borrowed tensor needs memory and an element type");
    }
    if (shape.has_dynamic()) {
        return fail(ErrorCode::InvalidArgument, "a tensor's shape is concrete");
    }
    const usize needed = shape.element_count() * stride;
    if (needed == 0 || bytes < needed) {
        return fail(ErrorCode::BufferTooSmall, "the borrowed memory does not cover the shape",
                    static_cast<i64>(needed));
    }

    Tensor tensor;
    tensor.allocator_ = nullptr;
    tensor.data_ = data;
    tensor.bytes_ = needed;
    tensor.capacity_ = bytes;
    tensor.shape_ = shape;
    tensor.type_ = type;
    return tensor;
}

Status Tensor::reshape(const TensorShape& shape) noexcept {
    if (data_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an empty tensor has nothing to reshape");
    }
    if (shape.has_dynamic()) {
        return fail(ErrorCode::InvalidArgument, "a tensor's shape is concrete");
    }
    const usize needed = shape.element_count() * element_size(type_);
    if (needed == 0) {
        return fail(ErrorCode::InvalidArgument, "a tensor holds at least one element");
    }
    if (needed > capacity_) {
        return fail(ErrorCode::BufferTooSmall,
                    "reshape reuses an allocation and never grows one; allocate at the largest "
                    "shape the session will run",
                    static_cast<i64>(needed));
    }
    shape_ = shape;
    bytes_ = needed;
    return ok();
}

// --- Specification matching ---------------------------------------------------------------------

const char* spec_mismatch_name(SpecMismatch mismatch) noexcept {
    switch (mismatch) {
        case SpecMismatch::None:
            return "none";
        case SpecMismatch::ElementType:
            return "element-type";
        case SpecMismatch::Rank:
            return "rank";
        case SpecMismatch::Dimension:
            return "dimension";
        case SpecMismatch::Count:
            break;
    }
    return "unknown";
}

SpecMismatch match_spec(const TensorSpec& spec, const Tensor& tensor, u32* dimension) noexcept {
    if (dimension != nullptr) {
        *dimension = 0;
    }
    if (spec.type != tensor.type()) {
        return SpecMismatch::ElementType;
    }
    if (spec.shape.rank != tensor.shape().rank) {
        return SpecMismatch::Rank;
    }
    for (u32 index = 0; index < spec.shape.rank; ++index) {
        const i64 declared = spec.shape.dimensions[index];
        if (declared != TensorShape::kDynamic && declared != tensor.shape().dimensions[index]) {
            if (dimension != nullptr) {
                *dimension = index;
            }
            return SpecMismatch::Dimension;
        }
    }
    return SpecMismatch::None;
}

}  // namespace cy::ml
