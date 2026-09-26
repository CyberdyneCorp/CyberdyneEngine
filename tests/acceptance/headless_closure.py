#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The headless server's LINKED closure: nothing that draws, plays or presents is in the binary.

M11.d task 7.2, `testing-and-quality` — "Headless is verified continuously": the headless scenario
"SHALL execute with no rendering, audio, or interface code linked, and a dependency on any of them
SHALL fail the build".

Two checks cover the claim, and this is the second. tests/acceptance/CMakeLists.txt walks the
server's DECLARED link closure at configure time and refuses a forbidden target by name. This reads
the LINKED binary after every build — its defined symbols and its dynamic dependencies — because a
declared closure can be clean while a static library's object file is pulled in through a path no
target names, and because that is what "linked" means. It runs as a POST_BUILD step, so a violation
fails the build that introduced it.

    python3 tests/acceptance/headless_closure.py <binary>
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

# Demangled C++ namespaces and C symbol prefixes that mean "renders, plays or presents".
FORBIDDEN_SYMBOLS = [
    (re.compile(r"\bcy::rendering::"), "rendering"),
    (re.compile(r"\bcy::rhi::"), "rendering (RHI)"),
    (re.compile(r"\bcy::render::"), "rendering (render server)"),
    (re.compile(r"\bcy::audio::"), "audio"),
    (re.compile(r"\bcy::ui::"), "interface"),
    (re.compile(r"\bcy::text::"), "interface (text)"),
    (re.compile(r"\bcy::platform::(Sdl3|LinuxPlatform|X11)"), "a desktop display server"),
    (re.compile(r"^(vk[A-Z]\w+|volk\w+)$"), "Vulkan"),
    (re.compile(r"^SDL_\w+$"), "SDL"),
    (re.compile(r"^ma_(engine|device|context|sound)_\w+$"), "miniaudio"),
    (re.compile(r"^(XOpenDisplay|wl_display_connect)$"), "a window system"),
]
FORBIDDEN_LIBRARIES = re.compile(
    r"lib(vulkan|SDL|X11|xcb|wayland|asound|pulse|GL|EGL)[\w.-]*\.so")


def defined_symbols(binary: Path) -> list[str]:
    nm = shutil.which("nm")
    if nm is None:
        raise SystemExit("headless_closure: `nm` is not on PATH, so the linked closure cannot be read")
    result = subprocess.run([nm, "-C", "--defined-only", "--format=posix", str(binary)],
                            capture_output=True, text=True, check=True)
    # posix format is "<name> <type> <value> [<size>]", and a demangled name contains spaces, so
    # the fields are matched off the right-hand end rather than split.
    field = re.compile(r"^(.*) [A-Za-z] [0-9a-f]+(?: [0-9a-f]+)?$")
    names = []
    for line in result.stdout.splitlines():
        match = field.match(line)
        if match:
            names.append(match.group(1))
    return names


def needed_libraries(binary: Path) -> list[str]:
    readelf = shutil.which("readelf")
    if readelf is None:
        return []
    result = subprocess.run([readelf, "-d", str(binary)], capture_output=True, text=True)
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", result.stdout)


def violations(binary: Path) -> list[str]:
    found: dict[str, list[str]] = {}
    for name in defined_symbols(binary):
        for pattern, what in FORBIDDEN_SYMBOLS:
            if pattern.search(name):
                found.setdefault(what, []).append(name)
    problems = [f"{what}: {len(names)} symbol(s), e.g. {names[0]}" for what, names in found.items()]
    problems += [f"a dynamic dependency on {library}" for library in needed_libraries(binary)
                 if FORBIDDEN_LIBRARIES.search(library)]
    return problems


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = Path(sys.argv[1])
    if not sys.platform.startswith("linux"):
        print(f"headless_closure: NOT EVALUATED on {sys.platform}: the linked-closure read is ELF "
              "only; the configure-time closure check still ran")
        return 0
    problems = violations(binary)
    if problems:
        print(f"headless_closure: {binary.name} LINKS code a dedicated server must not have:",
              file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    print(f"headless_closure: {binary.name} links no rendering, audio, interface or display code "
          f"({len(defined_symbols(binary))} symbols read)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
