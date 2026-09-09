#pragma once
// The hosted runtime's side of the editor bridge: the control channel a `cyberdyne-editor
// --host <socket>` talks to. M7 tasks 5b.3 and 5b.4.
//
// ================================================================================================
// WHY THIS EXISTS NOW
// ================================================================================================
//
// `editor/crates/cy-editor-protocol/` has carried this message set since M5, and until now the only
// thing that ever answered it was `cy-runtime-stub` — a Rust binary in the editor's own workspace
// whose module comment says what it is: "What it is not: an engine. It holds no world, and it
// echoes what it is asked to apply. The real hosted runtime is a C++ binary … speaking this same
// message set, over this same committed encoding."
//
// This is that C++ side. It is what lets `Message::GizmoIntent` reach `cy::render::
// build_gizmo_layout` and `Message::GizmoGeometry` carry the answer back, which is the whole of
// task 5b.3 — the protocol, the editor's reader and the hit test were all built at M6 and nothing
// in `src/` produced a layout, so a click had nothing to land on.
//
// ================================================================================================
// THE FRAMING, AND WHY A CHECKSUM OVER A UNIX SOCKET
// ================================================================================================
//
// A four-byte little-endian length, a four-byte FNV-1a checksum, then the payload —
// `cy_editor_protocol::frame`, restated. The checksum is not for corruption, of which a Unix domain
// socket has none. It is for the case the peer CRASHED MID-WRITE, which
// `editor-rust-application` requires the editor to survive and which M5's artefact tests by killing
// a process: a length read out of a half-written frame must be refused rather than believed.
//
// ================================================================================================
// IT NEVER BLOCKS, IN EITHER DIRECTION
// ================================================================================================
//
// `poll()` returns what has arrived and nothing else. A runtime that blocked reading the editor
// would stop rendering because the editor was busy, which is the inversion the whole out-of-process
// design exists to prevent — and it is the same rule, in the other direction, that
// `cy_editor_protocol::Session` states as "there is no blocking send".
//
// A partial frame is KEPT rather than discarded: the reader accumulates into its own buffer and
// answers "nothing yet" until a whole frame is there. Discarding would turn a large message split
// across two reads into a protocol error.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// THE IMAGE. Frames travel over the viewport transport (`cy/backends/viewport/publisher.h`), on a
// different socket, because they are pixels and this is control. The two are joined only by the
// frame identifier they both carry.
//
// A WORLD. This module decodes and encodes; what a pick or a gizmo intent MEANS is the host's,
// because the host is what holds a scene. That is why `poll` hands back a request rather than
// calling a handler: a bridge that owned the answers would be a bridge that owned the world.
//
// TRANSACTIONS. `Apply` is decoded and can be answered, but nothing here interprets the bytes: the
// transaction encoding is `cy_editor_core::codec`'s and belongs to whatever applies it. Live
// editing is M8's, and this module is the wire it will arrive on.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::runtime {

/// The message tags `cy_editor_protocol::Message` encodes.
///
/// The numbers are the wire, so they are written out rather than derived. A tag this build does not
/// know is refused by name, which is what `Message::decode` does in the other direction.
enum class EditorMessage : u8 {
    Hello = 0,
    Welcome = 1,
    Refused = 2,
    Apply = 3,
    Applied = 4,
    Rejected = 5,
    Ping = 6,
    Pong = 7,
    Reload = 8,
    Reloaded = 9,
    Pick = 10,
    Picked = 11,
    GizmoIntent = 12,
    GizmoGeometry = 13,
    /// A view that frames what the runtime is holding, offered once. See `send_view_suggestion`.
    ViewSuggested = 14,
    /// Press play, pause or stop. M8.a task 5.1; `cy_editor_protocol::Message::Play`.
    Play = 15,
    /// What the play session is doing now. `cy_editor_protocol::Message::Playing`.
    Playing = 16,
    Unknown = 255,
};

[[nodiscard]] const char* editor_message_name(EditorMessage message) noexcept;

/// One decoded message from the editor.
///
/// `payload` points into a buffer the bridge holds for exactly one message and is valid until the
/// next `poll`. A caller that needs the bytes for longer copies them.
///
/// **It is a buffer of its own rather than a view into the read buffer**, and that is not a
/// nicety: `poll` compacts the read buffer once it has taken a frame out of it, so a span into it
/// would be pointing at the NEXT message by the time the caller read it. That defect reached the
/// artefact and reported itself as "a transaction ends before its actor" — a decoder failing on
/// bytes that were another message's, which reads as a protocol version mismatch and is not one.
struct EditorRequest {
    EditorMessage kind = EditorMessage::Unknown;
    /// The request's identity, so an answer can be paired with it. Zero for `Ping`, which is
    /// matched by its frame instead.
    u64 request = 0;
    /// The frame the editor is talking about — the one it was showing when the user clicked, not
    /// the one the runtime has since rendered.
    u64 frame = 0;
    /// Which viewport, for a gizmo intent. A session has several and they differ in camera.
    u64 viewport = 0;
    /// The editor's ABI, for `Hello`.
    u32 abi_major = 0;
    u32 abi_minor = 0;
    /// The opaque bytes: a transaction, a pick request, a gizmo intent, or — for `Play` — the
    /// state's name as UTF-8 with no terminator, depending on `kind`.
    ///
    /// A LENGTH-PREFIXED STRING RATHER THAN AN ENUMERATOR, and the argument is on the protocol's
    /// own `Message::Play`: a fourth play state added on one side and not the other must be refused
    /// BY NAME, and a `u8` that fell through a `switch` would be silently treated as the closest
    /// one. `cy::gameplay::play_state_of` is what refuses it.
    Span<const u8> payload;
};

/// The runtime's end of the editor's control socket.
///
/// NOT THREAD-SAFE, and single-connection: one editor at a time, because a second editor over one
/// runtime is a question about shared authoring rather than about a socket.
class EditorBridge {
public:
    EditorBridge() = default;
    ~EditorBridge();

    EditorBridge(const EditorBridge&) = delete;
    EditorBridge& operator=(const EditorBridge&) = delete;

    /// Bind and listen.
    ///
    /// Refuses a path past `sun_path`'s 107 bytes rather than truncating it — a truncated path
    /// binds a DIFFERENT socket, everything starts, and the editor simply never connects. M7 task
    /// 5b.6 is the same defect on the other socket.
    [[nodiscard]] Status listen(const char* path) noexcept;

    /// Accept a waiting editor, or notice that the one we had has gone. Called once per frame.
    void service() noexcept;

    [[nodiscard]] bool connected() const noexcept { return client_ >= 0; }
    /// How many editors have connected. More than one means a restart against a live runtime.
    [[nodiscard]] u64 connections() const noexcept { return connections_; }

    /// The next message, if a whole one has arrived. Never blocks.
    ///
    /// `false` means "nothing yet", which is by far the common answer and is not an error. A frame
    /// whose checksum does not match, or whose length is past the limit, closes the connection: the
    /// stream is out of step and the only honest thing left is to start again.
    [[nodiscard]] bool poll(EditorRequest& out) noexcept;

    // --- The answers ---------------------------------------------------------------------------
    //
    // Each is one message, framed and written. A closed connection is reported rather than
    // asserted: the editor is entitled to disappear between a request and its answer, and a
    // runtime that treated that as an error would be a runtime a restart could crash.

    [[nodiscard]] Status send_welcome(u32 abi_major, u32 abi_minor, const char* runtime) noexcept;
    [[nodiscard]] Status send_refused(const char* reason, const char* remedy) noexcept;
    [[nodiscard]] Status send_pong(u64 frame) noexcept;
    [[nodiscard]] Status send_applied(u64 request, u64 frame, Span<const u8> observed) noexcept;
    [[nodiscard]] Status send_rejected(u64 request, const char* reason,
                                       const char* remedy) noexcept;
    [[nodiscard]] Status send_picked(u64 request, Span<const u8> candidates) noexcept;
    [[nodiscard]] Status send_gizmo_geometry(u64 request, Span<const u8> layout) noexcept;

    /// What the play session is doing now, and what it did. M8.a task 5.1.
    ///
    /// `state` is the word `cy::gameplay::play_state_name` spells — and it is the state now IN
    /// FORCE, which may not be the one asked for: a runtime that refused to enter play answers with
    /// "editing" and a `detail` that says why, rather than with a silence the editor would have to
    /// interpret as either a refusal or a lost connection.
    [[nodiscard]] Status send_playing(u64 request, const char* state, const char* detail) noexcept;

    /// Offer the editor a camera that frames what this runtime is holding. **Once per session.**
    ///
    /// The editor owns the camera — navigation is intent — but a viewport opens at the origin
    /// looking down −Z, and where the content is is a question only the runtime can answer. Without
    /// it the editor's first view is inside whatever sits at the origin, and every manipulation
    /// reports zero because a pivot at the camera's own position has no screen-space direction.
    /// After the editor takes it, the camera is the editor's and this must not be sent again.
    [[nodiscard]] Status send_view_suggestion(const f32 position[3], const f32 rotation[4],
                                              f32 fov_y_radians, f32 near_plane) noexcept;

    /// The framing, exposed for the suite that checks it against the editor's own reader.
    ///
    /// Public because the property worth testing is "these bytes are a frame the editor accepts",
    /// and a test that could only observe it through a socket would be testing the socket.
    [[nodiscard]] static Status frame_message(Span<const u8> payload, Array<u8>& out) noexcept;
    /// FNV-1a over the payload, which is what the length header carries beside the length.
    [[nodiscard]] static u32 checksum(Span<const u8> payload) noexcept;

private:
    [[nodiscard]] Status send(Span<const u8> payload) noexcept;
    void drop_client() noexcept;
    [[nodiscard]] bool decode(
        Span<const u8> payload,
        EditorRequest& out) noexcept;  // NOLINT(readability-convert-member-functions-to-static)

    int listener_ = -1;
    int client_ = -1;
    char path_[108] = {};
    u64 connections_ = 0;
    /// What has been read and not yet consumed. One buffer, compacted after each whole frame.
    Array<u8> incoming_;
    /// The one message `poll` last handed out, copied out of `incoming_` before it was compacted.
    Array<u8> message_;
    Array<u8> outgoing_;
};

}  // namespace cy::runtime
