#!/usr/bin/env python3
"""samples/09-multiplayer — M9's closing artefact. Section 6.

`just run-multiplayer` is the recipe; `integration.multiplayer_session` is the CTest entry; this
file is what both of them run. It drives `cy_sample_multiplayer` — five machines on one seeded,
lossy substrate — checks every claim that program printed, paints the two pictures from the
program's own trace, and reports through `samples/harness/artefact.py`, which is what makes a
recorded gap a non-zero exit and refuses an extreme value as the figure the run leads with.

--- THE FIVE ACTS, AND WHAT EACH ONE CLAIMS ------------------------------------------------------

  1. SESSION      four players, a host, and a network that loses, duplicates, delays and reorders.
                  The loss is INJECTED and counted; the host substitutes for the inputs that never
                  arrived; the clients predict, learn they were wrong and roll back. Three claims
                  come out of it: every client converges on the host's state hash, every tick is
                  ultimately simulated from authoritative input, and no explosion plays twice.
  2. REPLAY       the host's log replayed into a fresh world. The same LOG DIGEST and the same state
                  hash at every tick — not a tolerance — and the same again after seeking to a
                  checkpoint and fast-forwarding from it.
  3. DIVERGENCE   one field of one component on one entity perturbed deliberately, and the report
                  the engine produces: which tick, which entity, which component, which field, and
                  the window that reproduces it.
  4. CRASH        the bounded replay buffer flushed into a crash artefact, written to a file, read
                  back, and re-simulated to the hash the artefact itself carries.
  5. CONTROL      the negative control, and it is what makes act 1 a measurement. The same session
                  on a PERFECT network drops nothing and substitutes nothing; the same session run
                  twice at one seed produces byte-identical output. A lossy test whose loss could
                  not be shown to be doing anything is a lossy test that proves nothing.

--- WHY BOTH PICTURES ARE DIAGRAMS, AND SAY SO --------------------------------------------------

Neither picture here comes off a graphics device, because there is no renderer anywhere in this
artefact's link closure — the session is four clients and a host, and what is worth seeing is what
happened between them. So both images are DRAWINGS OF THE PROGRAM'S OWN OUTPUT and their banners say
so, in the shape `samples/08-vertical-slice`'s `--shot` uses: every number in them was produced in
C++ before this script saw it, and this script paints it.
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

PLAYERS = 4


# --- Running the program --------------------------------------------------------------------------


def run_sample(binary: Path, arguments: list[str]) -> dict[str, str]:
    """Run the program once and parse its `key = value` lines."""
    completed = subprocess.run(
        [str(binary), *arguments], cwd=str(ROOT), capture_output=True, text=True, timeout=1800,
    )
    if completed.returncode != 0:
        raise Failed(
            f"cy_sample_multiplayer exited {completed.returncode}\n"
            f"{completed.stdout[-4000:]}\n{completed.stderr[-4000:]}"
        )
    values: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        if " = " not in line:
            continue
        key, value = line.split(" = ", 1)
        values[key] = value
    values["_stdout"] = completed.stdout
    return values


def number(values: dict[str, str], key: str) -> float:
    raw = values.get(key)
    expect(raw is not None, f"the program printed no '{key}'")
    return float(str(raw))


def whole(values: dict[str, str], key: str) -> int:
    return int(number(values, key))


def each_client(values: dict[str, str], suffix: str) -> list[int]:
    return [whole(values, f"client{player}_{suffix}") for player in range(PLAYERS)]


def check(report: Report, name: str, ok: bool, detail: str) -> bool:
    """One claim, recorded either way. Returns whether it held, so a caller can branch."""
    if ok:
        report.did(name, detail)
    else:
        report.gap(name, detail)
    return ok


# --- Act 1: the four-player session ---------------------------------------------------------------


def act_session(report: Report, values: dict[str, str], ticks: int) -> None:
    print("\n--- act 1: four players, and a network that loses packets ---")

    # TASK 1.4b's STARTUP LINE, ASSERTED BY AN ARTEFACT. M8.c left the determinism firewall armed
    # and guarding nothing — `guarded_count()` was zero because nothing outside a test had ever
    # called `declare()`. This is the check that it is no longer zero in a program that plays a
    # game, and `underived` is asserted beside it because a firewall that guards a tenth of the
    # world and one that guards all of it read identically otherwise.
    check(report, "the determinism firewall is armed over a real component registry",
          whole(values, "firewall_armed") == 1 and whole(values, "firewall_guarded") > 0
          and whole(values, "firewall_underived") == 0
          and whole(values, "firewall_guarded") == whole(values, "firewall_components"),
          values.get("firewall_line", ""))

    dropped = whole(values, "session_datagrams_dropped")
    offered = whole(values, "session_datagrams_offered")
    substitutions = whole(values, "session_substitutions")
    check(report, "the network really did lose packets, and the host really did notice",
          dropped > 0 and substitutions > 0,
          f"{dropped} of {offered} datagrams destroyed, "
          f"{whole(values, 'session_datagrams_duplicated')} duplicated; the host reached its "
          f"deadline with nothing {substitutions} times and repeated the previous input")

    rollbacks = each_client(values, "rollbacks")
    resimulated = each_client(values, "resimulated")
    check(report, "all four clients predicted, were wrong, and rolled back",
          all(count > 0 for count in rollbacks) and all(count > 0 for count in resimulated),
          f"rollbacks per client {rollbacks}, ticks re-simulated {resimulated}, "
          f"mispredicted ticks {each_client(values, 'mispredicted')}")
    check(report, "no rollback was refused for want of a window",
          all(count == 0 for count in each_client(values, "refusals")),
          "every request fell inside the snapshot ring, so nothing was reported as requiring a "
          "full resynchronisation")

    # **THE CONVERGENCE CLAIM.** Two halves, and both are needed: a client that never received the
    # truth would agree with nothing, and a client that received it and did not reconcile would
    # disagree with the host.
    frontier = each_client(values, "frontier")
    unconfirmed = each_client(values, "first_unconfirmed")
    check(report, "every tick of the session ended up simulated from authoritative input",
          all(value == ticks for value in frontier) and all(value == ticks for value in unconfirmed),
          f"each client holds the host's inputs for all {ticks} ticks, and none of them ended the "
          "session still holding a prediction")
    check(report, "all four clients converge on the host's state hash",
          whole(values, "session_converged_clients") == PLAYERS,
          f"host {whole(values, 'session_host_hash')}, and "
          f"{whole(values, 'session_converged_clients')} of {PLAYERS} clients that reached it after "
          f"{sum(rollbacks)} rollbacks over {sum(resimulated)} re-simulated ticks")

    # **THE DUPLICATE-EFFECT CLAIM**, counted by the program itself rather than inferred from the
    # ledger's own arithmetic — see net.h. Both halves again: a ledger that suppressed EVERYTHING
    # would report zero duplicates too, so the effects actually played must be non-zero.
    offered_effects = each_client(values, "effects_offered")
    suppressed = each_client(values, "effects_suppressed")
    played = each_client(values, "explosions")
    check(report, "no explosion is played twice for the same entity at the same tick",
          whole(values, "session_duplicate_effects") == 0 and all(count > 0 for count in played)
          and all(count > 0 for count in suppressed),
          f"{sum(offered_effects)} effects offered through RollbackEngine::offer, "
          f"{sum(suppressed)} suppressed by the ledger, {sum(played)} played, "
          f"{whole(values, 'session_duplicate_effects')} played twice")

    check(report, "the session recorded one log and dropped nothing out of it",
          whole(values, "session_records") > 0 and whole(values, "session_records_dropped") == 0,
          f"{whole(values, 'session_records')} records over {ticks} ticks, "
          f"{whole(values, 'session_checkpoints')} checkpoints, none refused by the log")


# --- Act 2: the replay ----------------------------------------------------------------------------


def act_replay(report: Report, values: dict[str, str]) -> None:
    print("\n--- act 2: the same session, replayed ---")

    # THE SAME DIGEST, NOT A TOLERANCE. The log half matters as much as the state half: a replay
    # that reached the same world through a different record — four participants flattened onto one
    # producer, say — would satisfy a state-only comparison and desync against a peer the moment
    # anybody compared the two logs.
    check(report, "the replayed log is the recorded log, digest for digest",
          whole(values, "replay_log_hash_replayed") == whole(values, "replay_log_hash_recorded")
          and whole(values, "replay_records_replayed") == whole(values, "replay_records_recorded"),
          f"{whole(values, 'replay_records_recorded')} records recorded and "
          f"{whole(values, 'replay_records_replayed')} replayed; RecordLog::hash "
          f"{whole(values, 'replay_log_hash_recorded')} against "
          f"{whole(values, 'replay_log_hash_replayed')}")
    check(report, "the replay reproduces the state hash at every tick, not only at the end",
          whole(values, "replay_mismatched_ticks") == 0
          and whole(values, "replay_state_hash") == whole(values, "session_host_hash"),
          "0 ticks differ; a replay that diverged at tick 3 and converged again by the last tick "
          "would pass a final-hash-only comparison and this one would not")
    check(report, "the replay reproduced the recording's producer topology",
          whole(values, "replay_unbound_participants") == 0
          and whole(values, "replay_commands_produced") == whole(values, "replay_commands_expected"),
          f"{whole(values, 'replay_commands_produced')} commands through four producers in the "
          "recording's own order — which is what the log digest above is sensitive to")

    check(report, "and the same hash again after seeking to a checkpoint",
          whole(values, "seek_valid") == 1 and whole(values, "seek_has_checkpoint") == 1
          and whole(values, "seek_mismatched_ticks") == 0
          and whole(values, "seek_state_hash") == whole(values, "session_host_hash"),
          f"restored the checkpoint at tick {whole(values, 'seek_checkpoint_tick')} and "
          f"fast-forwarded to the end, with presentation suppressed "
          f"({whole(values, 'seek_presentation_suppressed')})")
    check(report, "the replay played the session's effects, once each",
          whole(values, "replay_explosions") == whole(values, "session_host_explosions"),
          f"{whole(values, 'replay_explosions')} explosions, against the host's "
          f"{whole(values, 'session_host_explosions')} — the effect records are in the log digest, "
          "so a replay that played a different number would fail the comparison above as well")


# --- Act 3: the injected divergence ---------------------------------------------------------------


def act_divergence(report: Report, values: dict[str, str]) -> None:
    print("\n--- act 3: a divergence, injected on purpose ---")

    if not check(report, "the injected divergence is detected",
                 whole(values, "divergence_detected") == 1,
                 f"first differing tick {values.get('divergence_first_tick')}, "
                 f"last agreeing {values.get('divergence_last_agreeing_tick')}, over "
                 f"{values.get('divergence_ticks_compared')} ticks compared"):
        return

    # **NARROWED, NOT MERELY DETECTED.** The entity, the component and the field, each by identity
    # and by name — and it must be the RIGHT one: a localiser that named the first thing it found
    # would satisfy "it named a field".
    check(report, "it is narrowed to one field of one component on one entity",
          whole(values, "divergence_narrowed") == 1
          and whole(values, "divergence_shape_mismatch") == 0
          and whole(values, "divergence_entity") == whole(values, "divergence_expected_entity")
          and values.get("divergence_component_name") == "Health"
          and values.get("divergence_field_name") == "shield"
          and whole(values, "divergence_left") != whole(values, "divergence_right"),
          values.get("divergence_line", ""))
    check(report, "and the window that reproduces it",
          whole(values, "divergence_window_valid") == 1
          and whole(values, "divergence_window_commands") > 0
          and whole(values, "divergence_window_records") > 0,
          f"the last agreeing checkpoint is tick "
          f"{whole(values, 'divergence_window_checkpoint')}, with "
          f"{whole(values, 'divergence_window_commands')} commands and the session seed "
          f"{whole(values, 'divergence_window_seed')} — the random trace is derivable from the "
          "seed and is deliberately not recorded")


# --- Act 4: the crash artefact --------------------------------------------------------------------


def act_crash(report: Report, values: dict[str, str], artefact: Path) -> None:
    print("\n--- act 4: a crash artefact that reproduces the crash ---")

    check(report, "the replay buffer is bounded, and the artefact says what it lost",
          whole(values, "session_crash_ring_size") <= whole(values, "session_crash_ring_capacity")
          and whole(values, "crash_records_lost") == whole(values,
                                                           "session_crash_ring_overwritten"),
          f"a {whole(values, 'session_crash_ring_capacity')}-record ring over a session that "
          f"produced {whole(values, 'session_records')}; the artefact covers ticks "
          f"{whole(values, 'crash_first_tick')} to {whole(values, 'crash_last_tick')} and states "
          f"the {whole(values, 'crash_records_lost')} records the ring overwrote")
    check(report, "the artefact carries the divergence report beside the records",
          whole(values, "crash_has_divergence") == 1,
          f"trigger {values.get('crash_trigger')}, {whole(values, 'crash_bytes')} bytes at "
          f"{artefact}")
    check(report, "its window starts at a checkpoint inside the window, not at its ragged edge",
          whole(values, "crash_checkpoint_inside") == 1,
          f"checkpoint at tick {whole(values, 'crash_checkpoint_tick')}, window from "
          f"{whole(values, 'crash_first_tick')}")

    # **THE CLAIM.** Not "the file parsed" — the final seconds of the session happened again.
    check(report, "the crash artefact re-simulates to the hash it carries",
          whole(values, "crash_reproduced") == 1
          and whole(values, "crash_unbound_participants") == 0,
          f"{whole(values, 'crash_commands_replayed')} commands replayed from the recovered log "
          f"and the world reached {whole(values, 'crash_reproduced_hash')}, which is the last "
          "state hash the artefact itself records")

    check(report, f"{artefact.name} was written", artefact.is_file() and artefact.stat().st_size > 0,
          f"{artefact.stat().st_size if artefact.is_file() else 0} bytes")


# --- Act 5: the controls --------------------------------------------------------------------------


def act_control(report: Report, binary: Path, ticks: int, seed: int, lossy: dict[str, str]) -> None:
    print("\n--- act 5: the controls ---")

    # THE NEGATIVE CONTROL FOR THE LOSS. If a perfect network produced the same numbers, act 1 would
    # be measuring something other than what it claims to.
    clean = run_sample(binary, ["--ticks", str(ticks), "--seed", str(seed), "--loss", "0",
                                "--latency", "0", "--only", "session"])
    check(report, "a perfect network destroys nothing and the host substitutes nothing",
          whole(clean, "session_datagrams_dropped") == 0
          and whole(clean, "session_substitutions") == 0
          and whole(lossy, "session_datagrams_dropped") > 0
          and whole(lossy, "session_substitutions") > 0,
          f"clean: {whole(clean, 'session_datagrams_dropped')} dropped, "
          f"{whole(clean, 'session_substitutions')} substituted. Lossy: "
          f"{whole(lossy, 'session_datagrams_dropped')} dropped, "
          f"{whole(lossy, 'session_substitutions')} substituted — so the adverse conditions in "
          "act 1 are doing the work the act says they are")
    check(report, "and it still converges, so convergence is not an artefact of the loss",
          whole(clean, "session_converged_clients") == PLAYERS,
          f"{whole(clean, 'session_converged_clients')} clients converged on a clean network as "
          f"well as {whole(lossy, 'session_converged_clients')} on a lossy one")

    # THE SEED. A lossy test that is not reproducible is a lossy test nobody can act on, and this is
    # the milestone whose subject is reproducibility.
    again = run_sample(binary, ["--ticks", str(ticks), "--seed", str(seed)])
    differing = [key for key, value in lossy.items()
                 if key != "_stdout" and again.get(key) != value]
    check(report, "two runs at one seed lose the same datagrams and reach the same hashes",
          not differing,
          f"{len(lossy) - 1} figures compared, none differ — including "
          f"{whole(lossy, 'session_datagrams_dropped')} dropped datagrams and every state hash"
          if not differing else f"{len(differing)} figure(s) differ: {differing[:8]}")


# --- The pictures ---------------------------------------------------------------------------------
#
# Both are DIAGRAMS and their banners say so. Every number in them was computed in C++ and written
# to the trace files before this script opened them.

INK = {
    "paper": (0x0E, 0x10, 0x12),
    "panel": (0x16, 0x19, 0x1C),
    "rule": (0x2A, 0x2F, 0x34),
    "primary": (0xE6, 0xE9, 0xEC),
    "secondary": (0xA2, 0xAB, 0xB4),
    "loss": (0xF0, 0x91, 0x3A),
    "bad": (0xC0, 0x45, 0x3A),
    "good": (0x35, 0xC0, 0x7C),
    "focus": (0x4C, 0x9A, 0xFF),
}

PLAYER_INK = [(0x4C, 0x9A, 0xFF), (0x35, 0xC0, 0x7C), (0xF0, 0x91, 0x3A), (0xC7, 0x6B, 0xD9)]


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


def read_trace(path: Path) -> dict:
    """Parse the program's own trace. One `tick`, `client` or `network` record per line."""
    ticks: list[dict] = []
    clients: list[dict] = []
    network = {"offered": 0, "dropped": 0, "duplicated": 0, "substituted": 0}
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "tick":
            ticks.append({
                "tick": int(fields[1]),
                "x": [float(fields[2 + player * 2]) for player in range(PLAYERS)],
                "y": [float(fields[3 + player * 2]) for player in range(PLAYERS)],
                "substituted": int(fields[10]),
                "rollbacks": [int(fields[12 + player * 2]) for player in range(PLAYERS)],
                "depth": [int(fields[13 + player * 2]) for player in range(PLAYERS)],
            })
        elif fields[0] == "client":
            clients.append({
                "index": int(fields[1]), "rollbacks": int(fields[2]),
                "resimulated": int(fields[3]), "explosions": int(fields[4]),
                "duplicates": int(fields[5]), "converged": int(fields[6]),
            })
        elif fields[0] == "network":
            network = {"offered": int(fields[1]), "dropped": int(fields[2]),
                       "duplicated": int(fields[3]), "substituted": int(fields[4])}
    return {"ticks": ticks, "clients": clients, "network": network}


def _paint_positions(draw, ticks, across, left, right, label, small) -> None:
    """Where the four players are, y against tick, host-authoritative."""
    top, bottom = 118, 430
    draw.rectangle([left, top, right, bottom], outline=INK["rule"])
    lowest = min(min(sample["y"]) for sample in ticks)
    highest = max(max(sample["y"]) for sample in ticks)
    extent = max(1.0, highest - lowest)
    for player in range(PLAYERS):
        points = [(across(index), bottom - (bottom - top) * (sample["y"][player] - lowest) / extent)
                  for index, sample in enumerate(ticks)]
        draw.line(points, fill=PLAYER_INK[player], width=2)
    draw.text((left, top - 24), "where the four players are — y against tick, host-authoritative",
              INK["secondary"], font=small)
    for player in range(PLAYERS):
        swatch = left + 620 + player * 130
        draw.line([(swatch, top - 17), (swatch + 20, top - 17)], fill=PLAYER_INK[player], width=3)
        draw.text((swatch + 26, top - 25), f"player {player}", PLAYER_INK[player], font=small)


def _paint_reconciliation(draw, ticks, across, left, right, bottom, small) -> None:
    """THE PLOT WORTH LOOKING AT: how far back each client had to go, tick by tick. One bar is one
    rollback and its height is the number of ticks re-simulated."""
    top = 486
    draw.rectangle([left, top, right, bottom], outline=INK["rule"])
    rows = (bottom - top) / PLAYERS
    deepest = max(1, max(max(sample["depth"]) for sample in ticks))
    for player in range(PLAYERS):
        base = top + rows * (player + 1)
        draw.line([(left, base), (right, base)], fill=INK["rule"])
        for index, sample in enumerate(ticks):
            if sample["depth"][player] == 0:
                continue
            x = across(index)
            draw.line([(x, base), (x, base - (rows - 4) * sample["depth"][player] / deepest)],
                      fill=PLAYER_INK[player], width=2)
        draw.text((left + 6, top + rows * player + 4),
                  f"player {player} — ticks re-simulated, deepest {deepest}", PLAYER_INK[player],
                  font=small)
    draw.text((left, top - 24), "how far back each client had to go — one bar is one rollback",
              INK["secondary"], font=small)


def _paint_loss(draw, ticks, across, left, bottom, small) -> None:
    """Every tick an input never arrived for, on its own row under both plots."""
    for index, sample in enumerate(ticks):
        if sample["substituted"] == 0:
            continue
        x = across(index)
        for player in range(PLAYERS):
            if sample["substituted"] & (1 << player):
                y = bottom + 10 + player * 7
                draw.line([(x, y), (x, y + 5)], fill=INK["loss"], width=2)
    draw.text((left, bottom + 44),
              "orange: a player's input never arrived and the host repeated the previous one",
              INK["loss"], font=small)


def _paint_panel(draw, trace, panel_left, panel_right, panel_bottom, label, small) -> None:
    """What the session amounted to, per client and over the network."""
    network = trace["network"]
    draw.rectangle([panel_left, 118, panel_right, panel_bottom], fill=INK["panel"],
                   outline=INK["rule"])
    lines = [
        ("the network", INK["primary"]),
        (f"{network['offered']} datagrams offered", INK["secondary"]),
        (f"{network['dropped']} destroyed", INK["loss"]),
        (f"{network['duplicated']} duplicated", INK["secondary"]),
        (f"{network['substituted']} inputs substituted", INK["loss"]),
        ("", INK["primary"]),
        ("each client", INK["primary"]),
    ]
    y = 134
    for text, colour in lines:
        draw.text((panel_left + 16, y), text, colour, font=label)
        y += 24
    for client in trace["clients"]:
        colour = INK["good"] if client["converged"] else INK["bad"]
        draw.text((panel_left + 16, y), f"player {client['index']}", PLAYER_INK[client["index"]],
                  font=label)
        draw.text((panel_left + 28, y + 20),
                  f"{client['rollbacks']} rollbacks, {client['resimulated']} ticks",
                  INK["secondary"], font=small)
        draw.text((panel_left + 28, y + 38),
                  f"{client['explosions']} effects, {client['duplicates']} played twice",
                  INK["secondary"], font=small)
        draw.text((panel_left + 28, y + 56),
                  "converged on the host" if client["converged"] else "DID NOT CONVERGE", colour,
                  font=small)
        y += 82


def paint_session(trace: dict, destination: Path) -> None:
    """The session, drawn from its own trace: four players, and every substituted input marked."""
    from PIL import Image, ImageDraw

    width, height = 1600, 900
    image = Image.new("RGB", (width, height), INK["paper"])
    draw = ImageDraw.Draw(image)
    title = _monospace(26)
    label = _monospace(16)
    small = _monospace(13)

    draw.text((28, 22), "M9 — a four-player session with rollback under packet loss", INK["primary"],
              font=title)
    draw.text((28, 58),
              "A DIAGRAM drawn from the run's own trace: every position, every substituted input "
              "and every count below was computed in C++ and written to session.txt before this "
              "picture existed.", INK["secondary"], font=small)

    ticks = trace["ticks"]
    if not ticks:
        image.save(destination)
        return

    left, right, bottom = 80, width - 360, height - 190
    span = max(1, len(ticks) - 1)

    def across(index: int) -> float:
        return left + (right - left) * index / span

    _paint_positions(draw, ticks, across, left, right, label, small)
    _paint_reconciliation(draw, ticks, across, left, right, bottom, small)
    _paint_loss(draw, ticks, across, left, bottom, small)
    _paint_panel(draw, trace, right + 24, width - 28, bottom, label, small)

    draw.text((28, height - 96),
              "Each client predicts its peers' inputs, learns from the host that it was wrong, and "
              "rolls back —", INK["secondary"], font=label)
    draw.text((28, height - 72),
              "re-simulating through the present without playing any effect a second time.",
              INK["secondary"], font=label)
    draw.text((28, height - 40), "samples/09-multiplayer · cy_sample_multiplayer + multiplayer.py",
              INK["rule"], font=small)
    destination.parent.mkdir(parents=True, exist_ok=True)
    image.save(destination)


def read_divergence(path: Path) -> dict:
    found: dict[str, list[str]] = {}
    for line in path.read_text().splitlines():
        fields = line.split(maxsplit=1)
        if len(fields) == 2:
            found[fields[0]] = fields[1].split()
    return found


def paint_divergence(found: dict, destination: Path) -> None:
    """The narrowing, drawn as the descent it actually is: world, archetype, entity, field."""
    from PIL import Image, ImageDraw

    width, height = 1600, 820
    image = Image.new("RGB", (width, height), INK["paper"])
    draw = ImageDraw.Draw(image)
    title = _monospace(26)
    label = _monospace(18)
    small = _monospace(14)
    mono = _monospace(15)

    draw.text((28, 22), "M9 — an injected divergence, narrowed to a field", INK["primary"],
              font=title)
    draw.text((28, 58),
              "A DIAGRAM of one report the engine produced: every identifier, name and value below "
              "was printed by cy_sample_multiplayer and is reproduced here unchanged.",
              INK["secondary"], font=small)

    tick = found.get("tick", ["?"])[0]
    agreeing = found.get("agreeing", ["?"])[0]
    checkpoint = found.get("checkpoint", ["?"])[0]
    entity = found.get("entity", ["?"])[0]
    component = found.get("component", ["?", "?"])
    field = found.get("field", ["?", "?"])
    values = found.get("values", ["?", "?"])
    commands = found.get("commands", ["?"])[0]
    seed = found.get("seed", ["?"])[0]

    # The descent. Four boxes, because four levels is what the hierarchical hash actually has and
    # the point of the picture is that the search is a descent rather than a scan.
    steps = [
        ("state hash tree", "the two runs' roots differ", f"tick {tick}"),
        (f"archetype {component[1]}", f"subject {component[0]}", "one of two archetypes"),
        (f"entity {entity}", "one of four", "the other three agree"),
        (f"field {field[1]}", f"id {field[0]}", "the other field agrees"),
    ]
    box_width, box_height, gap = 330, 150, 42
    left = 60
    for index, (heading, first, second) in enumerate(steps):
        x = left + index * (box_width + gap)
        y = 140
        focus = index == len(steps) - 1
        draw.rectangle([x, y, x + box_width, y + box_height],
                       fill=INK["panel"], outline=INK["focus"] if focus else INK["rule"], width=2)
        draw.text((x + 18, y + 20), heading, INK["focus"] if focus else INK["primary"], font=label)
        draw.text((x + 18, y + 58), first, INK["secondary"], font=small)
        draw.text((x + 18, y + 84), second, INK["secondary"], font=small)
        if index + 1 < len(steps):
            arrow_y = y + box_height / 2
            draw.line([(x + box_width + 8, arrow_y), (x + box_width + gap - 8, arrow_y)],
                      fill=INK["rule"], width=2)
            draw.polygon([(x + box_width + gap - 8, arrow_y), (x + box_width + gap - 18, arrow_y - 6),
                          (x + box_width + gap - 18, arrow_y + 6)], fill=INK["rule"])

    draw.rectangle([60, 330, width - 60, 470], fill=INK["panel"], outline=INK["rule"])
    draw.text((80, 350), "the two hashes at that field", INK["primary"], font=label)
    draw.text((80, 386), f"left  {values[0]}", INK["good"], font=mono)
    draw.text((80, 412), f"right {values[1]}", INK["bad"], font=mono)
    draw.text((80, 440), "one run's shield was moved by 7 and nothing else was touched",
              INK["secondary"], font=small)

    draw.rectangle([60, 496, width - 60, 636], fill=INK["panel"], outline=INK["rule"])
    draw.text((80, 516), "the window that reproduces it", INK["primary"], font=label)
    draw.text((80, 552), f"last agreeing tick {agreeing}   ·   checkpoint at tick {checkpoint}"
                         f"   ·   {commands} commands in between", INK["secondary"], font=mono)
    draw.text((80, 580), f"session seed {seed}", INK["secondary"], font=mono)
    draw.text((80, 606),
              "the random trace is a pure function of the seed, so it is carried rather than "
              "recorded", INK["secondary"], font=small)

    line = " ".join(found.get("line", []))
    draw.text((60, 676), "the line a person reads:", INK["primary"], font=label)
    draw.text((60, 706), line[:150], INK["secondary"], font=mono)
    if len(line) > 150:
        draw.text((60, 730), line[150:300], INK["secondary"], font=mono)
    draw.text((60, height - 40), "samples/09-multiplayer · cy_sample_multiplayer + multiplayer.py",
              INK["rule"], font=small)
    destination.parent.mkdir(parents=True, exist_ok=True)
    image.save(destination)


# --- The run ---------------------------------------------------------------------------------------


def parse(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="M9's closing artefact: a four-player session.")
    parser.add_argument("--sample", type=Path, help="the built cy_sample_multiplayer")
    parser.add_argument("--build-dir", type=Path, default=None)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--work", type=Path, default=None, help="where the run's files go")
    parser.add_argument("--ticks", type=int, default=180)
    parser.add_argument("--seed", type=int, default=0x0910C0FF)
    parser.add_argument("--only", default="all",
                        choices=("all", "session", "replay", "divergence", "crash", "control"))
    parser.add_argument("--shots", type=Path, default=None,
                        help="where the two committed pictures are written")
    return parser.parse_args(argv)


def locate(options: argparse.Namespace) -> Path:
    if options.sample is not None:
        binary = options.sample
    else:
        build = options.build_dir or (ROOT / "build" / options.profile)
        binary = build / "samples" / "09-multiplayer" / "cy_sample_multiplayer"
    if not binary.is_file():
        raise Absent(
            f"no cy_sample_multiplayer at {binary}. It is not declared with -D CY_NETWORKING=OFF, "
            "which is the configuration in which this artefact has no subject."
        )
    return binary


def wanted(options: argparse.Namespace, act: str) -> bool:
    return options.only in ("all", act)


def main(argv: list[str]) -> int:
    options = parse(argv)
    report = Report()
    try:
        binary = locate(options)
        work = options.work or (ROOT / "build" / options.profile / "multiplayer")
        work.mkdir(parents=True, exist_ok=True)
        trace_path = work / "session.txt"
        divergence_path = work / "divergence.txt"
        crash_path = work / "session.cycrash"

        values = run_sample(binary, [
            "--ticks", str(options.ticks), "--seed", str(options.seed),
            "--trace", str(trace_path), "--divergence", str(divergence_path),
            "--crash", str(crash_path),
        ])

        if wanted(options, "session"):
            act_session(report, values, options.ticks)
        if wanted(options, "replay"):
            act_replay(report, values)
        if wanted(options, "divergence"):
            act_divergence(report, values)
        if wanted(options, "crash"):
            act_crash(report, values, crash_path)
        if wanted(options, "control"):
            act_control(report, binary, options.ticks, options.seed, values)

        if options.shots is not None:
            trace = read_trace(trace_path)
            session_shot = options.shots / "m9-multiplayer-session.png"
            paint_session(trace, session_shot)
            report.shot(session_shot)
            divergence_shot = options.shots / "m9-divergence-narrowed.png"
            paint_divergence(read_divergence(divergence_path), divergence_shot)
            report.shot(divergence_shot)

        # THE HEADLINE IS A TOTAL, WHICH IS A FIGURE THAT REPRODUCES. `artefact.Report.headline`
        # refuses a maximum by name, and the seeded network is what makes even this one exact.
        report.figure(Statistic.stable("dropped datagrams",
                                       whole(values, "session_datagrams_dropped"), "datagrams"))
        report.figure(Statistic.stable("substituted inputs",
                                       whole(values, "session_substitutions"), "inputs"))
        report.headline(Statistic.stable(
            "total ticks re-simulated by four clients under packet loss",
            sum(each_client(values, "resimulated")), "ticks"))
    except Absent as absent:
        print(f"\n    n/a   {absent}")
        return 3
    except Failed as failure:
        report.failed(str(failure))
    return report.summarise(options.shots)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
