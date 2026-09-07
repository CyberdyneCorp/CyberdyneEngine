#include <cy/servers/render/virtual_texturing/feedback.h>

#include <thread>

namespace cy::render::vt {

Status FeedbackBuffer::configure(u32 capacity) noexcept {
    if (capacity == 0) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "virtual texturing: a feedback buffer with no capacity would "
                                     "drop every sample"});
    }
    for (Array<u64>& bank : banks_) {
        if (Status sized = bank.resize(capacity); !sized) {
            return sized;
        }
    }
    capacity_ = capacity;
    reset();
    return ok();
}

bool FeedbackBuffer::record(u64 encoded_address) noexcept {
    if (capacity_ == 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // THE WRITER HALF OF THE HANDSHAKE. Announce which bank is being written, THEN re-read which
    // bank is current. Sequential consistency on both, so that this store cannot be reordered past
    // that load — the flipper's mirror-image pair is in `swap()`, and relaxing either side lets a
    // writer land in a bank the resolver has already started reading.
    const u32 bank = active_.load(std::memory_order_seq_cst) & 1U;
    writers_[bank].fetch_add(1, std::memory_order_seq_cst);
    if ((active_.load(std::memory_order_seq_cst) & 1U) != bank) {
        writers_[bank].fetch_sub(1, std::memory_order_release);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;  // the frame ended underneath this sample; dropping it costs one blurry page
    }

    const u32 slot = heads_[bank].fetch_add(1, std::memory_order_relaxed);
    const bool accepted = slot < capacity_;
    if (accepted) {
        banks_[bank][slot] = encoded_address;
        recorded_.fetch_add(1, std::memory_order_relaxed);
    } else {
        // FULL IS NOT A STALL. The alternative to dropping is waiting for the resolver, and a
        // feedback buffer that waits is a feedback buffer that blocks a frame.
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    writers_[bank].fetch_sub(1, std::memory_order_release);
    return accepted;
}

void FeedbackBuffer::set_density(u32 pixels_per_sample) noexcept {
    u32 density =
        (pixels_per_sample < kMinFeedbackDensity) ? kMinFeedbackDensity : pixels_per_sample;
    density = (density > kMaxFeedbackDensity) ? kMaxFeedbackDensity : density;
    density_.store(density, std::memory_order_relaxed);
}

void FeedbackBuffer::swap() noexcept {
    // THE FLIPPER HALF OF THE HANDSHAKE. Store the new active bank, THEN read the retired bank's
    // writer count. See `record()`.
    const u32 previous = active_.load(std::memory_order_relaxed) & 1U;
    const u32 next = previous ^ 1U;
    heads_[next].store(0, std::memory_order_relaxed);
    active_.store(next, std::memory_order_seq_cst);

    while (writers_[previous].load(std::memory_order_seq_cst) != 0) {
        drain_spins_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }

    retired_ = previous;
    ++swaps_;
}

Status FeedbackBuffer::resolve(Array<FeedbackRequest>& out) noexcept {
    out.clear();
    if (capacity_ == 0) {
        return ok();
    }
    seen_.clear();

    const u32 count = heads_[retired_].load(std::memory_order_relaxed);
    const u32 written = (count > capacity_) ? capacity_ : count;
    for (u32 index = 0; index < written; ++index) {
        const u64 address = banks_[retired_][index];
        if (u32* slot = seen_.find(address); slot != nullptr) {
            ++out[*slot].samples;
            continue;
        }
        FeedbackRequest request;
        request.address = address;
        request.samples = 1;
        if (Status pushed = out.push_back(request); !pushed) {
            return pushed;
        }
        if (auto placed = seen_.insert(address, static_cast<u32>(out.size() - 1)); !placed) {
            out.pop_back();
            return make_unexpected(placed.error());
        }
    }
    return ok();
}

void FeedbackBuffer::reset() noexcept {
    for (std::atomic<u32>& head : heads_) {
        head.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<u32>& writers : writers_) {
        writers.store(0, std::memory_order_relaxed);
    }
    active_.store(0, std::memory_order_relaxed);
    recorded_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    drain_spins_.store(0, std::memory_order_relaxed);
    seen_.clear();
    retired_ = 1;
    swaps_ = 0;
}

}  // namespace cy::render::vt
