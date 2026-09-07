"""The artefact harness's own checks. `unit.artefact_harness` runs this.

The harness is the thing that decides whether every later artefact can pass while reporting a gap,
so it is the one script in `samples/` that needs checks of its own. Each case below is one of the
two M6 defects, stated as something that must be impossible rather than as something a reviewer
should notice.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

from artefact import EXIT_GAP, Failed, Report, Statistic, socket_path


def a_gap_cannot_exit_zero() -> None:
    """Task 5b.5. The M6 defect: `GAP the gizmo drag` beside a green tick and a zero status."""
    report = Report()
    report.did("the viewport is the engine's frame", "chroma 221")
    assert report.exit_code == 0, "a run with no gap passes"
    report.gap("the gizmo drag", "the drag committed nothing")
    assert report.exit_code == EXIT_GAP, (
        "a run that recorded a gap returned 0; that is the defect this module exists to make "
        "impossible"
    )
    # And there is no way back: the status is derived from the gaps, so nothing can clear it but
    # not having recorded one.
    assert not hasattr(report, "exit_code_override")
    assert report.summarise() == EXIT_GAP


def a_claim_this_machine_cannot_evaluate_is_neither_a_pass_nor_a_failure() -> None:
    """`delivery-roadmap`: not evaluated is reported as not evaluated, and never as passed.

    Both directions matter. An absence that failed the run would make the artefact unrunnable on a
    smaller machine — which is how a check comes to be deleted; an absence that passed is the M6
    defect wearing a different hat.
    """
    report = Report()
    report.not_evaluated("keyboard-first operation", "this X server delivers no synthesised keys")
    assert report.exit_code == 0, "an absence is not a failure"
    assert len(report.steps) == 0, "and it is not a pass either"
    assert len(report.absences) == 1
    report.gap("the gizmo drag", "it committed nothing")
    assert report.exit_code == EXIT_GAP, "a gap beside an absence still fails"


def an_extreme_cannot_be_the_headline() -> None:
    """Task 5b.5b. `worst tick 358 us` reached a screenshot, a report and the roadmap page."""
    report = Report()
    worst = Statistic.extreme("worst tick", 358.0, "us")
    report.figure(worst)  # printing it is fine; a hitch has to show up somewhere
    try:
        report.headline(worst)
    except Failed as refused:
        assert "extreme" in str(refused), refused
    else:
        raise AssertionError("an extreme value was accepted as a headline")
    assert report.leading is None


def an_extreme_cannot_be_relabelled_as_stable() -> None:
    """The same defect with `Statistic.stable` in front of it, which is how it would come back."""
    for label in ("worst tick", "peak memory", "max latency", "the slowest frame"):
        try:
            Statistic.stable(label, 1.0)
        except Failed as refused:
            assert "extreme" in str(refused), refused
        else:
            raise AssertionError(f"{label!r} was accepted as a stable statistic")


def a_headline_must_say_what_kind_of_figure_it_is() -> None:
    report = Report()
    try:
        report.headline(Statistic.stable("tick", 12.0, "us"))
    except Failed as refused:
        assert "kind of figure" in str(refused), refused
    else:
        raise AssertionError("an unlabelled figure was accepted as a headline")
    led = report.headline(Statistic.median("median tick", [365.9, 371.2, 384.0, 392.2], "us"))
    assert report.leading is led
    assert led.samples == 4
    assert 371.0 < led.value < 385.0, led
    assert "over 4 samples" in str(led)


def a_median_reproduces_where_a_maximum_does_not() -> None:
    """Why the rule above is a rule and not a preference.

    Four runs of the same workload, drawn from one distribution with a heavy tail — which is what a
    tick histogram is. The maximum is a sample OF THE TAIL and moves by a large multiple of what
    the median moves, run to run, with nothing about the subject having changed. M6's open-world
    artefact measured the same shape for real: `worst tick 358 us` in the screenshot, 365.9 to
    392.2 over four re-runs on a verified-empty machine, and a median that moved 0.6 us.
    """
    import random

    generator = random.Random(20260907)
    runs = [
        [generator.gauss(300.0, 8.0) + generator.expovariate(1 / 40.0) for _ in range(2000)]
        for _ in range(4)
    ]
    medians = [Statistic.median("median tick", run, "us").value for run in runs]
    maxima = [max(run) for run in runs]
    median_spread = max(medians) - min(medians)
    maximum_spread = max(maxima) - min(maxima)
    assert maximum_spread > 10 * median_spread, (medians, maxima)
    # And the harness refuses the one and accepts the other, which is the whole of the rule.
    report = Report()
    report.headline(Statistic.median("median tick", runs[0], "us"))
    report.figure(Statistic.extreme("worst tick", maxima[0], "us"))


def a_long_socket_path_is_shortened_rather_than_refused() -> None:
    """Task 5b.6, found by M6's release run: a build tree past `sun_path`'s 108 bytes."""
    with tempfile.TemporaryDirectory() as short_root:
        short = socket_path(Path(short_root), "viewport.sock")
        assert short.parent == Path(short_root), short
        assert short.name == "viewport.sock"

        deep = Path(short_root).joinpath(*[f"a-directory-with-a-long-name-{n:02d}" for n in range(8)])
        fallback = socket_path(deep, "viewport.sock")
        assert len(str(fallback).encode()) < 104, fallback
        assert fallback != deep / "viewport.sock"
        # Derived, so the same build gets the same socket twice and two builds get two.
        assert socket_path(deep, "viewport.sock") == fallback
        other = socket_path(deep.parent / "elsewhere", "viewport.sock")
        assert other != fallback


CASES = (
    a_gap_cannot_exit_zero,
    a_claim_this_machine_cannot_evaluate_is_neither_a_pass_nor_a_failure,
    an_extreme_cannot_be_the_headline,
    an_extreme_cannot_be_relabelled_as_stable,
    a_headline_must_say_what_kind_of_figure_it_is,
    a_median_reproduces_where_a_maximum_does_not,
    a_long_socket_path_is_shortened_rather_than_refused,
)


def run_selftest() -> int:
    failures = 0
    for case in CASES:
        try:
            case()
        except AssertionError as problem:
            failures += 1
            print(f"    FAIL  {case.__name__}: {problem}")
        else:
            print(f"    ok    {case.__name__}")
    print(f"artefact-harness: {len(CASES) - failures}/{len(CASES)} case(s) passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(run_selftest())
