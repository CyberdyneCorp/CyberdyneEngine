// SPDX-License-Identifier: MIT
// The macOS half of the engine viewport publisher: IOSurface-backed Metal textures handed to the
// editor without copying across the process boundary. Platform-neutral ownership lives in wire.cpp.

#include <cy/backends/viewport/publisher.h>

#include <cy/core/memory/system_allocator.h>

#include "wire.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>

namespace cy::viewport {
namespace {

constexpr u32 kInFlight = 3;
constexpr u32 kBytesPerPixel = 4;

struct SharedImage {
    IOSurfaceRef surface = nullptr;
    id<MTLTexture> __strong texture = nil;
};

[[nodiscard]] Status set_nonblocking(int descriptor) noexcept {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        return fail(ErrorCode::Unavailable, "making a viewport socket non-blocking", errno);
    }
    (void)::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    return ok();
}

}  // namespace

struct Publisher::Impl {
    ~Impl() { teardown(); }

    [[nodiscard]] Status open(const PublisherOptions& options) noexcept;
    void service() noexcept;
    [[nodiscard]] bool has_client() const noexcept { return client_ >= 0; }
    [[nodiscard]] FrameStaging begin_frame() noexcept;
    [[nodiscard]] Expected<u64, Error> publish() noexcept;
    [[nodiscard]] const char* adapter_name() const noexcept { return adapter_name_; }
    [[nodiscard]] constexpr u64 modifier() const noexcept { return 0; }
    [[nodiscard]] const PublisherStatistics& statistics() const noexcept { return statistics_; }
    void teardown() noexcept;

    id<MTLDevice> __strong device_ = nil;
    id<MTLCommandQueue> __strong queue_ = nil;
    id<MTLBuffer> __strong staging_[kInFlight]{};
    SharedImage images_[kMaxBuffers]{};
    char adapter_name_[256] = {};

    Ring ring_;
    AnnouncementPage page_;
    Handshake handshake_{};
    int listener_ = -1;
    int client_ = -1;
    char socket_path_[108] = {};

    u32 width_ = 0;
    u32 height_ = 0;
    u32 buffers_ = 0;
    u64 frame_id_ = 0;
    u32 reserved_slot_ = Ring::kNoSlot;
    u32 reserved_in_flight_ = 0;
    PublisherStatistics statistics_{};
};

Status Publisher::Impl::open(const PublisherOptions& options) noexcept {
    if (options.width == 0 || options.height == 0) {
        return fail(ErrorCode::InvalidArgument, "the viewport image must have a non-zero size");
    }
    if (options.buffers == 0 || options.buffers > kMaxBuffers) {
        return fail(ErrorCode::InvalidArgument, "the viewport ring holds one to four images");
    }

    @autoreleasepool {
        device_ = MTLCreateSystemDefaultDevice();
        if (device_ == nil) {
            return fail(ErrorCode::Unavailable, "MTLCreateSystemDefaultDevice returned no device");
        }
        const char* name = device_.name.UTF8String;
        (void)::snprintf(adapter_name_, sizeof(adapter_name_), "%s",
                         name != nullptr ? name : "Metal");
        if (options.adapter != nullptr && options.adapter[0] != '\0' &&
            std::strstr(adapter_name_, options.adapter) == nullptr) {
            return fail(ErrorCode::Unavailable,
                        "the default Metal device does not match the requested adapter");
        }
        queue_ = [device_ newCommandQueue];
        if (queue_ == nil) {
            return fail(ErrorCode::Unavailable, "creating the viewport Metal command queue");
        }
    }

    width_ = options.width;
    height_ = options.height;
    buffers_ = options.buffers;
    if (Status resized = ring_.resize(buffers_); !resized) {
        return resized;
    }
    if (Status created = page_.create(); !created) {
        return created;
    }

    const usize bytes = static_cast<usize>(width_) * height_ * kBytesPerPixel;
    @autoreleasepool {
        for (u32 index = 0; index < kInFlight; ++index) {
            staging_[index] =
                [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            if (staging_[index] == nil) {
                return fail(ErrorCode::OutOfMemory, "allocating a viewport staging buffer");
            }
        }

        for (u32 slot = 0; slot < buffers_; ++slot) {
            // wgpu 30 can import IOSurface IDs without an XPC service. The socket is mode 0600,
            // but global lookup remains a documented same-user boundary rather than a sandbox.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            NSDictionary* properties = @{
                (id)kIOSurfaceWidth : @(width_),
                (id)kIOSurfaceHeight : @(height_),
                (id)kIOSurfaceBytesPerElement : @(kBytesPerPixel),
                (id)kIOSurfaceBytesPerRow : @(width_ * kBytesPerPixel),
                (id)kIOSurfaceAllocSize : @(bytes),
                (id)kIOSurfacePixelFormat : @(kFourccAbgr8888),
                (id)kIOSurfaceIsGlobal : @YES,
            };
            images_[slot].surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
#pragma clang diagnostic pop
            if (images_[slot].surface == nullptr) {
                return fail(ErrorCode::OutOfMemory, "creating a viewport IOSurface");
            }
            MTLTextureDescriptor* description = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                             width:width_
                                            height:height_
                                         mipmapped:NO];
            description.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            description.storageMode = MTLStorageModeShared;
            images_[slot].texture = [device_ newTextureWithDescriptor:description
                                                             iosurface:images_[slot].surface
                                                                plane:0];
            if (images_[slot].texture == nil) {
                return fail(ErrorCode::Unavailable,
                            "creating a Metal texture over the viewport IOSurface");
            }
            handshake_.planes[slot] = PlaneDescription{
                static_cast<u64>(IOSurfaceGetBytesPerRow(images_[slot].surface)),
                static_cast<u64>(IOSurfaceGetID(images_[slot].surface)),
                static_cast<u64>(IOSurfaceGetAllocSize(images_[slot].surface))};
        }
    }

    handshake_.width = width_;
    handshake_.height = height_;
    handshake_.buffer_count = buffers_;
    handshake_.generation = ring_.generation();
    handshake_.has_timelines = 0;
    handshake_.modifier = 0;

    if (options.socket_path == nullptr || options.socket_path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "the viewport publisher needs a socket path");
    }
    const usize path_length = std::strlen(options.socket_path);
    if (path_length >= sizeof(socket_path_)) {
        return fail(ErrorCode::InvalidArgument,
                    "the viewport socket path is longer than a Unix socket allows");
    }
    std::memcpy(socket_path_, options.socket_path, path_length + 1);

    listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_ < 0) {
        return fail(ErrorCode::Unavailable, "creating the viewport socket", errno);
    }
    if (Status flags = set_nonblocking(listener_); !flags) {
        return flags;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path_, path_length + 1);
    (void)::unlink(socket_path_);
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return fail(ErrorCode::Unavailable, "binding the viewport socket", errno);
    }
    (void)::chmod(socket_path_, S_IRUSR | S_IWUSR);
    if (::listen(listener_, 4) != 0) {
        return fail(ErrorCode::Unavailable, "listening on the viewport socket", errno);
    }
    return ok();
}

void Publisher::Impl::service() noexcept {
    if (client_ >= 0) {
        if (peer_closed(client_)) {
            (void)::close(client_);
            client_ = -1;
            ring_.reclaim();
        }
        return;
    }
    if (listener_ < 0) {
        return;
    }
    const int stream = ::accept(listener_, nullptr, nullptr);
    if (stream < 0) {
        return;
    }
    if (!set_nonblocking(stream)) {
        (void)::close(stream);
        return;
    }
    int no_sigpipe = 1;
    (void)::setsockopt(stream, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
    const int descriptor = page_.descriptor();
    if (Status handed = send_descriptors(stream, &descriptor, 1, &handshake_, sizeof(handshake_));
        !handed) {
        (void)::close(stream);
        return;
    }
    client_ = stream;
    statistics_.connections += 1;
}

FrameStaging Publisher::Impl::begin_frame() noexcept {
    reserved_slot_ = Ring::kNoSlot;
    if (client_ < 0) {
        return {};
    }
    ring_.observe_held(page_.held());
    const u32 slot = ring_.pick(~0ULL);
    if (slot == Ring::kNoSlot) {
        statistics_.dropped_full_ring += 1;
        return {};
    }
    page_.set_writing(true, slot);
    if (claim_names(page_.held(), slot)) {
        page_.set_writing(false, slot);
        statistics_.vetoed += 1;
        return {};
    }
    reserved_slot_ = slot;
    reserved_in_flight_ = static_cast<u32>(frame_id_ % kInFlight);
    return FrameStaging{static_cast<u8*>(staging_[reserved_in_flight_].contents), width_, height_,
                        width_ * kBytesPerPixel, slot};
}

Expected<u64, Error> Publisher::Impl::publish() noexcept {
    if (reserved_slot_ == Ring::kNoSlot) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "publish called without a reserved viewport image"});
    }
    const u64 submitted = monotonic_nanos();
    @autoreleasepool {
        id<MTLCommandBuffer> commands = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (commands == nil || blit == nil) {
            page_.set_writing(false, reserved_slot_);
            reserved_slot_ = Ring::kNoSlot;
            return make_unexpected(
                Error{ErrorCode::Unavailable, "creating the viewport Metal command buffer"});
        }
        [blit copyFromBuffer:staging_[reserved_in_flight_]
                sourceOffset:0
           sourceBytesPerRow:width_ * kBytesPerPixel
         sourceBytesPerImage:static_cast<NSUInteger>(width_) * height_ * kBytesPerPixel
                  sourceSize:MTLSizeMake(width_, height_, 1)
                   toTexture:images_[reserved_slot_].texture
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError) {
            page_.set_writing(false, reserved_slot_);
            reserved_slot_ = Ring::kNoSlot;
            return make_unexpected(
                Error{ErrorCode::Unavailable, "the viewport Metal upload command failed"});
        }
    }

    const u64 published = ++frame_id_;
    ring_.record_published(reserved_slot_, published);
    page_.publish(Announcement{published, reserved_slot_, ring_.generation(), 0, submitted});
    page_.set_writing(false, reserved_slot_);
    reserved_slot_ = Ring::kNoSlot;
    statistics_.published += 1;
    statistics_.upload_micros += (monotonic_nanos() - submitted) / 1'000ULL;
    return published;
}

void Publisher::Impl::teardown() noexcept {
    if (client_ >= 0) {
        (void)::close(client_);
        client_ = -1;
    }
    if (listener_ >= 0) {
        (void)::close(listener_);
        listener_ = -1;
    }
    if (socket_path_[0] != '\0') {
        (void)::unlink(socket_path_);
        socket_path_[0] = '\0';
    }
    @autoreleasepool {
        for (u32 index = 0; index < kInFlight; ++index) {
            staging_[index] = nil;
        }
        for (SharedImage& image : images_) {
            image.texture = nil;
            if (image.surface != nullptr) {
                CFRelease(image.surface);
                image.surface = nullptr;
            }
        }
        queue_ = nil;
        device_ = nil;
    }
}

const char* ring_advisory(u32 buffers) noexcept {
    switch (buffers) {
        case 1:
            return "a one-image ring wedges: the engine and the editor want the same image at the "
                   "same time";
        case 2:
            return "a two-image ring throttles the engine to the editor's refresh rate and costs a "
                   "whole editor frame of latency";
        default:
            return nullptr;
    }
}

Publisher::~Publisher() {
    if (impl_ != nullptr) {
        Allocator& allocator = system_allocator(MemoryDomain::Renderer);
        impl_->~Impl();
        allocator.deallocate(impl_, sizeof(Impl), alignof(Impl));
        impl_ = nullptr;
    }
}

void Publisher::service() noexcept { impl_->service(); }
bool Publisher::has_client() const noexcept { return impl_->has_client(); }
FrameStaging Publisher::begin_frame() noexcept { return impl_->begin_frame(); }
Expected<u64, Error> Publisher::publish() noexcept { return impl_->publish(); }
const char* Publisher::adapter_name() const noexcept { return impl_->adapter_name(); }
u64 Publisher::modifier() const noexcept { return impl_->modifier(); }
const PublisherStatistics& Publisher::statistics() const noexcept { return impl_->statistics(); }

Expected<UniquePtr<Publisher>, Error> Publisher::create(const PublisherOptions& options) noexcept {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    void* storage = allocator.allocate(sizeof(Impl), alignof(Impl));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the viewport publisher's implementation");
    }
    Impl* impl = new (storage) Impl{};
    if (Status opened = impl->open(options); !opened) {
        impl->~Impl();
        allocator.deallocate(storage, sizeof(Impl), alignof(Impl));
        return make_unexpected(opened.error());
    }
    Expected<UniquePtr<Publisher>, Error> owner = make_unique<Publisher>(allocator, impl);
    if (!owner) {
        impl->~Impl();
        allocator.deallocate(storage, sizeof(Impl), alignof(Impl));
    }
    return owner;
}

}  // namespace cy::viewport
