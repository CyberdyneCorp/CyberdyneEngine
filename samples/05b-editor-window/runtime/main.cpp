// `cy_editor_window_runtime` — the ENGINE, on the far end of the editor's viewport. M7 task 5b.1.
//
// ================================================================================================
// WHAT THIS REPLACES, AND WHY IT IS THE MILESTONE'S MOST VISIBLE OUTCOME
// ================================================================================================
//
// Six milestones of editor work produced a viewport that had never shown anything the engine drew.
// The thing on the far end of the transport was `cy-viewport-publisher` — a Vulkan fixture in the
// editor's own Cargo workspace that clears an image to a colour and moves a white bar — because M6
// shipped only the CONTROL half of the engine's side: `src/servers/render/viewport_transport.h`
// carries frame identity, view state and cost, and its own header says there is no device and no
// swapchain there.
//
// This program is the other half. It renders a real scene through the engine's render graph on a
// real device, publishes the frames over the transport the editor already speaks, and answers the
// editor's gizmo intents with the geometry it drew. The editor did not change to accept it; if it
// had, the wire would have been re-negotiated rather than implemented.
//
// ================================================================================================
// THE FOUR PIECES, AND WHOSE THEY ARE
// ================================================================================================
//
//   cy::sample-first-light      the scene and the renderer. M3's artefact, as a library — a lit,
//                               shadowed, textured, camera-relative frame through the render graph.
//                               Reused rather than reimplemented: a second renderer written for
//                               this artefact would drift from the one the golden images
//                               photograph.
//   cy::backends-viewport       the pixel half of the transport: exported dma-buf images, timeline
//                               semaphores, the shared announcement page, the socket.
//   cy::servers-render          `build_gizmo_layout` — where the handles are, in this frame's
//                               pixels, at a constant screen size.
//   cy::runtime-editor-bridge   the control socket the editor's `--host` connects to.
//
// This file is the loop that joins them, and it holds the one thing none of them can: which of the
// scene's objects the editor's selection means. `session.h` says why that association exists and
// what it is a stand-in for.
//
// ================================================================================================
// USAGE
// ================================================================================================
//
//   cy_editor_window_runtime [--socket PATH] [--host PATH] [--width W] [--height H]
//                            [--buffers N] [--seconds S] [--frames N] [--rate HZ]
//                            [--adapter SUBSTRING] [--orbit TURNS-PER-SECOND]
//                            [--layout PATH] [--no-validation]
//
// `--layout` writes the gizmo layout this runtime published, as one line of JSON, every time it
// publishes one. It is how `window.py` aims a drag at a HANDLE rather than at a coordinate — M7
// task 5b.4, whose defect was that M6's artefact dragged from a hard-coded (47 %, 38 %) hoping a
// handle was there.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/backends/viewport/publisher.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/runtime/editor_bridge/bridge.h>
#include <cy/servers/render/gizmo.h>
#include <cy/servers/render/picking.h>
#include <cy/servers/render/viewport_transport.h>

#include "overlay.h"
#include "session.h"

#include "renderer.h"
#include "scene.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <csignal>

namespace {

using namespace cy;
using namespace cy::sample::editor_window;
namespace first_light = cy::sample::first_light;

constexpr const char* kTag = "editor-window-runtime";

volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_signal(int) {
    g_stop = 1;
}

struct Options {
    const char* socket = "/tmp/cy-viewport.sock";
    const char* host = "";
    const char* adapter = "";
    const char* layout_path = "";
    u32 width = 1280;
    u32 height = 720;
    u32 buffers = 4;
    f64 seconds = 180.0;
    u64 frames = 0;
    f64 rate = 60.0;
    f64 orbit = 0.008;
    bool validation = true;
};

[[nodiscard]] const char* value_of(int argc, char** argv, const char* key,
                                   const char* fallback) noexcept {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], key) == 0) {
            return argv[index + 1];
        }
    }
    return fallback;
}

[[nodiscard]] f64 number_of(int argc, char** argv, const char* key, f64 fallback) noexcept {
    const char* text = value_of(argc, argv, key, nullptr);
    return (text != nullptr) ? std::strtod(text, nullptr) : fallback;
}

[[nodiscard]] bool flag(int argc, char** argv, const char* key) noexcept {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], key) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Options parse(int argc, char** argv) noexcept {
    Options options;
    options.socket = value_of(argc, argv, "--socket", options.socket);
    options.host = value_of(argc, argv, "--host", options.host);
    options.adapter = value_of(argc, argv, "--adapter", options.adapter);
    options.layout_path = value_of(argc, argv, "--layout", options.layout_path);
    options.width = static_cast<u32>(number_of(argc, argv, "--width", options.width));
    options.height = static_cast<u32>(number_of(argc, argv, "--height", options.height));
    options.buffers = static_cast<u32>(number_of(argc, argv, "--buffers", options.buffers));
    options.seconds = number_of(argc, argv, "--seconds", options.seconds);
    options.frames = static_cast<u64>(number_of(argc, argv, "--frames", 0.0));
    options.rate = number_of(argc, argv, "--rate", options.rate);
    options.orbit = number_of(argc, argv, "--orbit", options.orbit);
    options.validation = !flag(argc, argv, "--no-validation");
    return options;
}

[[nodiscard]] u64 monotonic_nanos() noexcept {
    timespec now{};
    (void)::clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<u64>(now.tv_sec) * 1'000'000'000ULL) + static_cast<u64>(now.tv_nsec);
}

void report(const char* what, const Error& error) noexcept {
    std::fprintf(stderr, "%s: %s failed: %s\n", kTag, what, error.message);
}

/// The engine `View` that describes the frame the renderer just drew.
///
/// CAMERA-RELATIVE, exactly as the renderer is: the camera sits at the origin of the space its own
/// positions are relative to, so the view matrix is a rotation with no translation in it. That is
/// what makes `cy::render::project_to_pixel` and `first_light::view_projection` agree — they are
/// the same two matrices, built from the same three numbers, and a gizmo placed by one and drawn by
/// the other would otherwise be off by whatever the two disagreed about.
[[nodiscard]] render::View view_of(const first_light::Camera& camera, u32 width,
                                   u32 height) noexcept {
    render::View view;
    view.desc.purpose = render::ViewPurpose::EditorViewport;
    view.desc.viewport = render::ViewportRect{0, 0, width, height};
    view.desc.projection.kind = render::ProjectionKind::Perspective;
    view.desc.projection.fov_y_radians = camera.fov_y_radians;
    view.desc.projection.near_plane = camera.near_plane;
    view.desc.projection.far_plane = 0.0F;  // infinite, which is what the renderer builds

    // The camera looks down its own −Z, so the basis is (right, up, −forward).
    const Vec3 forward = normalize(camera.forward);
    const Vec3 right = normalize(cross(forward, camera.up));
    const Vec3 up = cross(right, forward);
    view.desc.camera.rotation = Quat::from_basis(right, up, -forward);
    view.desc.camera.translation = Vec3{0.0F, 0.0F, 0.0F};
    view.refresh();
    return view;
}

/// The rotation a camera's forward and up describe. The inverse of `Camera::forward`/`up`.
[[nodiscard]] Quat rotation_of(const first_light::Camera& camera) noexcept {
    const Vec3 forward = normalize(camera.forward);
    const Vec3 right = normalize(cross(forward, camera.up));
    const Vec3 up = cross(right, forward);
    return Quat::from_basis(right, up, -forward);
}

/// The phase of the scene's own orbit the runtime offers the editor as an opening view.
///
/// A quarter turn in: the pillar is off centre, the ring of boxes reads as a ring, and the sun is
/// behind the camera's shoulder — which is the framing `render.golden` photographs and therefore
/// the one a reader of this project has already seen.
constexpr f32 kFramingPhase = 0.12F;

/// One object's position, relative to the camera. The space everything on this side works in.
[[nodiscard]] Vec3 relative_position(const first_light::Object& object,
                                     const first_light::Camera& camera) noexcept {
    return Vec3{static_cast<f32>(object.world_position[0] - camera.position[0]),
                static_cast<f32>(object.world_position[1] - camera.position[1]),
                static_cast<f32>(object.world_position[2] - camera.position[2])};
}

/// Write the published layout where the artefact's driver can read it.
///
/// One line of JSON, rewritten each time: a driver that wants to aim at a handle reads the file and
/// gets the newest answer. It is written to a temporary and renamed, so a reader never sees half a
/// line — the driver and the runtime are two processes and the file is the only thing between them.
void write_layout(const char* path, const render::GizmoLayout& layout) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    char temporary[512] = {};
    (void)std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    std::FILE* file = std::fopen(temporary, "w");
    if (file == nullptr) {
        return;
    }
    (void)std::fprintf(file, R"({"frame":%llu,"mode":"%s","centre":[%.2f,%.2f],)",
                       static_cast<unsigned long long>(layout.frame_id),
                       render::gizmo_mode_name(layout.mode), static_cast<double>(layout.centre_x),
                       static_cast<double>(layout.centre_y));
    (void)std::fprintf(file, R"("extent":%.2f,"handles":{)", static_cast<double>(layout.extent));
    bool first = true;
    for (const render::GizmoHandleSpot& spot : layout.spots) {
        (void)std::fprintf(file, R"(%s"%s":[%.2f,%.2f,%.2f])", first ? "" : ",",
                           render::gizmo_handle_name(spot.handle), static_cast<double>(spot.x),
                           static_cast<double>(spot.y), static_cast<double>(spot.radius));
        first = false;
    }
    (void)std::fprintf(file, "}}\n");
    (void)std::fclose(file);
    (void)std::rename(temporary, path);
}

/// Everything the loop needs, gathered so the frame function is readable.
struct Host {
    Options options;
    first_light::Scene* scene = nullptr;
    first_light::Renderer* renderer = nullptr;
    viewport::Publisher* publisher = nullptr;
    /// M6'S CONTROL HALF, JOINED TO M7'S PIXEL HALF. `cy::render::ViewportTransport` records the
    /// frame's identity, the view state it was rendered with, and what it cost; its own header says
    /// the bytes "are the business of the module that owns a device", and until this artefact
    /// nothing owned one, so it was reachable from its tests and from nothing else.
    ///
    /// It is published to with the SAME frame identity the dma-buf ring announces, so the pacing it
    /// reports is about the frames the editor actually received rather than about a second count.
    render::ViewportTransport transport{render::ViewportTransportKind::SharedTexture};
    runtime::EditorBridge* bridge = nullptr;
    EditorSession session;
    /// The gizmo in the FRAME's pixels, which is what is drawn into the frame.
    render::GizmoLayout layout;
    /// The size the editor last said its viewport is, or zero. What the published layout is scaled
    /// into — see `cy::render::rescale_gizmo_layout` and `GizmoIntent::viewport_width`.
    u32 asked_width = 0;
    u32 asked_height = 0;
    /// The camera the editor last asked to see, and whether it has asked at all.
    ///
    /// THE FRAME IS THE EDITOR'S VIEW, which is what makes the editor's manipulation arithmetic
    /// and this runtime's gizmo geometry two halves of one thing rather than two answers. Until an
    /// editor asks, the runtime's own orbit is what is rendered — which is what a headless run and
    /// a `cy-viewport-transport-probe` see.
    first_light::Camera asked_camera{};
    bool editor_camera = false;
    render::View view;
    first_light::Camera camera{};
    u64 frames_published = 0;
    u64 gizmos_answered = 0;
    u64 moves_applied = 0;
    /// What the editor sent, by kind. Printed at the end rather than logged per message: an
    /// artefact that reports "0 gizmos answered" needs to be able to say whether the editor asked
    /// and the runtime refused, or whether nothing arrived at all — which is two very different
    /// places to look, and the difference cost this milestone an afternoon.
    u64 received[static_cast<usize>(runtime::EditorMessage::GizmoGeometry) + 1] = {};
    u64 unknown_messages = 0;
};

/// Take the camera the editor asked for, when it asked for one.
///
/// A zero field of view means the editor did not say — an orthographic viewport, or an editor built
/// before the intent carried a camera — and the runtime keeps rendering its own view. Anything else
/// is the editor navigating, and the frame follows it.
void adopt_camera(Host& host, const render::GizmoIntent& intent) noexcept {
    if (!(intent.fov_y_radians > 0.0F)) {
        return;
    }
    const Quat rotation{intent.camera_rotation[0], intent.camera_rotation[1],
                        intent.camera_rotation[2], intent.camera_rotation[3]};
    host.asked_camera.position[0] = static_cast<f64>(intent.camera_position[0]);
    host.asked_camera.position[1] = static_cast<f64>(intent.camera_position[1]);
    host.asked_camera.position[2] = static_cast<f64>(intent.camera_position[2]);
    // The camera looks down its local −Z, which is the same convention on both sides.
    host.asked_camera.forward = normalize(rotation * Vec3{0.0F, 0.0F, -1.0F});
    host.asked_camera.up = normalize(rotation * Vec3{0.0F, 1.0F, 0.0F});
    host.asked_camera.fov_y_radians = intent.fov_y_radians;
    host.asked_camera.near_plane = (intent.near_plane > 0.0F) ? intent.near_plane : 0.1F;
    host.editor_camera = true;
}

/// Send the gizmo the editor asked for, in the pixels the editor asked about.
///
/// TWO LAYOUTS, ONE COMPUTATION. `host.layout` is in the frame's pixels and is what `draw_gizmo`
/// puts on the screen; what the editor receives is that same layout moved into the viewport's
/// pixels, because the editor stretches the frame to fill its panel and hit-tests a pointer in the
/// panel's coordinates. Rescaling rather than recomputing is what keeps "what is grabbed is what
/// was drawn" true across the stretch — a second `build_gizmo_layout` at the other size would be a
/// second answer.
[[nodiscard]] Status publish_layout(Host& host, u64 request) noexcept {
    render::GizmoLayout published;
    published.frame_id = host.layout.frame_id;
    published.mode = host.layout.mode;
    published.centre_x = host.layout.centre_x;
    published.centre_y = host.layout.centre_y;
    published.extent = host.layout.extent;
    if (Status copied = published.spots.append(host.layout.spots.span()); !copied) {
        return copied;
    }
    render::rescale_gizmo_layout(published, host.options.width, host.options.height,
                                 host.asked_width, host.asked_height);
    Array<u8> bytes;
    if (Status encoded = render::encode_gizmo_layout(published, bytes); !encoded) {
        return encoded;
    }
    if (Status sent = host.bridge->send_gizmo_geometry(request, bytes.span()); !sent) {
        return sent;
    }
    write_layout(host.options.layout_path, published);
    return ok();
}

/// Answer one gizmo intent: where this runtime would draw the handles, in the frame it just
/// published.
void answer_gizmo(Host& host, const runtime::EditorRequest& request) noexcept {
    render::GizmoIntent intent;
    if (Status decoded = render::decode_gizmo_intent(request.payload, intent); !decoded) {
        (void)host.bridge->send_rejected(request.request, "read the gizmo intent",
                                         "rebuild the editor and the runtime together");
        return;
    }
    host.session.set_mode(intent.mode);
    host.asked_width = intent.viewport_width;
    host.asked_height = intent.viewport_height;
    adopt_camera(host, intent);
    const u32 object_count = static_cast<u32>(host.scene->objects().size());
    if (intent.identities.empty() || object_count == 0) {
        // An empty selection is a REQUEST, not an absence: it must take the gizmo off the screen.
        host.session.anchor(EditorSession::kNoObject);
        host.layout.spots.clear();
        host.layout.extent = 0.0F;
        // THE FRAME THE INTENT NAMED, which is inside the intent rather than on the message.
        // `Message::GizmoIntent` carries a request and a viewport and no frame — the frame is the
        // first field of `cy_editor_services::gizmo::Request` — and answering with the message's
        // empty `frame` publishes every layout as frame 0, which the editor then refuses because
        // it names a frame the viewport is not showing. The gizmo simply never appears, and both
        // sides are individually correct.
        host.layout.frame_id = intent.frame_id;
        (void)publish_layout(host, request.request);
        return;
    }
    const u32 object = host.session.object_for(intent.identities[0], object_count);
    host.session.anchor(object);

    const Vec3 pivot = relative_position(host.scene->objects()[object], host.camera);
    // THE FRAME THE EDITOR IS SHOWING, not the one this runtime has since rendered. The editor
    // refuses a layout that names a different frame, and it is right to: hit-testing a click
    // against a layout from another frame is the same defect as resolving a pick against a newer
    // camera.
    host.layout = render::build_gizmo_layout(
        host.view, pivot, render::gizmo_axes(intent, host.view, Quat::identity()), intent.mode,
        intent.frame_id);
    if (Status sent = publish_layout(host, request.request); sent) {
        host.gizmos_answered += 1;
    }
}

/// Apply what the editor committed, so that the frame the editor is looking at moves with it.
void apply_transaction(Host& host, const runtime::EditorRequest& request) noexcept {
    Array<TranslationDelta> deltas;
    const Expected<u32, Error> operations = read_translations(request.payload, deltas);
    if (!operations) {
        // Declining is not a failure of the editor's edit: the document has already recorded it,
        // and this runtime simply has nothing to do with a create or a delete until the worlds are
        // shared. The editor is told so rather than left waiting.
        (void)host.bridge->send_rejected(request.request, operations.error().message,
                                         "this runtime holds its own scene; M8's live editing "
                                         "shares one");
        return;
    }
    const u32 object_count = static_cast<u32>(host.scene->objects().size());
    // A Vec3 that changed is a translation only while the editor's stated mode is a move. See
    // session.h: the identifiers in a transaction are the document's, so this is the signal the
    // runtime legitimately has, and it refuses to guess at a scale.
    const bool moving = host.session.mode() == render::GizmoMode::Translate ||
                        host.session.mode() == render::GizmoMode::Universal;
    for (const TranslationDelta& delta : deltas) {
        if (!moving || object_count == 0) {
            continue;
        }
        const u32 object = host.session.object_for(delta.identity, object_count);
        // INTO THE SCENE, so the next frame the editor sees has the object where the drag put it.
        // The gizmo's pivot is read back out of the same place, so the handles follow the box
        // rather than the box following a copy of the handles.
        first_light::Object& moved = host.scene->objects_mutable()[object];
        moved.world_position[0] += static_cast<f64>(delta.amount.x);
        moved.world_position[1] += static_cast<f64>(delta.amount.y);
        moved.world_position[2] += static_cast<f64>(delta.amount.z);
        host.moves_applied += 1;
    }
    (void)host.bridge->send_applied(request.request, request.frame, request.payload);
}

void serve_editor(Host& host) noexcept {
    const u64 before = host.bridge->connections();
    host.bridge->service();
    if (host.bridge->connections() != before) {
        std::fprintf(stdout, "%s: an editor attached to the control socket\n", kTag);
        (void)std::fflush(stdout);
    }
    runtime::EditorRequest request;
    while (host.bridge->poll(request)) {
        if (request.kind == runtime::EditorMessage::Unknown) {
            host.unknown_messages += 1;
        } else {
            host.received[static_cast<usize>(request.kind)] += 1;
        }
        switch (request.kind) {
            case runtime::EditorMessage::Hello: {
                (void)host.bridge->send_welcome(request.abi_major, request.abi_minor,
                                                "cy_editor_window_runtime");
                // AND WHERE THE CONTENT IS, once. See `EditorBridge::send_view_suggestion`: a
                // viewport opens at the origin looking down −Z, and this runtime is the only side
                // that knows where its world is. Sent on the handshake rather than on every frame,
                // because a runtime that re-aimed the camera continuously would own it.
                const first_light::Camera framed = host.scene->camera_at(kFramingPhase);
                const f32 position[3] = {static_cast<f32>(framed.position[0]),
                                         static_cast<f32>(framed.position[1]),
                                         static_cast<f32>(framed.position[2])};
                const Quat rotation = rotation_of(framed);
                const f32 lanes[4] = {rotation.x, rotation.y, rotation.z, rotation.w};
                (void)host.bridge->send_view_suggestion(position, lanes, framed.fov_y_radians,
                                                        framed.near_plane);
                break;
            }
            case runtime::EditorMessage::Ping:
                (void)host.bridge->send_pong(request.frame);
                break;
            case runtime::EditorMessage::GizmoIntent:
                answer_gizmo(host, request);
                break;
            case runtime::EditorMessage::Apply:
                apply_transaction(host, request);
                break;
            case runtime::EditorMessage::Pick:
                // ENGINE-SIDE PICKING NEEDS THE DRAW LIST THIS FRAME PRODUCED, and this renderer is
                // M3's: it draws from a scene rather than publishing `GpuInstance` records, so
                // there is nothing for `cy::render::pick_ray` to resolve against. Refused by name
                // rather than answered with a guess — an invented hit is the forbidden pattern
                // `editor-viewport-and-gizmos` names, and a refusal the editor can show is the
                // honest answer until the render server drives this frame.
                (void)host.bridge->send_rejected(
                    request.request, "resolve a pick against this frame",
                    "this runtime renders through M3's sample renderer, which publishes no draw "
                    "list for cy::render::pick_ray to resolve against");
                break;
            default:
                break;
        }
    }
}

/// Render one frame, composite the gizmo into it, and publish it.
[[nodiscard]] bool publish_frame(Host& host, f32 phase) noexcept {
    host.camera = host.editor_camera ? host.asked_camera : host.scene->camera_at(phase);
    host.view = view_of(host.camera, host.options.width, host.options.height);

    const Expected<first_light::FrameReport, Error> frame =
        host.renderer->render(*host.scene, host.camera);
    if (!frame) {
        report("frame", frame.error());
        return false;
    }
    const Span<const u32> texels = host.renderer->color_texels();
    if (texels.empty()) {
        report("readback", Error{ErrorCode::Unavailable, "the renderer read back no pixels"});
        return false;
    }

    viewport::FrameStaging staging = host.publisher->begin_frame();
    if (!staging.valid()) {
        return true;  // the ring is full; the frame is this runtime's to drop
    }
    const usize bytes = static_cast<usize>(staging.width) * staging.height * 4U;
    std::memcpy(staging.pixels, texels.data(), bytes);

    // The gizmo, drawn from the layout that was published for the frame the editor is showing. It
    // is the SAME layout, not a second computation: what a person aims at and what the editor
    // hit-tests came out of one call to `build_gizmo_layout`.
    const Canvas canvas{staging.pixels, staging.width, staging.height};
    if (host.session.anchored() != EditorSession::kNoObject && !host.layout.empty()) {
        const u32 object = host.session.anchored();
        const Vec3 pivot = relative_position(host.scene->objects()[object], host.camera);
        Vec2 marker{0.0F, 0.0F};
        if (project_to_pixel(host.view, pivot, marker)) {
            draw_selection_marker(canvas, marker.x, marker.y, 26.0F);
        }
        // Rebuilt for THIS frame's camera, and republished with it, so the drawn gizmo and the
        // published one are the same handles even while the camera moves.
        host.layout = render::build_gizmo_layout(host.view, pivot, Quat::identity(),
                                                 host.session.mode(), host.layout.frame_id);
        draw_gizmo(canvas, host.layout, render::GizmoHandle::Count);
    }

    const Expected<u64, Error> published = host.publisher->publish();
    if (!published) {
        report("publish", published.error());
        return false;
    }
    // AND THE CONTROL HALF, with the frame the pixel half just announced. The two carry the same
    // identity on purpose: `editor-viewport-and-gizmos` requires the transport to carry "the
    // frame's view state and identifiers … so that picking, gizmo interaction, and overlay
    // alignment are correct for the frame actually presented", and a control record whose number
    // did not match the image's would be a second answer to which frame is on screen.
    const u64 produced_micros = monotonic_nanos() / 1000ULL;
    if (Expected<render::ViewportFrameId, Error> recorded =
            host.transport.publish(host.view, produced_micros, render::TextureHandle{});
        !recorded) {
        report("transport", recorded.error());
    }
    host.layout.frame_id = *published;
    host.frames_published += 1;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    (void)std::signal(SIGPIPE, SIG_IGN);
    (void)std::signal(SIGTERM, on_signal);
    (void)std::signal(SIGINT, on_signal);

    const Options options = parse(argc, argv);
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);

    (void)rhi::null::register_null_backend();
    (void)rhi::vulkan::register_vulkan_backend();

    rhi::DeviceDescription description;
    description.application_name = "cy_editor_window_runtime";
    description.enable_validation = options.validation;
    description.enable_synchronisation_validation = options.validation;
    rhi::BackendSelection selection;
    // "vulkan" BY NAME, not the default. A fall-back to the null backend here would produce a
    // process that starts, publishes nothing and reports success — which is the shape of failure
    // this milestone exists to stop, so it is refused instead.
    const Expected<rhi::Device*, Error> device =
        rhi::create_device(allocator, "vulkan", description, selection);
    if (!device) {
        report("device", device.error());
        return 1;
    }
    std::fprintf(stdout, "%s: device   backend=%s\n", kTag,
                 selection.selected != nullptr ? selection.selected : "(none)");

    first_light::SceneDescription scene_description;
    scene_description.box_count = 6;
    scene_description.sun_shadows = true;
    first_light::Scene scene(allocator);
    if (Status built = scene.build(scene_description); !built) {
        report("scene", built.error());
        rhi::destroy_device(allocator, device.value());
        return 1;
    }

    first_light::RendererOptions renderer_options;
    renderer_options.width = options.width;
    renderer_options.height = options.height;
    renderer_options.readback = true;

    int exit_code = 0;
    {
        first_light::Renderer renderer(allocator, *device.value());
        if (Status prepared = renderer.prepare(scene, renderer_options); !prepared) {
            report("renderer", prepared.error());
            rhi::destroy_device(allocator, device.value());
            return 1;
        }

        viewport::PublisherOptions publisher_options;
        publisher_options.socket_path = options.socket;
        publisher_options.width = options.width;
        publisher_options.height = options.height;
        publisher_options.buffers = options.buffers;
        publisher_options.adapter = options.adapter;
        publisher_options.validation = options.validation;
        Expected<UniquePtr<viewport::Publisher>, Error> publisher =
            viewport::Publisher::create(publisher_options);
        if (!publisher) {
            report("publisher", publisher.error());
            rhi::destroy_device(allocator, device.value());
            return 1;
        }
        if (const char* advisory = viewport::ring_advisory(options.buffers); advisory != nullptr) {
            std::fprintf(stderr, "%s: warning: %s\n", kTag, advisory);
        }
        std::fprintf(stdout, "%s: publisher %s  %ux%u  %u image(s)  modifier 0x%llx  on %s\n", kTag,
                     options.socket, options.width, options.height, options.buffers,
                     static_cast<unsigned long long>(publisher->get()->modifier()),
                     publisher->get()->adapter_name());

        runtime::EditorBridge bridge;
        if (options.host[0] != '\0') {
            if (Status listening = bridge.listen(options.host); !listening) {
                report("bridge", listening.error());
                rhi::destroy_device(allocator, device.value());
                return 1;
            }
            std::fprintf(stdout, "%s: bridge   %s\n", kTag, options.host);
        }

        Host host;
        host.options = options;
        host.scene = &scene;
        host.renderer = &renderer;
        host.publisher = publisher->get();
        host.bridge = &bridge;
        const u64 started = monotonic_nanos();
        const u64 interval_nanos =
            (options.rate > 0.0) ? static_cast<u64>(1'000'000'000.0 / options.rate) : 0;
        u64 next_due = started;
        while (g_stop == 0) {
            const u64 now = monotonic_nanos();
            const f64 elapsed = static_cast<f64>(now - started) / 1'000'000'000.0;
            if (options.seconds > 0.0 && elapsed > options.seconds) {
                break;
            }
            if (options.frames > 0 && host.frames_published >= options.frames) {
                break;
            }
            if (interval_nanos > 0 && now < next_due) {
                const u64 remaining = next_due - now;
                timespec sleep{static_cast<time_t>(remaining / 1'000'000'000ULL),
                               static_cast<long>(remaining % 1'000'000'000ULL)};
                (void)::nanosleep(&sleep, nullptr);
            }
            // The next deadline, measured from whichever is later: the deadline that was due, or
            // now. A run that fell behind does not then sprint to catch up, which is what adding to
            // a stale deadline would do.
            const u64 base = (next_due > now) ? next_due : now;
            next_due = ((interval_nanos > 0) ? base : now) + interval_nanos;

            host.publisher->service();
            serve_editor(host);
            const f32 phase = static_cast<f32>(elapsed * options.orbit);
            if (!publish_frame(host, phase - std::floor(phase))) {
                exit_code = 1;
                break;
            }
        }

        const render::ViewportPacing& pacing = host.transport.pacing();
        std::fprintf(stdout,
                     "%s: pacing    %llu published, %llu late, mean interval %u us, worst %u us\n",
                     kTag, static_cast<unsigned long long>(pacing.published),
                     static_cast<unsigned long long>(pacing.late), pacing.mean_interval_micros,
                     pacing.worst_interval_micros);

        const viewport::PublisherStatistics& statistics = host.publisher->statistics();
        const f64 seconds = static_cast<f64>(monotonic_nanos() - started) / 1'000'000'000.0;
        std::fprintf(stdout,
                     "%s: published %llu frames in %.2f s = %.1f fps, dropped %llu on a full ring, "
                     "%llu vetoed; %llu gizmo(s) answered, %llu move(s) applied; %llu us of host "
                     "copy\n",
                     kTag, static_cast<unsigned long long>(statistics.published), seconds,
                     seconds > 0.0 ? static_cast<double>(statistics.published) / seconds : 0.0,
                     static_cast<unsigned long long>(statistics.dropped_full_ring),
                     static_cast<unsigned long long>(statistics.vetoed),
                     static_cast<unsigned long long>(host.gizmos_answered),
                     static_cast<unsigned long long>(host.moves_applied),
                     static_cast<unsigned long long>(statistics.upload_micros));
        std::fprintf(stdout,
                     "%s: editor    %llu connection(s); hello %llu, ping %llu, gizmo-intent %llu, "
                     "apply %llu, pick %llu, unknown %llu\n",
                     kTag, static_cast<unsigned long long>(bridge.connections()),
                     static_cast<unsigned long long>(
                         host.received[static_cast<usize>(runtime::EditorMessage::Hello)]),
                     static_cast<unsigned long long>(
                         host.received[static_cast<usize>(runtime::EditorMessage::Ping)]),
                     static_cast<unsigned long long>(
                         host.received[static_cast<usize>(runtime::EditorMessage::GizmoIntent)]),
                     static_cast<unsigned long long>(
                         host.received[static_cast<usize>(runtime::EditorMessage::Apply)]),
                     static_cast<unsigned long long>(
                         host.received[static_cast<usize>(runtime::EditorMessage::Pick)]),
                     static_cast<unsigned long long>(host.unknown_messages));
    }

    rhi::destroy_device(allocator, device.value());
    return exit_code;
}
