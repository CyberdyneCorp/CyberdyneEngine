// SPDX-License-Identifier: MIT
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/device.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/platform/ios_display_server.h>
#include <cy/platform/ios_platform.h>
#include <cy/platform/ios_touch_input_source.h>
#include <cy/servers/input/server.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

constexpr char kWorldShader[] = R"msl(
#include <metal_stdlib>
using namespace metal;

struct Raster { float4 position [[position]]; float2 uv; };
struct Frame { float time; float aspect; float2 pad; };

vertex Raster world_vertex(uint id [[vertex_id]]) {
    const float2 p[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    Raster out;
    out.position = float4(p[id], 0.0, 1.0);
    out.uv = p[id] * 0.5 + 0.5;
    return out;
}

float terrain(float2 p) {
    float h = 0.0;
    float a = 0.55;
    for (int i = 0; i < 6; ++i) {
        h += a * sin(p.x * 0.71 + cos(p.y * 0.43)) * cos(p.y * 0.63 - p.x * 0.19);
        p = float2(p.x * 1.73 - p.y * 0.29, p.x * 0.29 + p.y * 1.73);
        a *= 0.48;
    }
    return h * 0.72;
}

float map_world(float3 p) { return p.y - terrain(p.xz); }

float3 normal_at(float3 p) {
    const float e = 0.02;
    return normalize(float3(map_world(p + float3(e,0,0)) - map_world(p - float3(e,0,0)),
                            2.0 * e,
                            map_world(p + float3(0,0,e)) - map_world(p - float3(0,0,e))));
}

fragment float4 world_fragment(Raster in [[stage_in]], constant Frame& frame [[buffer(0)]]) {
    const float day = fract(frame.time / 28.0);
    const float angle = day * 6.2831853 - 1.5707963;
    const float3 sun = normalize(float3(cos(angle), sin(angle), -0.35));
    const float daylight = smoothstep(-0.13, 0.16, sun.y);
    const float2 screen = float2((in.uv.x * 2.0 - 1.0) * frame.aspect,
                                 in.uv.y * 2.0 - 1.0);
    const float3 camera = float3(sin(frame.time * 0.035) * 3.0, 2.1,
                                 5.8 + cos(frame.time * 0.035) * 3.0);
    const float3 target = float3(0.0, 0.15, 0.0);
    const float3 forward = normalize(target - camera);
    const float3 right = normalize(cross(forward, float3(0,1,0)));
    const float3 up = cross(right, forward);
    const float3 ray = normalize(forward * 1.55 + right * screen.x + up * screen.y);

    const float3 day_sky = mix(float3(0.62,0.77,0.92), float3(0.08,0.32,0.68), in.uv.y);
    const float stars = step(0.997, fract(sin(dot(floor(in.uv * 520.0), float2(12.9898,78.233))) * 43758.5453));
    float3 color = mix(float3(0.006,0.012,0.04) + stars * 0.65, day_sky, daylight);
    const float sun_disk = pow(max(dot(ray, sun), 0.0), 900.0);
    color += float3(1.0,0.67,0.28) * sun_disk * (0.4 + daylight);

    float distance = 0.0;
    float3 point = camera;
    bool hit = false;
    for (int step = 0; step < 92; ++step) {
        point = camera + ray * distance;
        const float d = map_world(point);
        if (d < 0.006) { hit = true; break; }
        distance += clamp(d * 0.42, 0.025, 0.34);
        if (distance > 28.0) break;
    }
    if (hit) {
        const float3 n = normal_at(point);
        const float diffuse = max(dot(n, sun), 0.0);
        const float snow = smoothstep(0.58, 1.08, point.y) * smoothstep(0.52, 0.9, n.y);
        const float rock = smoothstep(0.22, 0.72, 1.0 - n.y);
        float3 ground = mix(float3(0.06,0.20,0.055), float3(0.26,0.21,0.14), rock);
        ground = mix(ground, float3(0.78,0.84,0.88), snow);
        const float night_ambient = 0.035;
        color = ground * (night_ambient + daylight * (0.18 + diffuse * 1.05));
        color += float3(0.08,0.13,0.23) * max(n.y, 0.0) * (0.25 + daylight * 0.3);
        const float fog = 1.0 - exp(-distance * 0.055);
        color = mix(color, mix(float3(0.01,0.02,0.06), day_sky, daylight), fog);
    }
    color = color / (color + 1.0);
    color = pow(color, float3(1.0 / 2.2));
    return float4(color, 1.0);
}
)msl";

struct FrameConstants { float time; float aspect; float padding[2]; };

void report(const char* operation, const cy::Error& error) {
    std::fprintf(stderr, "CY_IOS_ERROR operation=%s code=%u message=%s\n", operation,
                 static_cast<unsigned>(error.code), error.message);
}

bool verify_ios_contract(cy::IosPlatform& platform, cy::IosDisplayServer& display,
                         cy::WindowId window, CAMetalLayer* layer,
                         cy::input::InputServer& input, cy::IosTouchInputSource& touch) {
    const auto child = platform.spawn_process(cy::ProcessOptions{});
    if (child || child.error().code != cy::ErrorCode::Unsupported) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL subprocess\n");
        return false;
    }
    const auto second_window = display.create_window(cy::WindowDescription{});
    if (second_window || second_window.error().code != cy::ErrorCode::Unsupported) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL second-window\n");
        return false;
    }
    const auto moved = display.set_window_position(window, {1, 1});
    const auto titled = display.set_window_title(window, "unsupported");
    const auto unsynchronised = display.set_window_vsync(window, cy::VSyncMode::Disabled);
    if (moved || titled || unsynchronised || moved.error().code != cy::ErrorCode::Unsupported ||
        titled.error().code != cy::ErrorCode::Unsupported ||
        unsynchronised.error().code != cy::ErrorCode::Unsupported) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL desktop-window-operation\n");
        return false;
    }
    cy::SurfaceDescription surface_description;
    surface_description.api = cy::GraphicsApi::Metal;
    const auto surface = display.create_surface(window, surface_description);
    if (!surface || surface->handle != (__bridge void*)layer) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL metal-surface\n");
        return false;
    }
    touch.began(0.25F, 0.75F, 0.5F, platform.monotonic_nanoseconds());
    if (input.pending().size() != 4) {
        std::fprintf(stderr, "CY_IOS_CONTRACT_FAIL touch-events=%zu\n", input.pending().size());
        return false;
    }
    input.resolve_tick(0, platform.monotonic_nanoseconds(), 1.0F / 60.0F);
    std::printf("CY_IOS_CONTRACT platform=ios window=single desktop_ops=unsupported surface=metal touch_events=4\n");
    std::fflush(stdout);
    return true;
}

class MobileWorldRenderer {
public:
    ~MobileWorldRenderer() { shutdown(); }

    bool start(CAMetalLayer* layer, cy::Extent pixels) {
        allocator_ = &cy::system_allocator(cy::MemoryDomain::Gpu);
        (void)cy::rhi::metal::register_metal_backend();
        cy::rhi::DeviceDescription device_description;
        device_description.application_name = "Cyberdyne iOS Open World";
        cy::rhi::BackendSelection selection;
        auto created = cy::rhi::create_device(*allocator_, cy::rhi::metal::kMetalBackendName,
                                              device_description, selection);
        if (!created) { report("create-device", created.error()); return false; }
        device_ = *created;

        cy::rhi::SwapchainDescription swapchain_description;
        swapchain_description.name = "iOS open world";
        swapchain_description.native_surface = (__bridge void*)layer;
        swapchain_description.extent = {static_cast<cy::u32>(pixels.width),
                                        static_cast<cy::u32>(pixels.height)};
        auto swapchain = device_->create_swapchain(swapchain_description);
        if (!swapchain) { report("create-swapchain", swapchain.error()); return false; }
        swapchain_ = *swapchain;

        auto acquired = device_->create_semaphore();
        auto rendered = device_->create_semaphore();
        if (!acquired || !rendered) return false;
        acquired_ = *acquired;
        rendered_ = *rendered;

        cy::Span<const cy::u8> source(reinterpret_cast<const cy::u8*>(kWorldShader),
                                      sizeof(kWorldShader) - 1);
        cy::rhi::ShaderModuleDescription shader;
        shader.native = source;
        shader.native_format = cy::rhi::ShaderFormat::Msl;
        shader.stage = cy::rhi::ShaderStage::Vertex;
        shader.entry_point = "world_vertex";
        auto vertex = device_->create_shader_module(shader);
        if (!vertex) { report("vertex-shader", vertex.error()); return false; }
        vertex_ = *vertex;
        shader.stage = cy::rhi::ShaderStage::Fragment;
        shader.entry_point = "world_fragment";
        auto fragment = device_->create_shader_module(shader);
        if (!fragment) { report("fragment-shader", fragment.error()); return false; }
        fragment_ = *fragment;

        cy::rhi::PushConstantRange range{cy::rhi::ShaderStage::Fragment, 0,
                                         sizeof(FrameConstants)};
        cy::rhi::PipelineLayoutDescription layout_description;
        layout_description.push_constants = {&range, 1};
        auto layout_handle = device_->create_pipeline_layout(layout_description);
        if (!layout_handle) { report("pipeline-layout", layout_handle.error()); return false; }
        layout_ = *layout_handle;

        const auto info = device_->swapchain_info(swapchain_);
        cy::rhi::ColorAttachmentState color{.format = info.format};
        cy::rhi::GraphicsPipelineDescription pipeline_description;
        pipeline_description.name = "mobile open world";
        pipeline_description.layout = layout_;
        pipeline_description.vertex_shader = vertex_;
        pipeline_description.fragment_shader = fragment_;
        pipeline_description.rasterisation.cull_mode = cy::rhi::CullMode::None;
        pipeline_description.color_attachments = {&color, 1};
        auto pipeline = device_->create_graphics_pipeline(pipeline_description);
        if (!pipeline) { report("graphics-pipeline", pipeline.error()); return false; }
        pipeline_ = *pipeline;
        started_ = true;
        began_ = std::chrono::steady_clock::now();
        std::printf("CY_IOS_READY backend=%s device=%s size=%dx%d\n", selection.selected,
                    device_->capabilities().device_name(), pixels.width, pixels.height);
        std::fflush(stdout);
        return true;
    }

    bool resize(cy::Extent pixels) {
        if (!started_) return false;
        return device_->resize_swapchain(swapchain_, {static_cast<cy::u32>(pixels.width),
                                                      static_cast<cy::u32>(pixels.height)}).has_value();
    }

    bool draw() {
        if (!started_) return false;
        auto frame = device_->begin_frame();
        if (!frame) { report("begin-frame", frame.error()); return false; }
        auto image = device_->acquire_next_image(swapchain_, acquired_, 1'000'000'000ULL);
        if (!image) { (void)device_->end_frame(); return false; }
        auto command = device_->acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
        if (!command || !device_->begin_command_buffer(*command)) return false;
        cy::rhi::CommandBuffer* commands = device_->command_buffer(*command);
        const auto info = device_->swapchain_info(swapchain_);
        cy::rhi::RenderAttachment attachment;
        attachment.view = device_->swapchain_view(swapchain_, *image);
        attachment.load = cy::rhi::LoadOp::Clear;
        attachment.store = cy::rhi::StoreOp::Store;
        cy::rhi::RenderingInfo rendering;
        rendering.render_area = {0, 0, info.extent.width, info.extent.height};
        rendering.color_attachments = {&attachment, 1};
        commands->begin_rendering(rendering);
        commands->set_viewport({0, 0, static_cast<float>(info.extent.width),
                                static_cast<float>(info.extent.height), 0, 1});
        commands->set_scissor({0, 0, info.extent.width, info.extent.height});
        commands->bind_graphics_pipeline(pipeline_);
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - began_).count();
        FrameConstants constants{seconds, static_cast<float>(info.extent.width) /
                                              static_cast<float>(info.extent.height), {0, 0}};
        commands->push_constants(layout_, cy::rhi::ShaderStage::Fragment, 0,
                                 {reinterpret_cast<const cy::u8*>(&constants), sizeof(constants)});
        commands->draw(3, 1, 0, 0);
        commands->end_rendering();
        if (!device_->end_command_buffer(*command)) return false;
        cy::rhi::SubmitInfo submit;
        submit.command_buffers = {&*command, 1};
        submit.wait_binary = acquired_;
        submit.signal_binary = rendered_;
        auto signal = device_->submit(submit);
        const bool presented = signal && device_->present(swapchain_, *image, rendered_).has_value();
        (void)device_->end_frame();
        ++frames_;
        return presented;
    }

    cy::u64 frames() const { return frames_; }

private:
    void shutdown() {
        if (device_ == nullptr) return;
        (void)device_->wait_idle();
        if (!pipeline_.is_null()) device_->destroy_graphics_pipeline(pipeline_);
        if (!layout_.is_null()) device_->destroy_pipeline_layout(layout_);
        if (!fragment_.is_null()) device_->destroy_shader_module(fragment_);
        if (!vertex_.is_null()) device_->destroy_shader_module(vertex_);
        if (!rendered_.is_null()) device_->destroy_semaphore(rendered_);
        if (!acquired_.is_null()) device_->destroy_semaphore(acquired_);
        if (!swapchain_.is_null()) device_->destroy_swapchain(swapchain_);
        cy::rhi::destroy_device(*allocator_, device_);
        device_ = nullptr;
    }

    cy::Allocator* allocator_ = nullptr;
    cy::rhi::Device* device_ = nullptr;
    cy::rhi::SwapchainHandle swapchain_{};
    cy::rhi::SemaphoreHandle acquired_{};
    cy::rhi::SemaphoreHandle rendered_{};
    cy::rhi::ShaderModuleHandle vertex_{};
    cy::rhi::ShaderModuleHandle fragment_{};
    cy::rhi::PipelineLayoutHandle layout_{};
    cy::rhi::GraphicsPipelineHandle pipeline_{};
    std::chrono::steady_clock::time_point began_{};
    cy::u64 frames_ = 0;
    bool started_ = false;
};

}  // namespace

@protocol CyberdyneTouchSink
- (void)touchBeganAt:(CGPoint)point pressure:(CGFloat)pressure;
- (void)touchMovedTo:(CGPoint)point pressure:(CGFloat)pressure;
- (void)touchEndedAt:(CGPoint)point;
@end

@interface CyberdyneMetalView : UIView
@property(nonatomic, weak) id<CyberdyneTouchSink> touchSink;
@end
@implementation CyberdyneMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    const CGFloat pressure = touch.maximumPossibleForce > 0
                                 ? touch.force / touch.maximumPossibleForce
                                 : 1.0;
    [self.touchSink touchBeganAt:[touch locationInView:self] pressure:pressure];
    (void)event;
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    const CGFloat pressure = touch.maximumPossibleForce > 0
                                 ? touch.force / touch.maximumPossibleForce
                                 : 1.0;
    [self.touchSink touchMovedTo:[touch locationInView:self] pressure:pressure];
    (void)event;
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self.touchSink touchEndedAt:[touches.anyObject locationInView:self]];
    (void)event;
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self touchesEnded:touches withEvent:event];
}
@end

@interface CyberdyneViewController : UIViewController <CyberdyneTouchSink> @end

@implementation CyberdyneViewController {
    cy::IosPlatform _platform;
    cy::IosDisplayServer _display;
    std::optional<cy::input::InputServer> _input;
    cy::IosTouchInputSource _touch;
    MobileWorldRenderer _renderer;
    CADisplayLink* _displayLink;
    UILabel* _stats;
    CFTimeInterval _sampleStart;
    cy::u64 _sampleFrames;
}

- (void)loadView {
    CyberdyneMetalView* view =
        [[CyberdyneMetalView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    view.touchSink = self;
    self.view = view;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    layer.framebufferOnly = NO;
    const CGFloat scale = UIScreen.mainScreen.nativeScale;
    self.view.contentScaleFactor = scale;
    const CGSize size = self.view.bounds.size;
    layer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
    const cy::Extent pixels{static_cast<cy::i32>(layer.drawableSize.width),
                            static_cast<cy::i32>(layer.drawableSize.height)};
    (void)_display.initialise((__bridge void*)self.view, (__bridge void*)layer, pixels,
                              static_cast<cy::f32>(scale), UIScreen.mainScreen.maximumFramesPerSecond);
    cy::WindowDescription window_description;
    window_description.mode = cy::WindowMode::Fullscreen;
    window_description.flags = cy::WindowFlags::HighDpi;
    const auto window = _display.create_window(window_description);
    _input.emplace(cy::system_allocator(cy::MemoryDomain::Engine));
    if (_input->initialize()) {
        _input->set_backend("ios-uikit", false);
        (void)_touch.attach(*_input, _platform.monotonic_nanoseconds());
    }
    if (!window || !_input ||
        !verify_ios_contract(_platform, _display, *window, layer, *_input, _touch)) {
        std::fprintf(stderr, "CY_IOS_ERROR operation=platform-contract\n");
        std::abort();
    }
    if (!_renderer.start(layer, pixels)) std::abort();

    _stats = [[UILabel alloc] initWithFrame:CGRectMake(18, 18, 360, 72)];
    _stats.textColor = UIColor.whiteColor;
    _stats.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
    _stats.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightSemibold];
    _stats.numberOfLines = 3;
    _stats.text = @"Cyberdyne iOS • Metal\nOpen world • day/night\nmeasuring FPS…";
    [self.view addSubview:_stats];

    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(drawFrame:)];
    [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    _sampleStart = CACurrentMediaTime();
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    const CGFloat scale = self.view.contentScaleFactor;
    const CGSize wanted = CGSizeMake(self.view.bounds.size.width * scale,
                                     self.view.bounds.size.height * scale);
    if (!CGSizeEqualToSize(layer.drawableSize, wanted)) {
        layer.drawableSize = wanted;
        const cy::Extent pixels{static_cast<cy::i32>(wanted.width), static_cast<cy::i32>(wanted.height)};
        _display.update_metrics(pixels, static_cast<cy::f32>(scale),
                                UIScreen.mainScreen.maximumFramesPerSecond);
        _renderer.resize(pixels);
    }
}

- (void)drawFrame:(CADisplayLink*)link {
    if (_renderer.draw()) ++_sampleFrames;
    if (_input) {
        _input->resolve_tick(_renderer.frames(), _platform.monotonic_nanoseconds(),
                             static_cast<cy::f32>(link.duration));
    }
    const CFTimeInterval elapsed = CACurrentMediaTime() - _sampleStart;
    if (elapsed >= 1.0) {
        const double fps = static_cast<double>(_sampleFrames) / elapsed;
        _stats.text = [NSString stringWithFormat:@"Cyberdyne iOS • Metal\nOpen world • day/night\n%.1f FPS • %ld Hz display",
                       fps, (long)UIScreen.mainScreen.maximumFramesPerSecond];
        std::printf("CY_IOS_FPS fps=%.2f frames=%llu seconds=%.3f\n", fps,
                    static_cast<unsigned long long>(_sampleFrames), elapsed);
        std::fflush(stdout);
        _sampleFrames = 0;
        _sampleStart = CACurrentMediaTime();
    }
    (void)link;
}

- (CGPoint)normaliseTouch:(CGPoint)point {
    const CGSize size = self.view.bounds.size;
    return CGPointMake(size.width > 0 ? point.x / size.width : 0,
                       size.height > 0 ? point.y / size.height : 0);
}
- (void)touchBeganAt:(CGPoint)point pressure:(CGFloat)pressure {
    const CGPoint p = [self normaliseTouch:point];
    _touch.began(static_cast<cy::f32>(p.x), static_cast<cy::f32>(p.y),
                 static_cast<cy::f32>(pressure), _platform.monotonic_nanoseconds());
}
- (void)touchMovedTo:(CGPoint)point pressure:(CGFloat)pressure {
    const CGPoint p = [self normaliseTouch:point];
    _touch.moved(static_cast<cy::f32>(p.x), static_cast<cy::f32>(p.y),
                 static_cast<cy::f32>(pressure), _platform.monotonic_nanoseconds());
}
- (void)touchEndedAt:(CGPoint)point {
    const CGPoint p = [self normaliseTouch:point];
    _touch.ended(static_cast<cy::f32>(p.x), static_cast<cy::f32>(p.y),
                 _platform.monotonic_nanoseconds());
}

- (void)dealloc {
    _touch.detach(_platform.monotonic_nanoseconds());
    if (_input) _input->shutdown();
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}
@end

@interface CyberdyneSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation CyberdyneSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
        options:(UISceneConnectionOptions*)connectionOptions {
    UIWindowScene* windowScene = (UIWindowScene*)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    self.window.rootViewController = [CyberdyneViewController new];
    [self.window makeKeyAndVisible];
    (void)session; (void)connectionOptions;
}
@end

@interface CyberdyneAppDelegate : UIResponder <UIApplicationDelegate> @end

@implementation CyberdyneAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application; (void)options;
    return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)application
        configurationForConnectingSceneSession:(UISceneSession*)session
        options:(UISceneConnectionOptions*)connectionOptions {
    (void)application; (void)session; (void)connectionOptions;
    return [[UISceneConfiguration alloc] initWithName:@"Default Configuration"
                                         sessionRole:UIWindowSceneSessionRoleApplication];
}
@end

int main(int argc, char** argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(CyberdyneAppDelegate.class)); }
}
