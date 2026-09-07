// The hosted runtime's side of the editor's control socket. M7 tasks 5b.3 and 5b.4.
//
// Two of these cases stand for defects this milestone actually shipped and then found, and both
// were invisible in a diff:
//
//   * `a_message_the_caller_reads_after_poll_is_still_its_own` — the payload span pointed into the
//     read buffer, which `poll` compacts, so a caller that read it saw the NEXT message's bytes.
//     It reported itself as "a transaction ends before its actor": a decoder failing on bytes that
//     were another message's, which reads as a protocol version mismatch and is not one.
//   * `a_socket_path_past_the_kernels_limit_is_refused_rather_than_truncated` — the same defect as
//     M7 task 5b.6 on the other socket. A truncated path binds a DIFFERENT socket, everything
//     starts, and the editor simply never connects.

#include <cy/core/memory/system_allocator.h>
#include <cy/runtime/editor_bridge/bridge.h>
#include <cy/test/test.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>

#include <cstring>

using cy::u32;
using cy::u64;
using cy::u8;
using namespace cy::runtime;

namespace {

/// Append a little-endian integer, the way `cy_editor_core::codec` writes one.
void little_endian(cy::Array<u8>& out, u64 value, cy::usize width) {
    for (cy::usize index = 0; index < width; ++index) {
        CY_REQUIRE(out.push_back(static_cast<u8>((value >> (index * 8)) & 0xFFU)));
    }
}

void text(cy::Array<u8>& out, const char* value) {
    const cy::usize length = std::strlen(value);
    little_endian(out, length, 4);
    for (cy::usize index = 0; index < length; ++index) {
        CY_REQUIRE(out.push_back(static_cast<u8>(value[index])));
    }
}

/// `Message::Hello`, as the editor encodes it.
[[nodiscard]] cy::Array<u8> hello(u32 major, u32 minor) {
    cy::Array<u8> bytes;
    CY_REQUIRE(bytes.push_back(0));
    little_endian(bytes, major, 4);
    little_endian(bytes, minor, 4);
    text(bytes, "cyberdyne-editor 0.5.0");
    return bytes;
}

/// `Message::GizmoIntent`, whose payload this suite does not interpret.
[[nodiscard]] cy::Array<u8> gizmo_intent(u64 request, u64 viewport, u8 marker) {
    cy::Array<u8> bytes;
    CY_REQUIRE(bytes.push_back(12));
    little_endian(bytes, request, 8);
    little_endian(bytes, viewport, 8);
    little_endian(bytes, 3, 4);
    for (u32 index = 0; index < 3; ++index) {
        CY_REQUIRE(bytes.push_back(static_cast<u8>(marker + index)));
    }
    return bytes;
}

/// A bridge listening on a real socket, with an editor connected to it.
///
/// A REAL LISTENER AND A REAL CONNECT, not a socket pair: `EditorBridge` has no entry point that
/// adopts a descriptor, because a runtime always listens — and a test that reached past the
/// interface to install one would be testing a shape the product does not have.
class Session {
public:
    Session() {
        (void)std::snprintf(path_, sizeof(path_), "/tmp/cy-bridge-%d.sock",
                            static_cast<int>(::getpid()));
        CY_REQUIRE(bridge_.listen(path_));

        editor_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CY_REQUIRE(editor_ >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_, std::strlen(path_) + 1);
        CY_REQUIRE(
            ::connect(editor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        bridge_.service();
        CY_REQUIRE(bridge_.connected());
    }

    ~Session() {
        if (editor_ >= 0) {
            (void)::close(editor_);
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] EditorBridge& bridge() noexcept { return bridge_; }
    [[nodiscard]] int editor() const noexcept { return editor_; }

    /// Put one or more framed messages on the wire in ONE write, from the editor's end.
    void send(std::initializer_list<const cy::Array<u8>*> payloads) const {
        cy::Array<u8> stream;
        for (const cy::Array<u8>* payload : payloads) {
            cy::Array<u8> framed;
            CY_REQUIRE(EditorBridge::frame_message(payload->span(), framed));
            CY_REQUIRE(stream.append(framed.span()));
        }
        CY_REQUIRE(::send(editor_, stream.data(), stream.size(), 0) ==
                   static_cast<ssize_t>(stream.size()));
    }

    /// Poll until a message arrives or the patience runs out. The socket is non-blocking and the
    /// bytes are already in the kernel, so this spins rather than sleeps.
    [[nodiscard]] bool next(EditorRequest& out) {
        for (u32 attempt = 0; attempt < 10'000; ++attempt) {
            if (bridge_.poll(out)) {
                return true;
            }
        }
        return false;
    }

private:
    EditorBridge bridge_;
    int editor_ = -1;
    char path_[64] = {};
};

}  // namespace

CY_TEST_CASE("a socket path past the kernel's limit is refused rather than truncated") {
    // M7 task 5b.6's defect, on the control socket. `sun_path` is 108 bytes; a longer path silently
    // binds a DIFFERENT socket, so the runtime starts, reports nothing wrong, and the editor never
    // connects.
    EditorBridge bridge;
    char path[200];
    std::memset(path, 'a', sizeof(path));
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';
    const cy::Status refused = bridge.listen(path);
    CY_CHECK_FALSE(refused);
    CY_CHECK_FALSE(bridge.listen(nullptr));
    CY_CHECK_FALSE(bridge.listen(""));
}

CY_TEST_CASE("a frame is a length, a checksum and a payload, in that order") {
    cy::Array<u8> payload;
    for (u8 value = 0; value < 32; ++value) {
        CY_REQUIRE(payload.push_back(value));
    }
    cy::Array<u8> framed;
    CY_REQUIRE(EditorBridge::frame_message(payload.span(), framed));
    CY_REQUIRE_EQ(framed.size(), payload.size() + 8U);
    CY_CHECK_EQ(framed[0], 32U);
    CY_CHECK_EQ(framed[1], 0U);
    // FNV-1a over the payload, which is what `cy_editor_protocol::frame` writes beside the length.
    // Restated as a number so that a change to either side fails here rather than as a connection
    // that drops on its first message.
    const u32 checksum = EditorBridge::checksum(payload.span());
    CY_CHECK_EQ(static_cast<u32>(framed[4]), checksum & 0xFFU);
    CY_CHECK_EQ(static_cast<u32>(framed[7]), (checksum >> 24) & 0xFFU);
    CY_CHECK_EQ(framed[8], 0U);
    CY_CHECK_EQ(framed[9], 1U);
}

CY_TEST_CASE("the checksum is the one the editor computes") {
    // `cy_editor_protocol::frame::checksum`, restated: FNV-1a, 32-bit, over the payload.
    const u8 bytes[] = {'c', 'y'};
    u32 expected = 0x811C'9DC5U;
    for (const u8 value : bytes) {
        expected ^= static_cast<u32>(value);
        expected *= 0x0100'0193U;
    }
    CY_CHECK_EQ(EditorBridge::checksum(cy::Span<const u8>{bytes, 2}), expected);
    CY_CHECK_EQ(EditorBridge::checksum(cy::Span<const u8>{}), 0x811C'9DC5U);
}

CY_TEST_CASE("a length past the limit ends the connection rather than being believed") {
    // The bound exists so that a length read from a stream that is NOT this protocol cannot become
    // an allocation sized by a peer. Checked on the READ side, where it matters and where it costs
    // eight bytes; the write side refuses the same number, and a case that built a sixteen-megabyte
    // message to prove it would be twenty milliseconds of memset in a suite budgeted at one.
    Session session;
    const u8 header[8] = {0x00, 0x00, 0x00, 0x7F, 0, 0, 0, 0};
    CY_REQUIRE(::send(session.editor(), header, sizeof(header), 0) == 8);
    EditorRequest request;
    for (u32 attempt = 0; attempt < 1000 && session.bridge().connected(); ++attempt) {
        (void)session.bridge().poll(request);
    }
    CY_CHECK_FALSE(session.bridge().connected());
}

CY_TEST_CASE("a hello is decoded with the editor's ABI on it") {
    Session session;
    const cy::Array<u8> message = hello(3, 7);
    session.send({&message});
    EditorRequest request;
    CY_REQUIRE(session.next(request));
    CY_CHECK(request.kind == EditorMessage::Hello);
    CY_CHECK_EQ(request.abi_major, 3U);
    CY_CHECK_EQ(request.abi_minor, 7U);
    CY_CHECK(std::strcmp(editor_message_name(request.kind), "hello") == 0);
}

CY_TEST_CASE("a message the caller reads after poll is still its own") {
    // REGRESSION, M7 task 5b.3. `EditorRequest::payload` was a span into the read buffer, and
    // `poll` compacts that buffer once it has taken a frame out of it — so a caller reading the
    // payload after `poll` returned was reading the NEXT message. With two messages in one write
    // it decoded the second one's tag as the first one's transaction, and reported "a transaction
    // ends before its actor": a decoder failing on bytes that belonged to something else, which
    // reads as a protocol version mismatch and is not one.
    Session session;
    const cy::Array<u8> first = gizmo_intent(11, 101, 0xA0);
    const cy::Array<u8> second = gizmo_intent(12, 102, 0xB0);
    session.send({&first, &second});

    EditorRequest request;
    CY_REQUIRE(session.next(request));
    CY_CHECK_EQ(request.request, 11ULL);
    CY_CHECK_EQ(request.viewport, 101ULL);
    CY_REQUIRE_EQ(request.payload.size(), 3U);
    // Read AFTER the poll that produced it, which is what every caller does and what the defect
    // made unsafe.
    CY_CHECK_EQ(request.payload[0], 0xA0U);
    CY_CHECK_EQ(request.payload[2], 0xA2U);

    CY_REQUIRE(session.next(request));
    CY_CHECK_EQ(request.request, 12ULL);
    CY_CHECK_EQ(request.payload[0], 0xB0U);
}

CY_TEST_CASE("a message split across two writes is completed rather than refused") {
    // A stream socket has no message boundaries, and a reader that discarded a partial frame would
    // turn a large message into a protocol error at whatever size the kernel decided to split it.
    Session session;
    const cy::Array<u8> message = gizmo_intent(5, 6, 0x40);
    cy::Array<u8> framed;
    CY_REQUIRE(EditorBridge::frame_message(message.span(), framed));

    CY_REQUIRE(::send(session.editor(), framed.data(), 6, 0) == 6);
    EditorRequest request;
    CY_CHECK_FALSE(session.bridge().poll(request));
    CY_REQUIRE(::send(session.editor(), framed.data() + 6, framed.size() - 6, 0) ==
               static_cast<ssize_t>(framed.size() - 6));
    CY_REQUIRE(session.next(request));
    CY_CHECK_EQ(request.request, 5ULL);
    CY_CHECK_EQ(request.payload[0], 0x40U);
}

CY_TEST_CASE("a frame whose checksum does not match ends the connection") {
    // The stream is out of step and the only honest thing left is to start again. The checksum is
    // not for corruption over a Unix socket, of which there is none: it is for a peer that crashed
    // mid-write, which is the case `editor-rust-application` requires the editor to survive.
    Session session;
    const cy::Array<u8> message = gizmo_intent(1, 2, 0x10);
    cy::Array<u8> framed;
    CY_REQUIRE(EditorBridge::frame_message(message.span(), framed));
    framed[framed.size() - 1] ^= 0xFFU;
    CY_REQUIRE(::send(session.editor(), framed.data(), framed.size(), 0) ==
               static_cast<ssize_t>(framed.size()));
    EditorRequest request;
    for (u32 attempt = 0; attempt < 1000 && session.bridge().connected(); ++attempt) {
        (void)session.bridge().poll(request);
    }
    CY_CHECK_FALSE(session.bridge().connected());
}

CY_TEST_CASE("a tag this build does not know is reported rather than fatal") {
    // The editor is entitled to be newer. A message a runtime cannot answer is a missing feature,
    // not a broken stream, and the host is what decides what to say about it.
    Session session;
    cy::Array<u8> unknown;
    CY_REQUIRE(unknown.push_back(200));
    little_endian(unknown, 1, 8);
    session.send({&unknown});
    EditorRequest request;
    CY_REQUIRE(session.next(request));
    CY_CHECK(request.kind == EditorMessage::Unknown);
    CY_CHECK(session.bridge().connected());
}

CY_TEST_CASE("every answer is a frame the editor's reader accepts") {
    // The answers, checked as BYTES: a length, a checksum that matches, and a tag the editor's
    // `Message::decode` knows. Two languages, one wire, and nothing that compares them but this.
    Session session;
    const cy::Array<u8> message = hello(1, 0);
    session.send({&message});
    EditorRequest request;
    CY_REQUIRE(session.next(request));
    CY_REQUIRE(session.bridge().send_welcome(1, 0, "cy_editor_window_runtime"));
    CY_REQUIRE(session.bridge().send_pong(1016));
    const u8 layout[] = {1, 2, 3};
    CY_REQUIRE(session.bridge().send_gizmo_geometry(9, cy::Span<const u8>{layout, 3}));
    const cy::f32 position[3] = {0.0F, 2.0F, 8.0F};
    const cy::f32 rotation[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    CY_REQUIRE(session.bridge().send_view_suggestion(position, rotation, 1.0472F, 0.1F));

    u8 buffer[512] = {};
    const ssize_t read = ::recv(session.editor(), buffer, sizeof(buffer), 0);
    CY_REQUIRE(read > 0);
    // Walk the four frames, checking each length and checksum and collecting the tags.
    cy::usize offset = 0;
    cy::Array<u8> tags;
    for (u32 index = 0; index < 4; ++index) {
        CY_REQUIRE(offset + 8 <= static_cast<cy::usize>(read));
        u32 length = 0;
        u32 expected = 0;
        std::memcpy(&length, buffer + offset, 4);
        std::memcpy(&expected, buffer + offset + 4, 4);
        CY_REQUIRE(offset + 8 + length <= static_cast<cy::usize>(read));
        CY_CHECK_EQ(EditorBridge::checksum(cy::Span<const u8>{buffer + offset + 8, length}),
                    expected);
        CY_REQUIRE(tags.push_back(buffer[offset + 8]));
        offset += 8 + length;
    }
    CY_CHECK_EQ(offset, static_cast<cy::usize>(read));
    CY_CHECK_EQ(tags[0], static_cast<u8>(EditorMessage::Welcome));
    CY_CHECK_EQ(tags[1], static_cast<u8>(EditorMessage::Pong));
    CY_CHECK_EQ(tags[2], static_cast<u8>(EditorMessage::GizmoGeometry));
    CY_CHECK_EQ(tags[3], static_cast<u8>(EditorMessage::ViewSuggested));
}

CY_TEST_CASE("an editor that goes away is noticed rather than written to for ever") {
    Session session;
    (void)::close(session.editor());
    EditorRequest request;
    for (u32 attempt = 0; attempt < 1000 && session.bridge().connected(); ++attempt) {
        (void)session.bridge().poll(request);
    }
    CY_CHECK_FALSE(session.bridge().connected());
    CY_CHECK_FALSE(session.bridge().send_pong(1));
}
