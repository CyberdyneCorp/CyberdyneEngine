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
  5. CAPTURE M8.c's, and it needs a graphics device, so it runs only when `--capture` names a
             prefix. The frame is RECORDED — the pipeline layer's five callbacks and the particle
             renderer's extension — executed on Vulkan with validation on, and read back as a PNG,
             together with the identical frame recorded with an EMPTY `FrameSinks`. The difference
             between those two images is the whole of M8.c section 1b, and it is measured in texels
             rather than described.

--- WHAT M8.c ADDED TO ACTS 1 AND 3 --------------------------------------------------------------

Act 1 gained the particles and the cut: effects played from the activation pipeline's own cues, the
scheduler's merged dispatches against its unmerged ones, the CPU fallback reported as a number
rather than a silence, and a cinematic that drives cameras through `cy::camera`'s stack and writes
NO camera transform — a measured zero, not a promise.

Act 3 gained the control that makes task 5.3 mean something: the same options run WITH the two new
systems and with `--no-spectacle`, requiring the identical state digest. A particle system or a
sequence that reached gameplay state would move it, and the ECS write firewall would refuse the
write before it could.

--- WHY ONE PICTURE IS DRAWN AND THE OTHERS ARE CAPTURED -----------------------------------------

The picture `--shot` writes is A DIAGRAM and its banner says so: every shape in it is one item of
the frame's SORTED DRAW LIST, its bounds are the spatial index's, its silhouette is the mesh its
`MeshRenderer` reference resolved to, and its corners were projected by `projection * view` in C++
before this script saw them — but this script paints it, so it is a drawing of the frame's answer
rather than the frame.

`--capture` is the other kind and is what M8.c commits. Those images come off a graphics device.
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

    # M8.c: THE PARTICLES, played from the activation pipeline's own cues. This is the seam
    # M8.b's README named and left open, used rather than described.
    check(report, "particles are played by the game's own activation cues",
          whole(values, "effects_played") > 0 and whole(values, "vfx_peak_particles") > 0
          and whole(values, "vfx_spawned") > 0 and whole(values, "vfx_killed") > 0,
          f"{whole(values, 'effects_played')} effects played from committed activations "
          f"({whole(values, 'effects_refused')} refused at the world's instance ceiling), "
          f"{whole(values, 'vfx_peak_particles')} live particles at the peak, "
          f"{whole(values, 'vfx_spawned')} spawned and {whole(values, 'vfx_killed')} killed over "
          f"the run, {whole(values, 'vfx_substeps')} simulation sub-steps")
    # THE SCHEDULER'S OWN REQUIREMENT, as the two numbers `vfx-system` asks for by name: "400
    # instances of the same explosion effect SHALL be simulated by a small number of merged
    # dispatches, not 400 separate ones".
    merged = whole(values, "vfx_dispatches_merged")
    unmerged = whole(values, "vfx_dispatches_unmerged")
    check(report, "the global scheduler merges dispatches over many copies of one effect",
          merged > 0 and merged < unmerged,
          f"{merged} merged dispatches against {unmerged} unmerged over the run — a factor of "
          f"{(unmerged / merged) if merged else 0:.1f}, from grouping emitters that share a "
          "compiled kernel and a compatible layout")
    # A FALLBACK IS A NUMBER, NEVER A SILENCE. `vfx-system` requires the CPU path to declare
    # itself; this is where a reader of the artefact finds out which path ran.
    check(report, "the simulation path each emitter took is reported rather than assumed",
          whole(values, "vfx_gpu_emitters") + whole(values, "vfx_cpu_emitters") > 0,
          f"{whole(values, 'vfx_gpu_emitters')} emitter-steps on the GPU path, "
          f"{whole(values, 'vfx_cpu_emitters')} on the CPU path, of which "
          f"{whole(values, 'vfx_cpu_fallbacks')} were declared FALLBACKS — the number "
          "`vfx-system` requires instead of a silent degradation")
    check(report, "every live particle reaches the renderer's ring",
          whole(values, "vfx_published") > 0 and whole(values, "vfx_dropped") == 0,
          f"{whole(values, 'vfx_published')} records published camera-relative through "
          f"publish_sprites, {whole(values, 'vfx_dropped')} dropped for want of ring capacity, "
          f"{whole(values, 'vfx_pool_used_bytes')} of "
          f"{whole(values, 'vfx_pool_total_bytes')} pool bytes in use")

    # M8.c: THE CUT. Two shots, a blend, and the zero that task 5.2 is about.
    check(report, "a compiled sequence runs inside the game and cuts between two shots",
          whole(values, "cut_ran") == 1 and whole(values, "cut_frames") > 0
          and whole(values, "cut_cuts") > 0,
          f"{whole(values, 'cut_segments')} segments and "
          f"{whole(values, 'cut_channels')} channels compiled; "
          f"{whole(values, 'cut_frames')} frames driven; "
          f"{whole(values, 'cut_pushed')} contributions pushed and "
          f"{whole(values, 'cut_released')} released; "
          f"{whole(values, 'cut_cuts')} cut(s), "
          f"{whole(values, 'cut_anticipated_cuts')} of them announced ahead of themselves so the "
          "camera's streaming source can prefetch")
    # THE BLEND IS VISIBLE RATHER THAN ASSERTED, and the lens is what says so: the two shots
    # declare different ones, so a blend that did nothing would read as one of the two ends.
    wide_fov = number(values, "cut_wide_fov")
    tight_fov = number(values, "cut_tight_fov")
    blend_fov = number(values, "cut_blend_fov")
    check(report, "the camera stack BLENDS the two shots rather than switching between them",
          whole(values, "cut_blend_frames") > 0
          and number(values, "cut_blend_wide") > 0.01
          and number(values, "cut_blend_tight") > 0.01
          and tight_fov < blend_fov < wide_fov,
          f"{whole(values, 'cut_blend_frames')} frames with both shots contributing; at the most "
          f"balanced of them the wide shot weighs {number(values, 'cut_blend_wide'):.2f} and the "
          f"long lens {number(values, 'cut_blend_tight'):.2f}, and the lens the stack produced is "
          f"{blend_fov:.3f} rad — strictly between the two rigs' own {tight_fov:.2f} and "
          f"{wide_fov:.2f}, which a switch could not be")
    # TASK 5.2's ZERO. `sequencing-and-cinematics` requires that a sequence not write camera
    # transforms; the sequence selects RIGS and the camera server blends the poses they produce.
    # Both counters would move the moment something wrote one.
    check(report, "the cut writes no camera transform",
          whole(values, "cut_camera_property_writes") == 0
          and whole(values, "cut_pose_overrides") == 0
          and whole(values, "cut_unresolved_rigs") == 0,
          # THE NUMBERS RATHER THAN THE WORD "zero", and this line was written the other way
          # first: a mutation that made the counter count the LIGHT track's writes instead of the
          # camera's turned the check red while its message still read "0 arbitrated writes",
          # because the zeros were literals. A failing check that reports the value it wanted is
          # the shape of a check nobody can debug.
          f"{whole(values, 'cut_camera_property_writes')} arbitrated writes addressed at "
          f"SubsystemId::Camera and {whole(values, 'cut_pose_overrides')} pose overrides over "
          f"{whole(values, 'cut_frames')} frames — the sequence selected rigs, CameraStackBridge "
          "pushed them, and cy::camera::CameraServer::evaluate_stack blended the poses its own "
          f"rigs produced. {whole(values, 'cut_unresolved_rigs')} rigs went unresolved")

    # AND THE COST. Task 5.1 asks for particles and a cut inside the slice HOLDING THE FRAME BUDGET
    # IT ALREADY DECLARES — so the check is the sum against M8.b's own number, unchanged by this
    # milestone, rather than a new budget written to fit what was measured.
    spectacle = number(values, "spectacle_us_median")
    simulation = number(values, "simulation_us_median")
    check(report, "the particles and the cut fit inside the budget the slice already declared",
          spectacle <= number(values, "budget_spectacle_us")
          and simulation + spectacle <= number(values, "budget_simulation_us"),
          f"{spectacle:.1f} us a tick — particles "
          f"{number(values, 'particles_us_median'):.1f} us, cinematic "
          f"{number(values, 'cinematic_us_median'):.1f} us — against a declared "
          f"{number(values, 'budget_spectacle_us'):.0f} us, and "
          f"{simulation + spectacle:.0f} us of simulation and spectacle together against the "
          f"{number(values, 'budget_simulation_us'):.0f} us M8.b declared and M8.c did not raise")

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


    # M8.c TASK 5.3, AND IT IS THE FIREWALL'S PROOF FROM THE ARTEFACT'S SIDE. The same options, run
    # once with the particles and the cut live and once with `--no-spectacle`, must fold the
    # IDENTICAL digest. A particle system that wrote a health value, or a sequence that wrote a
    # component instead of issuing a command, would move it — and `cy/ecs/firewall.h`'s
    # `WriteOrigin::Vfx` scope would refuse the write before it got that far.
    #
    # This check can fail, and the way to prove it is to fold anything the spectacle touches into
    # the digest: `fold(spectacle_->report().vfx_live_particles)` in `Slice::fold_tick` turns it red
    # immediately, which is what was done while it was written.
    without = run_sample(
        binary, ["--agents", str(agents), "--ticks", str(ticks), "--no-spectacle"])
    check(report, "the digest is the same with the particles and the cut live and without them",
          without["state_digest"] == within["state_digest"]
          and whole(without, "effects_played") == 0 and whole(within, "effects_played") > 0,
          f"{within['state_digest']} with {whole(within, 'effects_played')} effects played and a "
          f"cinematic driving the camera; {without['state_digest']} with neither. VFX writes only "
          "its own pool and its own event router, and a sequence is a command producer — neither "
          "can reach a component, and the ECS write firewall is what makes that a refusal rather "
          "than a convention")


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


# --- Act 5: the frame, captured -------------------------------------------------------------------


def act_capture(report: Report, binary: Path, agents: int, ticks: int, prefix: Path) -> None:
    """M8.c tasks 5.4, 5.5 and 5.6. Needs a graphics device; the caller decides whether to run it."""
    print("\n--- act 5: the frame, captured ---")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    values = run_sample(
        binary,
        ["--agents", str(agents), "--ticks", str(ticks), "--capture", str(prefix)],
    )

    check(report, "the frame is RECORDED on a graphics device, not merely assembled",
          whole(values, "capture_device") == 1 and whole(values, "capture_captured") == 1
          and whole(values, "capture_passes") > 0 and whole(values, "capture_opaque_draws") > 0,
          f"{whole(values, 'capture_passes')} stages recorded — "
          f"{whole(values, 'capture_prepass_draws')} depth-prepass draws, "
          f"{whole(values, 'capture_opaque_draws')} opaque, "
          f"{whole(values, 'capture_transparent_draws')} transparent, "
          f"{whole(values, 'capture_skipped_draws')} skipped for want of geometry, "
          f"{whole(values, 'capture_uploaded_bytes')} bytes moved by the Prepare transfer")
    check(report, "the particle renderer draws the game's own effects inside that frame",
          whole(values, "capture_extensions_run") > 0
          and whole(values, "capture_particles_drawn") > 0
          and whole(values, "capture_particles_dropped") == 0,
          f"{whole(values, 'capture_extensions_run')} pass extension(s) ran and drew "
          f"{whole(values, 'capture_particles_drawn')} sprite instances in the transparent stage — "
          "the effect's own particles plus the readout, through one 32-byte record type")
    # VALIDATION IS A NUMBER HERE. The pipeline layer's own suite found three distinct validation
    # defects that no structural test could see; a capture that ran clean is worth saying so.
    check(report, "the recorded frame produces no Vulkan validation error",
          whole(values, "capture_validation_errors") == 0,
          "0 errors from the validation layers over both captures, with validation enabled")

    # TASK 5.5's BEFORE-AND-AFTER PAIR, AS A MEASUREMENT. The control frame is the identical
    # assemble-compile-barrier-execute with an EMPTY `FrameSinks` — what every caller in this tree
    # supplied before M8.c — so the difference between the two images is the record callbacks and
    # nothing else.
    lit = whole(values, "capture_lit_texels")
    control_lit = whole(values, "control_lit_texels")
    differing = whole(values, "capture_differing_texels")
    check(report, "the record callbacks are the whole difference between two identical frames",
          whole(values, "control_device") == 1 and whole(values, "control_passes") == 0
          and control_lit == 0 and lit > 0 and differing > 0,
          f"the recorded frame lights {lit} texels and the same frame with no callbacks lights "
          f"{control_lit}; {differing} texels differ by more than one 8-bit step. The control ran "
          f"{whole(values, 'control_passes')} record callbacks and "
          f"{whole(values, 'control_opaque_draws')} draws")

    for suffix in ("", "-no-callbacks", "-cut"):
        image = prefix.parent / f"{prefix.name}{suffix}.png"
        check(report, f"{image.name} came off the device",
              image.is_file() and image.stat().st_size > 0,
              f"{image.stat().st_size if image.is_file() else 0} bytes at {image}")
        if image.is_file():
            report.shot(image)


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
    # LABELLED A DIAGRAM, which is what M8.c task 5.6 requires of an image that is not the engine's
    # own output. Every shape below is the frame's own answer — the sorted draw list, the spatial
    # index's bounds, the mesh handle the reference resolved to — but this script paints it, so it
    # is a drawing OF the frame rather than the frame. `--capture` writes the photographs.
    draw.text((152, 15), "M8.c — samples/08-vertical-slice · DIAGRAM: the frame's draw list, "
                         "projected", font=font, fill=INK["secondary"])
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
        f"vfx {values.get('effects_played', '0')} effects from activation cues · "
        f"{values.get('vfx_peak_particles', '0')} particles at the peak · "
        f"{values.get('vfx_dispatches_merged', '0')} merged dispatches of "
        f"{values.get('vfx_dispatches_unmerged', '0')} · "
        f"{values.get('vfx_cpu_fallbacks', '0')} declared CPU fallbacks",
        f"cut {values.get('cut_frames', '0')} frames · "
        f"{values.get('cut_blend_frames', '0')} mid-blend · "
        f"{values.get('cut_camera_property_writes', '0')} camera transforms written · "
        f"{values.get('cut_pose_overrides', '0')} pose overrides",
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
    parser.add_argument("--capture",
                        help="M8.c act 5: RECORD the frame on a graphics device and write "
                             "<prefix>.png, <prefix>-no-callbacks.png and <prefix>-cut.png. "
                             "Needs Vulkan; the act does not run without this flag")
    parser.add_argument("--only", choices=("play", "scale", "replay", "control", "capture"),
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
        # ACT 5 IS OPT-IN BECAUSE IT NEEDS A GRAPHICS DEVICE. Every other act is judgeable on a
        # machine with neither a device nor a display, which is what keeps `smoke.vertical_slice`
        # runnable on a hosted runner; the ledger criterion that runs this one carries
        # `requires = "gpu"` and is reported NOT EVALUATED where there is none.
        if arguments.capture and wanted in (None, "capture"):
            act_capture(report, binary, arguments.agents, arguments.ticks,
                        Path(arguments.capture))
        elif wanted == "capture":
            raise Absent("act 5 needs --capture <prefix> and a Vulkan device")

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
