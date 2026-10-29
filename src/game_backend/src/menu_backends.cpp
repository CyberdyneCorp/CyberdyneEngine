// SPDX-License-Identifier: MIT
// The display settings and the player's files behind ABI 1.17. See cy/game_backend/menu_backends.h.

#include <cy/abi/errors.h>
#include <cy/core/assets/file.h>
#include <cy/game_backend/menu_backends.h>

#include <cstring>

namespace cy::game_backend {
namespace {

[[nodiscard]] bool same(const CyVideoSettings& a, const CyVideoSettings& b) noexcept {
    return a.width == b.width && a.height == b.height && a.quality == b.quality &&
           a.upscale == b.upscale && a.flags == b.flags;
}

[[nodiscard]] bool offered(Span<const CyVideoMode> modes, u32 width, u32 height) noexcept {
    for (const CyVideoMode& mode : modes) {
        if (mode.width == width && mode.height == height) {
            return true;
        }
    }
    return false;
}

}  // namespace

Status VideoSettingsAdapter::start(Span<const CyVideoMode> modes,
                                   const CyVideoSettings& initial) noexcept {
    if (modes.empty() || !offered(modes, initial.width, initial.height)) {
        return fail(ErrorCode::InvalidArgument,
                    "video settings: the initial resolution is not one of the modes offered");
    }
    modes_.clear();
    if (Status kept = modes_.append(modes); !kept) {
        return kept;
    }
    current_ = initial;
    current_.struct_size = sizeof(CyVideoSettings);
    return ok();
}

CyResult VideoSettingsAdapter::settings(CyVideoSettings& out) const noexcept {
    out = current_;
    return CY_RESULT_OK;
}

CyResult VideoSettingsAdapter::apply(const CyVideoSettings& settings) noexcept {
    if (!offered(modes_.span(), settings.width, settings.height)) {
        return cy::abi::report(CY_RESULT_NOT_FOUND,
                               "video_set_settings: the display does not offer that resolution");
    }
    if (!same(settings, current_)) {
        current_ = settings;
        current_.struct_size = sizeof(CyVideoSettings);
        ++revision_;
    }
    return CY_RESULT_OK;
}

CyResult VideoSettingsAdapter::modes(CyVideoMode* out, u32 capacity, u32& count) const noexcept {
    count = static_cast<u32>(modes_.size());
    if (out != nullptr) {
        const u32 fits = capacity < count ? capacity : count;
        if (fits > 0U) {
            std::memcpy(out, modes_.data(), sizeof(CyVideoMode) * fits);
        }
    }
    return CY_RESULT_OK;
}

Status UserDataDirectory::start(std::string_view root) noexcept {
    root_.clear();
    if (Status kept = root_.append(Span<const char>(root.data(), root.size())); !kept) {
        return kept;
    }
    if (Status ended = root_.push_back('\0'); !ended) {
        return ended;
    }
    return assets::fs::exists(root_.data()) ? ok() : assets::fs::create_directories(root_.data());
}

bool UserDataDirectory::path_of(const char* name, Array<char>& out) const noexcept {
    out.clear();
    if (root_.empty()) {
        return false;
    }
    const usize root = root_.size() - 1U;
    return out.append(Span<const char>(root_.data(), root)).has_value() &&
           out.push_back('/').has_value() &&
           out.append(Span<const char>(name, std::strlen(name) + 1U)).has_value();
}

CyResult UserDataDirectory::write(const char* name, const void* bytes, u64 size) noexcept {
    Array<char> path(root_.allocator());
    if (!path_of(name, path)) {
        return cy::abi::report(CY_RESULT_UNAVAILABLE, "user_data_write: no directory was started");
    }
    if (Status written = assets::fs::write_atomic(path.data(), bytes, static_cast<usize>(size));
        !written) {
        return cy::abi::report(CY_RESULT_IO, written.error().message);
    }
    return CY_RESULT_OK;
}

CyResult UserDataDirectory::read(const char* name, void* buffer, u64 capacity, u64& size) noexcept {
    Array<char> path(root_.allocator());
    if (!path_of(name, path)) {
        return cy::abi::report(CY_RESULT_UNAVAILABLE, "user_data_read: no directory was started");
    }
    Expected<assets::File, Error> file = assets::File::open(path.data(), assets::FileMode::Read);
    if (!file) {
        return cy::abi::report(CY_RESULT_NOT_FOUND, "user_data_read: there is no such file");
    }
    Expected<u64, Error> length = file->size();
    if (!length) {
        return cy::abi::report(CY_RESULT_IO, length.error().message);
    }
    size = *length;
    if (buffer == nullptr || capacity == 0U) {
        return CY_RESULT_OK;
    }
    const u64 wanted = capacity < size ? capacity : size;
    Expected<usize, Error> read = file->read(buffer, static_cast<usize>(wanted));
    if (!read || *read != wanted) {
        return cy::abi::report(CY_RESULT_IO, "user_data_read: the file could not be read whole");
    }
    return CY_RESULT_OK;
}

void bind(cy::abi::Host& host, VideoSettingsAdapter* video, UserDataDirectory* files) noexcept {
    host.game.video = video;
    host.game.user_data = files;
}

}  // namespace cy::game_backend
