#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`cy_build lightmap`, as the editor's bake command drives it. `integration.build_lightmap_cli`.

A quad is imported with a lightmap unwrap by the real `cy_import_cli` into a project's
`.cy/cooked/` — one `.cyasset` per sub-asset — a level of two of its instances is described, and `cy_build lightmap` bakes it:

  * the progress lines arrive in stage order and the trace counts up to its total;
  * the `baked` line names what was baked, and the cooked lightmap is written;
  * a `cancel` line on stdin, sent as soon as the trace reports, stops the bake: `cancelled`, exit
    status 3, and no output file — the previous cooked lightmap, where there is one, is untouched.

Usage: test_lightmap_cli.py --cy-build <cy_build> --cy-import <cy_import_cli>
"""

import argparse
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import time

STAGES = ["prepare", "trace", "filter", "finish"]


def quad(project: pathlib.Path) -> None:
    buffer = b"".join(struct.pack("<3f", *p) for p in [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0)])
    buffer += struct.pack("<6H", 0, 1, 2, 2, 1, 3)
    assets = project / "assets"
    assets.mkdir(parents=True)
    (assets / "quad.bin").write_bytes(buffer)
    (assets / "quad.gltf").write_text(
        '{"asset":{"version":"2.0"},"buffers":[{"byteLength":%d,"uri":"quad.bin"}],'
        '"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},'
        '{"buffer":0,"byteOffset":48,"byteLength":12}],"accessors":['
        '{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},'
        '{"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}],'
        '"meshes":[{"name":"Panel","primitives":[{"attributes":{"POSITION":0},"indices":1,'
        '"mode":4}]}],"nodes":[{"name":"Quad","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0}'
        % len(buffer))


def import_quad(cy_import: str, project: pathlib.Path) -> str:
    """The quad's cooked mesh, as the importer writes it into the project: `.cy/cooked/<id>.cyasset`."""
    report = subprocess.run([cy_import, "--project", str(project), "--out",
                             str(project / ".cy/cooked"), "--cache", str(project / ".cy/cache"),
                             "--set", "generate-lightmap-uvs=true", "--json", "assets/quad.gltf"],
                            check=True, capture_output=True, text=True).stdout
    match = re.search(r'"name": "mesh/Panel", "id": "([0-9a-f]{32})"', report)
    if match is None:
        raise AssertionError(f"the importer reported no mesh/Panel:\n{report}")
    cooked = f".cy/cooked/{match.group(1)}.cyasset"
    assert (project / cooked).is_file(), f"{cooked} was not written"
    return cooked


def level(bundle: str, samples: int) -> str:
    return (
        "cylightmap 1\nmode directional\nbounces 1\n"
        f"samples {samples}\ndensity 8\npage 256\nsky 0.2 0.25 0.3\n"
        "light point 0 1 1.5 20 20 1 1 1 stationary\n"
        'material "white" 0.7 0.7 0.7\nmaterial "red" 0.8 0.1 0.1\n'
        f'instance "{bundle}" "mesh" "red" 1 4 0 0 -2  0 4 0 0  0 0 4 0\n'
        f'instance "{bundle}" "mesh" "white" 1 4 0 0 -2  0 0 4 0  0 -4 0 4\n')


def bake(cy_build: str, project: pathlib.Path, description: str, out: str,
         cancel_on_trace: bool) -> tuple[int, list[str], float]:
    started = time.monotonic()
    child = subprocess.Popen(
        [cy_build, "lightmap", "--description", str(project / description), "--project",
         str(project), "--out", str(project / out)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lines = []
    assert child.stdout is not None and child.stdin is not None
    for line in child.stdout:
        lines.append(line.strip())
        if cancel_on_trace and line.startswith("progress trace"):
            child.stdin.write("cancel\n")
            child.stdin.flush()
            cancel_on_trace = False
    child.stdin.close()
    status = child.wait()
    if status not in (0, 3):
        sys.stderr.write(child.stderr.read() if child.stderr else "")
    return status, lines, time.monotonic() - started


def check_progress(lines: list[str]) -> None:
    progress = [line.split() for line in lines if line.startswith("progress ")]
    assert progress, "no progress was reported"
    order = [STAGES.index(words[1]) for words in progress]
    assert order == sorted(order), f"stages out of order: {[w[1] for w in progress]}"
    trace = [(int(words[2]), int(words[3])) for words in progress if words[1] == "trace"]
    assert trace and trace[-1][0] == trace[-1][1], f"the trace did not reach its total: {trace}"
    assert all(a[0] <= b[0] for a, b in zip(trace, trace[1:])), "the trace went backwards"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cy-build", required=True)
    parser.add_argument("--cy-import", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cy-lightmap-cli-") as scratch:
        project = pathlib.Path(scratch) / "project"
        quad(project)
        bundle = import_quad(args.cy_import, project)
        (project / "levels").mkdir()
        (project / "levels/level.cylightmap").write_text(level(bundle, 8))
        (project / "levels/slow.cylightmap").write_text(level(bundle, 4096))

        status, lines, seconds = bake(args.cy_build, project, "levels/level.cylightmap",
                                      ".cy/cooked/level.lightmap", False)
        assert status == 0, f"the bake failed: {lines}"
        check_progress(lines)
        baked = [line for line in lines if line.startswith("baked ")]
        assert len(baked) == 1, f"expected one baked line: {lines}"
        fields = dict(word.split("=", 1) for word in baked[0].split()[1:])
        assert int(fields["objects"]) == 2 and int(fields["texels"]) > 0, fields
        payload = (project / ".cy/cooked/level.lightmap").read_bytes()
        assert payload[:4] == b"CYLM", "the output is not a cooked lightmap"
        print(f"baked: {baked[0]} ({seconds:.2f} s, {len(lines)} lines)")

        status, lines, seconds = bake(args.cy_build, project, "levels/slow.cylightmap",
                                      ".cy/cooked/slow.lightmap", True)
        assert status == 3, f"a cancelled bake exited {status}: {lines[-3:]}"
        assert lines[-1] == "cancelled", f"the last line was {lines[-1]!r}"
        assert not any(line.startswith("baked ") for line in lines)
        assert not (project / ".cy/cooked/slow.lightmap").exists(), "a cancelled bake wrote"
        assert (project / ".cy/cooked/level.lightmap").read_bytes() == payload
        print(f"cancelled after {seconds:.2f} s and {len(lines)} lines; nothing written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
