#!/usr/bin/env python3
"""Read a CyberdyneEngine trace capture without the engine.

`diagnostics-profiling-and-crash` — "Captures are readable offline": a capture opened without the
game resolves its identifiers from its own metadata. This is that reader, and it is deliberately a
separate implementation of `src/core/diagnostics/include/cy/core/diagnostics/format.h` — a format
only one program can read is a format nobody can check.

It also reads the chunk index at the tail rather than the whole file, so a long capture opens by
loading its regions on demand. Pass --events to spend the time reading them.

Run by `just diagnose-trace`. Task 3.5.6.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"CYTRACE\x00"
FILE_HEADER = struct.Struct("<8sIIQQQIBBHQQ")
CHUNK_HEADER = struct.Struct("<IIQQ")
RECORD_HEADER = struct.Struct("<HBBI")
RECORD_BODY = struct.Struct("<QIHHQQ")
FIELD_RECORD = struct.Struct("<IHHQ")

TAG_META = b"META"
TAG_EVENTS = b"EVTS"
TAG_LOSS = b"LOSS"
TAG_INDEX = b"ENDX"

KIND_NAMES = [
    "padding", "scope-begin", "scope-end", "instant", "counter", "flow-begin", "flow-end",
    "allocation", "free", "task-begin", "task-end", "gpu-begin", "gpu-end", "io-request",
    "io-complete", "network", "tick-begin", "tick-end", "state-hash", "breadcrumb", "log", "loss",
    "frame-begin", "frame-end",
]
CHANNEL_NAMES = ["critical", "important", "verbose", "sampled"]
FIELD_TYPES = ["u64", "i64", "f64", "boolean", "string", "id", "duration-ns", "bytes"]
PRIVACY_NAMES = ["public", "developer", "potentially-personal", "sensitive", "secret"]
LOSS_REASONS = ["buffer-pressure", "record-too-large", "unclassified-field", "registry-full",
                "policy-redaction"]
LOG_LEVELS = ["trace", "debug", "info", "warning", "error", "fatal", "off"]

FIELD_REDACTED = 1


def name_or(table: dict[int, str], key: int, fallback: str = "?") -> str:
    return table.get(key, fallback)


def enum_name(table: list[str], value: int) -> str:
    return table[value] if 0 <= value < len(table) else f"#{value}"


@dataclass
class FieldMeta:
    name: str
    type: int
    privacy: int


@dataclass
class Record:
    kind: int
    channel: int
    name: int
    category: int
    timestamp: int
    a: int
    b: int
    thread: int
    fields: list[tuple[int, int, int, bytes]] = field(default_factory=list)


@dataclass
class Chunk:
    tag: bytes
    thread: int
    offset: int
    payload_bytes: int
    first_timestamp: int
    last_timestamp: int


class Reader:
    """A cursor over one chunk payload."""

    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.offset = 0

    def take(self, size: int) -> bytes:
        chunk = self.payload[self.offset:self.offset + size]
        self.offset += size
        return chunk

    def u32(self) -> int:
        return struct.unpack_from("<I", self.payload, self._advance(4))[0]

    def u8(self) -> int:
        return struct.unpack_from("<B", self.payload, self._advance(1))[0]

    def u16(self) -> int:
        return struct.unpack_from("<H", self.payload, self._advance(2))[0]

    def u64(self) -> int:
        return struct.unpack_from("<Q", self.payload, self._advance(8))[0]

    def text(self) -> str:
        length = self.u16()
        return self.take(length).decode("utf-8", "replace")

    def done(self) -> bool:
        return self.offset >= len(self.payload)

    def _advance(self, size: int) -> int:
        offset = self.offset
        self.offset += size
        return offset


class Capture:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        self.names: dict[int, str] = {}
        self.categories: dict[int, str] = {}
        self.fields: dict[int, FieldMeta] = {}
        self.identity: dict[str, str] = {}
        self.chunks: list[Chunk] = []
        self.losses: list[tuple[int, int, int, int]] = []
        self.records: list[Record] = []
        self.header = self._read_header()

    def _read_header(self):
        if len(self.data) < FILE_HEADER.size:
            raise ValueError(f"{self.path}: too short to be a capture")
        values = FILE_HEADER.unpack_from(self.data, 0)
        if values[0] != MAGIC:
            raise ValueError(f"{self.path}: not a CyberdyneEngine capture")
        return {
            "format_version": values[1], "header_bytes": values[2], "trace_id": values[3],
            "wall_ns": values[4], "monotonic_ns": values[5], "process_id": values[6],
            "max_classification": values[7], "compression": values[8], "flags": values[9],
        }

    def read_index(self) -> None:
        """The chunk index lives at the tail; its offset is the file's last eight bytes."""
        if len(self.data) < 8:
            return
        index_offset = struct.unpack_from("<Q", self.data, len(self.data) - 8)[0]
        if index_offset + CHUNK_HEADER.size > len(self.data):
            self._scan_chunks()
            return
        tag, _flags, payload_bytes, _raw = CHUNK_HEADER.unpack_from(self.data, index_offset)
        if tag.to_bytes(4, "little") != TAG_INDEX:
            self._scan_chunks()
            return
        payload = self.data[index_offset + CHUNK_HEADER.size:
                            index_offset + CHUNK_HEADER.size + payload_bytes]
        reader = Reader(payload)
        for _ in range(reader.u32()):
            self.chunks.append(Chunk(reader.u32().to_bytes(4, "little"), reader.u32(), reader.u64(),
                                     reader.u64(), reader.u64(), reader.u64()))
        self.total_events = reader.u64()
        self.total_dropped = reader.u64()

    def _scan_chunks(self) -> None:
        """A capture truncated by a crash has no index. Walk it instead, and say so."""
        offset = FILE_HEADER.size
        while offset + CHUNK_HEADER.size <= len(self.data) - 8:
            tag, flags, payload_bytes, _raw = CHUNK_HEADER.unpack_from(self.data, offset)
            if offset + CHUNK_HEADER.size + payload_bytes > len(self.data):
                break
            self.chunks.append(Chunk(tag.to_bytes(4, "little"), flags, offset, payload_bytes,
                                     0, 0))
            offset += CHUNK_HEADER.size + payload_bytes
        self.total_events = 0
        self.total_dropped = 0

    def payload_of(self, chunk: Chunk) -> bytes:
        # A Chunk.offset is always the chunk header's own position, in the index and in a scan.
        start = chunk.offset + CHUNK_HEADER.size
        return self.data[start:start + chunk.payload_bytes]

    def read_metadata(self) -> None:
        for chunk in self.chunks:
            if chunk.tag != TAG_META:
                continue
            reader = Reader(self.payload_of(chunk))
            # Read into locals: Python evaluates an assignment's right-hand side before its
            # subscript, which would reverse the order these were written in.
            for _ in range(reader.u32()):
                identifier = reader.u32()
                self.names[identifier] = reader.text()
            for _ in range(reader.u32()):
                identifier = reader.u32()
                self.categories[identifier] = reader.text()
            for _ in range(reader.u32()):
                identifier = reader.u32()
                type_index = reader.u8()
                privacy = reader.u8()
                self.fields[identifier] = FieldMeta(reader.text(), type_index, privacy)
            for _ in range(reader.u32()):
                key = reader.text()
                self.identity[key] = reader.text()
        for chunk in self.chunks:
            if chunk.tag != TAG_LOSS:
                continue
            reader = Reader(self.payload_of(chunk))
            for _ in range(reader.u32()):
                thread = reader.u32()
                channel = reader.u8()
                reason = reader.u8()
                reader.u16()
                self.losses.append((thread, channel, reason, reader.u64()))

    def read_events(self) -> None:
        for chunk in self.chunks:
            if chunk.tag != TAG_EVENTS:
                continue
            payload = self.payload_of(chunk)
            offset = 0
            while offset + RECORD_HEADER.size + RECORD_BODY.size <= len(payload):
                size, kind, channel, name = RECORD_HEADER.unpack_from(payload, offset)
                if size == 0 or offset + size > len(payload):
                    break
                timestamp, category, field_count, text_bytes, a, b = RECORD_BODY.unpack_from(
                    payload, offset + RECORD_HEADER.size)
                record = Record(kind, channel, name, category, timestamp, a, b, chunk.thread)
                base = offset + RECORD_HEADER.size + RECORD_BODY.size
                text_at = base + field_count * FIELD_RECORD.size
                for index in range(field_count):
                    identifier, flags, text_offset, bits = FIELD_RECORD.unpack_from(
                        payload, base + index * FIELD_RECORD.size)
                    text = b""
                    meta = self.fields.get(identifier)
                    if meta and meta.type == 4 and not flags & FIELD_REDACTED:
                        text = payload[text_at + text_offset:text_at + text_offset + bits]
                    record.fields.append((identifier, flags, bits, text))
                self.records.append(record)
                offset += size


def format_field(capture: Capture, entry: tuple[int, int, int, bytes]) -> str:
    identifier, flags, bits, text = entry
    meta = capture.fields.get(identifier)
    name = meta.name if meta else f"field#{identifier}"
    if flags & FIELD_REDACTED:
        classification = enum_name(PRIVACY_NAMES, meta.privacy) if meta else "unclassified"
        return f"{name}=<redacted:{classification}>"
    if meta and meta.type == 4:
        return f"{name}={text.decode('utf-8', 'replace')!r}"
    if meta and meta.type == 2:
        return f"{name}={struct.unpack('<d', struct.pack('<Q', bits))[0]}"
    return f"{name}={bits}"


def print_summary(capture: Capture) -> None:
    header = capture.header
    print(f"{capture.path}")
    print(f"  format          version {header['format_version']}, "
          f"{'uncompressed' if header['compression'] == 0 else 'zstd'}, {len(capture.data)} bytes")
    print(f"  trace id        {header['trace_id']:#018x}")
    print(f"  process         {header['process_id']}")
    print(f"  classifications up to {enum_name(PRIVACY_NAMES, header['max_classification'])}")
    for key, value in capture.identity.items():
        print(f"  {key:<15} {value}")
    print(f"  tables          {len(capture.names)} names, {len(capture.categories)} categories, "
          f"{len(capture.fields)} fields")
    counts: dict[bytes, int] = {}
    for chunk in capture.chunks:
        counts[chunk.tag] = counts.get(chunk.tag, 0) + 1
    print("  chunks          " + ", ".join(
        f"{tag.decode()} x{count}" for tag, count in sorted(counts.items())))


# Two different losses, reported in one chunk and printed apart: a record the buffering policy
# refused is a gap in the timeline, and a field the export policy removed is a value the artefact was
# never allowed to carry.
EVENT_LOSS_REASONS = (0, 1, 3)


def print_loss(capture: Capture) -> None:
    events = [entry for entry in capture.losses if entry[2] in EVENT_LOSS_REASONS and entry[3]]
    fields = [entry for entry in capture.losses if entry[2] not in EVENT_LOSS_REASONS and entry[3]]

    if not events:
        print("\nevents lost: none — every event a producer emitted reached the artefact")
    else:
        print("\nevents lost (refused by the buffering policy, not silently dropped)")
        total = 0
        for thread, channel, reason, count in events:
            total += count
            print(f"  thread {thread:<3} {enum_name(CHANNEL_NAMES, channel):<10} "
                  f"{enum_name(LOSS_REASONS, reason):<18} {count}")
        print(f"  total {total}")

    for _thread, _channel, reason, count in fields:
        print(f"fields removed: {count} ({enum_name(LOSS_REASONS, reason)})")


def print_privacy(capture: Capture) -> None:
    by_classification: dict[int, list[str]] = {}
    for meta in capture.fields.values():
        by_classification.setdefault(meta.privacy, []).append(meta.name)
    redacted = sum(1 for record in capture.records for entry in record.fields
                   if entry[1] & FIELD_REDACTED)
    print("\nprivacy")
    for privacy in sorted(by_classification):
        names = ", ".join(sorted(by_classification[privacy]))
        print(f"  {enum_name(PRIVACY_NAMES, privacy):<22} {names}")
    if capture.records:
        print(f"  redacted field values  {redacted}")


def print_events(capture: Capture, limit: int, kind_filter: str | None) -> None:
    print(f"\nevents ({len(capture.records)} in the capture)")
    origin = capture.records[0].timestamp if capture.records else 0
    shown = 0
    for record in capture.records:
        kind = enum_name(KIND_NAMES, record.kind)
        if kind_filter and kind_filter not in kind:
            continue
        if shown >= limit:
            print(f"  ... {len(capture.records) - shown} more")
            break
        shown += 1
        micros = (record.timestamp - origin) / 1000.0
        detail = ""
        if record.kind == 20:  # log
            detail = (f" level={enum_name(LOG_LEVELS, record.a)} "
                      f"at={name_or(capture.names, record.b, '')}")
        elif record.kind == 21:  # loss
            detail = f" channel={enum_name(CHANNEL_NAMES, record.a)} dropped={record.b}"
        elif record.a or record.b:
            detail = f" a={record.a} b={record.b}"
        fields = " ".join(format_field(capture, entry) for entry in record.fields)
        print(f"  {micros:12.3f}us t{record.thread} {kind:<12} "
              f"{name_or(capture.names, record.name):<28} "
              f"[{name_or(capture.categories, record.category, '-')}]{detail} {fields}".rstrip())


def to_json(capture: Capture) -> str:
    return json.dumps({
        "header": capture.header,
        "identity": capture.identity,
        "names": capture.names,
        "categories": capture.categories,
        "fields": {str(k): {"name": v.name, "type": enum_name(FIELD_TYPES, v.type),
                            "privacy": enum_name(PRIVACY_NAMES, v.privacy)}
                   for k, v in capture.fields.items()},
        "losses": [{"thread": t, "channel": enum_name(CHANNEL_NAMES, c),
                    "reason": enum_name(LOSS_REASONS, r), "count": n}
                   for t, c, r, n in capture.losses],
        "events": [{"kind": enum_name(KIND_NAMES, r.kind),
                    "channel": enum_name(CHANNEL_NAMES, r.channel),
                    "name": name_or(capture.names, r.name),
                    "category": name_or(capture.categories, r.category, ""),
                    "timestamp_ns": r.timestamp, "thread": r.thread, "a": r.a, "b": r.b,
                    "fields": [format_field(capture, e) for e in r.fields]}
                   for r in capture.records],
    }, indent=2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path, help="the .cytrace file to read")
    parser.add_argument("--events", type=int, default=0, metavar="N",
                        help="list the first N events (0 lists none)")
    parser.add_argument("--kind", help="only events whose kind contains this text")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()

    if not args.capture.exists():
        print(f"{args.capture}: no such file", file=sys.stderr)
        return 2
    try:
        capture = Capture(args.capture)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    capture.read_index()
    capture.read_metadata()
    if args.events or args.json:
        capture.read_events()

    if args.json:
        print(to_json(capture))
        return 0

    print_summary(capture)
    print_loss(capture)
    print_privacy(capture)
    if args.events:
        print_events(capture, args.events, args.kind)
    return 0


if __name__ == "__main__":
    sys.exit(main())
