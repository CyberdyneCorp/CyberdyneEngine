// SPDX-License-Identifier: MIT
// Native Direct3D 12 device. M11.d.5.

#include "d3d12_internal.h"

#include <cy/backends/rhi-d3d12/backend.h>
#include <cy/backends/rhi/pipeline_cache_file.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/pressure.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace cy::rhi::d3d12 {
namespace {

constexpr u32 kResourceDescriptorCapacity = 65'536;
constexpr u32 kSamplerDescriptorCapacity = 2'048;
constexpr u32 kAttachmentDescriptorCapacity = 4'096;
constexpr u32 kBindlessCapacity = 16'384;
constexpr u64 kTier2Pool = ~0ULL;
constexpr u64 kTier1Buffers = 1ULL << 0U;
constexpr u64 kTier1Textures = 1ULL << 1U;
constexpr u64 kTier1RenderTargets = 1ULL << 2U;

[[nodiscard]] D3D12_RENDER_TARGET_BLEND_DESC default_blend_attachment() noexcept {
    D3D12_RENDER_TARGET_BLEND_DESC state{};
    state.BlendEnable = FALSE;
    state.LogicOpEnable = FALSE;
    state.SrcBlend = D3D12_BLEND_ONE;
    state.DestBlend = D3D12_BLEND_ZERO;
    state.BlendOp = D3D12_BLEND_OP_ADD;
    state.SrcBlendAlpha = D3D12_BLEND_ONE;
    state.DestBlendAlpha = D3D12_BLEND_ZERO;
    state.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    state.LogicOp = D3D12_LOGIC_OP_NOOP;
    state.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return state;
}

[[nodiscard]] D3D12_DEPTH_STENCILOP_DESC default_stencil_face() noexcept {
    D3D12_DEPTH_STENCILOP_DESC state{};
    state.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    state.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    state.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    state.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    return state;
}

[[nodiscard]] u64 align_to(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1U) / alignment * alignment;
}

[[nodiscard]] u32 clamp_frames(u32 requested) noexcept {
    if (requested == 0) {
        return kDefaultFramesInFlight;
    }
    return std::min(requested, kMaxFramesInFlight);
}

[[nodiscard]] DXGI_FORMAT dxgi_format(Format format) noexcept {
    switch (format) {
        case Format::R8Unorm:
            return DXGI_FORMAT_R8_UNORM;
        case Format::R8Uint:
            return DXGI_FORMAT_R8_UINT;
        case Format::Rg8Unorm:
            return DXGI_FORMAT_R8G8_UNORM;
        case Format::Rgba8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::Rgba8Srgb:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::Bgra8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::Bgra8Srgb:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case Format::R16Uint:
            return DXGI_FORMAT_R16_UINT;
        case Format::R16Sfloat:
            return DXGI_FORMAT_R16_FLOAT;
        case Format::Rg16Sfloat:
            return DXGI_FORMAT_R16G16_FLOAT;
        case Format::Rgba16Sfloat:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32Uint:
            return DXGI_FORMAT_R32_UINT;
        case Format::R32Sint:
            return DXGI_FORMAT_R32_SINT;
        case Format::R32Sfloat:
            return DXGI_FORMAT_R32_FLOAT;
        case Format::Rg32Sfloat:
            return DXGI_FORMAT_R32G32_FLOAT;
        case Format::Rgb32Sfloat:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::Rgba32Sfloat:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::Rgb10A2Unorm:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::B10G11R11Ufloat:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::D16Unorm:
            return DXGI_FORMAT_D16_UNORM;
        case Format::D32Sfloat:
            return DXGI_FORMAT_D32_FLOAT;
        case Format::D24UnormS8Uint:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32SfloatS8Uint:
            return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case Format::Bc1RgbaUnorm:
            return DXGI_FORMAT_BC1_UNORM;
        case Format::Bc1RgbaSrgb:
            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::Bc3Unorm:
            return DXGI_FORMAT_BC3_UNORM;
        case Format::Bc3Srgb:
            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case Format::Bc4Unorm:
            return DXGI_FORMAT_BC4_UNORM;
        case Format::Bc5Unorm:
            return DXGI_FORMAT_BC5_UNORM;
        case Format::Bc6HUfloat:
            return DXGI_FORMAT_BC6H_UF16;
        case Format::Bc7Unorm:
            return DXGI_FORMAT_BC7_UNORM;
        case Format::Bc7Srgb:
            return DXGI_FORMAT_BC7_UNORM_SRGB;
        case Format::Undefined:
        case Format::Count:
            break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] DXGI_FORMAT resource_format(const TextureDescription& desc) noexcept {
    if (!has_usage(desc.usage, TextureUsage::DepthStencilAttachment) ||
        !has_usage(desc.usage, TextureUsage::Sampled)) {
        return dxgi_format(desc.format);
    }
    switch (desc.format) {
        case Format::D16Unorm:
            return DXGI_FORMAT_R16_TYPELESS;
        case Format::D24UnormS8Uint:
            return DXGI_FORMAT_R24G8_TYPELESS;
        case Format::D32Sfloat:
            return DXGI_FORMAT_R32_TYPELESS;
        case Format::D32SfloatS8Uint:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        default:
            return dxgi_format(desc.format);
    }
}

[[nodiscard]] DXGI_FORMAT shader_resource_format(Format format) noexcept {
    switch (format) {
        case Format::D16Unorm:
            return DXGI_FORMAT_R16_UNORM;
        case Format::D24UnormS8Uint:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case Format::D32Sfloat:
            return DXGI_FORMAT_R32_FLOAT;
        case Format::D32SfloatS8Uint:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:
            return dxgi_format(format);
    }
}

[[nodiscard]] D3D12_RESOURCE_DIMENSION resource_dimension(TextureDimension dimension) noexcept {
    switch (dimension) {
        case TextureDimension::Texture1D:
            return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        case TextureDimension::Texture2D:
        case TextureDimension::Cube:
            return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        case TextureDimension::Texture3D:
            return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    }
    return D3D12_RESOURCE_DIMENSION_UNKNOWN;
}

[[nodiscard]] D3D12_RESOURCE_FLAGS texture_flags(const TextureDescription& desc) noexcept {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (has_usage(desc.usage, TextureUsage::ColorAttachment)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (has_usage(desc.usage, TextureUsage::DepthStencilAttachment)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!has_usage(desc.usage, TextureUsage::Sampled)) {
            flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    }
    if (has_usage(desc.usage, TextureUsage::Storage)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    return flags;
}

[[nodiscard]] D3D12_RESOURCE_DESC native_texture_desc(const TextureDescription& desc) noexcept {
    D3D12_RESOURCE_DESC native{};
    native.Dimension = resource_dimension(desc.dimension);
    native.Alignment = 0;
    native.Width = desc.extent.width;
    native.Height = desc.extent.height;
    native.DepthOrArraySize = desc.dimension == TextureDimension::Texture3D
                                  ? static_cast<UINT16>(desc.extent.depth)
                                  : static_cast<UINT16>(desc.array_layers);
    native.MipLevels = desc.mip_levels;
    native.Format = resource_format(desc);
    native.SampleDesc.Count = desc.sample_count;
    native.SampleDesc.Quality = 0;
    native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    native.Flags = texture_flags(desc);
    return native;
}

[[nodiscard]] D3D12_RESOURCE_DESC native_buffer_desc(u64 size) noexcept {
    D3D12_RESOURCE_DESC native{};
    native.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    native.Width = size;
    native.Height = 1;
    native.DepthOrArraySize = 1;
    native.MipLevels = 1;
    native.Format = DXGI_FORMAT_UNKNOWN;
    native.SampleDesc.Count = 1;
    native.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    native.Flags = D3D12_RESOURCE_FLAG_NONE;
    return native;
}

[[nodiscard]] D3D12_HEAP_TYPE heap_type(MemoryUse memory) noexcept {
    switch (memory) {
        case MemoryUse::Upload:
            return D3D12_HEAP_TYPE_UPLOAD;
        case MemoryUse::Readback:
            return D3D12_HEAP_TYPE_READBACK;
        case MemoryUse::DeviceLocal:
        case MemoryUse::HostVisibleDeviceLocal:
            return D3D12_HEAP_TYPE_DEFAULT;
    }
    return D3D12_HEAP_TYPE_DEFAULT;
}

[[nodiscard]] D3D12_RESOURCE_STATES initial_buffer_state(MemoryUse memory) noexcept {
    if (memory == MemoryUse::Upload) {
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    if (memory == MemoryUse::Readback) {
        return D3D12_RESOURCE_STATE_COPY_DEST;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

[[nodiscard]] GpuMemoryCategory memory_category(MemoryUse memory) noexcept {
    switch (memory) {
        case MemoryUse::Upload:
            return GpuMemoryCategory::Upload;
        case MemoryUse::Readback:
            return GpuMemoryCategory::Readback;
        case MemoryUse::DeviceLocal:
        case MemoryUse::HostVisibleDeviceLocal:
            return GpuMemoryCategory::Persistent;
    }
    return GpuMemoryCategory::Persistent;
}

[[nodiscard]] D3D12_COMMAND_LIST_TYPE command_type(QueueKind queue) noexcept {
    switch (queue) {
        case QueueKind::Graphics:
            return D3D12_COMMAND_LIST_TYPE_DIRECT;
        case QueueKind::AsyncCompute:
            return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        case QueueKind::Transfer:
            return D3D12_COMMAND_LIST_TYPE_COPY;
        case QueueKind::Count:
            break;
    }
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

[[nodiscard]] bool contains_ci(const char* value, const char* fragment) noexcept {
    if (value == nullptr || fragment == nullptr) {
        return false;
    }
    const usize value_size = std::strlen(value);
    const usize fragment_size = std::strlen(fragment);
    if (fragment_size > value_size) {
        return false;
    }
    for (usize at = 0; at + fragment_size <= value_size; ++at) {
        bool same = true;
        for (usize index = 0; index < fragment_size; ++index) {
            char a = value[at + index];
            char b = fragment[index];
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            same = same && a == b;
        }
        if (same) {
            return true;
        }
    }
    return false;
}

void adapter_name(const DXGI_ADAPTER_DESC1& desc, char (&out)[128]) noexcept {
    const int written = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, out,
                                            static_cast<int>(sizeof(out)), nullptr, nullptr);
    if (written <= 0) {
        out[0] = '\0';
    }
}

[[nodiscard]] ComPtr<IDXGIAdapter1> choose_adapter(IDXGIFactory6* factory,
                                                   AdapterIdentity& identity) noexcept {
    ComPtr<IDXGIAdapter1> unknown;
    AdapterIdentity unknown_identity{};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc))) {
            continue;
        }
        AdapterIdentity observed{};
        adapter_name(desc, observed.name);
        observed.vendor_id = desc.VendorId;
        observed.device_id = desc.DeviceId;
        observed.dxgi_software_flag = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        observed.classification = classify_adapter(observed.name, observed.vendor_id);
        if (observed.classification == AdapterClass::Hardware &&
            SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            identity = observed;
            return candidate;
        }
        if (!unknown && observed.classification == AdapterClass::Unknown &&
            SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            unknown = candidate;
            unknown_identity = observed;
        }
    }
    if (unknown) {
        identity = unknown_identity;
        return unknown;
    }
    ComPtr<IDXGIAdapter> warp;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        return {};
    }
    ComPtr<IDXGIAdapter1> warp1;
    if (FAILED(warp.As(&warp1))) {
        return {};
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(warp1->GetDesc1(&desc))) {
        return {};
    }
    adapter_name(desc, identity.name);
    identity.vendor_id = desc.VendorId;
    identity.device_id = desc.DeviceId;
    identity.dxgi_software_flag = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    identity.classification = classify_adapter(identity.name, identity.vendor_id);
    return warp1;
}

[[nodiscard]] D3D12_HEAP_FLAGS pool_flags(MemoryPoolClass pool) noexcept {
    if (pool.token == kTier2Pool) {
        return D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    }
    if ((pool.token & kTier1Buffers) != 0) {
        return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    }
    if ((pool.token & kTier1RenderTargets) != 0) {
        return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    }
    return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
}

[[nodiscard]] D3D12_BLEND blend_factor(BlendFactor factor) noexcept {
    switch (factor) {
        case BlendFactor::Zero:
            return D3D12_BLEND_ZERO;
        case BlendFactor::One:
            return D3D12_BLEND_ONE;
        case BlendFactor::SourceColor:
            return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSourceColor:
            return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::DestinationColor:
            return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDestinationColor:
            return D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::SourceAlpha:
            return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSourceAlpha:
            return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DestinationAlpha:
            return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDestinationAlpha:
            return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

[[nodiscard]] D3D12_BLEND_OP blend_op(BlendOp operation) noexcept {
    switch (operation) {
        case BlendOp::Add:
            return D3D12_BLEND_OP_ADD;
        case BlendOp::Subtract:
            return D3D12_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract:
            return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOp::Min:
            return D3D12_BLEND_OP_MIN;
        case BlendOp::Max:
            return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

[[nodiscard]] D3D12_COMPARISON_FUNC compare_op(CompareOp operation) noexcept {
    return static_cast<D3D12_COMPARISON_FUNC>(static_cast<u32>(operation) + 1U);
}

[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(PrimitiveTopology topology) noexcept {
    switch (topology) {
        case PrimitiveTopology::PointList:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case PrimitiveTopology::LineList:
        case PrimitiveTopology::LineStrip:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::TriangleStrip:
            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY primitive_topology(PrimitiveTopology topology) noexcept {
    switch (topology) {
        case PrimitiveTopology::PointList:
            return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case PrimitiveTopology::LineList:
            return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveTopology::LineStrip:
            return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrimitiveTopology::TriangleList:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveTopology::TriangleStrip:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }
    return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

[[nodiscard]] D3D12_FILTER sampler_filter(const SamplerDescription& desc) noexcept {
    if (desc.compare_enable) {
        return desc.min_filter == Filter::Linear || desc.mag_filter == Filter::Linear
                   ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                   : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    }
    if (desc.max_anisotropy > 1.0F) {
        return D3D12_FILTER_ANISOTROPIC;
    }
    return desc.min_filter == Filter::Linear || desc.mag_filter == Filter::Linear
               ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
               : D3D12_FILTER_MIN_MAG_MIP_POINT;
}

[[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE address_mode(AddressMode mode) noexcept {
    switch (mode) {
        case AddressMode::Repeat:
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::MirroredRepeat:
            return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case AddressMode::ClampToEdge:
            return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case AddressMode::ClampToBorder:
            return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

}  // namespace

void StoredName::assign(const char* source) noexcept {
    std::snprintf(text, sizeof(text), "%s", source != nullptr ? source : "");
}

AdapterIdentity query_d3d12_identity() noexcept {
    AdapterIdentity identity{};
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return identity;
    }
    ComPtr<IDXGIAdapter1> adapter = choose_adapter(factory.Get(), identity);
    ComPtr<ID3D12Device> device;
    if (adapter && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                               IID_PPV_ARGS(&device)))) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                  sizeof(options)))) {
            identity.resource_heap_tier = static_cast<u32>(options.ResourceHeapTier);
        }
    }
    return identity;
}

bool d3d12_device_present() noexcept {
    AdapterIdentity identity{};
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    const ComPtr<IDXGIAdapter1> adapter = choose_adapter(factory.Get(), identity);
    return adapter && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                                  __uuidof(ID3D12Device), nullptr));
}

D3D12Device::D3D12Device(Allocator& allocator, const DeviceDescription& desc) noexcept
    : allocator_(&allocator),
      description_(desc),
      frames_in_flight_(clamp_frames(desc.frames_in_flight)),
      transient_textures_(allocator),
      transient_buffers_(allocator),
      live_commands_(allocator),
      buffers_(MemoryDomain::Gpu, "rhi.d3d12.buffers"),
      textures_(MemoryDomain::Gpu, "rhi.d3d12.textures"),
      views_(MemoryDomain::Gpu, "rhi.d3d12.views"),
      samplers_(MemoryDomain::Gpu, "rhi.d3d12.samplers"),
      shaders_(MemoryDomain::Gpu, "rhi.d3d12.shaders"),
      set_layouts_(MemoryDomain::Gpu, "rhi.d3d12.set-layouts"),
      pipeline_layouts_(MemoryDomain::Gpu, "rhi.d3d12.pipeline-layouts"),
      descriptor_sets_(MemoryDomain::Gpu, "rhi.d3d12.descriptor-sets"),
      graphics_pipelines_(MemoryDomain::Gpu, "rhi.d3d12.graphics-pipelines"),
      compute_pipelines_(MemoryDomain::Gpu, "rhi.d3d12.compute-pipelines"),
      query_pools_(MemoryDomain::Gpu, "rhi.d3d12.queries"),
      fences_(MemoryDomain::Gpu, "rhi.d3d12.fences"),
      semaphores_(MemoryDomain::Gpu, "rhi.d3d12.semaphores"),
      command_buffers_(MemoryDomain::Gpu, "rhi.d3d12.command-buffers"),
      swapchains_(MemoryDomain::Gpu, "rhi.d3d12.swapchains"),
      bindless_free_(allocator),
      barriers_(this) {}

D3D12Device::~D3D12Device() {
    (void)wait_idle();
    for (HANDLE& event : timeline_events_) {
        if (event != nullptr) {
            CloseHandle(event);
        }
        event = nullptr;
    }
}

Status D3D12Device::initialize() noexcept {
    UINT factory_flags = 0;
    if (description_.enable_validation) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
            if (description_.enable_synchronisation_validation) {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                }
            }
        }
    }
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)))) {
        return fail(ErrorCode::Unavailable, "D3D12 could not create a DXGI factory");
    }
    AdapterIdentity identity{};
    adapter_ = choose_adapter(factory_.Get(), identity);
    if (!adapter_) {
        return fail(ErrorCode::Unavailable, "D3D12 found no usable adapter or WARP");
    }
    if (FAILED(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
        return fail(ErrorCode::Unavailable, "D3D12CreateDevice failed at feature level 12_0");
    }
    if (description_.enable_validation && SUCCEEDED(device_.As(&info_queue_))) {
        info_queue_->ClearStoredMessages();
    }

    configure_capabilities(identity);
    probe_format_features();
    if (Status status = create_command_signatures(); !status) {
        return status;
    }
    if (Status status = create_queues(); !status) {
        return status;
    }
    if (Status status = create_descriptor_heaps(); !status) {
        return status;
    }
    return create_bindless_table();
}

Status D3D12Device::create_command_signatures() noexcept {
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;

    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    signature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    if (FAILED(device_->CreateCommandSignature(&signature, nullptr,
                                               IID_PPV_ARGS(&draw_indexed_signature_)))) {
        return fail(ErrorCode::Unavailable, "D3D12 could not create the indexed-draw signature");
    }

    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    if (FAILED(device_->CreateCommandSignature(&signature, nullptr,
                                               IID_PPV_ARGS(&dispatch_signature_)))) {
        return fail(ErrorCode::Unavailable, "D3D12 could not create the dispatch signature");
    }
    return ok();
}

void D3D12Device::configure_capabilities(const AdapterIdentity& identity) noexcept {
    capabilities_.set_backend(BackendKind::D3D12);
    capabilities_.set_native_shader_format(ShaderFormat::Dxil);
    capabilities_.set_device_name(identity.name);
    capabilities_.set_vendor_id(identity.vendor_id);
    capabilities_.set_driver_version("DXGI");
    capabilities_.set_needs_queue_ownership_transfer(false);
    capabilities_.set(Capability::ComputeShaders, true);
    capabilities_.set(Capability::DynamicRendering, true);
    capabilities_.set(Capability::ParallelPassRecording, false);
    capabilities_.set(Capability::AsyncCompute, description_.request_async_compute);
    capabilities_.set(Capability::DedicatedTransferQueue, description_.request_transfer_queue);
    capabilities_.set(Capability::TimestampQueries, true);
    capabilities_.set(Capability::DebugMarkers, true);
    capabilities_.set(Capability::HostVisibleDeviceLocalMemory, false);

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(
            device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
        heap_tier_ = options.ResourceHeapTier;
        const bool tier3 = options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
        descriptor_model_ = tier3 ? DescriptorModel::Bindless : DescriptorModel::Compatibility;
        capabilities_.set(Capability::Bindless, tier3);
        capabilities_.set(Capability::BindlessPartiallyBound, tier3);
        capabilities_.set(Capability::DescriptorIndexingNonUniform, tier3);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1,
                                               sizeof(options1)))) {
        capabilities_.set(Capability::ShaderInt64Atomics, options1.Int64ShaderOps != FALSE);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4,
                                               sizeof(options4)))) {
        capabilities_.set(Capability::ShaderFloat16,
                          options4.Native16BitShaderOpsSupported != FALSE);
    }

    DeviceLimits& limits = capabilities_.limits();
    limits.max_bound_descriptor_sets = kMaxDescriptorSets;
    limits.max_push_constant_bytes = kMaxPushConstantBytes;
    limits.max_vertex_attributes = kMaxVertexAttributes;
    limits.max_color_attachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
    limits.max_texture_dimension_2d = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    limits.max_texture_array_layers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    limits.max_compute_workgroup_size[0] = D3D12_CS_THREAD_GROUP_MAX_X;
    limits.max_compute_workgroup_size[1] = D3D12_CS_THREAD_GROUP_MAX_Y;
    limits.max_compute_workgroup_size[2] = D3D12_CS_THREAD_GROUP_MAX_Z;
    limits.max_compute_workgroup_invocations = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
    limits.subgroup_size = 32;
    limits.max_sampled_images_per_stage = kResourceDescriptorCapacity;
    limits.max_storage_buffers_per_stage = kResourceDescriptorCapacity;
    limits.min_uniform_buffer_offset_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    limits.min_storage_buffer_offset_alignment = 16;
    limits.optimal_buffer_copy_offset_alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    limits.non_coherent_atom_size = 1;
    limits.max_sampler_anisotropy = D3D12_MAX_MAXANISOTROPY;
}

void D3D12Device::probe_format_features() noexcept {
    for (u32 index = 1; index < static_cast<u32>(Format::Count); ++index) {
        const Format format = static_cast<Format>(index);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{dxgi_format(format)};
        FormatFeature features = FormatFeature::None;
        if (support.Format != DXGI_FORMAT_UNKNOWN &&
            SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                   sizeof(support)))) {
            if ((support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0) {
                features = features | FormatFeature::SampledImage;
            }
            if ((support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0) {
                features = features | FormatFeature::ColorAttachment;
            }
            if ((support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0) {
                features = features | FormatFeature::ColorAttachmentBlend;
            }
            if ((support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0) {
                features = features | FormatFeature::DepthStencilAttachment;
            }
            if ((support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0) {
                features = features | FormatFeature::StorageImage;
            }
        }
        capabilities_.set_format_features(format, features);
    }
}

Status D3D12Device::create_queues() noexcept {
    for (u32 index = 0; index < kQueueKindCount; ++index) {
        const QueueKind queue = static_cast<QueueKind>(index);
        if (!has_queue(queue)) {
            continue;
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = command_type(queue);
        if (FAILED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queues_[index])))) {
            return fail(ErrorCode::Unavailable, "D3D12 could not create a command queue");
        }
        if (FAILED(
                device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timelines_[index])))) {
            return fail(ErrorCode::Unavailable, "D3D12 could not create a queue timeline");
        }
        timeline_events_[index] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (timeline_events_[index] == nullptr) {
            return fail(ErrorCode::Unavailable, "D3D12 could not create a timeline event");
        }
    }
    return ok();
}

Status D3D12Device::create_descriptor_heaps() noexcept {
    struct HeapRequest {
        D3D12_DESCRIPTOR_HEAP_TYPE type;
        u32 count;
        D3D12_DESCRIPTOR_HEAP_FLAGS flags;
        ComPtr<ID3D12DescriptorHeap>* heap;
        u32* stride;
    } requests[] = {
        {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kResourceDescriptorCapacity,
         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &resource_heap_, &resource_stride_},
        {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerDescriptorCapacity,
         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &sampler_heap_, &sampler_stride_},
        {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kAttachmentDescriptorCapacity,
         D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &rtv_heap_, &rtv_stride_},
        {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kAttachmentDescriptorCapacity,
         D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &dsv_heap_, &dsv_stride_},
    };
    for (HeapRequest& request : requests) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = request.type;
        desc.NumDescriptors = request.count;
        desc.Flags = request.flags;
        if (FAILED(
                device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(request.heap->GetAddressOf())))) {
            return fail(ErrorCode::OutOfMemory, "D3D12 could not create a descriptor heap");
        }
        *request.stride = device_->GetDescriptorHandleIncrementSize(request.type);
    }
    return ok();
}

bool D3D12Device::has_queue(QueueKind queue) const noexcept {
    switch (queue) {
        case QueueKind::Graphics:
            return true;
        case QueueKind::AsyncCompute:
            return description_.request_async_compute;
        case QueueKind::Transfer:
            return description_.request_transfer_queue;
        case QueueKind::Count:
            break;
    }
    return false;
}

void D3D12Device::set_validation_callback(ValidationCallback callback, void* user) noexcept {
    validation_callback_ = callback;
    validation_user_ = user;
}

void D3D12Device::report_validation(ValidationSeverity severity, const char* message) noexcept {
    if (severity == ValidationSeverity::Error) {
        ++stats_.validation_errors;
    }
    if (severity == ValidationSeverity::Warning) {
        ++stats_.validation_warnings;
    }
    if (validation_callback_ != nullptr) {
        validation_callback_(severity, message, validation_user_);
    }
}

bool D3D12Device::drain_validation_messages() noexcept {
    if (!info_queue_) {
        return false;
    }
    bool found_error = false;
    const u64 count = info_queue_->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (u64 index = 0; index < count; ++index) {
        SIZE_T bytes = 0;
        if (FAILED(info_queue_->GetMessage(index, nullptr, &bytes)) || bytes == 0) {
            continue;
        }
        Array<u8> storage(*allocator_);
        if (Status sized = storage.resize(bytes); !sized) {
            report_validation(ValidationSeverity::Error,
                              "D3D12 could not allocate storage for a debug-layer message");
            found_error = true;
            continue;
        }
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(info_queue_->GetMessage(index, message, &bytes))) {
            continue;
        }
        ValidationSeverity severity = ValidationSeverity::Info;
        if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
            severity = ValidationSeverity::Warning;
        } else if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                   message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            severity = ValidationSeverity::Error;
            found_error = true;
        }
        report_validation(severity, message->pDescription);
    }
    info_queue_->ClearStoredMessages();
    return found_error;
}

void D3D12Device::charge(GpuMemoryCategory category, u64 bytes) noexcept {
    const u32 index = static_cast<u32>(category);
    memory_.live_bytes[index] += bytes;
    memory_.peak_bytes[index] = std::max(memory_.peak_bytes[index], memory_.live_bytes[index]);
    memory_.device_heap_used += bytes;
    ++memory_.allocation_count;
}

void D3D12Device::discharge(GpuMemoryCategory category, u64 bytes) noexcept {
    const u32 index = static_cast<u32>(category);
    memory_.live_bytes[index] =
        memory_.live_bytes[index] > bytes ? memory_.live_bytes[index] - bytes : 0;
    memory_.device_heap_used =
        memory_.device_heap_used > bytes ? memory_.device_heap_used - bytes : 0;
}

Expected<BufferHandle, Error> D3D12Device::create_buffer(const BufferDescription& desc) {
    ValidationMessage validation;
    if (Status valid = validate_buffer(desc, validation); !valid) {
        report_validation(ValidationSeverity::Error, validation.text);
        return make_unexpected(valid.error());
    }
    D3D12Buffer buffer;
    buffer.desc = desc;
    buffer.name.assign(desc.name);
    buffer.desc.name = buffer.name.text;
    const D3D12_RESOURCE_DESC native = native_buffer_desc(desc.size);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type(desc.memory);
    const HRESULT created = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &native, initial_buffer_state(desc.memory), nullptr,
        IID_PPV_ARGS(&buffer.resource));
    if (FAILED(created)) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not create a buffer");
    }
    buffer.bytes = desc.size;
    if (desc.memory == MemoryUse::Upload || desc.memory == MemoryUse::Readback) {
        const D3D12_RANGE read_range{
            0, desc.memory == MemoryUse::Readback ? static_cast<SIZE_T>(desc.size) : 0};
        if (FAILED(buffer.resource->Map(0, &read_range, &buffer.mapped))) {
            return fail(ErrorCode::Unavailable, "D3D12 could not map a host-visible buffer");
        }
    }
    Expected<BufferHandle, Error> handle = buffers_.create(std::move(buffer));
    if (handle) {
        charge(memory_category(desc.memory), desc.size);
    }
    return handle;
}

void D3D12Device::destroy_buffer(BufferHandle handle) noexcept {
    D3D12Buffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        report_validation(ValidationSeverity::Error, "destroy_buffer on a stale handle");
        return;
    }
    if (buffer->mapped != nullptr) {
        buffer->resource->Unmap(0, nullptr);
    }
    if (!buffer->transient) {
        discharge(memory_category(buffer->desc.memory), buffer->bytes);
    }
    (void)buffers_.destroy(handle);
    ++stats_.resources_freed;
}

bool D3D12Device::is_valid(BufferHandle handle) const noexcept {
    return buffers_.resolve(handle) != nullptr;
}

void* D3D12Device::buffer_mapped_pointer(BufferHandle handle) noexcept {
    D3D12Buffer* buffer = buffers_.resolve(handle);
    return buffer != nullptr ? buffer->mapped : nullptr;
}

const BufferDescription* D3D12Device::buffer_description(BufferHandle handle) const noexcept {
    const D3D12Buffer* buffer = buffers_.resolve(handle);
    return buffer != nullptr ? &buffer->desc : nullptr;
}

Expected<TextureHandle, Error> D3D12Device::create_texture(const TextureDescription& desc) {
    ValidationMessage validation;
    if (Status valid = validate_texture(desc, capabilities_, validation); !valid) {
        report_validation(ValidationSeverity::Error, validation.text);
        return make_unexpected(valid.error());
    }
    D3D12Texture texture;
    texture.desc = desc;
    texture.name.assign(desc.name);
    texture.desc.name = texture.name.text;
    const D3D12_RESOURCE_DESC native = native_texture_desc(desc);
    const D3D12_RESOURCE_ALLOCATION_INFO allocation =
        device_->GetResourceAllocationInfo(0, 1, &native);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = dxgi_format(desc.format);
    const D3D12_CLEAR_VALUE* optimized = nullptr;
    if (has_usage(desc.usage, TextureUsage::DepthStencilAttachment)) {
        clear.DepthStencil.Depth = 0.0F;
        clear.DepthStencil.Stencil = 0;
        optimized = &clear;
    } else if (has_usage(desc.usage, TextureUsage::ColorAttachment)) {
        optimized = &clear;
    }
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native,
                                                D3D12_RESOURCE_STATE_COMMON, optimized,
                                                IID_PPV_ARGS(&texture.resource)))) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not create a texture");
    }
    texture.bytes = allocation.SizeInBytes;
    Expected<TextureHandle, Error> handle = textures_.create(std::move(texture));
    if (handle) {
        charge(memory_category(desc.memory), allocation.SizeInBytes);
    }
    return handle;
}

void D3D12Device::destroy_texture(TextureHandle handle) noexcept {
    D3D12Texture* texture = textures_.resolve(handle);
    if (texture == nullptr) {
        report_validation(ValidationSeverity::Error, "destroy_texture on a stale handle");
        return;
    }
    if (texture->swapchain_owned) {
        report_validation(ValidationSeverity::Error, "a swapchain owns this D3D12 texture");
        return;
    }
    if (!texture->transient) {
        discharge(memory_category(texture->desc.memory), texture->bytes);
    }
    (void)textures_.destroy(handle);
    ++stats_.resources_freed;
}

bool D3D12Device::is_valid(TextureHandle handle) const noexcept {
    return textures_.resolve(handle) != nullptr;
}

const TextureDescription* D3D12Device::texture_description(TextureHandle handle) const noexcept {
    const D3D12Texture* texture = textures_.resolve(handle);
    return texture != nullptr ? &texture->desc : nullptr;
}

Expected<TextureViewHandle, Error> D3D12Device::create_texture_view(
    const TextureViewDescription& desc) {
    D3D12Texture* texture = textures_.resolve(desc.texture);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 texture view has a stale texture");
    }
    if (!texture->bound || !texture->resource) {
        return fail(ErrorCode::Unavailable,
                    "D3D12 transient texture must be bound before its view");
    }
    D3D12TextureView view;
    view.desc = desc;
    const Format format = desc.format == Format::Undefined ? texture->desc.format : desc.format;
    if (has_usage(texture->desc.usage, TextureUsage::Sampled) ||
        has_usage(texture->desc.usage, TextureUsage::InputAttachment)) {
        if (next_resource_descriptor_ >= kResourceDescriptorCapacity) {
            return fail(ErrorCode::OutOfMemory, "D3D12 resource descriptor heap is full");
        }
        view.srv = resource_heap_->GetCPUDescriptorHandleForHeapStart();
        view.srv.ptr += static_cast<SIZE_T>(next_resource_descriptor_++) * resource_stride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = shader_resource_format(format);
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = texture->desc.dimension == TextureDimension::Texture3D
                                ? D3D12_SRV_DIMENSION_TEXTURE3D
                                : D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = desc.range.base_mip;
        srv.Texture2D.MipLevels = desc.range.mip_count == 0
                                      ? texture->desc.mip_levels - desc.range.base_mip
                                      : desc.range.mip_count;
        srv.Texture2D.ResourceMinLODClamp = 0.0F;
        device_->CreateShaderResourceView(texture->resource.Get(), &srv, view.srv);
        view.has_srv = true;
    }
    if (has_usage(texture->desc.usage, TextureUsage::ColorAttachment)) {
        if (next_rtv_descriptor_ >= kAttachmentDescriptorCapacity) {
            return fail(ErrorCode::OutOfMemory, "D3D12 RTV heap is full");
        }
        view.rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        view.rtv.ptr += static_cast<SIZE_T>(next_rtv_descriptor_++) * rtv_stride_;
        device_->CreateRenderTargetView(texture->resource.Get(), nullptr, view.rtv);
        view.has_rtv = true;
    }
    if (has_usage(texture->desc.usage, TextureUsage::DepthStencilAttachment)) {
        if (next_dsv_descriptor_ >= kAttachmentDescriptorCapacity) {
            return fail(ErrorCode::OutOfMemory, "D3D12 DSV heap is full");
        }
        view.dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        view.dsv.ptr += static_cast<SIZE_T>(next_dsv_descriptor_++) * dsv_stride_;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = dxgi_format(format);
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(texture->resource.Get(), &dsv, view.dsv);
        view.has_dsv = true;
    }
    return views_.create(std::move(view));
}

void D3D12Device::destroy_texture_view(TextureViewHandle handle) noexcept {
    if (views_.resolve(handle) == nullptr) {
        report_validation(ValidationSeverity::Error, "destroy_texture_view on a stale handle");
        return;
    }
    (void)views_.destroy(handle);
    ++stats_.resources_freed;
}

bool D3D12Device::is_valid(TextureViewHandle handle) const noexcept {
    return views_.resolve(handle) != nullptr;
}

Expected<SamplerHandle, Error> D3D12Device::create_sampler(const SamplerDescription& desc) {
    ValidationMessage validation;
    if (Status valid = validate_sampler(desc, capabilities_.limits(), validation); !valid) {
        return make_unexpected(valid.error());
    }
    const u32 slot = allocate_sampler_descriptors(1);
    if (slot == ~0U) {
        return fail(ErrorCode::OutOfMemory, "D3D12 sampler descriptor heap is full");
    }
    D3D12Sampler sampler;
    sampler.desc = desc;
    sampler.descriptor = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
    sampler.descriptor.ptr += static_cast<SIZE_T>(slot) * sampler_stride_;
    D3D12_SAMPLER_DESC native{};
    native.Filter = sampler_filter(desc);
    native.AddressU = address_mode(desc.address_u);
    native.AddressV = address_mode(desc.address_v);
    native.AddressW = address_mode(desc.address_w);
    native.MipLODBias = desc.mip_lod_bias;
    native.MaxAnisotropy = static_cast<UINT>(desc.max_anisotropy);
    native.ComparisonFunc = compare_op(desc.compare_op);
    native.MinLOD = desc.min_lod;
    native.MaxLOD = desc.max_lod;
    device_->CreateSampler(&native, sampler.descriptor);
    return samplers_.create(std::move(sampler));
}

void D3D12Device::destroy_sampler(SamplerHandle handle) noexcept {
    if (samplers_.resolve(handle) == nullptr) {
        return;
    }
    (void)samplers_.destroy(handle);
    ++stats_.resources_freed;
}

Expected<TextureHandle, Error> D3D12Device::create_transient_texture(
    const TextureDescription& desc) {
    ValidationMessage validation;
    if (Status valid = validate_texture(desc, capabilities_, validation); !valid) {
        return make_unexpected(valid.error());
    }
    D3D12Texture texture;
    texture.desc = desc;
    texture.name.assign(desc.name);
    texture.desc.name = texture.name.text;
    texture.transient = true;
    texture.bound = false;
    Expected<TextureHandle, Error> handle = textures_.create(std::move(texture));
    if (handle && !transient_textures_.push_back(*handle)) {
        (void)textures_.destroy(*handle);
        return fail(ErrorCode::OutOfMemory, "D3D12 could not remember a transient texture");
    }
    return handle;
}

Expected<BufferHandle, Error> D3D12Device::create_transient_buffer(const BufferDescription& desc) {
    ValidationMessage validation;
    if (Status valid = validate_buffer(desc, validation); !valid) {
        return make_unexpected(valid.error());
    }
    D3D12Buffer buffer;
    buffer.desc = desc;
    buffer.name.assign(desc.name);
    buffer.desc.name = buffer.name.text;
    buffer.transient = true;
    buffer.bound = false;
    Expected<BufferHandle, Error> handle = buffers_.create(std::move(buffer));
    if (handle && !transient_buffers_.push_back(*handle)) {
        (void)buffers_.destroy(*handle);
        return fail(ErrorCode::OutOfMemory, "D3D12 could not remember a transient buffer");
    }
    return handle;
}

Expected<MemoryRequirements, Error> D3D12Device::texture_memory_requirements(
    TextureHandle handle) const {
    const D3D12Texture* texture = textures_.resolve(handle);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 texture handle is stale");
    }
    const D3D12_RESOURCE_DESC native = native_texture_desc(texture->desc);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &native);
    const HeapResourceClass resource_class =
        has_usage(texture->desc.usage, TextureUsage::ColorAttachment) ||
                has_usage(texture->desc.usage, TextureUsage::DepthStencilAttachment)
            ? HeapResourceClass::RenderTarget
            : HeapResourceClass::Texture;
    return MemoryRequirements{info.SizeInBytes, info.Alignment,
                              memory_pool_class(static_cast<u32>(heap_tier_), resource_class)};
}

Expected<MemoryRequirements, Error> D3D12Device::buffer_memory_requirements(
    BufferHandle handle) const {
    const D3D12Buffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 buffer handle is stale");
    }
    const D3D12_RESOURCE_DESC native = native_buffer_desc(buffer->desc.size);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &native);
    return MemoryRequirements{
        info.SizeInBytes, info.Alignment,
        memory_pool_class(static_cast<u32>(heap_tier_), HeapResourceClass::Buffer)};
}

Status D3D12Device::reserve_transient_memory(u64 bytes, MemoryPoolClass pool_class) {
    if (pool_class.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "D3D12 transient resources have no common heap class");
    }
    if (bytes == 0) {
        return ok();
    }
    const D3D12_HEAP_FLAGS flags = pool_flags(pool_class);
    if (transient_heap_ && bytes <= transient_bytes_ && flags == transient_flags_) {
        return ok();
    }
    (void)wait_idle();
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = align_to(bytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Flags = flags;
    ComPtr<ID3D12Heap> heap;
    if (FAILED(device_->CreateHeap(&desc, IID_PPV_ARGS(&heap)))) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not reserve the transient placement heap");
    }
    if (transient_bytes_ != 0) {
        discharge(GpuMemoryCategory::Transient, transient_bytes_);
    }
    transient_heap_ = std::move(heap);
    transient_bytes_ = desc.SizeInBytes;
    transient_flags_ = flags;
    charge(GpuMemoryCategory::Transient, transient_bytes_);
    return ok();
}

Status D3D12Device::bind_transient(TextureHandle handle, u64 offset) {
    D3D12Texture* texture = textures_.resolve(handle);
    if (texture == nullptr || !texture->transient) {
        return fail(ErrorCode::NotFound, "D3D12 transient texture handle is stale");
    }
    if (!transient_heap_) {
        return fail(ErrorCode::Unavailable, "D3D12 transient heap is not reserved");
    }
    const D3D12_RESOURCE_DESC native = native_texture_desc(texture->desc);
    if (FAILED(device_->CreatePlacedResource(transient_heap_.Get(), offset, &native,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&texture->resource)))) {
        return fail(ErrorCode::InvalidArgument,
                    "D3D12 could not bind a transient texture at this offset");
    }
    texture->bound = true;
    return ok();
}

Status D3D12Device::bind_transient(BufferHandle handle, u64 offset) {
    D3D12Buffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr || !buffer->transient) {
        return fail(ErrorCode::NotFound, "D3D12 transient buffer handle is stale");
    }
    if (!transient_heap_) {
        return fail(ErrorCode::Unavailable, "D3D12 transient heap is not reserved");
    }
    const D3D12_RESOURCE_DESC native = native_buffer_desc(buffer->desc.size);
    if (FAILED(device_->CreatePlacedResource(transient_heap_.Get(), offset, &native,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&buffer->resource)))) {
        return fail(ErrorCode::InvalidArgument,
                    "D3D12 could not bind a transient buffer at this offset");
    }
    buffer->bound = true;
    return ok();
}

void D3D12Device::release_transient_resources() noexcept {
    for (TextureHandle handle : transient_textures_) {
        (void)textures_.destroy(handle);
    }
    for (BufferHandle handle : transient_buffers_) {
        (void)buffers_.destroy(handle);
    }
    transient_textures_.clear();
    transient_buffers_.clear();
}

Expected<ShaderModuleHandle, Error> D3D12Device::create_shader_module(
    const ShaderModuleDescription& desc) {
    ValidationMessage validation;
    if (Status valid = validate_shader_module(desc, capabilities_, validation); !valid) {
        return make_unexpected(valid.error());
    }
    D3D12ShaderModule module(*allocator_);
    module.name.assign(desc.name);
    module.entry_point.assign(desc.entry_point);
    module.stage = desc.stage;
    if (!module.bytecode.resize(desc.native.size())) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not retain DXIL bytecode");
    }
    std::memcpy(module.bytecode.data(), desc.native.data(), desc.native.size());
    return shaders_.create(std::move(module));
}

void D3D12Device::destroy_shader_module(ShaderModuleHandle handle) noexcept {
    if (shaders_.resolve(handle) == nullptr) {
        return;
    }
    (void)shaders_.destroy(handle);
    ++stats_.resources_freed;
}

Expected<DescriptorSetLayoutHandle, Error> D3D12Device::create_descriptor_set_layout(
    const DescriptorSetLayoutDescription& desc) {
    if (desc.bindings.size() > kD3D12MaxLayoutBindings) {
        return fail(ErrorCode::OutOfRange, "D3D12 descriptor set layout has too many bindings");
    }
    D3D12DescriptorSetLayout layout;
    layout.name.assign(desc.name);
    u32 cbv_register = 0;
    u32 srv_register = 0;
    u32 uav_register = 0;
    u32 sampler_register = 0;
    for (usize index = 0; index < desc.bindings.size(); ++index) {
        const DescriptorBinding& source = desc.bindings[index];
        D3D12LayoutBinding& binding = layout.bindings[layout.binding_count++];
        binding.binding = source.binding;
        binding.kind = source.kind;
        binding.count = source.count == 0 ? kBindlessCapacity : source.count;
        if (source.kind == DescriptorKind::Sampler) {
            binding.sampler_offset = layout.sampler_count;
            binding.sampler_register = sampler_register;
            layout.sampler_count += binding.count;
            sampler_register += binding.count;
        } else {
            binding.resource_offset = layout.resource_count;
            layout.resource_count += binding.count;
            switch (source.kind) {
                case DescriptorKind::UniformBuffer:
                    binding.shader_register = cbv_register;
                    cbv_register += binding.count;
                    break;
                case DescriptorKind::StorageTexture:
                    binding.shader_register = uav_register;
                    uav_register += binding.count;
                    break;
                case DescriptorKind::StorageBuffer:
                    // Storage buffers are writable in the engine's descriptor vocabulary.
                    binding.shader_register = uav_register;
                    uav_register += binding.count;
                    break;
                case DescriptorKind::SampledTexture:
                case DescriptorKind::CombinedTextureSampler:
                case DescriptorKind::InputAttachment:
                    binding.shader_register = srv_register;
                    srv_register += binding.count;
                    break;
                case DescriptorKind::Sampler:
                    binding.sampler_register = sampler_register;
                    sampler_register += binding.count;
                    break;
            }
            if (source.kind == DescriptorKind::CombinedTextureSampler) {
                binding.sampler_offset = layout.sampler_count;
                binding.sampler_register = sampler_register;
                layout.sampler_count += binding.count;
                sampler_register += binding.count;
            }
        }
    }
    return set_layouts_.create(layout);
}

void D3D12Device::destroy_descriptor_set_layout(DescriptorSetLayoutHandle handle) noexcept {
    if (set_layouts_.resolve(handle) != nullptr) {
        (void)set_layouts_.destroy(handle);
    }
}

Expected<PipelineLayoutHandle, Error> D3D12Device::create_pipeline_layout(
    const PipelineLayoutDescription& desc) {
    ValidationMessage validation;
    if (Status valid = validate_pipeline_layout(desc, validation); !valid) {
        return make_unexpected(valid.error());
    }

    D3D12PipelineLayout layout;
    layout.name.assign(desc.name);
    layout.set_count = static_cast<u32>(desc.set_layouts.size());

    D3D12_ROOT_PARAMETER parameters[kMaxDescriptorSets * 2 + 1]{};
    D3D12_DESCRIPTOR_RANGE ranges[kMaxDescriptorSets * kD3D12MaxLayoutBindings * 2]{};
    u32 parameter_count = 0;
    u32 range_count = 0;

    for (u32 set_index = 0; set_index < layout.set_count; ++set_index) {
        D3D12DescriptorSetLayout* set = set_layouts_.resolve(desc.set_layouts[set_index]);
        if (set == nullptr) {
            return fail(ErrorCode::NotFound, "D3D12 pipeline layout has a stale set layout");
        }
        layout.set_layouts[set_index] = desc.set_layouts[set_index];

        const u32 resource_begin = range_count;
        u32 resource_ranges = 0;
        const u32 sampler_begin = range_count + set->binding_count;
        u32 sampler_ranges = 0;
        for (u32 binding_index = 0; binding_index < set->binding_count; ++binding_index) {
            const D3D12LayoutBinding& binding = set->bindings[binding_index];
            if (binding.kind == DescriptorKind::Sampler) {
                D3D12_DESCRIPTOR_RANGE& range = ranges[sampler_begin + sampler_ranges++];
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                range.NumDescriptors = binding.count;
                range.BaseShaderRegister = binding.sampler_register;
                range.RegisterSpace = set_index + 1;
                range.OffsetInDescriptorsFromTableStart = binding.sampler_offset;
                continue;
            }
            D3D12_DESCRIPTOR_RANGE& range = ranges[resource_begin + resource_ranges++];
            switch (binding.kind) {
                case DescriptorKind::UniformBuffer:
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                    break;
                case DescriptorKind::StorageBuffer:
                case DescriptorKind::StorageTexture:
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                    break;
                case DescriptorKind::SampledTexture:
                case DescriptorKind::CombinedTextureSampler:
                case DescriptorKind::InputAttachment:
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    break;
                case DescriptorKind::Sampler:
                    break;
            }
            range.NumDescriptors = binding.count;
            range.BaseShaderRegister = binding.shader_register;
            range.RegisterSpace = set_index + 1;
            range.OffsetInDescriptorsFromTableStart = binding.resource_offset;
            if (binding.kind == DescriptorKind::CombinedTextureSampler) {
                D3D12_DESCRIPTOR_RANGE& sampler = ranges[sampler_begin + sampler_ranges++];
                sampler.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                sampler.NumDescriptors = binding.count;
                sampler.BaseShaderRegister = binding.sampler_register;
                sampler.RegisterSpace = set_index + 1;
                sampler.OffsetInDescriptorsFromTableStart = binding.sampler_offset;
            }
        }
        range_count = sampler_begin + sampler_ranges;
        layout.resource_root[set_index] = ~0U;
        layout.sampler_root[set_index] = ~0U;
        if (resource_ranges != 0) {
            layout.resource_root[set_index] = parameter_count;
            parameters[parameter_count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[parameter_count].DescriptorTable.NumDescriptorRanges = resource_ranges;
            parameters[parameter_count].DescriptorTable.pDescriptorRanges = &ranges[resource_begin];
            parameters[parameter_count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            ++parameter_count;
        }
        if (sampler_ranges != 0) {
            layout.sampler_root[set_index] = parameter_count;
            parameters[parameter_count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[parameter_count].DescriptorTable.NumDescriptorRanges = sampler_ranges;
            parameters[parameter_count].DescriptorTable.pDescriptorRanges = &ranges[sampler_begin];
            parameters[parameter_count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            ++parameter_count;
        }
    }

    u32 push_bytes = 0;
    for (const PushConstantRange& range : desc.push_constants) {
        push_bytes = std::max(push_bytes, range.offset + range.size);
    }
    if (push_bytes != 0) {
        layout.push_root = parameter_count;
        layout.push_dwords = (push_bytes + 3U) / 4U;
        parameters[parameter_count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[parameter_count].Constants.ShaderRegister = 0;
        parameters[parameter_count].Constants.RegisterSpace = 0;
        parameters[parameter_count].Constants.Num32BitValues = layout.push_dwords;
        parameters[parameter_count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ++parameter_count;
    }

    D3D12_ROOT_SIGNATURE_DESC native{};
    native.NumParameters = parameter_count;
    native.pParameters = parameters;
    native.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(
            D3D12SerializeRootSignature(&native, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))) {
        return fail(ErrorCode::InvalidArgument, "D3D12 root signature serialization failed");
    }
    if (FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&layout.root_signature)))) {
        return fail(ErrorCode::InvalidArgument, "D3D12 root signature creation failed");
    }
    return pipeline_layouts_.create(std::move(layout));
}

void D3D12Device::destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept {
    if (pipeline_layouts_.resolve(handle) != nullptr) {
        (void)pipeline_layouts_.destroy(handle);
    }
}

Expected<DescriptorSetHandle, Error> D3D12Device::allocate_descriptor_set(
    DescriptorSetLayoutHandle layout_handle, bool per_frame) {
    D3D12DescriptorSetLayout* layout = set_layouts_.resolve(layout_handle);
    if (layout == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 descriptor set layout is stale");
    }
    D3D12DescriptorSet set;
    set.layout = layout_handle;
    set.per_frame = per_frame;
    set.frame_slot = frame_slot_;
    set.resource_base = allocate_resource_descriptors(layout->resource_count);
    set.sampler_base = allocate_sampler_descriptors(layout->sampler_count);
    if ((layout->resource_count != 0 && set.resource_base == ~0U) ||
        (layout->sampler_count != 0 && set.sampler_base == ~0U)) {
        return fail(ErrorCode::OutOfMemory, "D3D12 shader-visible descriptor heap is full");
    }
    return descriptor_sets_.create(set);
}

Status D3D12Device::update_descriptor_set(DescriptorSetHandle set_handle,
                                          Span<const DescriptorWrite> writes) {
    D3D12DescriptorSet* set = descriptor_sets_.resolve(set_handle);
    if (set == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 descriptor set is stale");
    }
    D3D12DescriptorSetLayout* layout = set_layouts_.resolve(set->layout);
    if (layout == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 descriptor set layout is stale");
    }
    for (const DescriptorWrite& write : writes) {
        const D3D12LayoutBinding* binding = nullptr;
        for (u32 index = 0; index < layout->binding_count; ++index) {
            if (layout->bindings[index].binding == write.binding) {
                binding = &layout->bindings[index];
            }
        }
        if (binding == nullptr || write.array_index >= binding->count) {
            return fail(ErrorCode::OutOfRange, "D3D12 descriptor write is outside its layout");
        }
        if (write.kind != binding->kind) {
            return fail(ErrorCode::InvalidArgument,
                        "D3D12 descriptor write kind differs from its layout");
        }

        if (write.kind == DescriptorKind::Sampler) {
            D3D12Sampler* sampler_record = samplers_.resolve(write.sampler);
            if (sampler_record == nullptr) {
                return fail(ErrorCode::NotFound, "D3D12 sampler handle is stale");
            }
            D3D12_CPU_DESCRIPTOR_HANDLE destination =
                sampler_heap_->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += static_cast<SIZE_T>(set->sampler_base + binding->sampler_offset +
                                                   write.array_index) *
                               sampler_stride_;
            device_->CopyDescriptorsSimple(1, destination, sampler_record->descriptor,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            continue;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE destination =
            resource_heap_->GetCPUDescriptorHandleForHeapStart();
        destination.ptr +=
            static_cast<SIZE_T>(set->resource_base + binding->resource_offset + write.array_index) *
            resource_stride_;
        if (write.kind == DescriptorKind::UniformBuffer ||
            write.kind == DescriptorKind::StorageBuffer) {
            D3D12Buffer* buffer = buffers_.resolve(write.buffer);
            if (buffer == nullptr || !buffer->resource) {
                return fail(ErrorCode::NotFound, "D3D12 buffer descriptor is stale");
            }
            const u64 remaining = buffer->desc.size - write.buffer_offset;
            const u64 bytes = write.buffer_range == 0 ? remaining : write.buffer_range;
            if (write.kind == DescriptorKind::UniformBuffer) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
                cbv.BufferLocation = buffer->resource->GetGPUVirtualAddress() + write.buffer_offset;
                cbv.SizeInBytes = static_cast<UINT>(
                    align_to(bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
                device_->CreateConstantBufferView(&cbv, destination);
            } else {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                uav.Format = DXGI_FORMAT_R32_TYPELESS;
                uav.Buffer.FirstElement = write.buffer_offset / 4U;
                uav.Buffer.NumElements = static_cast<UINT>(bytes / 4U);
                uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                device_->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &uav,
                                                   destination);
            }
        } else {
            D3D12TextureView* texture_view = views_.resolve(write.texture_view);
            if (texture_view == nullptr || !texture_view->has_srv) {
                return fail(ErrorCode::NotFound, "D3D12 texture descriptor has no sampled view");
            }
            device_->CopyDescriptorsSimple(1, destination, texture_view->srv,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    return ok();
}

Status D3D12Device::create_bindless_table() noexcept {
    if (descriptor_model_ != DescriptorModel::Bindless) {
        return ok();
    }
    DescriptorBinding bindings[2]{};
    bindings[0] = {kGlobalTableTextureBinding, DescriptorKind::SampledTexture, kBindlessCapacity,
                   ShaderStage::Fragment, true};
    bindings[1] = {kGlobalTableSamplerBinding, DescriptorKind::Sampler, 1, ShaderStage::Fragment,
                   false};
    DescriptorSetLayoutDescription layout_desc;
    layout_desc.name = "D3D12 global texture table";
    layout_desc.bindings = {bindings, 2};
    Expected<DescriptorSetLayoutHandle, Error> layout = create_descriptor_set_layout(layout_desc);
    if (!layout) {
        return make_unexpected(layout.error());
    }
    bindless_layout_ = *layout;
    Expected<DescriptorSetHandle, Error> set = allocate_descriptor_set(*layout, false);
    if (!set) {
        return make_unexpected(set.error());
    }
    bindless_set_ = *set;
    return ok();
}

BindlessIndex D3D12Device::bind_texture_globally(TextureViewHandle view_handle,
                                                 SamplerHandle sampler_handle) noexcept {
    if (bindless_set_.is_null()) {
        return kInvalidBindlessIndex;
    }
    BindlessIndex index = kInvalidBindlessIndex;
    if (!bindless_free_.empty()) {
        index = bindless_free_[bindless_free_.size() - 1];
        bindless_free_.pop_back();
    } else if (bindless_next_ < kBindlessCapacity) {
        index = bindless_next_++;
    }
    if (index == kInvalidBindlessIndex) {
        return index;
    }
    DescriptorWrite write;
    write.binding = kGlobalTableTextureBinding;
    write.array_index = index;
    write.kind = DescriptorKind::SampledTexture;
    write.texture_view = view_handle;
    if (!update_descriptor_set(bindless_set_, {&write, 1})) {
        return kInvalidBindlessIndex;
    }
    if (!sampler_handle.is_null() && !set_global_sampler(sampler_handle)) {
        return kInvalidBindlessIndex;
    }
    return index;
}

void D3D12Device::release_bindless_index(BindlessIndex index) noexcept {
    if (index < kBindlessCapacity) {
        (void)bindless_free_.push_back(index);
    }
}

Status D3D12Device::set_global_sampler(SamplerHandle sampler_handle) noexcept {
    if (bindless_set_.is_null()) {
        return fail(ErrorCode::Unsupported, "D3D12 bindless table is unavailable");
    }
    if (!bindless_sampler_.is_null() && bindless_sampler_.bits() != sampler_handle.bits()) {
        return fail(ErrorCode::InvalidArgument,
                    "D3D12 global table already has a different sampler");
    }
    DescriptorWrite write;
    write.binding = kGlobalTableSamplerBinding;
    write.kind = DescriptorKind::Sampler;
    write.sampler = sampler_handle;
    Status status = update_descriptor_set(bindless_set_, {&write, 1});
    if (status) {
        bindless_sampler_ = sampler_handle;
    }
    return status;
}

Expected<GraphicsPipelineHandle, Error> D3D12Device::create_graphics_pipeline(
    const GraphicsPipelineDescription& desc) {
    D3D12PipelineLayout* layout = pipeline_layouts_.resolve(desc.layout);
    D3D12ShaderModule* vertex = shaders_.resolve(desc.vertex_shader);
    D3D12ShaderModule* fragment =
        desc.fragment_shader.is_null() ? nullptr : shaders_.resolve(desc.fragment_shader);
    if (layout == nullptr || vertex == nullptr ||
        (!desc.fragment_shader.is_null() && fragment == nullptr)) {
        return fail(ErrorCode::NotFound, "D3D12 graphics pipeline has a stale dependency");
    }

    D3D12_INPUT_ELEMENT_DESC inputs[kMaxVertexAttributes]{};
    for (u32 index = 0; index < desc.vertex_attributes.size(); ++index) {
        const VertexAttribute& source = desc.vertex_attributes[index];
        inputs[index].SemanticName = source.location == 0   ? "POSITION"
                                     : source.location == 1 ? "NORMAL"
                                                            : "TEXCOORD";
        inputs[index].SemanticIndex = source.location < 2 ? 0 : source.location - 2;
        inputs[index].Format = dxgi_format(source.format);
        inputs[index].InputSlot = source.binding;
        inputs[index].AlignedByteOffset = source.offset;
        inputs[index].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        inputs[index].InstanceDataStepRate = 0;
        for (const VertexBinding& binding : desc.vertex_bindings) {
            if (binding.binding == source.binding &&
                binding.input_rate == VertexInputRate::PerInstance) {
                inputs[index].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                inputs[index].InstanceDataStepRate = 1;
            }
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
    native.pRootSignature = layout->root_signature.Get();
    native.VS = {vertex->bytecode.data(), vertex->bytecode.size()};
    if (fragment != nullptr) {
        native.PS = {fragment->bytecode.data(), fragment->bytecode.size()};
    }
    native.InputLayout = {inputs, static_cast<UINT>(desc.vertex_attributes.size())};
    native.PrimitiveTopologyType = topology_type(desc.topology);
    native.SampleMask = UINT_MAX;
    native.SampleDesc.Count = desc.sample_count;
    native.RasterizerState.FillMode = desc.rasterisation.polygon_mode == PolygonMode::Line
                                          ? D3D12_FILL_MODE_WIREFRAME
                                          : D3D12_FILL_MODE_SOLID;
    switch (desc.rasterisation.cull_mode) {
        case CullMode::None:
            native.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            break;
        case CullMode::Front:
            native.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
            break;
        case CullMode::Back:
            native.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            break;
    }
    native.RasterizerState.FrontCounterClockwise =
        desc.rasterisation.front_face == FrontFace::CounterClockwise;
    native.RasterizerState.DepthClipEnable = !desc.rasterisation.depth_clamp_enable;
    native.RasterizerState.DepthBias = static_cast<INT>(desc.rasterisation.depth_bias_constant);
    native.RasterizerState.SlopeScaledDepthBias = desc.rasterisation.depth_bias_slope;
    native.BlendState.AlphaToCoverageEnable = FALSE;
    native.BlendState.IndependentBlendEnable = TRUE;
    for (D3D12_RENDER_TARGET_BLEND_DESC& blend : native.BlendState.RenderTarget) {
        blend = default_blend_attachment();
    }
    for (u32 index = 0; index < desc.color_attachments.size(); ++index) {
        const ColorAttachmentState& source = desc.color_attachments[index];
        D3D12_RENDER_TARGET_BLEND_DESC& blend = native.BlendState.RenderTarget[index];
        blend.BlendEnable = source.blend_enable;
        blend.LogicOpEnable = FALSE;
        blend.SrcBlend = blend_factor(source.source_color);
        blend.DestBlend = blend_factor(source.destination_color);
        blend.BlendOp = blend_op(source.color_op);
        blend.SrcBlendAlpha = blend_factor(source.source_alpha);
        blend.DestBlendAlpha = blend_factor(source.destination_alpha);
        blend.BlendOpAlpha = blend_op(source.alpha_op);
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = static_cast<UINT8>(source.write_mask);
        native.RTVFormats[index] = dxgi_format(source.format);
    }
    native.NumRenderTargets = static_cast<UINT>(desc.color_attachments.size());
    native.DepthStencilState.DepthEnable = desc.depth_stencil.depth_test_enable;
    native.DepthStencilState.DepthWriteMask = desc.depth_stencil.depth_write_enable
                                                  ? D3D12_DEPTH_WRITE_MASK_ALL
                                                  : D3D12_DEPTH_WRITE_MASK_ZERO;
    native.DepthStencilState.DepthFunc = compare_op(desc.depth_stencil.depth_compare);
    native.DepthStencilState.StencilEnable = desc.depth_stencil.stencil_test_enable;
    native.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    native.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    native.DepthStencilState.FrontFace = default_stencil_face();
    native.DepthStencilState.BackFace = default_stencil_face();
    native.DSVFormat = dxgi_format(desc.depth_stencil.format);

    D3D12GraphicsPipeline pipeline;
    pipeline.layout = desc.layout;
    pipeline.topology = primitive_topology(desc.topology);
    pipeline.vertex_binding_count = static_cast<u32>(desc.vertex_bindings.size());
    for (u32 index = 0; index < desc.vertex_bindings.size(); ++index) {
        pipeline.vertex_strides[index] = desc.vertex_bindings[index].stride;
    }
    if (FAILED(device_->CreateGraphicsPipelineState(&native, IID_PPV_ARGS(&pipeline.pipeline)))) {
        return fail(ErrorCode::InvalidArgument, "D3D12 graphics pipeline creation failed");
    }
    return graphics_pipelines_.create(std::move(pipeline));
}

void D3D12Device::destroy_graphics_pipeline(GraphicsPipelineHandle handle) noexcept {
    if (graphics_pipelines_.resolve(handle) != nullptr) {
        (void)graphics_pipelines_.destroy(handle);
    }
}

Expected<ComputePipelineHandle, Error> D3D12Device::create_compute_pipeline(
    const ComputePipelineDescription& desc) {
    D3D12PipelineLayout* layout = pipeline_layouts_.resolve(desc.layout);
    D3D12ShaderModule* shader = shaders_.resolve(desc.shader);
    if (layout == nullptr || shader == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 compute pipeline has a stale dependency");
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC native{};
    native.pRootSignature = layout->root_signature.Get();
    native.CS = {shader->bytecode.data(), shader->bytecode.size()};
    D3D12ComputePipeline pipeline;
    pipeline.layout = desc.layout;
    if (FAILED(device_->CreateComputePipelineState(&native, IID_PPV_ARGS(&pipeline.pipeline)))) {
        return fail(ErrorCode::InvalidArgument, "D3D12 compute pipeline creation failed");
    }
    return compute_pipelines_.create(std::move(pipeline));
}

void D3D12Device::destroy_compute_pipeline(ComputePipelineHandle handle) noexcept {
    if (compute_pipelines_.resolve(handle) != nullptr) {
        (void)compute_pipelines_.destroy(handle);
    }
}

Status D3D12Device::save_pipeline_cache(const char* path) {
    (void)path;
    return fail(ErrorCode::Unsupported,
                "D3D12 pipeline library serialization is not available in this backend yet");
}

Status D3D12Device::load_pipeline_cache(const char* path) {
    (void)path;
    return fail(ErrorCode::Unsupported,
                "D3D12 pipeline library deserialization is not available in this backend yet");
}

Expected<QueryPoolHandle, Error> D3D12Device::create_query_pool(const QueryPoolDescription& desc) {
    if (desc.count == 0) {
        return fail(ErrorCode::InvalidArgument, "D3D12 query pool is empty");
    }
    D3D12QueryPool pool;
    pool.kind = desc.kind;
    pool.count = desc.count;
    D3D12_QUERY_HEAP_DESC heap{};
    heap.Count = desc.count;
    switch (desc.kind) {
        case QueryKind::Timestamp:
            heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            break;
        case QueryKind::Occlusion:
            heap.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
            break;
        case QueryKind::PipelineStatistics:
            heap.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
            break;
    }
    if (FAILED(device_->CreateQueryHeap(&heap, IID_PPV_ARGS(&pool.heap)))) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not create a query heap");
    }
    const u64 stride = desc.kind == QueryKind::PipelineStatistics
                           ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)
                           : sizeof(u64);
    const D3D12_RESOURCE_DESC resource = native_buffer_desc(stride * desc.count);
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &resource,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&pool.readback)))) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not create query readback memory");
    }
    return query_pools_.create(std::move(pool));
}

void D3D12Device::destroy_query_pool(QueryPoolHandle handle) noexcept {
    if (query_pools_.resolve(handle) != nullptr) {
        (void)query_pools_.destroy(handle);
    }
}

Expected<u32, Error> D3D12Device::read_query_results(QueryPoolHandle handle, u32 first, u32 count,
                                                     Span<u64> out) {
    D3D12QueryPool* pool = query_pools_.resolve(handle);
    if (pool == nullptr || first + count > pool->count || out.size() < count) {
        return fail(ErrorCode::OutOfRange, "D3D12 query result range is invalid");
    }
    if (pool->kind == QueryKind::PipelineStatistics) {
        return fail(ErrorCode::Unsupported, "D3D12 pipeline statistics need a typed result buffer");
    }
    void* mapped = nullptr;
    const D3D12_RANGE range{static_cast<SIZE_T>(first * sizeof(u64)),
                            static_cast<SIZE_T>((first + count) * sizeof(u64))};
    if (FAILED(pool->readback->Map(0, &range, &mapped))) {
        return fail(ErrorCode::Unavailable, "D3D12 could not map query results");
    }
    std::memcpy(out.data(), static_cast<const u8*>(mapped) + first * sizeof(u64),
                count * sizeof(u64));
    pool->readback->Unmap(0, nullptr);
    return count;
}

Expected<u32, Error> D3D12Device::begin_frame() {
    if (frame_open_) {
        return fail(ErrorCode::InvalidArgument, "D3D12 frame is already open");
    }
    // Recycle only after all queues are done. This is deliberately conservative for the first
    // backend version; the public contract is correct and per-frame fence narrowing is internal.
    if (Status idle = wait_idle(); !idle) {
        return make_unexpected(idle.error());
    }
    for (CommandBufferHandle handle : live_commands_) {
        (void)command_buffers_.destroy(handle);
    }
    live_commands_.clear();
    frame_slot_ = static_cast<u32>(frame_index_ % frames_in_flight_);
    frame_open_ = true;
    ++stats_.frames_begun;
    return frame_slot_;
}

Status D3D12Device::end_frame() {
    if (!frame_open_) {
        return fail(ErrorCode::InvalidArgument, "D3D12 frame is not open");
    }
    frame_open_ = false;
    ++frame_index_;
    ++stats_.frames_completed;
    return ok();
}

Expected<CommandBufferHandle, Error> D3D12Device::acquire_command_buffer(QueueKind queue,
                                                                         bool secondary) {
    if (!has_queue(queue)) {
        return fail(ErrorCode::Unsupported, "D3D12 queue is unavailable");
    }
    if (secondary) {
        return fail(ErrorCode::Unsupported,
                    "D3D12 records whole passes sequentially: the RHI secondary form cannot "
                    "preserve barriers between parallel direct command lists");
    }
    const std::lock_guard<std::mutex> lock(acquire_mutex_);
    Expected<CommandBufferHandle, Error> handle =
        command_buffers_.create(this, queue, secondary, frame_slot_);
    if (!handle) {
        return handle;
    }
    D3D12CommandBuffer* command = command_buffers_.resolve(*handle);
    command->set_handle(*handle);
    const D3D12_COMMAND_LIST_TYPE type = command_type(queue);
    if (FAILED(device_->CreateCommandAllocator(type, IID_PPV_ARGS(&command->allocator))) ||
        FAILED(device_->CreateCommandList(0, type, command->allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&command->list)))) {
        (void)command_buffers_.destroy(*handle);
        return fail(ErrorCode::OutOfMemory, "D3D12 could not allocate a command list");
    }
    (void)command->list->Close();
    if (!live_commands_.push_back(*handle)) {
        (void)command_buffers_.destroy(*handle);
        return fail(ErrorCode::OutOfMemory, "D3D12 could not remember a command list");
    }
    return handle;
}

CommandBuffer* D3D12Device::command_buffer(CommandBufferHandle handle) noexcept {
    return command_buffers_.resolve(handle);
}

Status D3D12Device::begin_command_buffer(CommandBufferHandle handle) {
    D3D12CommandBuffer* command = command_buffers_.resolve(handle);
    if (command == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 command buffer is stale");
    }
    if (command->recording()) {
        return fail(ErrorCode::InvalidArgument, "D3D12 command buffer is already recording");
    }
    if (FAILED(command->allocator->Reset()) ||
        FAILED(command->list->Reset(command->allocator.Get(), nullptr))) {
        return fail(ErrorCode::Unavailable, "D3D12 could not reset a command list");
    }
    command->set_recording(true);
    ++stats_.command_buffers_recorded;
    return ok();
}

Status D3D12Device::end_command_buffer(CommandBufferHandle handle) {
    D3D12CommandBuffer* command = command_buffers_.resolve(handle);
    if (command == nullptr || !command->recording()) {
        return fail(ErrorCode::InvalidArgument, "D3D12 command buffer is not recording");
    }
    if (FAILED(command->list->Close())) {
        return fail(ErrorCode::Unavailable, "D3D12 could not close a command list");
    }
    command->set_recording(false);
    return ok();
}

Status D3D12Device::execute_secondary(CommandBufferHandle, Span<const CommandBufferHandle>) {
    return fail(ErrorCode::Unsupported,
                "D3D12 parallel direct command lists do not map to the RHI secondary form");
}

Expected<u64, Error> D3D12Device::submit(const SubmitInfo& info) {
    const u32 queue_index = static_cast<u32>(info.queue);
    if (queue_index >= kQueueKindCount || !queues_[queue_index]) {
        return fail(ErrorCode::Unsupported, "D3D12 submit queue is unavailable");
    }
    for (const TimelineWait& wait : info.waits) {
        const u32 wait_index = static_cast<u32>(wait.queue);
        if (wait_index >= kQueueKindCount || !timelines_[wait_index]) {
            return fail(ErrorCode::InvalidArgument,
                        "D3D12 timeline wait names an unavailable queue");
        }
        if (FAILED(queues_[queue_index]->Wait(timelines_[wait_index].Get(), wait.value))) {
            return fail(ErrorCode::Unavailable, "D3D12 queue timeline wait failed");
        }
        ++stats_.semaphore_waits;
    }
    if (!info.wait_binary.is_null()) {
        D3D12Semaphore* semaphore = semaphores_.resolve(info.wait_binary);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "D3D12 wait semaphore is stale");
        }
        if (FAILED(queues_[queue_index]->Wait(semaphore->fence.Get(), semaphore->next_wait++))) {
            return fail(ErrorCode::Unavailable, "D3D12 binary semaphore wait failed");
        }
    }
    ID3D12CommandList* lists[64]{};
    if (info.command_buffers.size() > 64) {
        return fail(ErrorCode::OutOfRange, "D3D12 submit has more than 64 command lists");
    }
    for (u32 index = 0; index < info.command_buffers.size(); ++index) {
        D3D12CommandBuffer* command = command_buffers_.resolve(info.command_buffers[index]);
        if (command == nullptr || command->recording()) {
            return fail(ErrorCode::InvalidArgument,
                        "D3D12 submit contains an invalid command list");
        }
        lists[index] = command->raw();
    }
    if (!info.command_buffers.empty()) {
        queues_[queue_index]->ExecuteCommandLists(static_cast<UINT>(info.command_buffers.size()),
                                                  lists);
    }
    const u64 value = ++timeline_values_[queue_index];
    if (FAILED(queues_[queue_index]->Signal(timelines_[queue_index].Get(), value))) {
        return fail(ErrorCode::Unavailable, "D3D12 queue timeline signal failed");
    }
    if (!info.signal_binary.is_null()) {
        D3D12Semaphore* semaphore = semaphores_.resolve(info.signal_binary);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "D3D12 signal semaphore is stale");
        }
        if (FAILED(
                queues_[queue_index]->Signal(semaphore->fence.Get(), semaphore->next_signal++))) {
            return fail(ErrorCode::Unavailable, "D3D12 binary semaphore signal failed");
        }
    }
    if (!info.signal_fence.is_null()) {
        D3D12Fence* fence = fences_.resolve(info.signal_fence);
        if (fence == nullptr) {
            return fail(ErrorCode::NotFound, "D3D12 signal fence is stale");
        }
        if (FAILED(queues_[queue_index]->Signal(fence->fence.Get(), fence->value))) {
            return fail(ErrorCode::Unavailable, "D3D12 fence signal failed");
        }
    }
    ++stats_.submissions;
    return value;
}

u64 D3D12Device::timeline_value(QueueKind queue) const noexcept {
    const u32 index = static_cast<u32>(queue);
    return index < kQueueKindCount && timelines_[index] ? timelines_[index]->GetCompletedValue()
                                                        : 0;
}

Status D3D12Device::wait_timeline(QueueKind queue, u64 value, u64 timeout_ns) {
    const u32 index = static_cast<u32>(queue);
    if (index >= kQueueKindCount || !timelines_[index]) {
        return fail(ErrorCode::Unsupported, "D3D12 timeline queue is unavailable");
    }
    if (timelines_[index]->GetCompletedValue() >= value) {
        return ok();
    }
    if (FAILED(timelines_[index]->SetEventOnCompletion(value, timeline_events_[index]))) {
        return fail(ErrorCode::Unavailable, "D3D12 timeline wait could not arm its event");
    }
    const DWORD timeout =
        timeout_ns == ~0ULL ? INFINITE : static_cast<DWORD>(timeout_ns / 1'000'000ULL);
    if (WaitForSingleObject(timeline_events_[index], timeout) != WAIT_OBJECT_0) {
        return fail(ErrorCode::Timeout, "D3D12 timeline wait timed out");
    }
    return ok();
}

Status D3D12Device::wait_idle() {
    for (u32 index = 0; index < kQueueKindCount; ++index) {
        if (!queues_[index]) {
            continue;
        }
        const u64 value = ++timeline_values_[index];
        if (FAILED(queues_[index]->Signal(timelines_[index].Get(), value))) {
            return fail(ErrorCode::Unavailable, "D3D12 idle signal failed");
        }
        if (Status waited = wait_timeline(static_cast<QueueKind>(index), value, ~0ULL); !waited) {
            return waited;
        }
    }
    if (drain_validation_messages()) {
        return fail(ErrorCode::Internal, "the D3D12 debug layer reported an error");
    }
    return ok();
}

Expected<FenceHandle, Error> D3D12Device::create_fence(bool signalled) {
    D3D12Fence fence;
    fence.value = 1;
    if (FAILED(device_->CreateFence(signalled ? 1 : 0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&fence.fence)))) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not create a fence");
    }
    fence.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence.event == nullptr) {
        return fail(ErrorCode::Unavailable, "D3D12 could not create a fence event");
    }
    return fences_.create(std::move(fence));
}

void D3D12Device::destroy_fence(FenceHandle handle) noexcept {
    if (fences_.resolve(handle) != nullptr) {
        (void)fences_.destroy(handle);
    }
}

Status D3D12Device::wait_fence(FenceHandle handle, u64 timeout_ns) {
    D3D12Fence* fence = fences_.resolve(handle);
    if (fence == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 fence is stale");
    }
    if (fence->fence->GetCompletedValue() >= fence->value) {
        return ok();
    }
    if (FAILED(fence->fence->SetEventOnCompletion(fence->value, fence->event))) {
        return fail(ErrorCode::Unavailable, "D3D12 fence wait could not arm its event");
    }
    const DWORD timeout =
        timeout_ns == ~0ULL ? INFINITE : static_cast<DWORD>(timeout_ns / 1'000'000ULL);
    if (WaitForSingleObject(fence->event, timeout) != WAIT_OBJECT_0) {
        return fail(ErrorCode::Timeout, "D3D12 fence wait timed out");
    }
    return ok();
}

Status D3D12Device::reset_fence(FenceHandle handle) {
    D3D12Fence* fence = fences_.resolve(handle);
    if (fence == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 fence is stale");
    }
    ++fence->value;
    return ok();
}

bool D3D12Device::fence_signalled(FenceHandle handle) const noexcept {
    const D3D12Fence* fence = fences_.resolve(handle);
    return fence != nullptr && fence->fence->GetCompletedValue() >= fence->value;
}

Expected<SemaphoreHandle, Error> D3D12Device::create_semaphore() {
    D3D12Semaphore semaphore;
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&semaphore.fence)))) {
        return fail(ErrorCode::OutOfMemory, "D3D12 could not create a semaphore fence");
    }
    return semaphores_.create(std::move(semaphore));
}

void D3D12Device::destroy_semaphore(SemaphoreHandle handle) noexcept {
    if (semaphores_.resolve(handle) != nullptr) {
        (void)semaphores_.destroy(handle);
    }
}

Expected<SwapchainHandle, Error> D3D12Device::create_swapchain(const SwapchainDescription& desc) {
    if (desc.native_surface == nullptr || desc.extent.width == 0 || desc.extent.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "D3D12 swapchain needs an HWND and a non-empty extent");
    }
    const u32 image_count = std::min(std::max(desc.min_image_count, 2U), kMaxFramesInFlight);
    DXGI_SWAP_CHAIN_DESC1 native{};
    native.Width = desc.extent.width;
    native.Height = desc.extent.height;
    native.Format = dxgi_format(desc.preferred_format);
    native.SampleDesc.Count = 1;
    native.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    native.BufferCount = image_count;
    native.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    native.Scaling = DXGI_SCALING_STRETCH;
    native.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    native.Flags =
        desc.present_mode == PresentMode::Immediate ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> base;
    if (FAILED(factory_->CreateSwapChainForHwnd(queues_[0].Get(),
                                                static_cast<HWND>(desc.native_surface), &native,
                                                nullptr, nullptr, &base))) {
        return fail(ErrorCode::Unavailable, "DXGI could not create the D3D12 swapchain");
    }
    D3D12Swapchain swapchain;
    if (FAILED(base.As(&swapchain.swapchain))) {
        return fail(ErrorCode::Unavailable, "DXGI swapchain does not expose IDXGISwapChain3");
    }
    swapchain.info.format = desc.preferred_format;
    swapchain.info.extent = desc.extent;
    swapchain.info.image_count = image_count;
    swapchain.info.present_mode = desc.present_mode;
    for (u32 index = 0; index < image_count; ++index) {
        D3D12Texture texture;
        texture.desc.name = "D3D12 swapchain image";
        texture.desc.format = desc.preferred_format;
        texture.desc.extent = {desc.extent.width, desc.extent.height, 1};
        texture.desc.usage = TextureUsage::ColorAttachment;
        texture.name.assign(texture.desc.name);
        texture.desc.name = texture.name.text;
        texture.swapchain_owned = true;
        if (FAILED(swapchain.swapchain->GetBuffer(index, IID_PPV_ARGS(&texture.resource)))) {
            return fail(ErrorCode::Unavailable, "DXGI could not expose a swapchain image");
        }
        Expected<TextureHandle, Error> texture_handle = textures_.create(std::move(texture));
        if (!texture_handle) {
            return make_unexpected(texture_handle.error());
        }
        swapchain.textures[index] = *texture_handle;
        TextureViewDescription view_desc;
        view_desc.name = "D3D12 swapchain view";
        view_desc.texture = *texture_handle;
        Expected<TextureViewHandle, Error> view_handle = create_texture_view(view_desc);
        if (!view_handle) {
            return make_unexpected(view_handle.error());
        }
        swapchain.views[index] = *view_handle;
    }
    return swapchains_.create(std::move(swapchain));
}

void D3D12Device::destroy_swapchain(SwapchainHandle handle) noexcept {
    D3D12Swapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr) {
        return;
    }
    (void)wait_idle();
    for (u32 index = 0; index < swapchain->info.image_count; ++index) {
        if (!swapchain->views[index].is_null()) {
            destroy_texture_view(swapchain->views[index]);
        }
        if (D3D12Texture* texture = textures_.resolve(swapchain->textures[index]);
            texture != nullptr) {
            texture->swapchain_owned = false;
            destroy_texture(swapchain->textures[index]);
        }
    }
    (void)swapchains_.destroy(handle);
}

Status D3D12Device::resize_swapchain(SwapchainHandle handle, Extent2D extent) {
    D3D12Swapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr || extent.width == 0 || extent.height == 0) {
        return fail(ErrorCode::InvalidArgument, "D3D12 swapchain resize is invalid");
    }
    if (Status idle = wait_idle(); !idle) {
        return idle;
    }
    const u32 count = swapchain->info.image_count;
    for (u32 index = 0; index < count; ++index) {
        if (!swapchain->views[index].is_null()) {
            destroy_texture_view(swapchain->views[index]);
        }
        if (D3D12Texture* texture = textures_.resolve(swapchain->textures[index]);
            texture != nullptr) {
            texture->swapchain_owned = false;
            destroy_texture(swapchain->textures[index]);
        }
        swapchain->views[index] = {};
        swapchain->textures[index] = {};
    }
    DXGI_SWAP_CHAIN_DESC native{};
    if (FAILED(swapchain->swapchain->GetDesc(&native)) ||
        FAILED(swapchain->swapchain->ResizeBuffers(count, extent.width, extent.height,
                                                   native.BufferDesc.Format, native.Flags))) {
        return fail(ErrorCode::Unavailable, "DXGI could not resize the swapchain");
    }
    swapchain->info.extent = extent;
    for (u32 index = 0; index < count; ++index) {
        D3D12Texture texture;
        texture.desc.name = "D3D12 swapchain image";
        texture.desc.format = swapchain->info.format;
        texture.desc.extent = {extent.width, extent.height, 1};
        texture.desc.usage = TextureUsage::ColorAttachment;
        texture.name.assign(texture.desc.name);
        texture.desc.name = texture.name.text;
        texture.swapchain_owned = true;
        if (FAILED(swapchain->swapchain->GetBuffer(index, IID_PPV_ARGS(&texture.resource)))) {
            return fail(ErrorCode::Unavailable, "DXGI could not expose a resized swapchain image");
        }
        Expected<TextureHandle, Error> texture_handle = textures_.create(std::move(texture));
        if (!texture_handle) {
            return make_unexpected(texture_handle.error());
        }
        swapchain->textures[index] = *texture_handle;
        TextureViewDescription view_desc;
        view_desc.texture = *texture_handle;
        Expected<TextureViewHandle, Error> view_handle = create_texture_view(view_desc);
        if (!view_handle) {
            return make_unexpected(view_handle.error());
        }
        swapchain->views[index] = *view_handle;
    }
    return ok();
}

SwapchainInfo D3D12Device::swapchain_info(SwapchainHandle handle) const noexcept {
    const D3D12Swapchain* swapchain = swapchains_.resolve(handle);
    return swapchain != nullptr ? swapchain->info : SwapchainInfo{};
}

Expected<u32, Error> D3D12Device::acquire_next_image(SwapchainHandle handle, SemaphoreHandle signal,
                                                     u64) {
    D3D12Swapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr) {
        return fail(ErrorCode::NotFound, "D3D12 swapchain is stale");
    }
    if (!signal.is_null()) {
        D3D12Semaphore* semaphore = semaphores_.resolve(signal);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "D3D12 acquire semaphore is stale");
        }
        if (FAILED(queues_[0]->Signal(semaphore->fence.Get(), semaphore->next_signal++))) {
            return fail(ErrorCode::Unavailable, "D3D12 acquire semaphore signal failed");
        }
    }
    return swapchain->swapchain->GetCurrentBackBufferIndex();
}

TextureHandle D3D12Device::swapchain_texture(SwapchainHandle handle, u32 index) const noexcept {
    const D3D12Swapchain* swapchain = swapchains_.resolve(handle);
    return swapchain != nullptr && index < swapchain->info.image_count ? swapchain->textures[index]
                                                                       : TextureHandle{};
}

TextureViewHandle D3D12Device::swapchain_view(SwapchainHandle handle, u32 index) const noexcept {
    const D3D12Swapchain* swapchain = swapchains_.resolve(handle);
    return swapchain != nullptr && index < swapchain->info.image_count ? swapchain->views[index]
                                                                       : TextureViewHandle{};
}

Status D3D12Device::present(SwapchainHandle handle, u32 image_index, SemaphoreHandle wait) {
    D3D12Swapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr || image_index >= swapchain->info.image_count) {
        return fail(ErrorCode::OutOfRange, "D3D12 present image is invalid");
    }
    if (!wait.is_null()) {
        D3D12Semaphore* semaphore = semaphores_.resolve(wait);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "D3D12 present semaphore is stale");
        }
        if (FAILED(queues_[0]->Wait(semaphore->fence.Get(), semaphore->next_wait++))) {
            return fail(ErrorCode::Unavailable, "D3D12 present semaphore wait failed");
        }
    }
    const UINT sync = swapchain->info.present_mode == PresentMode::Immediate ? 0 : 1;
    const UINT flags =
        swapchain->info.present_mode == PresentMode::Immediate ? DXGI_PRESENT_ALLOW_TEARING : 0;
    if (FAILED(swapchain->swapchain->Present(sync, flags))) {
        return fail(ErrorCode::Unavailable, "DXGI present failed");
    }
    return ok();
}

void D3D12Device::publish_memory_pressure() noexcept {
    u64 total = 0;
    for (u64 bytes : memory_.live_bytes) {
        total += bytes;
    }
    memory_.device_heap_used = total;
    if (total > reported_gpu_bytes_) {
        domain_record_allocation(MemoryDomain::Gpu, total - reported_gpu_bytes_);
    } else if (reported_gpu_bytes_ > total) {
        domain_record_free(MemoryDomain::Gpu, reported_gpu_bytes_ - total);
    }
    reported_gpu_bytes_ = total;
    (void)update_memory_pressure();
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::resource_gpu(u32 index) const noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = resource_heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * resource_stride_;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::sampler_gpu(u32 index) const noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = sampler_heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * sampler_stride_;
    return handle;
}

u32 D3D12Device::allocate_resource_descriptors(u32 count) noexcept {
    if (count > kResourceDescriptorCapacity - next_resource_descriptor_) {
        return ~0U;
    }
    const u32 base = next_resource_descriptor_;
    next_resource_descriptor_ += count;
    return base;
}

u32 D3D12Device::allocate_sampler_descriptors(u32 count) noexcept {
    if (count > kSamplerDescriptorCapacity - next_sampler_descriptor_) {
        return ~0U;
    }
    const u32 base = next_sampler_descriptor_;
    next_sampler_descriptor_ += count;
    return base;
}

Expected<Device*, Error> create_d3d12_device(Allocator& allocator,
                                             const DeviceDescription& desc) noexcept {
    void* storage = allocator.allocate(sizeof(D3D12Device), alignof(D3D12Device));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no memory for a D3D12 device");
    }
    auto* device = ::new (storage) D3D12Device(allocator, desc);
    if (Status initialized = device->initialize(); !initialized) {
        const Error error = initialized.error();
        device->~D3D12Device();
        allocator.deallocate(device, sizeof(D3D12Device), alignof(D3D12Device));
        return make_unexpected(error);
    }
    return static_cast<Device*>(device);
}

void destroy_d3d12_device(Allocator& allocator, Device* device) noexcept {
    if (device == nullptr) {
        return;
    }
    auto* native = static_cast<D3D12Device*>(device);
    native->~D3D12Device();
    allocator.deallocate(native, sizeof(D3D12Device), alignof(D3D12Device));
}

}  // namespace cy::rhi::d3d12
