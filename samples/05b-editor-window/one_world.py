#!/usr/bin/env python3
"""One world, over the wire, against the real runtime. M8.a tasks 1.1, 1.3 and 1.4.

WHY THIS EXISTS BESIDE THE C++ TESTS. `integration.editor_window_one_world` proves the arithmetic
without a device: the world loads, a transaction lands, a pick resolves. What it cannot prove is the
part that only exists when there are two processes and a Vulkan device between them — that the
runtime on the far end of the editor's control socket answers a `Message::Pick` at all (M7 refused
it by name), that the gizmo it publishes sits on the object the editor created, and that the frame
carrying an edit is the frame after the commit rather than some frame later.

So this speaks the editor's own protocol — `cy_editor_protocol::frame` and `Message` — to a real
`cy_editor_window_runtime`, and asserts on what comes back. It is a DRIVER, not a second editor:
every byte it writes is written out from the encoding in `editor/crates/cy-editor-protocol/`, and a
change there fails here with a decode error rather than silently.

    python3 samples/05b-editor-window/one_world.py --runtime build/<label>/cy_editor_window_runtime

Exits non-zero on any gap, and prints the numbers it measured. Both are `samples/harness/artefact.py`
rules and they apply to a probe as much as to an artefact.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
PROJECT = REPO / "samples" / "05b-editor-window" / "project"
ASSET_PATH = "worlds/city.cyworld"

# --- cy_editor_core::ids ----------------------------------------------------------------------
#
# The identities are DERIVED on both sides rather than transmitted, which is the whole of task 1.1.
# This is the third independent implementation of the same hash — Rust, C++, and here — and the
# three agreeing is what the claim rests on.

_FNV_OFFSET = 0x6C62272E07BB014262B821756295C58D
_FNV_PRIME = 0x0000000001000000000000000000013B
_MASK = (1 << 128) - 1


def fnv1a_128(data: bytes) -> int:
    value = _FNV_OFFSET
    for byte in data:
        value ^= byte
        value = (value * _FNV_PRIME) & _MASK
    return value


def document_id(asset_path: str) -> int:
    return fnv1a_128(asset_path.encode("utf-8"))


def node_id(document: int, ordinal: int) -> int:
    return fnv1a_128(document.to_bytes(16, "little") + ordinal.to_bytes(8, "little"))


# --- cy_editor_core::codec --------------------------------------------------------------------


class Writer:
    def __init__(self) -> None:
        self.data = bytearray()

    def u8(self, value: int) -> "Writer":
        self.data.append(value & 0xFF)
        return self

    def u32(self, value: int) -> "Writer":
        self.data += struct.pack("<I", value & 0xFFFFFFFF)
        return self

    def u64(self, value: int) -> "Writer":
        self.data += struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)
        return self

    def u128(self, value: int) -> "Writer":
        return self.u64(value & 0xFFFFFFFFFFFFFFFF).u64(value >> 64)

    def f32(self, value: float) -> "Writer":
        self.data += struct.pack("<f", value)
        return self

    def text(self, value: str) -> "Writer":
        raw = value.encode("utf-8")
        return self.u32(len(raw)).raw(raw)

    def raw(self, value: bytes) -> "Writer":
        self.data += value
        return self

    def bytes_field(self, value: bytes) -> "Writer":
        return self.u32(len(value)).raw(value)


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.at = 0

    def take(self, count: int) -> bytes:
        if self.at + count > len(self.data):
            raise ValueError("a message ended early")
        out = self.data[self.at : self.at + count]
        self.at += count
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def f32(self) -> float:
        return struct.unpack("<f", self.take(4))[0]

    def text(self) -> str:
        return self.take(self.u32()).decode("utf-8", "replace")

    def bytes_field(self) -> bytes:
        return self.take(self.u32())


# --- cy_editor_protocol::frame -----------------------------------------------------------------


def checksum(payload: bytes) -> int:
    value = 0x811C9DC5
    for byte in payload:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


class Bridge:
    """The editor's end of the runtime's control socket."""

    def __init__(self, path: str) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10.0)
        self.sock.connect(path)
        self.buffer = b""

    def send(self, payload: bytes) -> None:
        self.sock.sendall(struct.pack("<II", len(payload), checksum(payload)) + payload)

    def _fill(self, wanted: int) -> None:
        while len(self.buffer) < wanted:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ValueError("the runtime closed the control socket")
            self.buffer += chunk

    def receive(self) -> tuple[int, Reader]:
        self._fill(8)
        length, expected = struct.unpack("<II", self.buffer[:8])
        self._fill(8 + length)
        payload = self.buffer[8 : 8 + length]
        self.buffer = self.buffer[8 + length :]
        if checksum(payload) != expected:
            raise ValueError("a frame's checksum does not match")
        reader = Reader(payload)
        return reader.u8(), reader

    def await_tag(self, tag: int, what: str, patience: int = 64):
        """The next message with this tag, skipping the ones a runtime volunteers."""
        for _ in range(patience):
            got, reader = self.receive()
            if got == tag:
                return reader
            if got == 5:  # Rejected
                reader.u64()
                raise ValueError(f"{what}: the runtime refused it — {reader.text()}")
        raise ValueError(f"{what}: no answer arrived")

    def close(self) -> None:
        self.sock.close()


# --- the messages this driver sends ------------------------------------------------------------


def hello() -> bytes:
    return Writer().u8(0).u32(1).u32(0).text("one_world.py").data


def apply_message(request: int, transaction: bytes) -> bytes:
    # `Message::Apply`: a request, the frame it is about, when to apply it, then the bytes.
    return Writer().u8(3).u64(request).u64(0).u8(0).bytes_field(transaction).data


def pick_message(request: int, frame: int, pick: bytes) -> bytes:
    return Writer().u8(10).u64(request).u64(frame).bytes_field(pick).data


def gizmo_message(request: int, viewport: int, intent: bytes) -> bytes:
    return Writer().u8(12).u64(request).u64(viewport).bytes_field(intent).data


def created_box(document: int, ordinal: int, position: tuple[float, float, float]) -> bytes:
    """The transaction the editor commits when a person creates an object and places it.

    Type 3 is `Transform` and fields 3, 4 and 5 are its rotation, translation and scale — read
    straight out of the world file's own `type` section, which is where the editor's document got
    them too. Nothing here guesses which field is which; see `world_transaction.h`.
    """
    node = node_id(document, ordinal)
    writer = Writer()
    writer.u64(0x0805).u128(document).text("Create a box")
    writer.u8(0).text("one_world.py")  # Actor::Human
    writer.u8(0)  # no coalesce key
    writer.u32(2)
    writer.u8(0).u128(node).u8(0)  # CreateNode, no parent
    writer.u8(4).u128(node).u64(3).u32(3)  # AddComponent Transform, three fields
    writer.u64(3).u8(8).f32(0.0).f32(0.0).f32(0.0).f32(1.0)  # rotation
    writer.u64(4).u8(6).f32(position[0]).f32(position[1]).f32(position[2])  # translation
    writer.u64(5).u8(6).f32(1.0).f32(1.0).f32(1.0)  # scale
    return bytes(writer.data)


def gizmo_intent(frame: int, identity: int, width: int, height: int,
                 camera: tuple[float, float, float]) -> bytes:
    """`cy_editor_services::gizmo::Request::encode`."""
    writer = Writer()
    writer.u64(frame).u8(0).u8(0).u8(0)  # frame, Translate, World space, the median pivot
    writer.u32(1).u64(identity)
    writer.u32(width).u32(height)
    for lane in camera:
        writer.f32(lane)
    writer.f32(0.0).f32(0.0).f32(0.0).f32(1.0)  # the identity rotation: looking down -Z
    writer.f32(0.9).f32(0.1)
    return bytes(writer.data)


def click(frame: int, x: float, y: float) -> bytes:
    """`cy_editor_viewport::picking::PickRequest::encode`, as a click."""
    writer = Writer()
    writer.u64(1).u64(frame)
    writer.u8(0).f32(x).f32(y)
    writer.u32(0xFFFFFFFF).u8(1).u32(16).u32(0)
    return bytes(writer.data)


def read_layout(payload: bytes) -> dict:
    """`cy::render::encode_gizmo_layout`."""
    reader = Reader(payload)
    version = reader.u8()
    if version != 1:
        raise ValueError(f"a gizmo layout of version {version}")
    layout = {
        "frame": reader.u64(),
        "mode": reader.u8(),
        "centre_x": reader.f32(),
        "centre_y": reader.f32(),
        "extent": reader.f32(),
        "handles": [],
    }
    for _ in range(reader.u32()):
        layout["handles"].append(
            (reader.u8(), reader.f32(), reader.f32(), reader.f32(), reader.f32())
        )
    return layout


def read_candidates(payload: bytes) -> tuple[int, list[tuple[int, float, bool]]]:
    """`cy_editor_viewport::picking::PickResponse::decode`."""
    reader = Reader(payload)
    frame = reader.u64()
    out = []
    for _ in range(reader.u32()):
        out.append((reader.u64(), reader.f32(), reader.u8() != 0))
    return frame, out


# --- the run -------------------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, help="the cy_editor_window_runtime binary")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--seconds", type=float, default=25.0)
    parser.add_argument(
        "--misname", action="store_true",
        help="tell the runtime a different path for the same bytes, so its identities are another "
             "document's. The run must then FAIL — it is how this probe proves that its own "
             "success means something, and that the seam is identity rather than order.",
    )
    args = parser.parse_args()

    # A SHORT SOCKET PATH. A Unix socket path is capped at 108 bytes and a build directory under a
    # home directory overruns it; the failure is an opaque "invalid argument" at bind.
    holder = tempfile.mkdtemp(prefix="cy1w-", dir="/tmp")
    control = os.path.join(holder, "c.sock")
    frames = os.path.join(holder, "f.sock")

    command = [
        args.runtime,
        "--project", str(PROJECT),
        "--world", ("./" + ASSET_PATH) if args.misname else ASSET_PATH,
        "--host", control,
        "--socket", frames,
        "--width", str(args.width),
        "--height", str(args.height),
        "--seconds", str(args.seconds),
        "--rate", "60",
        "--no-validation",
    ]
    print("$ " + " ".join(command), flush=True)
    runtime = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)

    gaps: list[str] = []
    measured: dict[str, object] = {}
    try:
        deadline = time.monotonic() + 20.0
        bridge = None
        while bridge is None and time.monotonic() < deadline:
            if runtime.poll() is not None:
                print(runtime.stdout.read())
                print("one_world: the runtime exited before it opened its control socket")
                return 2
            try:
                bridge = Bridge(control)
            except OSError:
                time.sleep(0.1)
        if bridge is None:
            gaps.append("the runtime never opened its control socket")
            raise SystemExit

        document = document_id(ASSET_PATH)
        created = node_id(document, 4) & 0xFFFFFFFFFFFFFFFF

        bridge.send(hello())
        welcome = bridge.await_tag(1, "the handshake")
        welcome.u32(), welcome.u32()
        measured["runtime"] = welcome.text()

        # 1.1 — an entity created in the editor. The runtime is told nothing but the transaction.
        started = time.monotonic()
        bridge.send(apply_message(1, created_box(document, 4, (0.0, 3.0, 0.0))))
        bridge.await_tag(4, "the create")
        measured["apply_round_trip_ms"] = round((time.monotonic() - started) * 1000.0, 3)

        # --- the two halves of "one world" ----------------------------------------------------
        #
        # A node the runtime LOADED and a node the editor CREATED, checked the same way. The first
        # is the harder claim: its identity was never transmitted at all, and it is right only if
        # the runtime derived exactly what the editor derived from the file it read.
        crate = node_id(document, 2) & 0xFFFFFFFFFFFFFFFF

        def focus(identity: int, camera: tuple[float, float, float], label: str) -> dict:
            """Ask where the gizmo is until the answer is for the camera the driver asked for.

            TWICE OR MORE, and the reason is `answer_gizmo`'s rule rather than a race: an intent
            carries the editor's camera and the runtime answers against THE FRAME THE EDITOR IS
            SHOWING, which is the frame before it adopted that camera. A layout hit-tested against
            a frame the user never saw is the defect the frame identifier exists to catch.
            """
            nonlocal request
            layout = None
            for _ in range(8):
                bridge.send(gizmo_message(
                    request, 1, gizmo_intent(0, identity, args.width, args.height, camera)))
                request += 1
                answer = bridge.await_tag(13, f"the gizmo on {label}")
                answer.u64()
                layout = read_layout(answer.bytes_field())
                if (abs(layout["centre_x"] - args.width / 2.0) < 2.0
                        and abs(layout["centre_y"] - args.height / 2.0) < 2.0
                        and layout["handles"]):
                    return layout
                time.sleep(0.05)
            return layout or {"centre_x": -1.0, "centre_y": -1.0, "handles": []}

        def check(identity: int, camera: tuple[float, float, float], label: str) -> None:
            layout = focus(identity, camera, label)
            measured[f"{label}_gizmo_centre"] = (round(layout["centre_x"], 1),
                                                 round(layout["centre_y"], 1))
            if not layout["handles"]:
                gaps.append(f"{label}: the gizmo carried no handles, so there is nothing to grab")
            if (abs(layout["centre_x"] - args.width / 2.0) > 2.0
                    or abs(layout["centre_y"] - args.height / 2.0) > 2.0):
                gaps.append(
                    f"{label}: the gizmo is not on the object — its centre is "
                    f"({layout['centre_x']:.1f}, {layout['centre_y']:.1f}) and the object projects "
                    f"to ({args.width / 2.0:.1f}, {args.height / 2.0:.1f})"
                )
                return
            # 1.4 — a pick at the pixel the gizmo named. What is picked must be what was drawn, and
            # the gizmo is the runtime's own statement of where it drew it.
            nonlocal request
            bridge.send(pick_message(request, 0, click(0, layout["centre_x"], layout["centre_y"])))
            request += 1
            answer = bridge.await_tag(11, f"the pick on {label}")
            answer.u64()
            _, candidates = read_candidates(answer.bytes_field())
            measured[f"{label}_candidates"] = [(f"{i:016x}", round(d, 2)) for i, d, _ in candidates]
            if not candidates:
                gaps.append(f"{label}: the pick found nothing where the gizmo says the object is")
            elif candidates[0][0] != identity:
                gaps.append(
                    f"{label}: the pick answered {candidates[0][0]:016x} and the editor meant "
                    f"{identity:016x}"
                )

        request = 2
        # The Crate, at (3, 0, -2) in the file this runtime read and nothing this driver sent.
        check(crate, (3.0, 0.0, 10.0), "authored")
        # And the box the driver created a moment ago, at (0, 3, 0).
        check(created, (0.0, 3.0, 12.0), "created")

        # A click on empty sky is an empty answer, not a refusal and not an invented hit.
        bridge.send(pick_message(request, 0, click(0, 4.0, 4.0)))
        answer = bridge.await_tag(11, "the sky pick")
        answer.u64()
        _, empty = read_candidates(answer.bytes_field())
        measured["sky_candidates"] = len(empty)
        if empty:
            gaps.append(f"a click on the sky answered {len(empty)} candidate(s)")

        bridge.close()
    except SystemExit:
        pass
    except Exception as error:  # noqa: BLE001 - a driver reports whatever went wrong
        gaps.append(f"{type(error).__name__}: {error}")
    finally:
        runtime.terminate()
        try:
            output = runtime.communicate(timeout=20)[0]
        except subprocess.TimeoutExpired:
            runtime.kill()
            output = runtime.communicate()[0]

    print(output)
    for line in output.splitlines():
        if "same-frame" in line:
            measured["same_frame_line"] = line.strip()
        if line.strip().startswith("editor-window-runtime: world "):
            measured["world_line"] = line.strip()
        if "picking " in line:
            measured["picking_line"] = line.strip()

    if args.misname:
        # THE NEGATIVE CONTROL. `./worlds/city.cyworld` is the same bytes and a different document,
        # because `DocumentId` is a hash of the string. Every identity the driver derives is then
        # one the runtime has never seen, so the gizmo has nothing to sit on and the pick has
        # nothing to find — and this probe reporting success here would mean it was not checking.
        if gaps:
            print("--- one_world ----------------------------------------------------------------")
            print(f"  misnamed: the run failed as it must, with {len(gaps)} gap(s)")
            for gap in gaps:
                print(f"    - {gap}")
            print("one_world: the negative control failed, which is a pass")
            return 0
        print("one_world: the negative control SUCCEEDED, so this probe checks nothing")
        return 1

    print("--- one_world ------------------------------------------------------------------")
    for key, value in measured.items():
        print(f"  {key}: {value}")
    if gaps:
        print("one_world: GAPS")
        for gap in gaps:
            print(f"  - {gap}")
        return 1
    print("one_world: the editor created an object, the gizmo landed on it and the pick found it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
