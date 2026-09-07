// The viewport transport's wire, on the engine's side. See wire.h.

#include "wire.h"

#include <cy/core/base/assert.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace cy::viewport {
namespace {

/// How many times a reader retries a torn read before giving up for this tick.
///
/// A writer holds the lock for the length of one 32-byte copy, so a reader that has spun 64 times
/// has not lost a race — it has been descheduled, or the writer died mid-write. Either way the
/// honest answer is "no new frame", which the editor already knows how to show.
constexpr u32 kReadAttempts = 64;

}  // namespace

Status Handshake::validate() const noexcept {
    if (magic != kProtocolMagic) {
        return fail(ErrorCode::InvalidArgument,
                    "the handshake's first four bytes are not this protocol's");
    }
    if (version != kProtocolVersion) {
        return fail(ErrorCode::InvalidArgument,
                    "the handshake names a protocol version this build does not speak");
    }
    if (buffer_count == 0 || buffer_count > 4) {
        return fail(ErrorCode::InvalidArgument, "a ring holds one to four images");
    }
    if (width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument, "the handshake announces an empty image");
    }
    for (u32 slot = 0; slot < buffer_count; ++slot) {
        if (planes[slot].stride < width || planes[slot].allocation_bytes == 0) {
            return fail(ErrorCode::InvalidArgument,
                        "a slot's stride or allocation cannot hold the announced image");
        }
    }
    return ok();
}

u32 Handshake::expected_descriptors() const noexcept {
    return buffer_count + ((has_timelines != 0) ? 2U : 0U) + 1U;
}

// --- AnnouncementPage ---------------------------------------------------------------------------

AnnouncementPage::~AnnouncementPage() {
    if (state_ != nullptr) {
        (void)::munmap(static_cast<void*>(state_), kPageBytes);
        state_ = nullptr;
    }
    if (descriptor_ >= 0) {
        (void)::close(descriptor_);
        descriptor_ = -1;
    }
}

Status AnnouncementPage::create() noexcept {
    CY_ASSERT_MSG(state_ == nullptr, "AnnouncementPage::create() twice");
    const int raw = static_cast<int>(::syscall(SYS_memfd_create, "cy-viewport-announce", 0U));
    if (raw < 0) {
        return fail(ErrorCode::Unavailable, "memfd_create for the announcement page", errno);
    }
    if (::ftruncate(raw, static_cast<off_t>(kPageBytes)) != 0) {
        const int failure = errno;
        (void)::close(raw);
        return fail(ErrorCode::Unavailable, "sizing the announcement page", failure);
    }
    void* mapping = ::mmap(nullptr, kPageBytes, PROT_READ | PROT_WRITE, MAP_SHARED, raw, 0);
    if (mapping == MAP_FAILED) {
        const int failure = errno;
        (void)::close(raw);
        return fail(ErrorCode::Unavailable, "mapping the announcement page", failure);
    }
    descriptor_ = raw;
    // The page is zero-filled by the kernel, which is exactly the initial state: sequence 0, no
    // frame, no heartbeat, nothing written and nothing held. Placement-new would rewrite the same
    // zeros and would make the atomics' initial values a compiler's decision rather than the
    // kernel's, which matters because the editor may map this page before the first publish.
    state_ = static_cast<SharedState*>(mapping);
    return ok();
}

void AnnouncementPage::publish(const Announcement& frame) noexcept {
    if (state_ == nullptr) {
        return;
    }
    // THE SEQLOCK, WITHOUT A STANDALONE FENCE. `std::atomic_thread_fence` is not supported under
    // `-fsanitize=thread` — GCC refuses to compile it — and it is not needed: an acquire-release
    // read-modify-write opens the write, which stops the payload stores below being reordered
    // above it, and a release store closes it, which orders those stores before the sequence
    // becomes even again. That is the pair the reader's two acquire loads are matched with.
    const u64 start = state_->sequence.fetch_add(1, std::memory_order_acq_rel);
    const u64 words[4] = {
        frame.frame_id, static_cast<u64>(frame.slot) | (static_cast<u64>(frame.generation) << 32U),
        frame.timeline_value, frame.submitted_nanos};
    for (u32 index = 0; index < 4; ++index) {
        state_->words[index].store(words[index], std::memory_order_relaxed);
    }
    state_->sequence.store(start + 2, std::memory_order_release);
    state_->heartbeat.fetch_add(1, std::memory_order_relaxed);
}

bool AnnouncementPage::read(Announcement& out) const noexcept {
    if (state_ == nullptr) {
        return false;
    }
    for (u32 attempt = 0; attempt < kReadAttempts; ++attempt) {
        const u64 before = state_->sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        u64 words[4] = {};
        for (u32 index = 0; index < 4; ++index) {
            words[index] = state_->words[index].load(std::memory_order_relaxed);
        }
        if (state_->sequence.load(std::memory_order_acquire) != before) {
            continue;
        }
        Announcement frame;
        frame.frame_id = words[0];
        frame.slot = static_cast<u32>(words[1] & 0xFFFF'FFFFULL);
        frame.generation = static_cast<u32>(words[1] >> 32U);
        frame.timeline_value = words[2];
        frame.submitted_nanos = words[3];
        if (frame.frame_id == 0) {
            return false;
        }
        out = frame;
        return true;
    }
    return false;
}

void AnnouncementPage::set_writing(bool writing, u32 slot) noexcept {
    if (state_ == nullptr) {
        return;
    }
    state_->writing.store(writing ? (static_cast<u64>(slot) + 1U) : 0U, std::memory_order_seq_cst);
}

u64 AnnouncementPage::held() const noexcept {
    return (state_ == nullptr) ? 0U : state_->held.load(std::memory_order_seq_cst);
}

u64 AnnouncementPage::heartbeat() const noexcept {
    return (state_ == nullptr) ? 0U : state_->heartbeat.load(std::memory_order_relaxed);
}

void AnnouncementPage::set_held_for_test(u64 packed) noexcept {
    if (state_ != nullptr) {
        state_->held.store(packed, std::memory_order_seq_cst);
    }
}

// --- Ring ---------------------------------------------------------------------------------------

Status Ring::resize(u32 count) noexcept {
    if (count == 0 || count > 4) {
        return fail(ErrorCode::InvalidArgument, "a ring holds one to four images");
    }
    count_ = count;
    for (RingSlot& slot : slots_) {
        slot = RingSlot{};
    }
    published_ = kNoSlot;
    held_slot_ = kNoSlot;
    return ok();
}

void Ring::observe_held(u64 packed) noexcept {
    if (packed == 0) {
        held_slot_ = kNoSlot;
        return;
    }
    const u32 slot = held_slot(packed);
    held_slot_ = slot;
    if (slot < count_ && slots_[slot].last_frame == held_frame(packed)) {
        // From here on, this slot may not be rewritten until the editor's release timeline reaches
        // this value. This assignment is what closes the corruption a release counter alone leaves
        // open: an editor that keeps re-sampling a frame it has already released is reading a slot
        // the engine believes it may write.
        slots_[slot].needs_release = held_frame(packed);
    }
}

u32 Ring::pick(u64 release_value) const noexcept {
    u32 best = kNoSlot;
    u64 best_age = 0;
    for (u32 index = 0; index < count_; ++index) {
        if (published_ == index || held_slot_ == index) {
            continue;
        }
        const RingSlot& slot = slots_[index];
        if (slot.needs_release != 0 && release_value < slot.needs_release) {
            continue;
        }
        if (best == kNoSlot || slot.last_frame < best_age) {
            best = index;
            best_age = slot.last_frame;
        }
    }
    return best;
}

void Ring::record_published(u32 index, u64 frame_id) noexcept {
    if (index < count_) {
        slots_[index].last_frame = frame_id;
        slots_[index].initialised = true;
    }
    published_ = index;
}

u64 Ring::release_requirement(u32 index) const noexcept {
    return (index < count_) ? slots_[index].needs_release : 0U;
}

void Ring::reclaim() noexcept {
    for (RingSlot& slot : slots_) {
        slot.needs_release = 0;
    }
    held_slot_ = kNoSlot;
    published_ = kNoSlot;
}

bool claim_names(u64 packed, u32 slot) noexcept {
    return packed != 0 && held_slot(packed) == slot;
}

// --- The socket ---------------------------------------------------------------------------------

Status send_descriptors(int socket, const int* descriptors, u32 count, const void* payload,
                        usize payload_bytes) noexcept {
    if (count == 0 || count > 16) {
        return fail(ErrorCode::InvalidArgument, "one message carries one to sixteen descriptors");
    }
    // 256 bytes holds CMSG_SPACE(16 * sizeof(int)) = 80 with room to spare, and being a fixed array
    // keeps this function allocation-free on a path that runs while an editor is connecting.
    alignas(struct cmsghdr) char control[256] = {};
    iovec io{};
    io.iov_base = const_cast<void*>(payload);
    io.iov_len = payload_bytes;

    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = CMSG_SPACE(count * sizeof(int));

    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(count * sizeof(int));
    std::memcpy(CMSG_DATA(header), descriptors, count * sizeof(int));

    const ssize_t sent = ::sendmsg(socket, &message, MSG_NOSIGNAL);
    if (sent < 0) {
        return fail(ErrorCode::Unavailable, "sending the viewport handshake", errno);
    }
    return ok();
}

bool peer_closed(int socket) noexcept {
    char byte = 0;
    const ssize_t read = ::recv(socket, &byte, 1, MSG_DONTWAIT);
    return read == 0;
}

u64 monotonic_nanos() noexcept {
    timespec now{};
    (void)::clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<u64>(now.tv_sec) * 1'000'000'000ULL) + static_cast<u64>(now.tv_nsec);
}

}  // namespace cy::viewport
