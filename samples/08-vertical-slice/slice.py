#!/usr/bin/env python3
"""samples/08-vertical-slice — M8.b's closing artefact. Section 12.

`just run-vertical-slice` is the recipe; `smoke.vertical_slice` is the CTest entry; this file is
what both of them run. It drives `cy_sample_vertical-slice` — the game — checks every claim that
program printed, draws the picture from the frame's own draw list, and reports through
`samples/harness/artefact.py`, which is what makes a recorded gap a non-zero exit and refuses an
extreme value as the figure the run leads with.

--- THE FOUR ACTS, AND WHAT EACH ONE CLAIMS ------------------------------------------------------

  1. PLAY    the game runs. A level with a navigation mesh built by Recast over its own collision
             triangles; five graphs authored in CyberGraph and COMPILED, none interpreted;
             characters that sense, think, steer, animate and cast; a hundred concurrent effects;
             a heads-up interface that passes its own accessibility audit; a 2D menu that batches;
             sound in importance tiers with the music following the fight; and a frame assembled
             out of the renderer's own parts, every draw of it resolving to a material slot.
  2. SCALE   the same game at 8,000 agents, against the same game at 2,000. Two claims, and the
             second is the one that means something: every declared budget is held, AND the cost
             per agent at four times the population is within the declared linearity factor.
             A budget alone measures the host; a linearity factor catches a quadratic.
  3. REPLAY  determinism, twice over. In one process (`--replay` builds a second slice from the
             same options and compares digests) and across two processes, which is the half that
             catches a digest depending on an address or on an allocation order.
  4. CONTROL the negative control. `--interpret-control` puts a per-entity virtual `tick()` in the
             loop; the audit must FIND it and the program must exit non-zero. The criterion passes
             only when that run FAILS — a check that cannot fail is not a check.

--- WHY THE PICTURE IS DRAWN AND NOT CAPTURED ----------------------------------------------------

`FrameAssembly` hands a pass's record callback to its CALLER — "it does not own the shaders or the
pipelines" — and this sample supplies none, so there is no swapchain to photograph. What is drawn
below is the frame's own answer: every shape is one item of the SORTED DRAW LIST the assembly
produced, its bounds are the spatial index's, its silhouette is the mesh its `MeshRenderer`
reference actually resolved to, and its corners were projected by `projection * view` in C++ before
this script saw them. The interface is `cy::ui`'s own flattened primitives and the menu is
`cy::rendering2d`'s own batched instances. Nothing here re-derives what is on screen; it reads it.

That is the difference from M8.a's photograph, and it is the milestone's: M8.a drew an authored
sphere as a unit box because the reference reached no renderer.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

SAMPLE_DIR = Path(__file__).resolve().parent
ROOT = SAMPLE_DIR.parents[1]
sys.path.insert(0, str(ROOT / "samples" / "harness"))

from artefact import Absent, Failed, Report, Statistic, expect  # noqa: E402


# --- Running the game -----------------------------------------------------------------------------


def run_sample(binary: Path, arguments: list[str], expect_failure: bool = False) -> dict[str, str]:
    """Run the program once and parse its `key = value` lines.

    `expect_failure` is the negative control's: that run MUST exit non-zero, and a zero from it is
    the failure. Everywhere else a non-zero exit is the failure, and the program's own output is
    carried into the message because a driver that hides the program's diagnosis makes every
    problem look like the driver's.
    """
    completed = subprocess.run(
        [str(binary), *arguments], cwd=str(ROOT), capture_output=True, text=True, timeout=1800,
    )
    if expect_failure and completed.returncode == 0:
        raise Failed(
            "the run was expected to fail and exited 0: "
            f"{' '.join(arguments)}\n{completed.stdout[-2000:]}"
        )
    if not expect_failure and completed.returncode != 0:
        raise Failed(
            f"cy_sample_vertical-slice exited {completed.returncode}\n"
            f"{completed.stdout[-4000:]}\n{completed.stderr[-4000:]}"
        )
    values: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        if " = " not in line:
            continue
        key, value = line.split(" = ", 1)
        # A repeated key becomes a list joined by a comma: `audit_offender` is printed once per
        # offending type, and a run with two of them must not report only the second.
        values[key] = f"{values[key]},{value}" if key in values else value
    values["_stdout"] = completed.stdout
    return values


def number(values: dict[str, str], key: str) -> float:
    raw = values.get(key)
    expect(raw is not None, f"the program printed no '{key}'")
    return float(str(raw))


def whole(values: dict[str, str], key: str) -> int:
    return int(number(values, key))


def check(report: Report, name: str, ok: bool, detail: str) -> bool:
    """One claim, recorded either way. Returns whether it held, so a caller can branch."""
    if ok:
        report.did(name, detail)
    else:
        report.gap(name, detail)
    return ok


# --- Act 1: the game plays -------------------------------------------------------------------------


def act_play(report: Report, binary: Path, agents: int, ticks: int, shot_data: Path) -> dict:
    print("\n--- act 1: the game ---")
    values = run_sample(
        binary,
        ["--agents", str(agents), "--ticks", str(ticks), "--shot-data", str(shot_data)],
    )

    # THE LEVEL. Real collision triangles, a navigation mesh built over them, and the arena's props
    # as entities the renderer will draw.
    check(report, "a level exists, with a navigation mesh over its own triangles",
          whole(values, "level_entities") > 0 and whole(values, "level_triangles") > 0
          and whole(values, "navmesh_polys") > 0,
          f"{whole(values, 'level_entities')} props, "
          f"{whole(values, 'level_triangles')} triangles, "
          f"{whole(values, 'navmesh_polys')} navigation polygons in "
          f"{whole(values, 'navmesh_tiles')} tile(s)")
    check(report, "the navigation mesh came from Recast rather than from the fallback",
          whole(values, "navmesh_recast") == 1,
          "NavBuildBackend::Recast — the engine-owned rasteriser is the other one, and "
          "integration.navigation_build asserts both agree")

    # THE FIVE PROGRAMS. Each is an authored CyberGraph lowered by the compiler its own
    # specification names; a digest is what says a program exists and is the one that was authored.
    digests = {name: values.get(f"{name}_digest", "") for name in
               ("script", "ability", "pose", "behaviour", "rig")}
    check(report, "five authored graphs compiled to five programs",
          all(digest and set(digest) != {"0"} for digest in digests.values()),
          ", ".join(f"{name} {digest}" for name, digest in digests.items()))
    check(report, "the camera rig is a program on the shared expression core",
          values.get("rig_ir_digest", "0" * 16) != "0" * 16 and whole(values, "rig_queries") > 0,
          f"IR {values.get('rig_ir_digest')}, {whole(values, 'rig_queries')} queries resolved in "
          f"{whole(values, 'rig_query_calls')} batched call(s)")

    # AN AUTHORED MESH REACHES A RENDERER. Task 11.3's chain, end to end: a reference on a
    # reflected component, resolved to a handle, published in a snapshot, drawn with a material.
    check(report, "every authored asset reference resolved to a renderer handle",
          whole(values, "meshes_bound") == whole(values, "level_entities") + agents
          and whole(values, "meshes_unresolved") == 0,
          f"{whole(values, 'meshes_bound')} mesh and "
          f"{whole(values, 'materials_bound')} material handles bound over "
          f"{whole(values, 'mesh_assets')} distinct mesh assets, none unresolved")

    # THE SYSTEMS. Each number is the module's own report rather than the sample's arithmetic.
    check(report, "characters sense, think, steer and animate",
          whole(values, "agents_thought") > 0 and whole(values, "perception_queries") > 0
          and whole(values, "crowd_adjusted") > 0 and whole(values, "poses_evaluated") > 0,
          f"{whole(values, 'agents_thought')} thinks over "
          f"{whole(values, 'think_instructions')} instructions, "
          f"{whole(values, 'perception_queries')} visibility queries, "
          f"{whole(values, 'crowd_adjusted')} avoidance adjustments, "
          f"{whole(values, 'poses_evaluated')} poses over "
          f"{whole(values, 'joints_sampled')} sampled joints")
    check(report, "root motion is integrated on the deterministic half of the animation system",
          number(values, "root_motion_travelled") > 0.0,
          f"{number(values, 'root_motion_travelled'):.2f} m travelled — advance() integrates it, "
          "evaluate() does not, which is what makes it independent of the LOD tier")
    check(report, "paths are found through the navigation mesh",
          whole(values, "paths_found") > 0,
          f"{whole(values, 'paths_found')} corridors found by find_path over the run")

    # ABILITIES AND EFFECTS.
    check(report, "abilities activate through the pipeline, and refusals are refusals",
          whole(values, "activations_committed") > 0 and whole(values, "activations_refused") > 0,
          f"{whole(values, 'activations_committed')} committed, "
          f"{whole(values, 'activations_refused')} refused (cost before cooldown, which is the "
          f"specification's order), {whole(values, 'cues_emitted')} cues emitted")
    check(report, "a hundred concurrent effects are live",
          whole(values, "effects_peak") >= 100,
          f"{whole(values, 'effects_peak')} concurrent at the peak, "
          f"{whole(values, 'effects_expired')} expired over the run")

    # THE INTERFACE, THE MENU AND THE SOUND.
    check(report, "the heads-up interface lays out, flattens and batches",
          whole(values, "ui_primitives") == whole(values, "ui_elements")
          and whole(values, "ui_batches") > 0,
          f"{whole(values, 'ui_elements')} elements to "
          f"{whole(values, 'ui_primitives')} primitives in "
          f"{whole(values, 'ui_batches')} batch(es)")
    check(report, "the interface passes its own accessibility audit",
          whole(values, "ui_accessible") == 1 and whole(values, "ui_findings") == 0,
          "audit_accessibility reports no finding: the one interactive element is focusable, "
          "labelled, large enough to hit and legible against its own background")
    check(report, "the 2D menu draws through the sprite batcher",
          whole(values, "menu_draws") > 0 and whole(values, "menu_batches") > 0,
          f"{whole(values, 'menu_draws')} draws in {whole(values, 'menu_batches')} batch(es) — "
          "a break per material, which is what the batch report names")
    check(report, "sound is scored into importance tiers and the music follows the fight",
          whole(values, "audio_sources") > 0 and whole(values, "audio_full_acoustic") > 0
          and whole(values, "music_transitions") > 0,
          f"{whole(values, 'audio_sources')} sources, "
          f"{whole(values, 'audio_full_acoustic')} at full acoustic simulation, "
          f"{whole(values, 'audio_virtual')} virtualised, "
          f"{whole(values, 'music_transitions')} transition(s) scheduled on a bar")

    # THE FRAME.
    check(report, "a frame is assembled out of the renderer's own parts",
          whole(values, "frame_assembled") == 1 and whole(values, "frame_draws") > 0
          and whole(values, "frame_passes") > 0,
          f"{whole(values, 'frame_draws')} draws, {whole(values, 'frame_visible')} visible, "
          f"{whole(values, 'frame_lights')} lights in "
          f"{whole(values, 'frame_clusters')} clusters, "
          f"{whole(values, 'frame_shadow_pages')} shadow pages, "
          f"{whole(values, 'frame_passes')} passes declared")
    check(report, "every draw resolves to a slot the material table holds",
          whole(values, "frame_draws_without_material") == 0
          and whole(values, "frame_material_slots") > 0,
          f"0 of {whole(values, 'frame_draws')} draws shade with slot zero, against a table of "
          f"{whole(values, 'frame_material_slots')} slots — the join M7's gate asked for")
    # AND THE MESHES ARE DISTINCT, which "every reference resolved" does not say. A resolver that
    # answered one handle to every reference leaves `meshes_unresolved` at zero and this at one,
    # and draws M8.a's picture again with a handle in front of it. Added by M8.b's closing gate,
    # which broke the resolver on purpose and watched every silhouette collapse to the ground mesh
    # while every other check stayed green.
    check(report, "the frame names as many distinct meshes as the level interned",
          whole(values, "frame_distinct_meshes") == whole(values, "mesh_assets"),
          f"{whole(values, 'frame_distinct_meshes')} distinct mesh handles over "
          f"{whole(values, 'frame_draws')} draws, against {whole(values, 'mesh_assets')} mesh "
          "assets — read back out of the scene index, not out of the sample's own table")

    # THE AUDIT. `docs/ROADMAP.md`: "No graph is interpreted at runtime — an audit finds no
    # per-entity virtual tick in any graph consumer."
    check(report, "no graph is interpreted at runtime",
          whole(values, "audit_passed") == 1,
          f"{whole(values, 'audit_graphs')} graphs audited clean AND complete, "
          f"{whole(values, 'audit_state_types')} per-entity state types checked and none "
          f"polymorphic, {whole(values, 'audit_compilations_before_loop')} compilations before "
          f"the loop and {whole(values, 'audit_compilations_during_loop')} inside it")
    return values


# --- Act 2: the scale the criterion names -----------------------------------------------------------


def act_scale(report: Report, binary: Path, scale_agents: int, ticks: int) -> tuple[dict, dict]:
    print("\n--- act 2: scale ---")
    baseline_agents = max(64, scale_agents // 4)
    baseline = run_sample(binary, ["--agents", str(baseline_agents), "--ticks", str(ticks)])
    large = run_sample(binary, ["--agents", str(scale_agents), "--ticks", str(ticks)])

    simulation = number(large, "simulation_us_median")
    effects = number(large, "effects_us_median")
    interface = number(large, "interface_us_median")
    budget_simulation = number(large, "budget_simulation_us")
    budget_effects = number(large, "budget_effects_us")
    budget_interface = number(large, "budget_interface_us")

    check(report, f"{scale_agents} agents hold the declared simulation budget",
          simulation <= budget_simulation,
          f"{simulation / 1000.0:.2f} ms a tick against a declared {budget_simulation / 1000:.0f} "
          f"ms — think {number(large, 'think_us_median'):.0f} us, "
          f"sense {number(large, 'sense_us_median'):.0f} us, "
          f"navigate {number(large, 'navigate_us_median'):.0f} us, "
          f"animate {number(large, 'animate_us_median'):.0f} us, "
          f"act {number(large, 'abilities_us_median'):.0f} us")
    check(report, f"{whole(large, 'effects_peak')} concurrent effects hold the declared budget",
          effects <= budget_effects and whole(large, "effects_peak") >= 100,
          f"EffectSystem::advance {effects:.2f} us against a declared {budget_effects:.0f} us, "
          f"over {whole(large, 'effects_peak')} concurrent effects")
    check(report, "the interface holds its frame budget",
          interface <= budget_interface,
          f"{interface:.2f} us against a declared {budget_interface / 1000:.0f} ms")

    # THE ONE THAT CATCHES A QUADRATIC. Four times the agents, and the cost per agent barely moves.
    small_per_agent = number(baseline, "per_agent_us_median")
    large_per_agent = number(large, "per_agent_us_median")
    factor = large_per_agent / small_per_agent if small_per_agent > 0 else float("inf")
    allowed = number(large, "budget_linearity")
    check(report, "cost is bounded by configuration rather than by population",
          factor <= allowed,
          f"{scale_agents} agents cost {large_per_agent:.3f} us each against "
          f"{small_per_agent:.3f} us at {baseline_agents} — a factor of {factor:.2f} for four "
          f"times the population, against a declared {allowed:.1f}. A quadratic gives four")

    # The LOD distribution, which is what "bounded by configuration" actually names. Not a claim:
    # a figure, so a reader can see which dial did the bounding.
    report.figure(Statistic.stable(
        "agents at Full on the last tick", whole(large, "agents_full"), "agents"))
    report.figure(Statistic.stable(
        "agents at Statistical on the last tick", whole(large, "agents_statistical"), "agents"))
    return baseline, large


# --- Act 3: determinism ------------------------------------------------------------------------------


def act_replay(report: Report, binary: Path, agents: int, ticks: int) -> None:
    print("\n--- act 3: determinism ---")
    within = run_sample(binary, ["--agents", str(agents), "--ticks", str(ticks), "--replay"])
    check(report, "two slices built from the same options agree, in one process",
          within["state_digest"] == within["replay_digest"],
          f"state {within['state_digest']} = replay {within['replay_digest']} — every ability "
          "activation, every pose evaluation and every character's placement, folded in tick order")

    across = run_sample(binary, ["--agents", str(agents), "--ticks", str(ticks)])
    check(report, "two processes agree, which is the half an address could break",
          across["state_digest"] == within["state_digest"],
          f"{across['state_digest']} in a second process — a digest that depended on an address "
          "or on an allocation order would differ here and nowhere else")


    # The compiled programs must agree too. A state digest that matched while a program digest
    # moved would mean the simulation happened to reach the same place through different code,
    # which is a coincidence rather than determinism.
    programs = ("script_digest", "ability_digest", "pose_digest", "behaviour_digest", "rig_digest")
    moved = [key.removesuffix("_digest") for key in programs if within[key] != across[key]]
    check(report, "the five compiled programs are identical across processes",
          not moved,
          "; ".join(f"{key.removesuffix('_digest')} {within[key]}" for key in programs)
          if not moved else
          f"{', '.join(moved)} differ between two runs of the same graph. THIS WAS A REAL DEFECT "
          "ONCE AND HERE IS ITS SHAPE, because a second one would most likely have the same one: "
          "`finish_digest` in src/graph/src/lower_script.cpp hashed each constant with "
          "`hash_bytes(&constant, sizeof(constant))`, and `script::Value` is 32 bytes of which 4 "
          "are padding between `z` and `handle` that no member initialiser touches — so the digest "
          "closed over indeterminate stack bytes, and a byte dump found a fragment of a spilled "
          "stack address in exactly those four. It hashes the five fields through "
          "`script::hash_constant` now, and src/graph/tests/test_lowering.cpp holds it there. A "
          "digest that moves again is a cook key and a back-end selection key that moves with it")


# --- Act 4: the negative control ----------------------------------------------------------------------


def act_control(report: Report, binary: Path, agents: int, ticks: int) -> None:
    print("\n--- act 4: the negative control ---")
    # `--interpret-control` adds a per-entity object with a virtual `tick()` and calls it in the
    # loop. The audit must FIND it, and the program must return non-zero because of it. A run that
    # passed here would mean the audit in act 1 proves nothing.
    values = run_sample(
        binary,
        ["--agents", str(agents), "--ticks", str(ticks), "--interpret-control"],
        expect_failure=True,
    )
    found = whole(values, "audit_polymorphic")
    check(report, "an interpreted consumer is FOUND and the run fails because of it",
          whole(values, "audit_passed") == 0 and found > 0,
          f"{found} polymorphic per-entity state type(s) named: "
          f"{values.get('audit_offender', 'none')}. The run exited non-zero, which is what makes "
          "act 1's clean audit a measurement rather than a formality")


# --- The committed picture --------------------------------------------------------------------------
#
# Read `shot.txt` — written by the program from its own draw list — and paint it. Three layers, in
# the order a frame composites them: the world, the heads-up interface, and the menu over both.

INK = {
    "window": (0x0E, 0x10, 0x12),
    "panel": (0x16, 0x19, 0x1C),
    "primary": (0xE6, 0xE9, 0xEC),
    "secondary": (0xA2, 0xAB, 0xB4),
    "live": (0x35, 0xC0, 0x7C),
    "warning": (0xF0, 0x91, 0x3A),
    "active": (0x4C, 0x9A, 0xFF),
}

#: A base colour per MATERIAL, which is the slot the frame resolved each draw to rather than a
#: palette kept beside the geometry. Two teams are two materials over one mesh, and that is what
#: makes the picture say which side a character is on — the shape says what it is, the material
#: says what it is painted with, and both came out of the same resolved reference.
MATERIAL_INK = {
    "ground": (0x33, 0x38, 0x3C),
    "wall": (0x4A, 0x50, 0x57),
    "crate": (0x86, 0x69, 0x40),
    "pillar": (0x5C, 0x63, 0x6C),
    "team-blue": (0x2E, 0x6F, 0xC0),
    "team-red": (0xC0, 0x45, 0x3A),
}


def _monospace(size: int):
    from PIL import ImageFont

    for candidate in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    ):
        if Path(candidate).is_file():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def _shade(colour: tuple[int, int, int], amount: float) -> tuple[int, int, int]:
    return tuple(max(0, min(255, int(channel * amount))) for channel in colour)  # type: ignore


def read_shot(path: Path) -> dict[str, list]:
    """Parse the program's own draw list. One `shape`, `hud` or `menu` per line."""
    shapes: list[dict] = []
    hud: list[tuple] = []
    menu: list[tuple] = []
    viewport = (1600, 900)
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "viewport":
            viewport = (int(fields[1]), int(fields[2]))
        elif fields[0] == "shape":
            corners = [float(value) for value in fields[5:]]
            shapes.append({
                "kind": fields[1],
                "material": fields[2],
                "depth": float(fields[3]),
                "shade": float(fields[4]),
                "points": [(corners[index * 2], corners[index * 2 + 1])
                           for index in range(len(corners) // 2)],
            })
        elif fields[0] in ("hud", "menu"):
            rect = tuple(float(value) for value in fields[1:5])
            colour = int(fields[5], 16)
            (hud if fields[0] == "hud" else menu).append((rect, colour))
    return {"viewport": viewport, "shapes": shapes, "hud": hud, "menu": menu}


def _paint_rect(image, rect, colour: int) -> None:
    """One premultiplied RGBA rectangle, composited by hand.

    Pillow's `ImageDraw.rectangle` has no alpha over an RGB image, and the interface's own
    primitives carry one — the vitals panel is 0xC0-alpha over the world, which is what makes it
    read as an overlay rather than as a hole. Compositing here keeps the colour the interface's.
    """
    from PIL import Image

    alpha = (colour >> 24) & 0xFF
    if alpha == 0:
        return
    x, y, width, height = rect
    if width <= 0 or height <= 0:
        return
    box = (int(x), int(y), int(x + width), int(y + height))
    patch = Image.new("RGB", (max(1, box[2] - box[0]), max(1, box[3] - box[1])),
                      ((colour >> 16) & 0xFF, (colour >> 8) & 0xFF, colour & 0xFF))
    image.paste(Image.blend(image.crop(box).convert("RGB"), patch, alpha / 255.0), box)


def render_shot(report: Report, shot: dict, values: dict, headline: Statistic, path: Path) -> None:
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print(f"    (no Pillow, so no screenshot at {path})", file=sys.stderr)
        return

    width, height = shot["viewport"]
    banner, footer_rows = 46, 5
    footer = 22 * footer_rows + 18
    image = Image.new("RGB", (width, banner + height + footer), INK["window"])
    world = Image.new("RGB", (width, height), (0x10, 0x14, 0x1A))
    draw = ImageDraw.Draw(world)

    # THE WORLD, back to front. The assembly sorted the draw list; the depth here is the draw's own
    # average clip-space w, and painting far shapes first is what a rasteriser's depth test does
    # for a set of convex boxes.
    for shape in sorted(shot["shapes"], key=lambda item: -item["depth"]):
        base = MATERIAL_INK.get(shape["material"], (0x80, 0x80, 0x80))
        points = shape["points"]
        # The convex hull of the eight projected corners IS the box's silhouette. Computed here
        # rather than in C++ because it is a property of the picture and not of the frame.
        hull = _convex_hull(points)
        if len(hull) >= 3:
            draw.polygon(hull, fill=_shade(base, shape["shade"]),
                         outline=_shade(base, shape["shade"] * 0.65))

    for rect, colour in shot["hud"]:
        _paint_rect(world, rect, colour)
    for rect, colour in shot["menu"]:
        _paint_rect(world, rect, colour)
    image.paste(world, (0, banner))

    font, small = _monospace(14), _monospace(12)
    draw = ImageDraw.Draw(image)
    draw.rectangle([0, 0, width, banner], fill=INK["panel"])
    draw.text((24, 14), "CyberEngine", font=_monospace(15), fill=INK["primary"])
    draw.text((152, 15), "M8.b — samples/08-vertical-slice", font=font, fill=INK["secondary"])
    draw.text((width - 320, 15), str(headline), font=font, fill=INK["live"])

    lines = [
        f"level {values['level_entities']} props · {values['level_triangles']} triangles · "
        f"{values['navmesh_polys']} navigation polygons from Recast",
        f"agents {values['agents']} · {values['agents_thought']} thinks · "
        f"{values['perception_queries']} visibility queries · "
        f"{values['crowd_adjusted']} avoidance adjustments · "
        f"{values['poses_evaluated']} poses",
        f"abilities {values['activations_committed']} committed · "
        f"{values['activations_refused']} refused · {values['effects_peak']} concurrent effects · "
        f"{values['cues_emitted']} cues",
        f"frame {values['frame_draws']} draws · {values['frame_lights']} lights · "
        f"{values['frame_clusters']} clusters · {values['frame_passes']} passes · "
        f"{values['frame_draws_without_material']} draws without a material",
        f"audit {values['audit_graphs']} graphs clean and complete · "
        f"{values['audit_state_types']} per-entity state types, none polymorphic · "
        f"{values['audit_compilations_during_loop']} compilations inside the loop",
    ]
    y = banner + height + 8
    for text in lines:
        draw.text((24, y), text[:180], font=small, fill=INK["secondary"])
        y += 22

    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    report.shot(path)


def _convex_hull(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Andrew's monotone chain, over eight points. Enough to outline a projected box."""
    ordered = sorted(set(points))
    if len(ordered) <= 2:
        return ordered

    def half(sequence):
        stack: list[tuple[float, float]] = []
        for point in sequence:
            while len(stack) >= 2:
                (ax, ay), (bx, by) = stack[-2], stack[-1]
                if (bx - ax) * (point[1] - ay) - (by - ay) * (point[0] - ax) > 0:
                    break
                stack.pop()
            stack.append(point)
        return stack[:-1]

    return half(ordered) + half(reversed(ordered))


# --- The recipe ----------------------------------------------------------------------------------------


def build(profile: str, build_dir: Path) -> Path:
    """Build the engine and find the sample, the way every other artefact's driver does."""
    completed = subprocess.run(
        ["just", "build-engine", "--profile", profile], cwd=str(ROOT), text=True,
    )
    if completed.returncode != 0:
        raise Failed(f"just build-engine --profile {profile} exited {completed.returncode}")
    binary = build_dir / "samples" / "08-vertical-slice" / "cy_sample_vertical-slice"
    if not binary.is_file():
        raise Absent(f"cy_sample_vertical-slice was not built into {binary}")
    return binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample", help="the program to drive; skips building")
    parser.add_argument("--profile", default="dev", help="the profile to build, without --sample")
    parser.add_argument("--build-dir", help="where that profile built")
    parser.add_argument("--work", default="build/vertical-slice", help="scratch directory")
    parser.add_argument("--agents", type=int, default=256, help="characters in the play act")
    parser.add_argument("--ticks", type=int, default=240, help="ticks in the play act")
    parser.add_argument("--scale-agents", type=int, default=8000,
                        help="the population the scale act runs; the criterion's figure is 8000")
    parser.add_argument("--scale-ticks", type=int, default=60)
    parser.add_argument("--shot", help="write the run's picture here")
    parser.add_argument("--only", choices=("play", "scale", "replay", "control"),
                        help="run one act")
    arguments = parser.parse_args()

    report = Report()
    work = Path(arguments.work)
    work.mkdir(parents=True, exist_ok=True)
    try:
        if arguments.sample:
            binary = Path(arguments.sample)
            if not binary.is_file():
                raise Absent(f"no such program: {binary}")
        else:
            build_dir = Path(arguments.build_dir or f"build/{arguments.profile}")
            binary = build(arguments.profile, build_dir)

        shot_data = work / "shot.txt"
        values: dict[str, str] = {}
        wanted = arguments.only
        if wanted in (None, "play"):
            values = act_play(report, binary, arguments.agents, arguments.ticks, shot_data)
        if wanted in (None, "scale"):
            act_scale(report, binary, arguments.scale_agents, arguments.scale_ticks)
        if wanted in (None, "replay"):
            act_replay(report, binary, arguments.agents, arguments.ticks)
        if wanted in (None, "control"):
            act_control(report, binary, min(arguments.agents, 64), 10)

        if values:
            # THE HEADLINE IS A MEDIAN AND THE HARNESS REFUSES ANYTHING ELSE. The figure this run
            # leads with is the simulation's median tick at the play act's population — every
            # system in one loop, over the ticks after the warm-up.
            headline = report.headline(Statistic.stable(
                "median simulation tick", number(values, "simulation_us_median"), "us",
                samples=arguments.ticks - arguments.ticks // 10))
            report.figure(Statistic.extreme(
                "worst simulation tick", number(values, "simulation_us_worst"), "us"))
            report.figure(Statistic.stable(
                "median assembled frame", number(values, "frame_us_median"), "us"))
            if arguments.shot:
                render_shot(report, read_shot(shot_data), values, headline, Path(arguments.shot))
    except Absent as absent:
        print(f"\n    n/a   {absent}")
        return 3
    except Failed as failed:
        report.failed(str(failed))

    return report.summarise(Path(arguments.shot).parent if arguments.shot else None)


if __name__ == "__main__":
    sys.exit(main())
