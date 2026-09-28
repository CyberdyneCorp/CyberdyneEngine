#!/usr/bin/env python3
"""Mutation driver for render.world_water_aerial_perspective (feat/aerial-water)."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path("/home/leonardo/work/cy-aerialwater")
BUILD = ROOT / "build/aerialwater"
SHADERS = ROOT / "samples/10-world/shaders"
SOURCE = SHADERS / "water.slang"
HEADERS = [SHADERS / "water_spirv.h", SHADERS / "water_msl.h"]
TOUCHED = [SOURCE, *HEADERS]
TARGET = "cy_test_render_world_water_aerial_perspective"
SAVE = ROOT / "build/aerialwater-mutation-saved"
OUT = ROOT / "openspec/changes/add-aerial-perspective-on-water/evidence/falsification.txt"

MUTATIONS = [
    ("a-reflection-hazed-twice", "let surface = (fresnel * reflected) +",
     "let surface = (fresnel * reflected * air.transmittance) +"),
    ("b-bed-hazed-twice", "if (airOn && bentDepth > 0.0)", "if (false && airOn && bentDepth > 0.0)"),
    ("c-in-scattering-under-the-mirror", "((1.0 - fresnel) * air.inScattering);",
     "air.inScattering;"),
    ("d-air-at-half-the-distance",
     "let air = cyAerialPerspectiveAt(table, world - push.eye.xyz);",
     "let air = cyAerialPerspectiveAt(table, (world - push.eye.xyz) * 0.5);"),
    ("e-near-surface-dimmed", "(own * air.transmittance)",
     "(own * min(air.transmittance, float3(0.99)))"),
    ("f-off-path-changed", "float3 lit = lerp(refracted, reflected, fresnel);",
     "float3 lit = lerp(refracted, reflected, fresnel * 0.999);"),
    ("g-water-ignores-the-table", "let airOn = cyAerialPerspectiveEnabled(table);",
     "let airOn = false;"),
]


def md5(path):
    return hashlib.md5(path.read_bytes()).hexdigest()


def run(cmd, cwd=ROOT, check=True):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"failed: {cmd}")
    return result


def regenerate():
    slangc = str(BUILD / "Development/bin/slangc")
    inc = ["-I", "../../../src/rendering/shaders"]
    for target, ext in (("spirv", "spv"), ("metal", "metal")):
        for entry, stage in (("waterVertex", "vertex"), ("waterFragment", "fragment")):
            extra = ["-profile", "spirv_1_5"] if target == "spirv" else []
            out = f"water_{stage}.{ext}"
            run([slangc, "water.slang", *inc, "-target", target, *extra, "-entry", entry, "-stage",
                 stage, "-o", out], cwd=SHADERS)
    run(["python3", "embed_spirv.py", "water_spirv.h", "kWaterVertexSpirv=water_vertex.spv",
         "kWaterFragmentSpirv=water_fragment.spv"], cwd=SHADERS)
    run(["python3", "embed_msl.py", "water_msl.h", "kWaterVertexMsl=water_vertex.metal",
         "kWaterFragmentMsl=water_fragment.metal"], cwd=SHADERS)
    run(["clang-format", "-i", "water_spirv.h", "water_msl.h"], cwd=SHADERS)
    for name in ("water_vertex.spv", "water_fragment.spv", "water_vertex.metal",
                 "water_fragment.metal"):
        (SHADERS / name).unlink(missing_ok=True)


def build_and_test():
    jobs = os.environ.get("CY_JOBS", "4")
    run(["cmake", "--build", str(BUILD), "--target", TARGET, "--parallel", jobs])
    return run(["ctest", "--test-dir", str(BUILD), "-R", "render.world_water_aerial_perspective$",
                "--output-on-failure"], check=False)


def main():
    SAVE.mkdir(exist_ok=True)
    originals = {}
    for path in TOUCHED:
        shutil.copy2(path, SAVE / path.name)
        originals[path] = md5(path)
    # The committed headers must be what the source regenerates to, or restoring proves nothing.
    regenerate()
    for path in HEADERS:
        if md5(path) != originals[path]:
            raise SystemExit(f"{path.name} does not reproduce from water.slang")
    lines = [
        "# Mutations applied to samples/10-world/shaders/water.slang, water_spirv.h and water_msl.h",
        "# regenerated with the recipe in the shader's header, cy_test_render_world_water_aerial_",
        "# perspective rebuilt and run, and all three files restored; md5 of each verified identical",
        "# after restore. Suite: render.world_water_aerial_perspective, Development, Vulkan.",
        "#",
        "# g-water-ignores-the-table skips the table in water.slang altogether. The off case's count of",
        "# water texels that move with the table on does NOT catch it: 7501 still move, because the",
        "# refraction and reflection pictures are drawn through the air by world.slang. The formula, the",
        "# shoreline and the far-water cases do.",
        "",
    ]
    selected = sys.argv[1:]
    for name, before, after in MUTATIONS:
        if selected and name not in selected:
            continue
        text = SOURCE.read_text()
        if text.count(before) != 1:
            raise SystemExit(f"{name}: '{before}' does not occur exactly once")
        SOURCE.write_text(text.replace(before, after))
        try:
            regenerate()
            result = build_and_test()
        finally:
            # copyfile, not copy2: the restored files must be newer than the mutated build, or
            # ninja keeps the mutated shader in the suite after the restore.
            for path in TOUCHED:
                shutil.copyfile(SAVE / path.name, path)
        kept = [l for l in (result.stdout + result.stderr).splitlines()
                if "MESSAGE:" in l or "ERROR:" in l or "test cases:" in l]
        state = "RED" if result.returncode != 0 else "GREEN"
        lines.append(f"== {name}: water.slang: '{before}' -> '{after}'")
        lines.append(f"exit {result.returncode} ({state})")
        lines.extend(l.strip() for l in kept)
        sums = ", ".join(f"{p.name} md5 {md5(p)}" for p in TOUCHED)
        same = all(md5(p) == originals[p] for p in TOUCHED)
        lines.append(f"restored ({sums}: {'identical' if same else 'DIFFERENT'})")
        lines.append("")
        print(name, state, flush=True)
        if not same:
            raise SystemExit("restore mismatch")
    build_and_test()
    OUT.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
