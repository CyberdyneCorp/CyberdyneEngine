"""The content-keyed incremental cache. Task 1.1.2.

`core-type-system` requires generation be incremental: "a change to one header regenerates only what
depends on it". The cache memoises the parse, which is the expensive half — the M1 spike measured
0.18 s to 1.68 s of cold parsing against a 9.69 s full build, dominated not by how many types a
header declares but by how much C++ it transitively includes.

**The key is content, never a modification time.** Per header: the digest of the header, the digest
of every in-tree file it included, the digest of the manifest entries that belong to it, the
attribute schemas, and a toolchain stamp — the libclang library name, the bindings version, the
generator version and the exact argument vector. A touched-but-unchanged file must not cost a parse,
and a file restored from git with an older modification time must not skip one; a timestamp gets
both of those backwards.

System headers are deliberately outside the digests: digesting libstdc++ per translation unit costs
more than the parse it would save, and a compiler change moves the toolchain stamp anyway.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .attrspec import Attributes, CustomSchema, CustomValue
from .model import ParsedField, ParsedHeader, ParsedType

CACHE_VERSION = 3


def digest_of(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return "missing"


def digest_of_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class Cache:
    """Memoised parses, keyed by content. A miss is a parse; a hit is a dictionary lookup."""

    def __init__(self, path: Path | None) -> None:
        self.path = path
        self.entries: dict[str, dict] = {}
        self.hits = 0
        self.misses = 0
        if path is not None and path.exists():
            try:
                document = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                return
            if document.get("version") == CACHE_VERSION:
                self.entries = document.get("headers", {})

    def key(self, source_root: Path, header_relative: str, includes: list[str],
            stamp: str) -> str:
        parts = [f"v{CACHE_VERSION}", stamp, header_relative,
                 digest_of(source_root / header_relative)]
        for included in sorted(includes):
            parts.append(f"{included}:{digest_of(source_root / included)}")
        return digest_of_text("\n".join(parts))

    def lookup(self, header_relative: str, source_root: Path, stamp: str,
               schemas: dict[str, CustomSchema]) -> ParsedHeader | None:
        entry = self.entries.get(header_relative)
        if entry is None:
            self.misses += 1
            return None
        expected = self.key(source_root, header_relative, entry.get("includes", []), stamp)
        if expected != entry.get("key"):
            self.misses += 1
            return None
        try:
            parsed = _header_from_json(entry["parsed"], schemas)
        except KeyError:
            # An attribute schema the entry referred to is gone. A miss, not a crash.
            self.misses += 1
            return None
        self.hits += 1
        return parsed

    def store(self, parsed: ParsedHeader, source_root: Path, stamp: str) -> None:
        self.entries[parsed.path] = {
            "key": self.key(source_root, parsed.path, parsed.includes, stamp),
            "includes": parsed.includes,
            "parsed": _header_to_json(parsed),
        }

    def save(self) -> None:
        if self.path is None:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        document = {"version": CACHE_VERSION, "headers": self.entries}
        self.path.write_text(json.dumps(document, indent=1, sort_keys=True), encoding="utf-8")


# --- The parsed model as JSON -----------------------------------------------------------------------


def _header_to_json(parsed: ParsedHeader) -> dict:
    return {
        "path": parsed.path,
        "include_path": parsed.include_path,
        "includes": parsed.includes,
        "types": [
            {
                "name": parsed_type.name,
                "line": parsed_type.line,
                "header": parsed_type.header,
                "module": parsed_type.module,
                "attributes": _attributes_to_json(parsed_type.attributes),
                "fields": [
                    {
                        "name": parsed_field.name,
                        "kind": parsed_field.kind,
                        "line": parsed_field.line,
                        "attributes": _attributes_to_json(parsed_field.attributes),
                    }
                    for parsed_field in parsed_type.fields
                ],
            }
            for parsed_type in parsed.types
        ],
    }


def _attributes_to_json(attributes: Attributes) -> dict:
    return {
        "declared": attributes.declared,
        "range": list(attributes.range) if attributes.range else None,
        "enumerators": [list(pair) for pair in attributes.enumerators]
        if attributes.enumerators
        else None,
        "category": attributes.category,
        "tooltip": attributes.tooltip,
        "replicated": list(attributes.replicated) if attributes.replicated else None,
        "asset_ref": attributes.asset_ref,
        "unit": attributes.unit,
        "persistence": attributes.persistence,
        "custom": [
            {"name": value.schema.name, "values": value.values} for value in attributes.custom
        ],
    }


def _header_from_json(document: dict, schemas: dict[str, CustomSchema]) -> ParsedHeader:
    header = ParsedHeader(
        path=document["path"],
        include_path=document["include_path"],
        includes=document.get("includes", []),
    )
    for entry in document["types"]:
        parsed_type = ParsedType(
            name=entry["name"],
            attributes=_attributes_from_json(entry["attributes"], schemas),
            header=entry["header"],
            module=entry["module"],
            line=entry["line"],
        )
        for sub in entry["fields"]:
            parsed_type.fields.append(
                ParsedField(
                    name=sub["name"],
                    kind=sub["kind"],
                    attributes=_attributes_from_json(sub["attributes"], schemas),
                    line=sub["line"],
                )
            )
        header.types.append(parsed_type)
    return header


def _attributes_from_json(document: dict, schemas: dict[str, CustomSchema]) -> Attributes:
    attributes = Attributes(
        declared=list(document["declared"]),
        range=tuple(document["range"]) if document["range"] else None,
        enumerators=[(name, value) for name, value in document["enumerators"]]
        if document["enumerators"]
        else None,
        category=document["category"],
        tooltip=document["tooltip"],
        replicated=tuple(document["replicated"]) if document["replicated"] else None,
        asset_ref=document["asset_ref"],
        unit=document["unit"],
        persistence=document["persistence"],
    )
    for entry in document.get("custom", []):
        schema = schemas.get(entry["name"])
        if schema is None:
            # The schema that declared this attribute is gone. Treat the entry as a miss rather than
            # emitting against a definition that no longer exists.
            raise KeyError(entry["name"])
        attributes.custom.append(CustomValue(schema=schema, values=entry["values"]))
    return attributes
