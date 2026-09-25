// SPDX-License-Identifier: MIT
#include <cy/vfx/authoring_capabilities.h>

#include <cy/vfx/renderers.h>

#include <string_view>

namespace cy::vfx {
namespace {

Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (usize byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xffU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status put_text(Array<u8>& out, std::string_view value) noexcept {
    if (Status length = put_u32(out, static_cast<u32>(value.size())); !length) {
        return length;
    }
    return out.append({reinterpret_cast<const u8*>(value.data()), value.size()});
}

Status put_renderer(Array<u8>& out, RendererKind kind) noexcept {
    if (Status value = out.push_back(static_cast<u8>(kind)); !value) {
        return value;
    }
    if (Status value = put_text(out, renderer_kind_name(kind)); !value) {
        return value;
    }
    const RendererAvailability availability = renderer_availability(kind);
    if (Status value = out.push_back(availability.available ? 1U : 0U); !value) {
        return value;
    }
    return put_text(out, availability.reason);
}

Status put_target(Array<u8>& out, SimulationPath path,
                  const DeviceCapability* device) noexcept {
    if (Status value = out.push_back(static_cast<u8>(path)); !value) {
        return value;
    }
    if (Status value = put_text(out, path_name(path)); !value) {
        return value;
    }
    const TargetAvailability availability = target_availability(path, device);
    if (Status value = out.push_back(availability.compile_available ? 1U : 0U); !value) {
        return value;
    }
    if (Status value = out.push_back(availability.runtime_available ? 1U : 0U); !value) {
        return value;
    }
    if (Status value = put_text(out, fallback_reason_name(availability.reason)); !value) {
        return value;
    }
    return put_text(out, availability.explanation);
}

}  // namespace

Status encode_authoring_capabilities(Array<u8>& out,
                                     const DeviceCapability* device) noexcept {
    out.clear();
    if (Status value = put_u32(out, 1); !value) {
        return value;
    }
    if (Status value = put_u32(out, kRendererKindCount); !value) {
        return value;
    }
    for (u32 index = 0; index < kRendererKindCount; ++index) {
        if (Status value = put_renderer(out, static_cast<RendererKind>(index)); !value) {
            return value;
        }
    }
    if (Status value = put_u32(out, 2); !value) {
        return value;
    }
    for (SimulationPath path : {SimulationPath::GpuPreferred, SimulationPath::CpuRequired}) {
        if (Status value = put_target(out, path, device); !value) {
            return value;
        }
    }
    return ok();
}

}  // namespace cy::vfx
