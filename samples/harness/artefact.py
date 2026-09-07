"""The artefact harness: what an artefact may claim, and what its exit status must say.

M7 tasks 5b.5 and 5b.5b. Every milestone artefact in `samples/` reports through this module, and it
exists because two defects found at M6's closing gate were not defects in any artefact's subject —
they were defects in how artefacts report, and an artefact that reports wrongly hides every
milestone after it as well as its own.

--- THE FIRST RULE: A GAP IS NOT A PASS (task 5b.5) -------------------------------------------------

`samples/05b-editor-window/window.py` printed

    GAP   the gizmo drag — the drag committed nothing

and returned **0**. `smoke.editor_window` was green, the milestone's headline interaction was
unproven, and the stamp, the tick and the exit code all agreed with each other and with nothing
else. The gap was honestly reported and structurally invisible.

So a gap is recorded here, not in each artefact's own bookkeeping, and `Report.exit_code` is the
only thing that turns a run into a status. An artefact that records a gap CANNOT exit zero, because
the number it returns is computed from the gaps rather than chosen alongside them.

--- THE SECOND RULE: LEAD WITH A FIGURE THAT REPRODUCES (task 5b.5b) --------------------------------

`samples/06-open-world` led with `worst tick 358 us`. Four re-runs on a verified-empty machine gave
365.9, 371.2, 384.0 and 392.2 — every one of them worse than the figure in the screenshot, the
report and the roadmap page — while the median over the same four runs moved 0.6 us. A single
sample of an extreme-value statistic is a draw, not a measurement.

So a figure carries its own kind. [`Statistic.stable`] is a median, a percentile below the tail or a
mean, and only a stable figure may be a headline. [`Statistic.extreme`] is a maximum, a minimum, a
worst or a peak: it may be printed — a hitch has to show up somewhere — and [`Report.headline`]
refuses it by name, with the sentence that says why.

The rule is enforced where the claim is made rather than where it is reviewed, which is the whole
point of putting it here: a later artefact cannot repeat either defect without deleting this module.

--- WHAT THIS MODULE IS NOT -------------------------------------------------------------------------

It is not a test framework. There are no fixtures, no discovery and no assertions beyond `expect`;
an artefact is a script that drives real programs, and the value of this module is entirely in the
two rules above plus the shared vocabulary that makes a run readable.
"""

from __future__ import annotations

import math
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

__all__ = [
    "Absent",
    "Failed",
    "Report",
    "Statistic",
    "Step",
    "expect",
    "socket_path",
]

#: The exit status for "this machine cannot run the artefact", which CTest maps to SKIP.
EXIT_ABSENT = 3
#: The exit status for "an act that was expected to work did not".
EXIT_FAILED = 1
#: The exit status for "the artefact ran and something it claims was not satisfied".
EXIT_GAP = 1


class Failed(Exception):
    """An act that was expected to work did not."""


class Absent(Exception):
    """This machine cannot run the artefact, and says which part is missing."""


def expect(condition: bool, what: str) -> None:
    """Refuse to continue, naming what was expected."""
    if not condition:
        raise Failed(what)


# --- Figures -------------------------------------------------------------------------------------


#: The words an extreme-value statistic is spelled with. A figure labelled with one of these is
#: refused as a headline whichever constructor produced it, so that a `Statistic.stable` built by
#: mistake around a maximum is caught by its own name rather than by review.
EXTREME_WORDS = (
    "worst", "max", "maximum", "min", "minimum", "peak", "best", "slowest", "fastest",
    "longest", "shortest", "spike", "hitch",
)

#: The words a stable statistic is spelled with. A headline must be labelled with one of these, so
#: that the figure a reader sees announces what kind of figure it is.
STABLE_WORDS = ("median", "p50", "mean", "average", "typical", "total", "count")


def _mentions(text: str, words: tuple[str, ...]) -> str | None:
    lowered = text.lower()
    for word in words:
        if word in lowered:
            return word
    return None


@dataclass(frozen=True)
class Statistic:
    """One measured figure, and whether it is the kind of figure a run may lead with.

    `kind` is `"stable"` or `"extreme"`. The distinction is not stylistic: a median over a handful
    of samples reproduces on a re-run and a maximum does not, so only one of the two can carry a
    claim that a reader is expected to check.
    """

    label: str
    value: float
    unit: str = ""
    kind: str = "stable"
    samples: int = 1

    @staticmethod
    def stable(label: str, value: float, unit: str = "", samples: int = 1) -> "Statistic":
        """A median, a percentile below the tail, or a mean.

        Refuses a label spelled with an extreme-value word, because that is the mistake this class
        exists to catch — `Statistic.stable("worst tick", ...)` is the M6 defect with a different
        constructor in front of it.
        """
        offending = _mentions(label, EXTREME_WORDS)
        if offending is not None:
            raise Failed(
                f"{label!r} is offered as a stable statistic and is labelled {offending!r}. "
                "An extreme value does not reproduce: build it with Statistic.extreme, and lead "
                "with the median beside it"
            )
        return Statistic(label, float(value), unit, "stable", int(samples))

    @staticmethod
    def extreme(label: str, value: float, unit: str = "", samples: int = 1) -> "Statistic":
        """A maximum, a minimum, a worst or a peak. Printable, never a headline."""
        return Statistic(label, float(value), unit, "extreme", int(samples))

    @staticmethod
    def median(label: str, values, unit: str = "") -> "Statistic":
        """The median of a run of samples, which is the figure an artefact should lead with."""
        series = [float(value) for value in values]
        if not series:
            raise Failed(f"{label!r} has no samples, so it has no median")
        return Statistic.stable(label, statistics.median(series), unit, samples=len(series))

    def __str__(self) -> str:
        magnitude = abs(self.value)
        if magnitude != 0 and (magnitude < 0.01 or magnitude >= 100_000):
            rendered = f"{self.value:.3g}"
        elif float(self.value).is_integer():
            rendered = f"{self.value:.0f}"
        else:
            rendered = f"{self.value:.1f}"
        unit = f" {self.unit}" if self.unit else ""
        over = f" over {self.samples} samples" if self.samples > 1 else ""
        return f"{self.label} {rendered}{unit}{over}"

    def is_reproducible(self) -> bool:
        """Whether this figure may be the one a run leads with."""
        return (
            self.kind == "stable"
            and _mentions(self.label, EXTREME_WORDS) is None
            and math.isfinite(self.value)
        )


# --- The report ------------------------------------------------------------------------------------


@dataclass
class Step:
    """One thing an artefact claims, and whether it was satisfied."""

    name: str
    detail: str = ""
    ok: bool = True


@dataclass
class Report:
    """What a run claimed, what it could not claim, and the status that follows from both.

    `exit_code` is DERIVED. There is deliberately no way to record a gap and then hand back zero:
    that combination is the M6 defect, and an artefact that wants a different status has to stop
    calling `gap`, which is a change a reviewer sees.
    """

    steps: list[Step] = field(default_factory=list)
    gaps: list[Step] = field(default_factory=list)
    absences: list[Step] = field(default_factory=list)
    shots: list[Path] = field(default_factory=list)
    figures: list[Statistic] = field(default_factory=list)
    _headline: Statistic | None = None
    failure: str | None = None

    # -- claims ------------------------------------------------------------------------------------

    def did(self, name: str, detail: str = "") -> None:
        """A claim that was satisfied."""
        self.steps.append(Step(name, detail))
        print(f"    ok    {name}" + (f" — {detail}" if detail else ""))

    def gap(self, name: str, detail: str) -> None:
        """A claim that was NOT satisfied. The run can continue; it cannot pass."""
        self.gaps.append(Step(name, detail, ok=False))
        print(f"    GAP   {name} — {detail}")

    def not_evaluated(self, name: str, detail: str) -> None:
        """A claim this MACHINE could not evaluate. Not a gap, and not a pass.

        `delivery-roadmap` is explicit: "a criterion which cannot be evaluated on the host running
        it SHALL be reported as not evaluated rather than as passed". The distinction from
        [`Report.gap`] is who is at fault — a gap is the subject's, an absence is the machine's —
        and it is a distinction the exit status has to respect in BOTH directions: reporting an
        absence as a failure would make an artefact unrunnable on a smaller machine, and reporting
        it as a pass is the defect this module exists to prevent.

        So it does not move the status, and it is printed and counted in the summary where a reader
        cannot miss it.
        """
        self.absences.append(Step(name, detail, ok=False))
        print(f"    n/a   {name} — {detail}")

    def failed(self, what: str) -> None:
        """An act that was expected to work did not. Recorded so `exit_code` accounts for it."""
        self.failure = what

    def shot(self, path: Path) -> None:
        self.shots.append(Path(path))
        print(f"    shot  {path}")

    # -- figures -----------------------------------------------------------------------------------

    def figure(self, statistic: Statistic) -> Statistic:
        """Record a figure. Any kind: an extreme is worth printing, it is just never the headline."""
        self.figures.append(statistic)
        print(f"    fig   {statistic}")
        return statistic

    def headline(self, statistic: Statistic) -> Statistic:
        """The one figure the run leads with — in its summary, its screenshot and its report page.

        Refuses an extreme-value statistic, naming it and saying why. That refusal is task 5b.5b:
        M6's open-world artefact led with `worst tick 358 us`, and four re-runs on an empty machine
        were all worse while the median moved 0.6 us.
        """
        if not statistic.is_reproducible():
            raise Failed(
                f"an artefact may not lead with {statistic.label!r}: it is an extreme value, and a "
                "single sample of an extreme value is a draw rather than a measurement. Lead with "
                "the median or the mean, and print the maximum beside it"
            )
        if _mentions(statistic.label, STABLE_WORDS) is None:
            raise Failed(
                f"the headline figure {statistic.label!r} does not say what kind of figure it is. "
                f"Name it with one of {', '.join(STABLE_WORDS)}, so a reader knows whether it "
                "reproduces"
            )
        self._headline = statistic
        if statistic not in self.figures:
            self.figures.append(statistic)
        return statistic

    @property
    def leading(self) -> Statistic | None:
        """The headline figure, or `None` when the run claims no measurement."""
        return self._headline

    # -- the status --------------------------------------------------------------------------------

    @property
    def exit_code(self) -> int:
        """The status this run must return. Derived, never chosen."""
        if self.failure is not None:
            return EXIT_FAILED
        return EXIT_GAP if self.gaps else 0

    def summarise(self, where: Path | None = None) -> int:
        """Print the run's summary and return the status it implies."""
        print("\n--- the session ---")
        shots = f", {len(self.shots)} screenshot(s)" + (f" in {where}" if where else "")
        print(f"    {len(self.steps)} step(s) satisfied{shots}")
        if self._headline is not None:
            print(f"    headline: {self._headline}")
        if self.absences:
            print(
                f"    {len(self.absences)} step(s) NOT EVALUATED on this machine, which is not the "
                "same as satisfied:"
            )
            for absence in self.absences:
                print(f"      · {absence.name} — {absence.detail}")
        if self.gaps:
            print(f"    {len(self.gaps)} step(s) NOT SATISFIED, each named above:")
            for gap in self.gaps:
                print(f"      · {gap.name} — {gap.detail}")
            print(
                "    A GAP IS NOT A PASS. This run returns "
                f"{EXIT_GAP} because something it claims was not satisfied."
            )
        if self.failure is not None:
            print(f"    the run failed: {self.failure}")
        return self.exit_code


# --- Where a Unix socket may live (task 5b.6) --------------------------------------------------------


#: What the kernel accepts in `sockaddr_un.sun_path`, including the terminating NUL.
#:
#: 108 on Linux, 104 on the BSDs and macOS. The smaller number is used everywhere, because a path
#: that works here and not there is a defect that only one platform's continuous integration finds.
SUN_PATH_MAX = 104


def socket_path(preferred: Path | str, name: str) -> Path:
    """A Unix socket path inside `preferred`, or a short one that fits when it would not.

    M7 task 5b.6, found by M6's release run. `window.py` derived its viewport socket from the build
    tree, and a build tree named `build/release-with-assertions/samples/05b-editor-window/session`
    is past the kernel's limit — the publisher died with

        bind: Error { kind: InvalidInput, message: "path must be shorter than SUN_LEN" }

    which reads as a defect in the transport and is a defect in the path. The failure is a property
    of where the artefact was built, so it appears on exactly the profile nobody runs by hand.

    The fallback is derived from the preferred path rather than random, so two runs of the same
    build share a socket and two different builds do not. It goes under `$XDG_RUNTIME_DIR` when
    there is one, because that is a per-user directory the system cleans up, and under `/tmp`
    otherwise.
    """
    preferred = Path(preferred)
    candidate = preferred / name
    if len(str(candidate).encode()) < SUN_PATH_MAX:
        return candidate

    import hashlib
    import os

    digest = hashlib.sha256(str(candidate).encode()).hexdigest()[:12]
    stem = Path(name).stem[:12]
    root = Path(os.environ.get("XDG_RUNTIME_DIR") or "/tmp")
    short = root / f"cy-{stem}-{digest}.sock"
    if len(str(short).encode()) >= SUN_PATH_MAX:
        short = Path("/tmp") / f"cy-{digest}.sock"
    short.parent.mkdir(parents=True, exist_ok=True)
    print(
        f"    note  the socket path under {preferred} is "
        f"{len(str(candidate).encode())} bytes and the kernel accepts {SUN_PATH_MAX - 1}; "
        f"using {short}"
    )
    return short


def main(argv: list[str] | None = None) -> int:
    """`python3 artefact.py --selftest` — the harness's own checks, as `unit.artefact_harness`."""
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--selftest" not in argv:
        print(__doc__)
        return 0
    from selftest import run_selftest  # noqa: PLC0415 — only the self-test needs it

    return run_selftest()


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main())
