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
//   cy::gameplay-play           M8.a's play session: the authored world becomes an ECS world with
//                               bodies, steps in the `Physics` stage, and is restored EXACTLY when
//                               play ends. This program is what presses it.
//
// This file is the loop that joins them, and it holds the one thing none of them can: which of the
// scene's objects the editor's selection means. `session.h` says why that association exists and
// what it is a stand-in for.
//
// ================================================================================================
// USAGE
// ================================================================================================
//
//   cy_editor_window_runtime [--socket PATH] [--host PATH] [--project DIR] [--world PATH]
//                            [--physics jolt|reference]
//                            [--width W] [--height H]
//                            [--buffers N] [--seconds S] [--frames N] [--rate HZ]
//                            [--adapter SUBSTRING] [--orbit TURNS-PER-SECOND]
//                            [--layout PATH] [--no-validation]
//
// `--layout` writes the gizmo layout this runtime published, as one line of JSON, every time the
// published geometry changes (`layout_file.h` says why not every time). It is how `window.py` aims
// a drag at a HANDLE rather than at a coordinate — M7 task 5b.4, whose defect was that M6's
// artefact dragged from a hard-coded (47 %, 38 %) hoping a handle was there.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#if defined(__APPLE__)
#    include <cy/backends/rhi-metal/backend.h>
#else
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#include <cy/backends/viewport/publisher.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/gameplay/play/session.h>
#include <cy/servers/physics/reference/server.h>
#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif
#include <cy/editor/material_service.h>
#include <cy/runtime/editor_bridge/bridge.h>
#include <cy/servers/render/gizmo.h>
#include <cy/servers/render/picking.h>
#include <cy/servers/render/viewport_transport.h>
#include <cy_reflect_generated_scene.h>

#include "layout_file.h"
#include "authored_frame.h"
#include "material_runtime.h"
#include "overlay.h"
#include "pick_wire.h"
#include "world_view.h"

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
    /// The project directory the world lives in, and the path the EDITOR names the world by.
    /// Separate on purpose: see `WorldView::open`.
    const char* project = "";
    const char* world = "";
    const char* adapter = "";
    const char* layout_path = "";
    /// Which solver a play session simulates in. `jolt` where this build has it — the reference
    /// backend integrates motion and resolves no contacts, so a sphere would fall through the
    /// floor and the artefact would photograph it doing so.
    const char* physics = "";
    u32 width = 1280;
    u32 height = 720;
    u32 buffers = 4;
    f64 seconds = 180.0;
    u64 frames = 0;
    f64 rate = 60.0;
    f64 orbit = 0.008;
    bool validation = true;
};

struct DeviceOwner {
    Allocator& allocator;
    rhi::Device* device;
    ~DeviceOwner() { rhi::destroy_device(allocator, device); }
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
    options.project = value_of(argc, argv, "--project", options.project);
    options.world = value_of(argc, argv, "--world", options.world);
    options.adapter = value_of(argc, argv, "--adapter", options.adapter);
    options.layout_path = value_of(argc, argv, "--layout", options.layout_path);
    options.physics = value_of(argc, argv, "--physics", options.physics);
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

/// Everything the loop needs, gathered so the frame function is readable.
struct Host {
    Options options;
    AuthoredFrame* authored_frame = nullptr;
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
    editor::MaterialService* editor_service = nullptr;
    CyServiceSession service_session = nullptr;
    /// THE WORLD, and the whole of what M7's `EditorSession` used to stand in for. See
    /// `world_view.h`: there is no association here, because the runtime opened the same file the
    /// editor did and a node's identity is derived on both sides from the same two numbers.
    WorldView* view_world = nullptr;
    /// M8.a TASK 5.1: what pressing play actually does. Null until a world is open — a play session
    /// over M3's ring would be a simulation of a fixture, which is what this milestone ends.
    gameplay::PlaySession* play = nullptr;
    /// The solver a session simulates in. Owned by `main`, not by the session: which backend a
    /// project uses is the host's decision (`cy::physics::PhysicsBridge`'s header argues it), and a
    /// session that created one would create and destroy a whole backend per press of play.
    physics::PhysicsServer* physics = nullptr;
    /// What the sessions did, for the report. Cumulative across presses of play, because a session
    /// that left something behind would show up in the SECOND one.
    /// The mode the current or most recent session was asked for. M11.b task 3.1.
    ///
    /// Held on the host rather than read back out of the session, because the answer to a `Play`
    /// that never reached a session — no world open, a refused state — still has to name a mode.
    gameplay::PlayMode play_mode = gameplay::PlayMode::InEditor;
    u64 play_sessions = 0;
    u64 play_ticks = 0;
    u64 play_bodies = 0;
    /// False the moment any session's stop failed to restore the authored world byte for byte,
    /// which is task 5.2's claim measured rather than asserted. Starts true and can only fall.
    bool play_restored_exactly = true;
    /// The gizmo mode the editor last asked for. Still kept — it decides which handles are drawn —
    /// but no longer used to guess what a changed value meant, because the transaction says.
    render::GizmoMode mode = render::GizmoMode::Translate;
    /// The scene object the gizmo is on, and the identity it stands for.
    u32 anchored = WorldView::kNoObject;
    u64 anchored_identity = 0;
    /// Whether the "that is not my document" warning has already been printed. Once, not per
    /// message: a wall of them would bury the one line that says what to do.
    bool warned_about_document = false;
    /// The frame identity the pixel half last announced. What a pick is resolved against.
    u64 published_frame = 0;
    /// The gizmo in the FRAME's pixels, which is what is drawn into the frame.
    render::GizmoLayout layout;
    /// Where `--layout` is written, rewritten only when the geometry changes. See `layout_file.h`
    /// for the quarter-second renames that made this necessary.
    LayoutFile layout_file;
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
    u64 picks_answered = 0;
    u64 pick_candidates = 0;
    u64 transactions_applied = 0;
    u64 nodes_created = 0;
    u64 nodes_deleted = 0;
    /// TASK 1.3, MEASURED RATHER THAN ASSERTED. When a transaction is applied, the number of frames
    /// published so far is recorded; the next frame published carries the change, so the difference
    /// is how many frames the editor waited to see its own edit. Zero is the claim, and the worst
    /// case over the run is what the report prints — an average would hide the one frame that was
    /// late.
    bool change_pending = false;
    u64 change_at_frame = 0;
    u64 change_at_micros = 0;
    u64 worst_frames_to_visible = 0;
    u64 worst_micros_to_visible = 0;
    u64 changes_measured = 0;
    /// The frame's records, kept across frames so a pick does not allocate on the message path.
    Array<render::GpuInstance> instances{system_allocator(MemoryDomain::Gpu)};
    Array<render::DrawItem> draws{system_allocator(MemoryDomain::Gpu)};
    /// What the editor sent, by kind. Printed at the end rather than logged per message: an
    /// artefact that reports "0 gizmos answered" needs to be able to say whether the editor asked
    /// and the runtime refused, or whether nothing arrived at all — which is two very different
    /// places to look, and the difference cost this milestone an afternoon.
    /// SIZED BY THE LAST TAG, not by the last one this runtime answers: `Play` is 15 and
    /// `GizmoGeometry` is 13, so an array sized by the latter would be written past its end by the
    /// counter above the switch the first time an editor pressed play.
    u64 received[static_cast<usize>(runtime::EditorMessage::ServiceCancel) + 1] = {};
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
    (void)host.layout_file.write(host.options.layout_path, published);
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
    host.mode = intent.mode;
    host.asked_width = intent.viewport_width;
    host.asked_height = intent.viewport_height;
    adopt_camera(host, intent);
    // THE OBJECT THE EDITOR NAMED, by identity. Not the next unused one: the world this runtime
    // holds is the world the editor has open, so an identity either names a node in it or names
    // nothing, and naming nothing must take the gizmo off the screen rather than move it to an
    // unrelated object.
    const u32 object = intent.identities.empty()
                           ? WorldView::kNoObject
                           : host.view_world->object_for(intent.identities[0]);
    if (object == WorldView::kNoObject) {
        // An empty selection is a REQUEST, not an absence: it must take the gizmo off the screen.
        host.anchored = WorldView::kNoObject;
        host.anchored_identity = 0;
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
    host.anchored = object;
    host.anchored_identity = intent.identities[0];

    Vec3 pivot{};
    if (host.authored_frame == nullptr) {
        pivot = relative_position(host.scene->objects()[object], host.camera);
    }
    Vec3 world_pivot;
    if (host.authored_frame != nullptr &&
        host.authored_frame->pivot_for(intent.identities[0], world_pivot)) {
        pivot = world_pivot - Vec3{static_cast<f32>(host.camera.position[0]),
                                   static_cast<f32>(host.camera.position[1]),
                                   static_cast<f32>(host.camera.position[2])};
    }
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

/// Answer one pick against the frame the editor was looking at. M8.a task 1.4.
///
/// **M7 refused this by name**, and its reason was exact: *"this runtime renders through M3's
/// sample renderer, which publishes no draw list for cy::render::pick_ray to resolve against"*.
/// There is one now. `WorldView::publish` writes a `GpuInstance` and a `DrawItem` for every object
/// the frame drew, out of the same placement the frame was drawn from, so what is picked is what
/// was rendered rather than a second traversal that agrees on the day it is written.
void answer_pick(Host& host, const runtime::EditorRequest& request) noexcept {
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    PickRequest pick(allocator);
    if (!decode_pick_request(request.payload, pick)) {
        (void)host.bridge->send_rejected(request.request, "read the pick request",
                                         "rebuild the editor and the runtime together");
        return;
    }
    // THE FRAME THE CLICK WAS AIMED AT. The records were published with the frame that produced
    // them, and a click against a frame this runtime has moved past would be resolved against a
    // camera the user never saw. A pick that names no frame — a probe, or a test — is answered
    // against the current one, which is the only frame there is to answer against.
    Array<render::PickCandidate> candidates(allocator);
    if (pick.frame == 0 || pick.frame == host.published_frame) {
        if (Status resolved =
                resolve_pick(pick, host.view, host.instances.span(), host.draws.span(), candidates);
            !resolved) {
            (void)host.bridge->send_rejected(request.request, resolved.error().message,
                                             "the pick names an intent or a size this runtime "
                                             "cannot resolve");
            return;
        }
    }
    Array<u8> reply(allocator);
    if (Status encoded = encode_pick_response(pick.frame, candidates.span(), reply); !encoded) {
        (void)host.bridge->send_rejected(request.request, "encode the pick answer",
                                         "the runtime ran out of memory");
        return;
    }
    if (Status sent = host.bridge->send_picked(request.request, reply.span()); sent) {
        host.picks_answered += 1;
        host.pick_candidates += candidates.size();
    }
}

/// Apply what the editor committed, to THE WORLD, so the next frame is the world it authored.
///
/// M7 applied a translation to a scene object it had associated with the identity in first-seen
/// order, and declined everything else — its own message said so: *"this runtime holds its own
/// scene; M8's live editing shares one"*. It shares one now. The bytes go to
/// `cy::scene::serialization::apply_transaction`, which addresses the world by the identity the
/// editor allocated and by the field identifiers the world file itself declared, so a create
/// creates, a delete deletes and a scale scales.
void apply_transaction(Host& host, const runtime::EditorRequest& request) noexcept {
    scene::serialization::TransactionReport report;
    if (Status applied = host.view_world->apply(request.payload, report); !applied) {
        // Still a refusal, but a different one: the stream was unreadable, not the operation
        // unsupported. The editor is told which, because they are two very different places to
        // look.
        (void)host.bridge->send_rejected(request.request, applied.error().message,
                                         "rebuild the editor and the runtime together");
        return;
    }
    if (!scene::serialization::verify_document_identity(host.view_world->world(),
                                                        report.document)) {
        // NOT AN ERROR, and worth saying once rather than per message: an editor may hold documents
        // this runtime never loaded, and a transaction for one of them changes nothing here.
        if (!host.warned_about_document) {
            std::fprintf(stderr,
                         "%s: warning: a transaction names a document this runtime did not load. "
                         "--world must be the path the editor opened, spelled the same way\n",
                         kTag);
            host.warned_about_document = true;
        }
    }
    host.transactions_applied += 1;
    host.moves_applied += report.applied;
    host.nodes_created += report.created;
    host.nodes_deleted += report.deleted;
    if (host.authored_frame != nullptr) {
        (void)host.view_world->present_authored();
        if (Status prepared = host.authored_frame->prepare_world(host.view_world->world());
            !prepared) {
            std::fprintf(stderr, "%s: authored scene: %s\n", kTag,
                         prepared.error().message);
        }
    }
    if (report.applied > 0 && !host.change_pending) {
        // TASK 1.3, MEASURED. The next frame published is the one that carries this change; the
        // report says how many frames it actually took, worst case over the run.
        host.change_pending = true;
        host.change_at_frame = host.frames_published;
        host.change_at_micros = monotonic_nanos() / 1000ULL;
    }
    (void)host.bridge->send_applied(request.request, request.frame, request.payload);
}

/// The mode the editor asked for, or `InEditor` when it named none.
///
/// An editor built before M11.b sends a `Play` with no mode word, and the two ends are versioned
/// rather than lock-stepped — so an empty mode is read as `in-editor`, which is the mode the editor
/// has always meant when it did not say. A NON-EMPTY word this build does not know is a different
/// thing and is refused by name; see `answer_play`.
[[nodiscard]] Expected<gameplay::PlayMode, Error> requested_mode(
    const runtime::EditorRequest& request) noexcept {
    if (request.mode.empty()) {
        return gameplay::PlayMode::InEditor;
    }
    const std::string_view named(reinterpret_cast<const char*>(request.mode.data()),
                                 request.mode.size());
    return gameplay::play_mode_of(named);
}

/// Enter, pause or leave play. M8.a tasks 5.1 and 5.2; the mode is M11.b task 3.1.
///
/// THE ANSWER IS ALWAYS THE STATE NOW IN FORCE, never a silence. A runtime that ignored a play it
/// could not honour would leave the editor showing "PLAYING" over a world that is not moving, which
/// is worse than a refusal: it is a refusal a person cannot see.
void answer_play(Host& host, const runtime::EditorRequest& request) noexcept {
    const std::string_view asked(reinterpret_cast<const char*>(request.payload.data()),
                                 request.payload.size());
    const Expected<gameplay::PlayState, Error> wanted = gameplay::play_state_of(asked);
    if (!wanted) {
        (void)host.bridge->send_playing(
            request.request,
            host.play == nullptr ? "editing" : gameplay::play_state_name(host.play->state()),
            gameplay::play_mode_name(host.play_mode), wanted.error().message);
        return;
    }

    // THE MODE, AND IT IS REFUSED BY NAME RATHER THAN REPLACED. `live-editing`: *"Selecting a mode
    // that is not available SHALL refuse, naming the mode and the reason. It SHALL NOT fall back to
    // another mode."* Two refusals live here — a word this build does not know, and a mode this
    // build cannot run — and neither of them starts anything.
    const Expected<gameplay::PlayMode, Error> mode = requested_mode(request);
    if (!mode) {
        (void)host.bridge->send_playing(
            request.request,
            host.play == nullptr ? "editing" : gameplay::play_state_name(host.play->state()),
            gameplay::play_mode_name(host.play_mode), mode.error().message);
        return;
    }
    const gameplay::PlayModeAvailability availability =
        gameplay::availability_of(*mode, gameplay::play_mode_support());
    if (!availability.available) {
        (void)host.bridge->send_playing(
            request.request,
            host.play == nullptr ? "editing" : gameplay::play_state_name(host.play->state()),
            gameplay::play_mode_name(host.play_mode), availability.reason);
        return;
    }

    if (host.play == nullptr) {
        // No world, so nothing to simulate. `--world` is what makes a play session possible at all,
        // and saying so is more useful than reporting a state that would be a lie.
        (void)host.bridge->send_playing(
            request.request, "editing", gameplay::play_mode_name(*mode),
            "this runtime has no world open; start it with --project and --world");
        return;
    }
    host.play_mode = *mode;

    char detail[192] = {};
    switch (*wanted) {
        case gameplay::PlayState::Playing: {
            if (host.play->state() == gameplay::PlayState::Paused) {
                if (Status resumed = host.play->resume(); !resumed) {
                    (void)host.bridge->send_playing(
                        request.request, gameplay::play_state_name(host.play->state()),
                        gameplay::play_mode_name(host.play_mode), resumed.error().message);
                    return;
                }
                (void)std::snprintf(detail, sizeof(detail), "resumed");
                break;
            }
            if (host.play->state() == gameplay::PlayState::Playing) {
                (void)std::snprintf(detail, sizeof(detail), "already playing");
                break;
            }
            gameplay::PlayConfiguration configuration;
            configuration.physics = host.physics;
            configuration.body_capacity = kWorldCapacity * 2;
            // The mode the editor asked for, carried into the session so that `enter` refuses one
            // this build cannot run rather than this function having to remember to.
            configuration.mode = host.play_mode;
            if (Status entered = host.play->enter(configuration); !entered) {
                (void)host.bridge->send_playing(request.request, "editing",
                                                gameplay::play_mode_name(host.play_mode),
                                                entered.error().message);
                return;
            }
            host.play_sessions += 1;
            host.play_bodies = host.play->report().bodies;
            (void)std::snprintf(detail, sizeof(detail), "%u entities, %u bodies, %u colliders",
                                host.play->report().entities, host.play->report().bodies,
                                host.play->report().colliders);
            break;
        }
        case gameplay::PlayState::Paused:
            if (Status paused = host.play->pause(); !paused) {
                (void)std::snprintf(detail, sizeof(detail), "%s", paused.error().message);
            } else {
                (void)std::snprintf(detail, sizeof(detail), "paused at tick %llu",
                                    static_cast<unsigned long long>(host.play->report().ticks));
            }
            break;
        case gameplay::PlayState::Editing: {
            const bool was_playing = host.play->state() != gameplay::PlayState::Editing;
            if (Status stopped = host.play->stop(); !stopped) {
                (void)std::snprintf(detail, sizeof(detail), "%s", stopped.error().message);
                break;
            }
            if (was_playing) {
                // TASK 5.2, REPORTED RATHER THAN ASSUMED. The session compared the world's bytes
                // before and after; if they differ it has already put the whole snapshot back, and
                // the editor is told so in the same sentence a designer reads.
                const gameplay::PlayReport& report = host.play->report();
                host.play_restored_exactly = host.play_restored_exactly && report.restored_exactly;
                (void)std::snprintf(
                    detail, sizeof(detail), "%llu ticks, %u placements restored, document %s",
                    static_cast<unsigned long long>(report.ticks), report.restored,
                    report.restored_exactly ? "identical" : "REBUILT FROM THE SNAPSHOT");
            } else {
                (void)std::snprintf(detail, sizeof(detail), "nothing was playing");
            }
            break;
        }
    }
    (void)host.bridge->send_playing(request.request, gameplay::play_state_name(host.play->state()),
                                    gameplay::play_mode_name(host.play_mode), detail);
}

void answer_service(Host& host, const runtime::EditorRequest& request) noexcept {
    if (host.editor_service == nullptr || host.service_session == nullptr) {
        (void)host.bridge->send_service_event(request.request, runtime::ServiceEventKind::Failed, 1,
                                              {});
        return;
    }
    if (request.kind == runtime::EditorMessage::ServiceCancel) {
        (void)host.editor_service->cancel(host.service_session, request.request);
    } else {
        char operation[64] = {};
        if (request.operation.size() >= sizeof(operation)) {
            (void)host.bridge->send_service_event(request.request,
                                                  runtime::ServiceEventKind::Failed, 1, {});
            return;
        }
        std::memcpy(operation, request.operation.data(), request.operation.size());
        const CyServiceRequest submitted{sizeof(CyServiceRequest), request.schema_version,
                                         request.request,          operation,
                                         request.payload.data(),   request.payload.size()};
        (void)host.editor_service->submit(host.service_session, submitted);
    }
    CyServiceEvent event{};
    bool present = false;
    if (host.editor_service->poll(host.service_session, event, present) == CY_RESULT_OK &&
        present) {
        (void)host.bridge->send_service_event(
            event.request_id, static_cast<runtime::ServiceEventKind>(event.kind),
            event.schema_version, {event.payload, event.payload_size});
    }
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
                const first_light::Camera framed = host.authored_frame != nullptr
                    ? host.authored_frame->framing(host.scene->camera_at(kFramingPhase))
                    : host.scene->camera_at(kFramingPhase);
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
                answer_pick(host, request);
                break;
            case runtime::EditorMessage::Play:
                answer_play(host, request);
                break;
            case runtime::EditorMessage::ServiceRequest:
            case runtime::EditorMessage::ServiceCancel:
                answer_service(host, request);
                break;
            default:
                break;
        }
    }
}

/// Open the world the editor opened, with the engine's own component types registered.
///
/// REFUSED RATHER THAN FALLEN BACK TO M3's RING. A runtime that silently rendered a different world
/// from the one the editor opened is the defect this milestone exists to end, and it would present
/// to a user as "my objects are not there" with nothing to look at.
///
/// Without `--world` this does nothing and answers `ok()`: that is the headless run and
/// `cy-viewport-transport-probe`'s, and it is still M3's ring.
[[nodiscard]] Status open_world(const Options& options, reflect::TypeRegistry& registry,
                                WorldView& out) noexcept {
    if (options.world[0] == '\0') {
        return ok();
    }
    // The engine's own scene components, which is what makes the file's `Transform` this build's
    // `cy::scene::LocalTransform` rather than a name that happens to match.
    if (Status registered = reflect::register_scene_types(registry); !registered) {
        return registered;
    }
    if (Status opened = out.open(options.project, options.world, registry); !opened) {
        return opened;
    }
    std::fprintf(stdout, "%s: world    %s  %llu node(s), %llu type(s), document %016llx%016llx\n",
                 kTag, options.world, static_cast<unsigned long long>(out.world().nodes().size()),
                 static_cast<unsigned long long>(out.world().types().size()),
                 static_cast<unsigned long long>(out.world().document().high),
                 static_cast<unsigned long long>(out.world().document().low));
    return ok();
}

/// The solver a play session simulates in.
///
/// **Jolt by default where this build has it**, because the reference backend integrates motion and
/// declares that it does not resolve contacts (`Capabilities::contact_resolution` is false) — a
/// sphere dropped on a box would fall straight through it and the artefact would photograph that
/// happening. `--physics reference` asks for the other one deliberately, which is what a run
/// checking that the bridge does not depend on a backend wants.
[[nodiscard]] physics::PhysicsServer* create_physics(Allocator& allocator, const char* wanted,
                                                     const char*& chosen) noexcept {
    const bool ask_reference = std::strcmp(wanted, "reference") == 0;
#if defined(CY_PHYSICS)
    if (!ask_reference) {
        const Expected<physics::PhysicsServer*, Error> made =
            physics::jolt::create_server(allocator, nullptr);
        if (made && (*made)->initialize()) {
            chosen = "jolt";
            return *made;
        }
        if (made) {
            physics::jolt::destroy_server(*made, allocator);
        }
    }
#else
    (void)ask_reference;
#endif
    const Expected<physics::PhysicsServer*, Error> made =
        physics::reference::create_server(allocator);
    if (!made) {
        return nullptr;
    }
    if (!(*made)->initialize()) {
        physics::reference::destroy_server(*made, allocator);
        return nullptr;
    }
    chosen = "reference";
    return *made;
}

void destroy_physics(Allocator& allocator, physics::PhysicsServer* server,
                     const char* chosen) noexcept {
    if (server == nullptr) {
        return;
    }
    server->shutdown();
#if defined(CY_PHYSICS)
    if (std::strcmp(chosen, "jolt") == 0) {
        physics::jolt::destroy_server(server, allocator);
        return;
    }
#else
    (void)chosen;
#endif
    physics::reference::destroy_server(server, allocator);
}

/// Render one frame, composite the gizmo into it, and publish it.
[[nodiscard]] bool publish_frame(Host& host, f32 phase) noexcept {
    host.camera = host.editor_camera ? host.asked_camera : host.scene->camera_at(phase);
    host.view = view_of(host.camera, host.options.width, host.options.height);

    // THE WORLD BECOMES THE FRAME, here, once, every frame. Everything the editor committed since
    // the last frame is already in the world — `serve_editor` ran first in this same iteration —
    // so a change and the frame that shows it are one pass of this loop apart, which is what task
    // 1.3 measures rather than assumes.
    // ONE FIXED STEP OF THE PLAY SESSION, BEFORE THE WORLD BECOMES THE FRAME. The session writes
    // the simulated placements back into the authored world, so the presentation below draws the
    // simulation without knowing a session exists — and when nothing is playing, `tick()` succeeds
    // and does nothing, which is why there is no state check here.
    if (host.play != nullptr) {
        if (Status stepped = host.play->tick(); !stepped) {
            report("play", stepped.error());
        } else if (host.play->state() == gameplay::PlayState::Playing) {
            host.play_ticks += 1;
        }
    }

    if (host.view_world->loaded()) {
        if (host.authored_frame != nullptr) (void)host.view_world->present_authored();
        else (void)host.view_world->present(*host.scene);
    }

    Span<const u32> texels;
    if (host.authored_frame != nullptr) {
        if (Status frame = host.authored_frame->render(host.view_world->world(), host.camera);
            !frame) {
            report("authored frame", frame.error());
            return false;
        }
        texels = host.authored_frame->pixels();
    } else {
        const Expected<first_light::FrameReport, Error> frame =
            host.renderer->render(*host.scene, host.camera);
        if (!frame) {
            report("frame", frame.error());
            return false;
        }
        texels = host.renderer->color_texels();
    }
    if (texels.empty()) {
        report("readback", Error{ErrorCode::Unavailable, "the renderer read back no pixels"});
        return false;
    }
    if (host.authored_frame != nullptr) {
        if (Status published = host.authored_frame->publish(host.camera, host.instances, host.draws);
            !published) report("pick records", published.error());
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
    if (host.anchored != WorldView::kNoObject && !host.layout.empty()) {
        const u32 object = host.anchored;
        Vec3 pivot{};
        if (host.authored_frame == nullptr && object < host.scene->objects().size()) {
            pivot = relative_position(host.scene->objects()[object], host.camera);
        }
        Vec3 world_pivot;
        if (host.authored_frame != nullptr &&
            host.authored_frame->pivot_for(host.anchored_identity, world_pivot)) {
            pivot = world_pivot - Vec3{static_cast<f32>(host.camera.position[0]),
                                       static_cast<f32>(host.camera.position[1]),
                                       static_cast<f32>(host.camera.position[2])};
        }
        Vec2 marker{0.0F, 0.0F};
        if (project_to_pixel(host.view, pivot, marker)) {
            draw_selection_marker(canvas, marker.x, marker.y, 26.0F);
        }
        // Rebuilt for THIS frame's camera, and republished with it, so the drawn gizmo and the
        // published one are the same handles even while the camera moves.
        host.layout = render::build_gizmo_layout(host.view, pivot, Quat::identity(), host.mode,
                                                 host.layout.frame_id);
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
    host.published_frame = *published;
    host.frames_published += 1;
    // TASK 1.3'S NUMBER, taken here because this is where the change actually reached a viewer.
    if (host.change_pending) {
        const u64 frames = host.frames_published - 1 - host.change_at_frame;
        const u64 micros = (monotonic_nanos() / 1000ULL) - host.change_at_micros;
        host.worst_frames_to_visible = math::max(host.worst_frames_to_visible, frames);
        host.worst_micros_to_visible = math::max(host.worst_micros_to_visible, micros);
        host.changes_measured += 1;
        host.change_pending = false;
    }
    return true;
}

}  // namespace

/// Everything the run measured, printed once at the end.
///
/// PRINTED RATHER THAN LOGGED PER EVENT, because the numbers a reader needs are differences: "0
/// gizmos answered" needs to be readable beside "1243 gizmo intents received" to say whether the
/// editor asked and the runtime refused, or whether nothing arrived at all. That distinction is two
/// very different places to look, and it cost M7 an afternoon.
void print_report(const Host& host, const WorldView& view_world,
                  const runtime::EditorBridge& bridge, u64 started) noexcept {
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
    // TASK 1.1, 1.3 AND 1.4, AS THREE NUMBERS A READER CAN CHECK. "0 frames" is the claim
    // that a change is visible in the frame the transaction commits; "n created" is the claim
    // that an entity the editor made exists here; "n candidate(s)" is the claim that a pick
    // resolved against what was drawn rather than being refused by name.
    if (view_world.loaded()) {
        std::fprintf(stdout,
                     "%s: world     %llu node(s) presented, %llu overflowed; "
                     "%llu transaction(s), %llu field(s) applied, %llu created, %llu deleted\n",
                     kTag, static_cast<unsigned long long>(view_world.presented()),
                     static_cast<unsigned long long>(view_world.overflowed()),
                     static_cast<unsigned long long>(host.transactions_applied),
                     static_cast<unsigned long long>(host.moves_applied),
                     static_cast<unsigned long long>(host.nodes_created),
                     static_cast<unsigned long long>(host.nodes_deleted));
        std::fprintf(stdout,
                     "%s: same-frame %llu change(s) measured, worst %llu frame(s) and %llu us "
                     "from commit to the frame that carried it\n",
                     kTag, static_cast<unsigned long long>(host.changes_measured),
                     static_cast<unsigned long long>(host.worst_frames_to_visible),
                     static_cast<unsigned long long>(host.worst_micros_to_visible));
        std::fprintf(stdout, "%s: picking   %llu answered, %llu candidate(s) reported\n", kTag,
                     static_cast<unsigned long long>(host.picks_answered),
                     static_cast<unsigned long long>(host.pick_candidates));
        // TASKS 5.1 AND 5.2, AS THREE NUMBERS AND A WORD. "n session(s)" is the claim that pressing
        // play reached this runtime at all; "n tick(s)" is the claim that it simulated rather than
        // sat in a mode; and "document identical" is the claim that stopping put the authored world
        // back byte for byte — which is the one a play session that left residue would fail.
        std::fprintf(stdout,
                     "%s: play      %llu session(s), %llu tick(s), %llu bodies, document %s\n",
                     kTag, static_cast<unsigned long long>(host.play_sessions),
                     static_cast<unsigned long long>(host.play_ticks),
                     static_cast<unsigned long long>(host.play_bodies),
                     host.play_restored_exactly ? "identical after every stop"
                                                : "REBUILT FROM THE SNAPSHOT — see task 5.2");
    }
    std::fprintf(stdout,
                 "%s: editor    %llu connection(s); hello %llu, ping %llu, gizmo-intent %llu, "
                 "apply %llu, pick %llu, play %llu, unknown %llu\n",
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
                 static_cast<unsigned long long>(
                     host.received[static_cast<usize>(runtime::EditorMessage::Play)]),
                 static_cast<unsigned long long>(host.unknown_messages));
    if (host.asked_width > 0 && host.asked_height > 0) {
        std::fprintf(stdout, "%s: viewport  latest requested extent %ux%u\n", kTag,
                     host.asked_width, host.asked_height);
    }
}

int main(int argc, char** argv) {
    (void)std::signal(SIGPIPE, SIG_IGN);
    (void)std::signal(SIGTERM, on_signal);
    (void)std::signal(SIGINT, on_signal);

    const Options options = parse(argc, argv);
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);

    (void)rhi::null::register_null_backend();
#if defined(__APPLE__)
    (void)rhi::metal::register_metal_backend();
    constexpr const char* kViewportBackend = "metal";
#else
    (void)rhi::vulkan::register_vulkan_backend();
    constexpr const char* kViewportBackend = "vulkan";
#endif

    rhi::DeviceDescription description;
    description.application_name = "cy_editor_window_runtime";
    description.enable_validation = options.validation;
    description.enable_synchronisation_validation = options.validation;
    rhi::BackendSelection selection;
    // The native backend BY NAME, not the default. A fall-back to null here would produce a
    // process that starts, publishes nothing and reports success — which is the shape of failure
    // this milestone exists to stop, so it is refused instead.
    const Expected<rhi::Device*, Error> device =
        rhi::create_device(allocator, kViewportBackend, description, selection);
    if (!device) {
        report("device", device.error());
        return 1;
    }
    DeviceOwner device_owner{allocator, device.value()};
    std::fprintf(stdout, "%s: device   backend=%s\n", kTag,
                 selection.selected != nullptr ? selection.selected : "(none)");

    // The first-light scene remains the no-world fixture and supplies a fallback camera.
    // Authored worlds use FrameAssembly and do not borrow its fixed box slots.
    first_light::SceneDescription scene_description;
    scene_description.box_count = 6;
    scene_description.sun_shadows = true;
    first_light::Scene scene(allocator);
    if (Status built = scene.build(scene_description); !built) {
        report("scene", built.error());
        return 1;
    }

    reflect::TypeRegistry registry;
    WorldView view_world(allocator);
    if (Status opened = open_world(options, registry, view_world); !opened) {
        report("world", opened.error());
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
            return 1;
        }
        AuthoredFrame authored_frame(allocator, *device.value());
        if (view_world.loaded()) {
            if (Status prepared = authored_frame.initialize(options.width, options.height,
                                                            options.project); !prepared) {
                report("authored frame", prepared.error());
                return 1;
            }
            if (Status prepared = authored_frame.prepare_world(view_world.world()); !prepared) {
                report("authored scene", prepared.error());
                return 1;
            }
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
                return 1;
            }
            std::fprintf(stdout, "%s: bridge   %s\n", kTag, options.host);
        }

        // THE PLAY SESSION AND ITS SOLVER, created only when there is a world to play. Both are
        // owned here, outside the loop, and destroyed after it in the order task 4.4 is about: the
        // session lets its bodies and its physics world go, and only then does the server go.
        const char* physics_backend = "none";
        physics::PhysicsServer* physics_server = nullptr;
        UniquePtr<gameplay::PlaySession> play;
        if (view_world.loaded()) {
            physics_server = create_physics(allocator, options.physics, physics_backend);
            if (physics_server == nullptr) {
                report("physics",
                       Error{ErrorCode::Unavailable,
                             "no physics backend could be created; play is unavailable"});
            } else {
                Expected<UniquePtr<gameplay::PlaySession>, Error> made =
                    make_unique<gameplay::PlaySession>(allocator, allocator, view_world.world());
                if (!made) {
                    report("play", made.error());
                } else {
                    play = std::move(*made);
                }
            }
            std::fprintf(stdout, "%s: play     backend=%s, session=%s\n", kTag, physics_backend,
                         play ? "ready" : "unavailable");
        }

        Host host;
#if defined(CY_EDITOR_MATERIAL_RUNTIME) && CY_EDITOR_MATERIAL_RUNTIME
        MetalMaterialRuntime material_runtime(allocator, renderer, view_world);
        editor::MaterialService editor_service(allocator, &material_runtime);
#else
        editor::MaterialService editor_service(allocator);
#endif
        CyServiceSession service_session = nullptr;
        if (editor_service.open(&service_session) != CY_RESULT_OK) {
            report("editor service", Error{ErrorCode::OutOfMemory,
                                           "the material service session could not be created"});
        }
        host.options = options;
        host.authored_frame = view_world.loaded() ? &authored_frame : nullptr;
        host.scene = &scene;
        host.view_world = &view_world;
        host.play = play.get();
        host.physics = physics_server;
        host.renderer = &renderer;
        host.publisher = publisher->get();
        host.bridge = &bridge;
        host.editor_service = &editor_service;
        host.service_session = service_session;
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

        print_report(host, view_world, bridge, started);
        editor_service.close(service_session);

        // THE SESSION BEFORE THE SERVER. A session destroyed after the server it holds a world in
        // would call `destroy_body` on freed memory, which is exactly the shape M5.5's gate found
        // one layer down. `PlaySession::~PlaySession` tears the bridge and the physics world down;
        // it deliberately does NOT stop play, because a destructor that wrote into the authored
        // world would put a restore on a path nobody asked for.
        if (play && play->state() != gameplay::PlayState::Editing) {
            (void)play->stop();
        }
        play.reset();
        destroy_physics(allocator, physics_server, physics_backend);
    }

    return exit_code;
}
