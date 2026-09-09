// The hosted runtime's side of the editor bridge. See cy/runtime/editor_bridge/bridge.h.

#include <cy/runtime/editor_bridge/bridge.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace cy::runtime {
namespace {

/// The largest message this protocol accepts.
///
/// A bound rather than a limit anyone should reach: control messages are tens of bytes. It exists
/// so that a length read from a stream that is NOT this protocol cannot make the reader allocate
/// sixteen megabytes before discovering that. The same number `cy_editor_protocol::frame` uses.
constexpr usize kMaxFrameBytes = static_cast<usize>(16U) * 1024U * 1024U;

/// The header: a four-byte length and a four-byte checksum.
constexpr usize kHeaderBytes = 8;

/// How much is read from the socket in one go.
constexpr usize kReadChunk = 4096;

[[nodiscard]] u32 read_u32(const u8* bytes) noexcept {
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
           (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

[[nodiscard]] u64 read_u64(const u8* bytes) noexcept {
    u64 value = 0;
    for (usize index = 0; index < 8; ++index) {
        value |= static_cast<u64>(bytes[index]) << (index * 8);
    }
    return value;
}

/// The editor's codec, in the one direction this module writes it.
///
/// `cy_editor_core::codec`: little-endian throughout, a `u32` length before a byte string, and a
/// string is a length-prefixed byte string. Restated rather than shared because the two ends are
/// two languages; `integration.editor_bridge` is what keeps the restatement true.
class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(&out) { out_->clear(); }

    [[nodiscard]] Status u8_value(u8 value) noexcept { return out_->push_back(value); }

    [[nodiscard]] Status u32_value(u32 value) noexcept {
        for (usize index = 0; index < 4; ++index) {
            if (Status pushed = out_->push_back(static_cast<u8>((value >> (index * 8)) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    [[nodiscard]] Status u64_value(u64 value) noexcept {
        for (usize index = 0; index < 8; ++index) {
            if (Status pushed = out_->push_back(static_cast<u8>((value >> (index * 8)) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    [[nodiscard]] Status bytes(Span<const u8> value) noexcept {
        if (Status written = u32_value(static_cast<u32>(value.size())); !written) {
            return written;
        }
        return out_->append(value);
    }

    [[nodiscard]] Status f32_value(f32 value) noexcept {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32_value(bits);
    }

    [[nodiscard]] Status text(const char* value) noexcept {
        const char* safe = (value != nullptr) ? value : "";
        return bytes(Span<const u8>{reinterpret_cast<const u8*>(safe), std::strlen(safe)});
    }

private:
    Array<u8>* out_;
};

/// A bounds-checked reader over one decoded frame.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u8_value(u8& out) noexcept {
        if (offset_ + 1 > bytes_.size()) {
            return false;
        }
        out = bytes_[offset_++];
        return true;
    }

    [[nodiscard]] bool u32_value(u32& out) noexcept {
        if (offset_ + 4 > bytes_.size()) {
            return false;
        }
        out = read_u32(bytes_.data() + offset_);
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool u64_value(u64& out) noexcept {
        if (offset_ + 8 > bytes_.size()) {
            return false;
        }
        out = read_u64(bytes_.data() + offset_);
        offset_ += 8;
        return true;
    }

    [[nodiscard]] bool byte_span(Span<const u8>& out) noexcept {
        u32 length = 0;
        if (!u32_value(length) || offset_ + length > bytes_.size()) {
            return false;
        }
        out = Span<const u8>{bytes_.data() + offset_, length};
        offset_ += length;
        return true;
    }

    /// Skip a length-prefixed string nothing here needs the contents of.
    [[nodiscard]] bool skip_text() noexcept {
        Span<const u8> ignored;
        return byte_span(ignored);
    }

private:
    Span<const u8> bytes_;
    usize offset_ = 0;
};

/// Read one message's payload into a request.
///
/// A free function rather than a member: it reads the bytes it is handed and nothing else, which
/// is what makes it checkable on its own and what keeps the decoding of a wire out of the class
/// that owns a socket.
[[nodiscard]] bool decode_message(Span<const u8> payload, EditorRequest& out) noexcept {
    Reader reader(payload);
    u8 tag = 0;
    if (!reader.u8_value(tag)) {
        return false;
    }
    out = EditorRequest{};
    switch (tag) {
        case static_cast<u8>(EditorMessage::Hello):
            out.kind = EditorMessage::Hello;
            return reader.u32_value(out.abi_major) && reader.u32_value(out.abi_minor) &&
                   reader.skip_text();
        case static_cast<u8>(EditorMessage::Ping):
            out.kind = EditorMessage::Ping;
            return reader.u64_value(out.frame);
        case static_cast<u8>(EditorMessage::Apply):
            out.kind = EditorMessage::Apply;
            {
                u8 when = 0;
                return reader.u64_value(out.request) && reader.u64_value(out.frame) &&
                       reader.u8_value(when) && reader.byte_span(out.payload);
            }
        case static_cast<u8>(EditorMessage::Pick):
            out.kind = EditorMessage::Pick;
            return reader.u64_value(out.request) && reader.u64_value(out.frame) &&
                   reader.byte_span(out.payload);
        case static_cast<u8>(EditorMessage::GizmoIntent):
            out.kind = EditorMessage::GizmoIntent;
            return reader.u64_value(out.request) && reader.u64_value(out.viewport) &&
                   reader.byte_span(out.payload);
        case static_cast<u8>(EditorMessage::Play):
            out.kind = EditorMessage::Play;
            // The state's name lands in `payload`, because a length-prefixed string and a
            // length-prefixed byte string are the same three lines on the wire. See the field.
            return reader.u64_value(out.request) && reader.byte_span(out.payload);
        default:
            // A tag this build does not know is REPORTED rather than closing the connection: the
            // editor is entitled to be newer, and a message a runtime cannot answer is a missing
            // feature rather than a broken stream. The host decides what to say about it.
            out.kind = EditorMessage::Unknown;
            return true;
    }
}

}  // namespace

const char* editor_message_name(EditorMessage message) noexcept {
    switch (message) {
        case EditorMessage::Hello:
            return "hello";
        case EditorMessage::Welcome:
            return "welcome";
        case EditorMessage::Refused:
            return "refused";
        case EditorMessage::Apply:
            return "apply";
        case EditorMessage::Applied:
            return "applied";
        case EditorMessage::Rejected:
            return "rejected";
        case EditorMessage::Ping:
            return "ping";
        case EditorMessage::Pong:
            return "pong";
        case EditorMessage::Reload:
            return "reload";
        case EditorMessage::Reloaded:
            return "reloaded";
        case EditorMessage::Pick:
            return "pick";
        case EditorMessage::Picked:
            return "picked";
        case EditorMessage::GizmoIntent:
            return "gizmo-intent";
        case EditorMessage::GizmoGeometry:
            return "gizmo-geometry";
        case EditorMessage::ViewSuggested:
            return "view-suggested";
        case EditorMessage::Play:
            return "play";
        case EditorMessage::Playing:
            return "playing";
        case EditorMessage::Unknown:
            break;
    }
    return "unknown";
}

EditorBridge::~EditorBridge() {
    drop_client();
    if (listener_ >= 0) {
        (void)::close(listener_);
        listener_ = -1;
        if (path_[0] != '\0') {
            (void)::unlink(path_);
        }
    }
}

u32 EditorBridge::checksum(Span<const u8> payload) noexcept {
    u32 hash = 0x811C'9DC5U;
    for (const u8 byte : payload) {
        hash ^= static_cast<u32>(byte);
        hash *= 0x0100'0193U;
    }
    return hash;
}

Status EditorBridge::frame_message(Span<const u8> payload, Array<u8>& out) noexcept {
    if (payload.size() > kMaxFrameBytes) {
        return fail(ErrorCode::InvalidArgument, "the message is past this protocol's size limit");
    }
    out.clear();
    if (Status reserved = out.reserve(payload.size() + kHeaderBytes); !reserved) {
        return reserved;
    }
    const u32 length = static_cast<u32>(payload.size());
    const u32 hash = checksum(payload);
    for (const u32 header : {length, hash}) {
        for (usize index = 0; index < 4; ++index) {
            if (Status pushed = out.push_back(static_cast<u8>((header >> (index * 8)) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
    }
    return out.append(payload);
}

Status EditorBridge::listen(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "the editor bridge needs a socket path");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const usize length = std::strlen(path);
    if (length >= sizeof(address.sun_path)) {
        return fail(ErrorCode::InvalidArgument,
                    "the runtime socket path is longer than a Unix socket allows (107 bytes)");
    }
    std::memcpy(address.sun_path, path, length + 1);
    std::memcpy(path_, path, length + 1);

    listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_ < 0) {
        return fail(ErrorCode::Unavailable, "creating the runtime socket", errno);
    }
    (void)::unlink(path);
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return fail(ErrorCode::Unavailable, "binding the runtime socket", errno);
    }
    if (::listen(listener_, 4) != 0) {
        return fail(ErrorCode::Unavailable, "listening on the runtime socket", errno);
    }
    return ok();
}

void EditorBridge::drop_client() noexcept {
    if (client_ >= 0) {
        (void)::close(client_);
        client_ = -1;
    }
    incoming_.clear();
}

void EditorBridge::service() noexcept {
    if (client_ >= 0 || listener_ < 0) {
        return;
    }
    const int stream = ::accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (stream < 0) {
        return;
    }
    client_ = stream;
    connections_ += 1;
}

bool EditorBridge::poll(EditorRequest& out) noexcept {
    if (client_ < 0) {
        return false;
    }
    // Read whatever is there before looking for a frame, so that a message split across two reads
    // is completed rather than reported as "nothing yet" for ever.
    for (;;) {
        const usize before = incoming_.size();
        if (Status grown = incoming_.resize(before + kReadChunk); !grown) {
            (void)incoming_.resize(before);
            break;
        }
        const ssize_t read = ::recv(client_, incoming_.data() + before, kReadChunk, MSG_DONTWAIT);
        if (read > 0) {
            (void)incoming_.resize(before + static_cast<usize>(read));
            continue;
        }
        (void)incoming_.resize(before);
        if (read == 0) {
            // A clean close and a crash look the same from here — the stream ends — and the
            // difference is a question for the host, which knows whether it asked the editor to go.
            drop_client();
            return false;
        }
        break;  // EAGAIN: nothing more for now
    }

    if (incoming_.size() < kHeaderBytes) {
        return false;
    }
    const u32 length = read_u32(incoming_.data());
    const u32 expected = read_u32(incoming_.data() + 4);
    if (length > kMaxFrameBytes) {
        // Not this protocol, or a stream that is out of step. Either way nothing after this point
        // can be trusted, and believing the length would be an allocation sized by a peer.
        drop_client();
        return false;
    }
    if (incoming_.size() < kHeaderBytes + length) {
        return false;
    }
    const Span<const u8> framed{incoming_.data() + kHeaderBytes, length};
    if (checksum(framed) != expected) {
        drop_client();
        return false;
    }
    // COPIED OUT BEFORE THE COMPACTION BELOW MOVES IT. `EditorRequest::payload` is a span, and the
    // caller reads it after `poll` returns; a span into `incoming_` would by then be pointing at
    // whatever message followed this one. See the note beside `EditorRequest::payload`.
    message_.clear();
    if (Status copied = message_.append(framed); !copied) {
        return false;
    }
    const bool decoded = decode_message(message_.span(), out);
    // Compact: move whatever follows this frame to the front. A ring buffer would avoid the move
    // and would be the wrong trade here — control messages are tens of bytes and arrive a handful
    // per frame, and the simple buffer is the one a reader can check by eye.
    const usize consumed = kHeaderBytes + length;
    const usize remaining = incoming_.size() - consumed;
    if (remaining > 0) {
        std::memmove(incoming_.data(), incoming_.data() + consumed, remaining);
    }
    (void)incoming_.resize(remaining);
    return decoded;
}

Status EditorBridge::send_view_suggestion(const f32 position[3], const f32 rotation[4],
                                          f32 fov_y_radians, f32 near_plane) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::ViewSuggested)); !written) {
        return written;
    }
    for (u32 lane = 0; lane < 3; ++lane) {
        if (Status written = writer.f32_value(position[lane]); !written) {
            return written;
        }
    }
    for (u32 lane = 0; lane < 4; ++lane) {
        if (Status written = writer.f32_value(rotation[lane]); !written) {
            return written;
        }
    }
    if (Status written = writer.f32_value(fov_y_radians); !written) {
        return written;
    }
    if (Status written = writer.f32_value(near_plane); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send(Span<const u8> payload) noexcept {
    if (client_ < 0) {
        return fail(ErrorCode::Unavailable, "no editor is connected to answer");
    }
    Array<u8> framed;
    if (Status built = frame_message(payload, framed); !built) {
        return built;
    }
    usize written = 0;
    while (written < framed.size()) {
        const ssize_t sent =
            ::send(client_, framed.data() + written, framed.size() - written, MSG_NOSIGNAL);
        if (sent > 0) {
            written += static_cast<usize>(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            // The socket is non-blocking and the answers are tens of bytes, so this is a full
            // kernel buffer rather than a slow editor. Retrying is bounded by the editor draining
            // it, and the alternative — an outgoing queue — would be state to keep for a case that
            // does not arise at this message size.
            continue;
        }
        drop_client();
        return fail(ErrorCode::Unavailable, "the editor went away before its answer was written",
                    errno);
    }
    return ok();
}

Status EditorBridge::send_welcome(u32 abi_major, u32 abi_minor, const char* runtime) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::Welcome)); !written) {
        return written;
    }
    if (Status written = writer.u32_value(abi_major); !written) {
        return written;
    }
    if (Status written = writer.u32_value(abi_minor); !written) {
        return written;
    }
    if (Status written = writer.text(runtime); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send_refused(const char* reason, const char* remedy) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::Refused)); !written) {
        return written;
    }
    if (Status written = writer.text(reason); !written) {
        return written;
    }
    if (Status written = writer.text(remedy); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send_pong(u64 frame) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::Pong)); !written) {
        return written;
    }
    if (Status written = writer.u64_value(frame); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send_applied(u64 request, u64 frame, Span<const u8> observed) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::Applied)); !written) {
        return written;
    }
    if (Status written = writer.u64_value(request); !written) {
        return written;
    }
    if (Status written = writer.u64_value(frame); !written) {
        return written;
    }
    if (Status written = writer.bytes(observed); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send_rejected(u64 request, const char* reason, const char* remedy) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::Rejected)); !written) {
        return written;
    }
    if (Status written = writer.u64_value(request); !written) {
        return written;
    }
    if (Status written = writer.text(reason); !written) {
        return written;
    }
    if (Status written = writer.text(remedy); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send_playing(u64 request, const char* state, const char* detail) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::Playing)); !written) {
        return written;
    }
    if (Status written = writer.u64_value(request); !written) {
        return written;
    }
    if (Status written = writer.text(state); !written) {
        return written;
    }
    if (Status written = writer.text(detail); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send_picked(u64 request, Span<const u8> candidates) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::Picked)); !written) {
        return written;
    }
    if (Status written = writer.u64_value(request); !written) {
        return written;
    }
    if (Status written = writer.bytes(candidates); !written) {
        return written;
    }
    return send(outgoing_.span());
}

Status EditorBridge::send_gizmo_geometry(u64 request, Span<const u8> layout) noexcept {
    Writer writer(outgoing_);
    if (Status written = writer.u8_value(static_cast<u8>(EditorMessage::GizmoGeometry)); !written) {
        return written;
    }
    if (Status written = writer.u64_value(request); !written) {
        return written;
    }
    if (Status written = writer.bytes(layout); !written) {
        return written;
    }
    return send(outgoing_.span());
}

}  // namespace cy::runtime
