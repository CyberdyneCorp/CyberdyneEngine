#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`cy_build lightmap`, as the editor's bake command drives it. `integration.build_lightmap_cli`.

A quad is imported with a lightmap unwrap by the real `cy_import_cli` into a project's
`.cy/cooked/` — one `.cyasset` per sub-asset — a level of two of its instances is described, and `cy_build lightmap` bakes it:

  * the progress lines arrive in stage order and the trace counts up to its total;
  * the `baked` line names what was baked, and the cooked lightmap is written;
  * a `cancel` line on stdin, sent as soon as the trace reports, stops the bake: `cancelled`, exit
    status 3, and no output file — the previous cooked lightmap, where there is one, is untouched;
  * the tool exits, refused or finished, while its stdin is still open: the editor holds the pipe
    open for a `cancel` it may never send, and glibc's `exit` once waited on the watcher's lock;
  * an unchanged level is not baked again: the second run prints `cached=1`, reports no progress
    and leaves the cooked lightmap untouched, and a changed description bakes again;
  * the level the editor writes (`data/editor_level/`) bakes as it is, its irradiance volume
    captured into `<out>.cyprobes`, and a level with no volume removes a stale probe file;
  * a `cooked` material — what the editor writes for the material an object draws with — bakes
    exactly as its base colour, and under a `tint` as the base colour times the tint.

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

STAGES = ["prepare", "trace", "filter", "finish", "probes"]


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


def exits_with_stdin_open(cy_build: str, project: pathlib.Path, description: str,
                          out: str) -> int:
    """The exit status of a run whose stdin stays open until it ends, as the editor's does."""
    child = subprocess.Popen(
        [cy_build, "lightmap", "--description", str(project / description), "--project",
         str(project), "--out", str(project / out)],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        return child.wait(timeout=60)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()
        raise AssertionError(f"`cy_build lightmap` on {description} did not exit while its stdin "
                             "was open") from None
    finally:
        assert child.stdin is not None
        child.stdin.close()


def baked_fields(lines: list[str]) -> dict[str, str]:
    baked = [line for line in lines if line.startswith("baked ")]
    assert len(baked) == 1, f"expected one baked line: {lines}"
    return dict(word.split("=", 1) for word in baked[0].split()[1:])


def check_cached(cy_build: str, project: pathlib.Path, payload: bytes) -> None:
    """A second bake of the unchanged level bakes nothing; a changed description bakes."""
    cooked = project / ".cy/cooked/level.lightmap"
    stamp = cooked.stat().st_mtime_ns
    status, lines, seconds = bake(cy_build, project, "levels/level.cylightmap",
                                  ".cy/cooked/level.lightmap", False)
    assert status == 0, f"the cached run failed: {lines}"
    assert not any(line.startswith("progress ") for line in lines), f"it baked again: {lines}"
    assert baked_fields(lines)["cached"] == "1", lines
    assert cooked.read_bytes() == payload and cooked.stat().st_mtime_ns == stamp
    print(f"unchanged level: cached in {seconds:.2f} s")

    description = project / "levels/level.cylightmap"
    original = description.read_text()
    description.write_text(original.replace("bounces 1", "bounces 2"))
    status, lines, _ = bake(cy_build, project, "levels/level.cylightmap",
                            ".cy/cooked/level.lightmap", False)
    assert status == 0 and baked_fields(lines)["cached"] == "0", lines
    assert any(line.startswith("progress trace") for line in lines)
    # And back: the key is the level's, not the last run's, so this bakes the first bytes again.
    description.write_text(original)
    status, lines, _ = bake(cy_build, project, "levels/level.cylightmap",
                            ".cy/cooked/level.lightmap", False)
    assert status == 0 and baked_fields(lines)["cached"] == "0", lines
    assert cooked.read_bytes() == payload, "the same level baked to different bytes"
    print("a changed description bakes again")


def check_editor_level(cy_build: str, scratch: pathlib.Path) -> None:
    """The level exactly as the editor writes it, baked from the fixture's own project."""
    level = pathlib.Path(__file__).resolve().parent / "data/editor_level"
    out = scratch / "editor/room.lightmap"
    out.parent.mkdir(parents=True)
    child = subprocess.run(
        [cy_build, "lightmap", "--description", str(level / "levels/room.cylightmap"),
         "--project", str(level), "--out", str(out)],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
    assert child.returncode == 0, child.stderr
    lines = child.stdout.splitlines()
    check_progress(lines)
    fields = baked_fields(lines)
    assert fields["objects"] == "1" and fields["volumes"] == "1" and fields["probes"] == "18", \
        fields
    probes = out.with_suffix(".cyprobes").read_bytes()
    assert probes[:4] == b"CYPV", "the probes are not a probe payload"
    assert struct.unpack_from("<I", probes, 8)[0] == 1, "one volume"
    print(f"the editor's level: {' '.join(f'{k}={v}' for k, v in fields.items())}")

    # The same level with its volume removed: no probes, and the stale file is gone.
    bare = scratch / "editor/bare.cylightmap"
    bare.write_text("".join(line for line in (level / "levels/room.cylightmap").read_text()
                            .splitlines(keepends=True) if not line.startswith("volume ")))
    child = subprocess.run(
        [cy_build, "lightmap", "--description", str(bare), "--project", str(level), "--out",
         str(out)], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
    assert child.returncode == 0, child.stderr
    assert baked_fields(child.stdout.splitlines())["volumes"] == "0"
    assert not out.with_suffix(".cyprobes").exists(), "a stale probe file was left"
    print("a level with no volume leaves no probe file")


def check_cooked_material(cy_import: str, cy_build: str, scratch: pathlib.Path) -> None:
    """A cooked material bakes byte for byte as the colour the importer cooked into it."""
    project = scratch / "materials"
    quad(project)
    gltf = project / "assets/quad.gltf"
    gltf.write_text(gltf.read_text().replace(
        '"meshes":[{"name":"Panel","primitives":[{"attributes":{"POSITION":0},"indices":1,'
        '"mode":4}]}]',
        '"materials":[{"name":"Brick","pbrMetallicRoughness":{"baseColorFactor":[0.8,0.2,0.1,1]}}],'
        '"meshes":[{"name":"Panel","primitives":[{"attributes":{"POSITION":0},"indices":1,'
        '"mode":4,"material":0}]}]'))
    report = subprocess.run([cy_import, "--project", str(project), "--out",
                             str(project / ".cy/cooked"), "--cache", str(project / ".cy/cache"),
                             "--set", "generate-lightmap-uvs=true", "--json", "assets/quad.gltf"],
                            check=True, capture_output=True, text=True).stdout
    ids = dict(re.findall(r'"name": "([^"]+)", "id": "([0-9a-f]{32})"', report))
    mesh = f".cy/cooked/{ids['mesh/Panel']}.cyasset"
    brick = f".cy/cooked/{ids['material/Brick']}.cyasset"
    (project / "levels").mkdir()

    def baked(material: str, name: str) -> bytes:
        description = project / f"levels/{name}.cylightmap"
        description.write_text(level(mesh, 8).replace('material "red" 0.8 0.1 0.1', material))
        out = project / f".cy/cooked/{name}.lightmap"
        child = subprocess.run(
            [cy_build, "lightmap", "--description", str(description), "--project", str(project),
             "--out", str(out)], stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=120)
        assert child.returncode == 0, child.stderr
        return out.read_bytes()

    assert baked(f'material "red" cooked "{brick}"', "cooked") == \
        baked('material "red" 0.8 0.2 0.1', "plain"), "a cooked material baked another colour"
    assert baked(f'material "red" cooked "{brick}" tint 0.5 1 1', "tinted") == \
        baked('material "red" 0.4 0.2 0.1', "times"), "a tint did not multiply the base colour"
    assert baked(f'material "red" cooked "{brick}"', "again") != \
        baked('material "red" 0.1 0.2 0.8', "other"), "the albedo made no difference"
    print("a cooked material bakes as its base colour, and a tint multiplies it")


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
        (project / "levels/malformed.cylightmap").write_text("not a description\n")

        status = exits_with_stdin_open(args.cy_build, project, "levels/malformed.cylightmap",
                                       ".cy/cooked/malformed.lightmap")
        assert status == 1, f"a malformed description exited {status}"
        status = exits_with_stdin_open(args.cy_build, project, "levels/level.cylightmap",
                                       ".cy/cooked/open.lightmap")
        assert status == 0, f"a bake with stdin open exited {status}"
        print("refused and finished runs exit with stdin open")

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
        check_cached(args.cy_build, project, payload)

        status, lines, seconds = bake(args.cy_build, project, "levels/slow.cylightmap",
                                      ".cy/cooked/slow.lightmap", True)
        assert status == 3, f"a cancelled bake exited {status}: {lines[-3:]}"
        assert lines[-1] == "cancelled", f"the last line was {lines[-1]!r}"
        assert not any(line.startswith("baked ") for line in lines)
        assert not (project / ".cy/cooked/slow.lightmap").exists(), "a cancelled bake wrote"
        assert (project / ".cy/cooked/level.lightmap").read_bytes() == payload
        print(f"cancelled after {seconds:.2f} s and {len(lines)} lines; nothing written")
        check_editor_level(args.cy_build, pathlib.Path(scratch))
        check_cooked_material(args.cy_import, args.cy_build, pathlib.Path(scratch))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
