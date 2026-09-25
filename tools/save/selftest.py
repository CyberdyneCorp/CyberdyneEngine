#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove every static check in `check_forbidden.py` red, by planting its pattern in the real tree.

`tools/quality/selftest.py` states the rule this follows: a gate that has never failed is a gate
nobody knows works. Each case copies the real save sources — src/save/ and every production file
that includes a `<cy/save/...>` header — at their real paths into a temporary tree, applies ONE
edit, and runs the checker there with `--root --static-only`:

  * case `clean`: the unedited copy passes. Every other case means something only because this one
    holds.
  * case `comments-are-not-code`: a comment and a string literal naming every trigger word pass —
    the checks read code, and prose explaining why encryption is not integrity is not a violation.
  * one case per plant below: the checker FAILS and names the pattern the plant introduces.
  * case `empty-tree`: pointed at a tree with no save sources, the checker FAILS rather than
    reporting ten clean patterns over nothing.

The runtime half is proved differently — each `forbidden save pattern <id>:` case in unit.save is
a test, and src/save/README.md records the mutation each was watched failing under.

    python3 tools/save/selftest.py
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools" / "save" / "check_forbidden.py"
ARCHIVE = "src/save/src/archive.cpp"
ARCHIVE_H = "src/save/include/cy/save/archive.h"
CONTAINER = "src/save/src/container.cpp"

# (case, pattern id it must be reported as, file, text appended to that file)
PLANTS: list[tuple[str, str, str, str]] = [
    ("backend-write-of-a-struct", "raw-memory", ARCHIVE,
     "\nvoid cy_plant(cy::save::SaveBackend& b, const Manifest& m) {\n"
     "    (void)b.write(\"k\", Span<const u8>(reinterpret_cast<const u8*>(&m), sizeof(Manifest)));\n}\n"),
    ("component-copied-into-a-record", "raw-memory", CONTAINER,
     "\nvoid cy_plant(serialize::ValueRecord& r, const ChunkRef& c) {\n"
     "    (void)r.set(reflect::FieldId(1), serialize::WireType::Bytes, &c, sizeof(ChunkRef));\n}\n"),
    ("identity-from-an-entity-index", "runtime-identity", ARCHIVE,
     "\nPersistentId cy_plant(Handle entity) { return PersistentId{0, entity.index}; }\n"),
    ("save-code-includes-the-ecs", "runtime-identity", CONTAINER, "\n#include <cy/ecs/world.h>\n"),
    ("snapshot-record-in-a-save", "derived-saved", CONTAINER,
     "\nStatus cy_plant(const reflect::TypeInfo& t, const void* o, serialize::ValueRecord& r) {\n"
     "    return serialize::record_from_object(t, o, serialize::Purpose::Snapshot, r);\n}\n"),
    ("commit-waits-for-residency", "whole-world-resident", ARCHIVE,
     "\nbool cy_plant(const Overlay& o) { return o.resident_region_count() == o.region_count(); }\n"),
    ("file-opened-for-writing", "destructive-in-place", ARCHIVE,
     "\nvoid cy_plant(const char* path) { std::FILE* f = std::fopen(path, \"wb\"); (void)f; }\n"),
    ("save-includes-the-package-writer", "cooked-migration", CONTAINER,
     "\n#include <cy/core/assets/package.h>\n"),
    ("save-migrates-a-package", "cooked-migration", ARCHIVE,
     "\nvoid cy_plant(void* package) { migrate_package(package); }\n"),
    ("hand-rolled-xor", "bespoke-crypto", ARCHIVE,
     "\nvoid cy_plant(Array<u8>& bytes, u8 key) { for (u8& b : bytes) { b ^= key; } }\n"),
    ("hand-rolled-cipher", "bespoke-crypto", CONTAINER,
     "\nu8 encrypt_chunk_byte(u8 value) noexcept { return static_cast<u8>(value + 13U); }\n"),
    ("decryption-as-integrity", "bespoke-crypto", CONTAINER,
     "\n#include <mbedtls/gcm.h>\nbool cy_plant(bool decrypted) { return decrypted_verified(decrypted); }\n"
     "bool cy_plant2() { const bool integrity = encrypt_ok; return integrity; }\n"
     "void cy_plant3() { if (decrypt(chunk)) { verified = true; } }\n"),
    ("load-returns-bool", "boolean-load", ARCHIVE_H,
     "\nnamespace cy::save { bool load_quick(const char* path) noexcept; }\n"),
]

PROSE = (
    "\n// encrypt, decrypt, cipher, AES, xor key, fopen(path, \"w\"), sizeof(Manifest), residency_of(),\n"
    "// bool load_everything(), #include <cy/ecs/world.h>, PersistentId{0, entity.index}\n"
    "static const char* const kCyPlantProse = \"encryption is not integrity; fopen( ; O_TRUNC\";\n"
)


def save_files() -> list[Path]:
    """The files the checker reads, found the way it finds them."""
    sys.path.insert(0, str(CHECKER.parent))
    import check_forbidden  # noqa: E402  (a sibling script, not a package)

    return [ROOT / source.path for source in check_forbidden.collect_sources(ROOT)]


def sandbox(files: list[Path]) -> Path:
    root = Path(tempfile.mkdtemp(prefix="cy-save-selftest-"))
    for path in files:
        target = root / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    return root


def run(root: Path) -> tuple[int, str]:
    result = subprocess.run([sys.executable, str(CHECKER), "--root", str(root), "--static-only"],
                            capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout + result.stderr


def main() -> int:
    files = save_files()
    if not any(path.as_posix().endswith(ARCHIVE) for path in files):
        print(f"selftest: {ARCHIVE} is not among the checked files — the checker reads the wrong tree")
        return 1
    failures = 0

    def report(case: str, passed: bool, detail: str) -> None:
        nonlocal failures
        failures += 0 if passed else 1
        print(f"{'ok  ' if passed else 'FAIL'} {case}{'' if passed else ': ' + detail}")

    root = sandbox(files)
    try:
        code, output = run(root)
        report("clean", code == 0, output)

        for case, pattern, relative, text in [("comments-are-not-code", "", ARCHIVE, PROSE)] + PLANTS:
            target = root / relative
            original = target.read_text(encoding="utf-8")
            target.write_text(original + text, encoding="utf-8")
            code, output = run(root)
            target.write_text(original, encoding="utf-8")
            if not pattern:
                report(case, code == 0, output)
                continue
            named = f"[{pattern}]" in output and f"FAIL {pattern}:" in output
            report(f"{case} -> {pattern}", code == 1 and named, output)
    finally:
        shutil.rmtree(root, ignore_errors=True)

    empty = Path(tempfile.mkdtemp(prefix="cy-save-selftest-empty-"))
    try:
        code, output = run(empty)
        report("empty-tree", code == 1 and "refusing" in output, output)
    finally:
        shutil.rmtree(empty, ignore_errors=True)

    covered = {pattern for _, pattern, _, _ in PLANTS}
    sys.path.insert(0, str(CHECKER.parent))
    import check_forbidden  # noqa: E402

    static_ids = {p.id for p in check_forbidden.PATTERNS if p.static is not None}
    report("every static check has a plant", covered == static_ids,
           f"unplanted: {sorted(static_ids - covered)}")

    print(f"{'selftest passed' if failures == 0 else f'{failures} selftest case(s) FAILED'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
