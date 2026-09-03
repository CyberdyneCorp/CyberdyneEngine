// The scratch arena's out-of-line half: the one allocation, the bump, and the poison.

#include <cy/core/jobs/scratch.h>

#include <cstring>
#include <new>
#include <utility>

namespace cy::jobs {
namespace {

/// Development builds poison released scratch; Profile and Shipping do not. See the header: a
/// pointer that outlives its task otherwise reads as valid memory belonging to the next task, and
/// the resulting defect is a wrong value rather than a crash.
constexpr bool kPoisonOnRelease =
#if defined(CY_DEVELOPMENT)
    true;
#else
    false;
#endif

}  // namespace

ScratchArena::~ScratchArena() {
    ::operator delete[](block_, std::nothrow);
    block_ = nullptr;
}

ScratchArena::ScratchArena(ScratchArena&& other) noexcept
    : block_(other.block_),
      capacity_(other.capacity_),
      used_(other.used_),
      high_water_(other.high_water_),
      exhaustions_(other.exhaustions_) {
    other.block_ = nullptr;
    other.capacity_ = 0;
    other.used_ = 0;
    other.high_water_ = 0;
    other.exhaustions_ = 0;
}

ScratchArena& ScratchArena::operator=(ScratchArena&& other) noexcept {
    if (this != &other) {
        ::operator delete[](block_, std::nothrow);
        block_ = std::exchange(other.block_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        used_ = std::exchange(other.used_, 0);
        high_water_ = std::exchange(other.high_water_, 0);
        exhaustions_ = std::exchange(other.exhaustions_, 0);
    }
    return *this;
}

Status ScratchArena::initialize(usize bytes) noexcept {
    if (block_ != nullptr) {
        return fail(ErrorCode::AlreadyExists, "the scratch arena is already initialised");
    }
    if (bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "a scratch arena of zero bytes serves no task");
    }
    // The one place this file touches the general heap. Task 2.1's allocator interface replaces
    // exactly this call; every allocation a task makes is a bump within the block it returns.
    auto* block = static_cast<u8*>(::operator new[](bytes, std::nothrow));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the scratch arena's block could not be reserved");
    }
    block_ = block;
    capacity_ = bytes;
    used_ = 0;
    high_water_ = 0;
    return ok();
}

void* ScratchArena::allocate(usize bytes, usize alignment) noexcept {
    CY_ASSERT_MSG(alignment != 0 && (alignment & (alignment - 1)) == 0,
                  "a scratch alignment is a power of two");
    if (block_ == nullptr || bytes == 0) {
        return nullptr;
    }

    const usize mask = alignment - 1;
    const usize aligned = (used_ + mask) & ~mask;
    // Written as a subtraction against the remaining space rather than as `aligned + bytes >
    // capacity_`, which overflows for a large request and then reads as a fit.
    if (aligned > capacity_ || bytes > capacity_ - aligned) {
        ++exhaustions_;
        return nullptr;
    }

    u8* result = block_ + aligned;
    used_ = aligned + bytes;
    if (used_ > high_water_) {
        high_water_ = used_;
    }
    return result;
}

void ScratchArena::release_to(usize mark) noexcept {
    CY_ASSERT_MSG(mark <= used_, "a scratch mark is released to, never advanced to");
    if (mark > used_) {
        return;
    }
    if (kPoisonOnRelease && block_ != nullptr && used_ > mark) {
        std::memset(block_ + mark, kScratchPoison, used_ - mark);
    }
    used_ = mark;
}

}  // namespace cy::jobs
