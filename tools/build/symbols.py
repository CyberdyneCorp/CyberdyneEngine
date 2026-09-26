#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shipping binaries stripped, their symbols archived separately and retrievable by build identity.

M11.d task 7.5, `build-and-packaging` — "Build provenance and symbols":

    Shipping binaries SHALL be stripped, with symbols archived separately and retrievable by build
    identity, so a crash report carrying that identity can be symbolicated.

--- WHAT "MATCHES THE SHIPPED BINARY" MEANS HERE, AND WHY IT IS FOUR CHECKS ----------------------

A debug file that belongs to a DIFFERENT build of the same program is the failure this exists to
prevent: it symbolicates without complaint and names the wrong line. So `verify` does not trust a
file because of where it sits or what it is called. It requires, of the shipped binary and the
archived symbols together:

  1. the shipped binary is STRIPPED — no `.symtab`, no `.debug_*` section — so what ships cannot
     be mistaken for what was archived;
  2. both carry the SAME GNU build-id note, which the linker derives from the linked bytes;
  3. the shipped binary's `.gnu_debuglink` names the archived file AND carries its CRC-32, which
     is a checksum over the debug file's own bytes — so a debug file edited or swapped after the
     split is refused even when its build-id was preserved;
  4. the archived symbols SYMBOLICATE an address in the shipped binary: `main`'s address, taken
     from the archive, resolves through `addr2line` to a source file the caller names, while the
     shipped binary alone resolves it to nothing.

--- THE ARCHIVE'S SHAPE -------------------------------------------------------------------------

    <store>/.build-id/<first two hex>/<rest>.debug     GDB's own `debug-file-directory` layout, so
                                                       `set debug-file-directory <store>` finds it
                                                       with no tool of ours in the loop
    <store>/builds/<package build id>                  one line per binary: `<name> <gnu build-id>`

The package build identity (`cy::build::PackageSet::build_id`, the content manifest hash) is what a
shipped build reports about ITSELF; the GNU build-id is what a crash report's module list can carry.
`locate` goes from the first to the second, so either one retrieves the symbols.

--- WHAT THIS HOST CANNOT ANSWER ----------------------------------------------------------------

ELF only. Mach-O (`dsymutil`, UUID load commands) and PE/PDB (`/DEBUG`, the CodeView GUID) are the
same four checks with different spellings, and this Linux host can produce neither — so a non-ELF
input is refused naming what it is, never treated as stripped.
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path


class SymbolError(Exception):
    """A refusal: the binary, the archive or the pairing is not what a shipped build needs."""


# --- ELF, read directly --------------------------------------------------------------------------
#
# Read rather than asked of `readelf`, because the checks are the point and a parser of a tool's
# human-readable output is one localisation away from reporting "no build-id" about a binary that
# has one.

NT_GNU_BUILD_ID = 3
SHT_SYMTAB = 2
SHT_NOBITS = 8


@dataclass
class Section:
    name: str
    kind: int
    offset: int
    size: int
    link: int
    entsize: int


@dataclass
class Elf:
    path: Path
    data: bytes
    sections: list[Section] = field(default_factory=list)

    def section(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def contents(self, section: Section) -> bytes:
        if section.kind == SHT_NOBITS:
            return b""
        return self.data[section.offset:section.offset + section.size]

    @property
    def debug_sections(self) -> list[str]:
        """`.debug_*` sections that carry bytes. `--only-keep-debug` leaves the ALLOCATED sections
        as NOBITS placeholders, so a placeholder is not debug information."""
        return [s.name for s in self.sections
                if s.name.startswith(".debug_") and s.kind != SHT_NOBITS and s.size > 0]

    @property
    def has_symtab(self) -> bool:
        return any(s.kind == SHT_SYMTAB for s in self.sections)

    def build_id(self) -> str:
        note = self.section(".note.gnu.build-id")
        if note is None:
            raise SymbolError(f"{self.path} carries no GNU build-id note: link with --build-id, "
                              "or nothing can ever say which symbols belong to it")
        raw = self.contents(note)
        namesz, descsz, kind = struct.unpack_from("<III", raw, 0)
        name_end = 12 + ((namesz + 3) & ~3)
        if kind != NT_GNU_BUILD_ID or raw[12:12 + namesz].rstrip(b"\0") != b"GNU":
            raise SymbolError(f"{self.path}: .note.gnu.build-id is not a GNU build-id note")
        return raw[name_end:name_end + descsz].hex()

    def debuglink(self) -> tuple[str, int] | None:
        section = self.section(".gnu_debuglink")
        if section is None:
            return None
        raw = self.contents(section)
        name = raw.split(b"\0", 1)[0].decode("utf-8")
        crc_at = (len(name) + 1 + 3) & ~3
        (crc,) = struct.unpack_from("<I", raw, crc_at)
        return name, crc

    def symbol(self, wanted: str) -> int | None:
        """A symbol's value from `.symtab`, or None."""
        for table in (s for s in self.sections if s.kind == SHT_SYMTAB):
            strings = self.contents(self.sections[table.link])
            raw = self.contents(table)
            for at in range(0, len(raw), table.entsize or 24):
                name_at, _info, _other, _shndx, value, _size = struct.unpack_from("<IBBHQQ", raw, at)
                end = strings.find(b"\0", name_at)
                if strings[name_at:end].decode("utf-8", "replace") == wanted and value != 0:
                    return value
        return None


def read_elf(path: Path) -> Elf:
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF":
        kind = "Mach-O" if data[:4] in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe") else (
            "PE" if data[:2] == b"MZ" else "an unrecognised format")
        raise SymbolError(f"{path} is {kind}, not ELF: dSYM and PDB handling are the same checks "
                          "with other spellings, and this host produces neither")
    if data[4] != 2 or data[5] != 1:
        raise SymbolError(f"{path}: only 64-bit little-endian ELF is read here")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx]
    names_raw = data[names[4]:names[4] + names[5]]
    elf = Elf(Path(path), data)
    for name_at, kind, _flags, _addr, offset, size, link, _info, _align, entsize in headers:
        end = names_raw.find(b"\0", name_at)
        elf.sections.append(Section(names_raw[name_at:end].decode(), kind, offset, size, link,
                                    entsize))
    return elf


# --- The archive ---------------------------------------------------------------------------------


def debug_path(store: Path, build_id: str) -> Path:
    return store / ".build-id" / build_id[:2] / f"{build_id[2:]}.debug"


def tool(name: str) -> str:
    found = shutil.which(name)
    if found is None:
        raise SymbolError(f"`{name}` is not on PATH; binutils is what splits and reads ELF symbols")
    return found


def run(command: list[str]) -> subprocess.CompletedProcess:
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if result.returncode != 0:
        raise SymbolError(f"{Path(command[0]).name} failed: {result.stderr.strip()}")
    return result


@dataclass
class Split:
    name: str
    build_id: str
    shipped: Path
    symbols: Path


def split(binary: Path, shipped: Path, store: Path, package_build: str | None = None) -> Split:
    """Strip `binary` into `shipped` and archive its symbols in `store` under its build-id."""
    source = read_elf(binary)
    if not source.debug_sections:
        # THE FAILURE THIS REFUSAL EXISTS FOR: a configuration that compiles without `-g` has
        # nothing to archive, and a split that "succeeded" over it would archive a symbol table and
        # call the crash it cannot place symbolicated. See cmake/profiles.cmake's Shipping row.
        raise SymbolError(f"{binary} has no debug information to archive: it was compiled "
                          "without -g, so a crash in it can never be placed on a source line")
    build_id = source.build_id()
    archived = debug_path(store, build_id)
    archived.parent.mkdir(parents=True, exist_ok=True)
    shipped.parent.mkdir(parents=True, exist_ok=True)

    objcopy = tool("objcopy")
    with tempfile.TemporaryDirectory() as scratch:
        # The debuglink records a BASENAME, and GDB looks it up under `.build-id/` by id rather
        # than by that name — so the file is written under the name the link will carry, then
        # moved into the archive's layout.
        staged = Path(scratch) / f"{binary.name}.debug"
        run([objcopy, "--only-keep-debug", binary, staged])
        run([objcopy, "--strip-all", f"--add-gnu-debuglink={staged}", binary, shipped])
        shutil.copyfile(staged, archived)
    shipped.chmod(binary.stat().st_mode & 0o777)

    if package_build:
        index = store / "builds" / package_build
        index.parent.mkdir(parents=True, exist_ok=True)
        lines = set(index.read_text().splitlines()) if index.exists() else set()
        lines.add(f"{binary.name} {build_id}")
        index.write_text("".join(f"{line}\n" for line in sorted(lines)))
    return Split(binary.name, build_id, shipped, archived)


def locate(store: Path, identity: str) -> list[tuple[str, Path]]:
    """Every archived symbol file for a package build identity or a GNU build-id."""
    index = store / "builds" / identity
    if index.exists():
        pairs = [line.split(" ", 1) for line in index.read_text().splitlines() if line]
        return [(name, debug_path(store, build_id)) for name, build_id in pairs]
    candidate = debug_path(store, identity)
    if len(identity) > 2 and candidate.exists():
        return [(identity, candidate)]
    raise SymbolError(f"no symbols are archived under {identity!r} in {store}")


@dataclass
class Verified:
    build_id: str
    symbols: Path
    main_address: int
    location: str


def verify(shipped: Path, store: Path, expect_source: str) -> Verified:
    """The four checks in this module's docstring, or a `SymbolError` naming the one that failed."""
    binary = read_elf(shipped)
    if binary.has_symtab or binary.debug_sections:
        raise SymbolError(f"{shipped} is not stripped: it carries "
                          f"{'.symtab ' if binary.has_symtab else ''}"
                          f"{' '.join(binary.debug_sections)}".rstrip())

    build_id = binary.build_id()
    archived = debug_path(store, build_id)
    if not archived.exists():
        raise SymbolError(f"no symbols are archived for build-id {build_id}")
    symbols = read_elf(archived)
    if symbols.build_id() != build_id:
        raise SymbolError(f"the archived symbols are for build-id {symbols.build_id()}, "
                          f"not the shipped binary's {build_id}")

    link = binary.debuglink()
    if link is None:
        raise SymbolError(f"{shipped} has no .gnu_debuglink naming its symbols")
    crc = zlib.crc32(archived.read_bytes()) & 0xFFFFFFFF
    if link[1] != crc:
        raise SymbolError(f"the debuglink's CRC-32 is {link[1]:08x} and the archived file's is "
                          f"{crc:08x}: the symbols were changed after the split")
    if not symbols.debug_sections:
        raise SymbolError(f"{archived} carries no debug information")

    address = symbols.symbol("main")
    if address is None:
        raise SymbolError(f"{archived} has no `main` to symbolicate")
    addr2line = tool("addr2line")
    resolved = run([addr2line, "-e", archived, "-f", "-C", hex(address)]).stdout.split("\n")
    location = resolved[1].strip() if len(resolved) > 1 else ""
    if not location.split(":")[0].endswith(expect_source):
        raise SymbolError(f"main at {hex(address)} resolved to {location!r} through the archive, "
                          f"not to {expect_source}")
    # And the other half: the shipped binary on its own must NOT place it. A shipped binary that
    # still symbolicates was never stripped, whatever its section table says.
    alone = run([addr2line, "-e", shipped, hex(address)]).stdout.strip()
    if alone and not alone.startswith("??"):
        raise SymbolError(f"the shipped binary resolved {hex(address)} to {alone!r} on its own")
    return Verified(build_id, archived, address, location)


# --- Command line --------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    split_command = commands.add_parser("split", help="strip a binary and archive its symbols")
    split_command.add_argument("binary", type=Path)
    split_command.add_argument("--out", type=Path, required=True, help="the stripped binary")
    split_command.add_argument("--store", type=Path, required=True)
    split_command.add_argument("--build", help="the package build identity to index it under")

    verify_command = commands.add_parser("verify", help="prove the archive matches a binary")
    verify_command.add_argument("shipped", type=Path)
    verify_command.add_argument("--store", type=Path, required=True)
    verify_command.add_argument("--source", required=True,
                                help="the source file `main` must resolve to")

    locate_command = commands.add_parser("locate", help="the symbols for a build identity")
    locate_command.add_argument("identity")
    locate_command.add_argument("--store", type=Path, required=True)

    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "split":
            done = split(arguments.binary, arguments.out, arguments.store, arguments.build)
            print(f"split {done.name} build-id {done.build_id}\n"
                  f"  shipped {done.shipped}\n  symbols {done.symbols}")
        elif arguments.command == "verify":
            done = verify(arguments.shipped, arguments.store, arguments.source)
            print(f"verified build-id {done.build_id}: stripped, one build-id, debuglink CRC "
                  f"matches, main at {hex(done.main_address)} -> {done.location}")
        else:
            for name, path in locate(arguments.store, arguments.identity):
                print(f"{name} {path}")
    except SymbolError as refusal:
        print(f"symbols: {refusal}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
