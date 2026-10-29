// SPDX-License-Identifier: MIT
#pragma once
// An exactly sized buffer for streaming's fixed allocations. Issue #87.
//
// A stream's footprint is a number the engine reports and a test bounds, so every buffer on the
// stream path is allocated at exactly the size it needs. `Array` grows geometrically from eight,
// which is right for a container and wrong here twice over: a 200 KiB arena would be a 256 KiB
// block, and shrinking it afterwards would hold both at once.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>

#include <cstring>
#include <type_traits>

namespace cy::audio {

/// A heap block of exactly `size()` trivially copyable `T`s. Move-only.
template <class T>
class AudioBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "an audio buffer holds plain samples or bytes");

public:
    explicit AudioBuffer(Allocator& allocator) noexcept : allocator_(&allocator) {}
    ~AudioBuffer() { release(); }

    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    AudioBuffer(AudioBuffer&& other) noexcept
        : allocator_(other.allocator_), data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }
    AudioBuffer& operator=(AudioBuffer&&) = delete;

    /// Replace the block with `count` zeroed elements. The old block is released first, so the
    /// peak is the larger of the two rather than their sum.
    [[nodiscard]] Status allocate(usize count) noexcept {
        release();
        if (count == 0) {
            return ok();
        }
        void* block = allocator_->allocate(count * sizeof(T), alignof(T) < 16 ? 16 : alignof(T));
        if (block == nullptr) {
            return fail(ErrorCode::OutOfMemory, "an audio buffer was refused by its allocator");
        }
        std::memset(block, 0, count * sizeof(T));
        data_ = static_cast<T*>(block);
        size_ = count;
        return ok();
    }

    /// Grow to `count` elements, keeping the contents. Both blocks exist for the copy.
    [[nodiscard]] Status grow(usize count) noexcept {
        if (count <= size_) {
            return ok();
        }
        AudioBuffer<T> larger(*allocator_);
        if (Status sized = larger.allocate(count); !sized) {
            return sized;
        }
        if (size_ > 0) {
            std::memcpy(larger.data_, data_, size_ * sizeof(T));
        }
        release();
        data_ = larger.data_;
        size_ = larger.size_;
        larger.data_ = nullptr;
        larger.size_ = 0;
        return ok();
    }

    void release() noexcept {
        if (data_ != nullptr) {
            allocator_->deallocate(data_, size_ * sizeof(T), alignof(T) < 16 ? 16 : alignof(T));
            data_ = nullptr;
            size_ = 0;
        }
    }

    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] usize bytes() const noexcept { return size_ * sizeof(T); }
    [[nodiscard]] T& operator[](usize index) noexcept { return data_[index]; }
    [[nodiscard]] const T& operator[](usize index) const noexcept { return data_[index]; }

private:
    Allocator* allocator_;
    T* data_ = nullptr;
    usize size_ = 0;
};

}  // namespace cy::audio
