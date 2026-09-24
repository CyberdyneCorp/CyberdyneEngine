#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run one compile or link inside the machine's job budget: `job_slot.py --slots N -- <command...>`.

THE GUARANTEE IS MACHINE-WIDE. A per-build `-j` of cores-2 leaves two cores free only while ONE
build runs. The milestone ledger evaluates up to eight criteria at once, the build matrix builds
several trees at once, a gate runs beside a close, and four agents build four trees: each of those
builds would take cores-2 and between them they saturate the machine. Ninja 1.11 cannot share a
jobserver between builds, so the cap is enforced one level down, where every build has to pass —
whichever recipe, script or developer started it. cmake/launchers.cmake puts this script in front of
every compile (as ccache's `prefix_command`, so a cache HIT takes no slot) and every link.

THE POOL IS A DIRECTORY OF LOCK FILES, `slot-0` .. `slot-<N-1>`, under
`/tmp/cyberdyne-job-slots-<uid>` — a fixed path on purpose: `TMPDIR` differs between sessions, and
two pools are no cap at all. A job holds one `flock(2)` on one slot file for as long as its command
runs. The KERNEL releases the lock when this process exits, however it exits, so a crash or a
`kill -9` leaks nothing and there is no daemon to keep alive. That is why this is not a named-pipe
jobserver: a token read out of a pipe by a process that is then killed is gone for good, and the
machine's budget shrinks by one per kill.

WHAT HAPPENS WHEN BUILDS OVERLAP. Each build still starts its own `-j` worth of these processes, but
at most N of them — across every build of this user on this machine — run their command at any
moment. The rest wait here, ASLEEP in `flock(2)` on the pool's `gate` file: a sleeping process does
not count toward the load average and costs no CPU. Exactly one waiter at a time holds the gate and
looks for a free slot, so a slot freed by build A is not simply taken again by A's next job while B
waits (which is what every job scanning the pool for itself would do). Two builds started together
SHARE the budget and each finishes later than it would alone; that is the trade the owner chose.

Slot counts may differ between builds (a tree configured with `CY_RESERVED_CORES=4` bakes in fewer):
every build uses slots from 0 upwards, so the machine total never exceeds the LARGEST count in use.

A LINK MAY USE MORE THAN ONE CORE. GCC's `-flto=auto`, which CMake's IPO uses for the Shipping
configuration, forks one LTRANS process per core of the machine unless it finds a GNU make
jobserver. With `--jobserver`, this script hands the link one: the slot it holds plus every slot
free at that moment, up to `--max-extra`, as tokens in a pipe named by `MAKEFLAGS`. The link can
never exceed the slots it actually holds, and it does not wait for more than one.

The command runs at `nice --nice` (10 by default) so that what the reserved cores are kept for — a
desktop, a terminal, a test with a timing budget — wins any contention it meets anyway.
"""

from __future__ import annotations

import fcntl
import os
import signal
import sys
import time

USAGE = "usage: job_slot.py --slots N [--nice K] [--jobserver [--max-extra M]] -- <command...>"
POOL_ROOT = "/tmp"
#: How long the gate holder sleeps between looks at the pool. Short enough that a freed slot is not
#: idle for long next to a compile that takes seconds; long enough that one waiter costs nothing.
POLL_S = 0.005


def pool_directory() -> str:
    override = os.environ.get("CY_JOB_SLOT_DIR", "").strip()
    return override or os.path.join(POOL_ROOT, f"cyberdyne-job-slots-{os.getuid()}")


def _open(path: str) -> int:
    return os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)


def _try_lock(descriptor: int) -> bool:
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return False
    return True


class Pool:
    """The slot files of one pool, opened once; locks are taken and dropped on these descriptors."""

    def __init__(self, directory: str, slots: int) -> None:
        os.makedirs(directory, mode=0o700, exist_ok=True)
        self.directory = directory
        self.slots = [_open(os.path.join(directory, f"slot-{index}")) for index in range(slots)]
        self.held: list[int] = []

    def take_free(self, limit: int) -> int:
        """Lock up to `limit` slots that are free right now, without waiting. Returns how many."""
        taken = 0
        for descriptor in self.slots:
            if taken == limit:
                break
            if descriptor not in self.held and _try_lock(descriptor):
                self.held.append(descriptor)
                taken += 1
        return taken

    def take_one(self) -> None:
        """Wait, asleep, for the gate; then for a free slot. Only the gate holder ever polls."""
        if self.take_free(1):
            # A free slot with nobody queued for it. Taken without the gate only when the gate is
            # free too, so that a job never overtakes one already waiting.
            if self._queue_is_empty():
                return
            self.release(self.held.pop())
        gate = _open(os.path.join(self.directory, "gate"))
        fcntl.flock(gate, fcntl.LOCK_EX)
        try:
            while not self.take_free(1):
                time.sleep(POLL_S)
        finally:
            os.close(gate)

    def _queue_is_empty(self) -> bool:
        gate = _open(os.path.join(self.directory, "gate"))
        try:
            return _try_lock(gate)
        finally:
            os.close(gate)

    @staticmethod
    def release(descriptor: int) -> None:
        fcntl.flock(descriptor, fcntl.LOCK_UN)


def _jobserver(tokens: int) -> dict[str, str]:
    """A GNU make jobserver with `tokens` tokens, as the environment a child finds it through."""
    read_end, write_end = os.pipe()
    os.write(write_end, b"+" * tokens)
    os.set_inheritable(read_end, True)
    os.set_inheritable(write_end, True)
    # An inherited jobserver (a build started from make) is replaced, not appended to: the pool is
    # what bounds this command now, and two `--jobserver-auth` words are read differently by make
    # and by GCC.
    inherited = [word for word in os.environ.get("MAKEFLAGS", "").split()
                 if not word.startswith(("--jobserver-auth=", "--jobserver-fds=", "-j"))]
    auth = [f"-j{tokens + 1}", f"--jobserver-auth={read_end},{write_end}"]
    environment = {"MAKEFLAGS": " ".join([*inherited, *auth])}
    # rustc reads Cargo's jobserver from CARGO_MAKEFLAGS before MAKEFLAGS: under Cargo that is the
    # one to replace, or a rustc holding one slot would run codegen on Cargo's every token.
    if "CARGO_MAKEFLAGS" in os.environ:
        environment["CARGO_MAKEFLAGS"] = " ".join(auth)
    return environment


def _run(command: list[str], environment: dict[str, str]) -> int:
    try:
        child = os.posix_spawnp(command[0], command, environment)
    except OSError as error:
        print(f"job_slot: cannot run {command[0]}: {error}", file=sys.stderr)
        return 127

    # Ninja interrupts a build by signalling the process group, which reaches the child directly;
    # a signal sent to this process alone is passed on, and the child's exit is still waited for.
    def forward(number: int, _frame: object) -> None:
        try:
            os.kill(child, number)
        except ProcessLookupError:
            pass

    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, forward)
    while True:
        try:
            _, status = os.waitpid(child, 0)
            break
        except InterruptedError:
            continue
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return os.waitstatus_to_exitcode(status)


def _environment_int(name: str, default: int) -> int:
    value = os.environ.get(name, "").strip()
    return int(value) if value.isdigit() else default


def _parse(argv: list[str]) -> tuple[dict[str, int], list[str]]:
    """`--slots N [--nice K] [--jobserver] [--max-extra M] -- <command...>`, or a bare command.

    A bare command — the first word does not start with `-` — takes every option from the
    environment instead: `CY_JOB_SLOTS`, `CY_JOB_SLOT_JOBSERVER`, `CY_JOB_SLOT_MAX_EXTRA`. That is
    the form Cargo's `RUSTC_WRAPPER` needs, because it names one program and passes it no options.
    """
    options = {
        "slots": _environment_int("CY_JOB_SLOTS", 0),
        "nice": 10,
        "jobserver": _environment_int("CY_JOB_SLOT_JOBSERVER", 0),
        "max-extra": _environment_int("CY_JOB_SLOT_MAX_EXTRA", 7),
    }
    if argv and not argv[0].startswith("-"):
        return options, argv
    index = 0
    while index < len(argv) and argv[index] != "--":
        name = argv[index].lstrip("-")
        if name == "jobserver":
            options["jobserver"] = 1
            index += 1
            continue
        if name not in options or index + 1 >= len(argv):
            raise SystemExit(f"job_slot: unknown option {argv[index]!r}\n{USAGE}")
        options[name] = int(argv[index + 1])
        index += 2
    command = argv[index + 1:]
    if not command:
        raise SystemExit(USAGE)
    return options, command


def _leave_quietly(number: int, _frame: object) -> None:
    """A build interrupted while this job still waits for a slot: no traceback, just the status."""
    os._exit(128 + number)


def main(argv: list[str]) -> int:
    options, command = _parse(argv)
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, _leave_quietly)
    environment = dict(os.environ)
    if options["slots"] <= 0:
        return _run(command, environment)

    pool = Pool(pool_directory(), options["slots"])
    pool.take_one()
    if options["jobserver"]:
        extra = pool.take_free(max(0, options["max-extra"]))
        environment.update(_jobserver(extra))
    if options["nice"]:
        os.nice(options["nice"])
    return _run(command, environment)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
