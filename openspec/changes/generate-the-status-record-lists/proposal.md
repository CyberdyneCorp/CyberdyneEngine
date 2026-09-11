# Generate the status record's three lists, and fail when they drift

## Why

**M8.c's closing gate found this by running the recipe and reading, recorded it as a finding, and
could not perform the fix inside its own scope.** M9's task 7.6 hands it forward in as many words:
`docs/roadmap/capability-matrix.md`'s three "The status record" lists — Complete, Working, Seed —
"have been hand-maintained since M7 and are two milestones stale; the honest fix is to generate them
from `tools/roadmap/record.py`".

By M9 they were **three** milestones stale. M8.b moved eleven capabilities and M8.c three, and the
lists still read as they did at M7's gate: seven Complete, thirty-seven Working, thirteen Seed. The
record said eight, fifty-two and nine.

**This is the shape `delivery-roadmap` already legislates against, inside its own documentation.**
The specification requires "exactly one authoritative record of per-capability implementation status"
and requires status to "be reported by a recipe rather than read by hand". A second, hand-typed copy
of that record living in another document is precisely the drift the requirement exists to prevent,
and nothing checked it because nothing was told to.

## What Changes

- **ADDED** `delivery-roadmap`: a requirement that any second rendering of the status record in the
  documentation is **generated from the record and checked by the status recipe**, so it is either
  current or a failing gate.
- `tools/roadmap/record.py` renders the three lists; `just roadmap-status` fails when the block
  between the markers in `docs/roadmap/capability-matrix.md` does not match, printing what the record
  says; `just roadmap-status --write-lists` rewrites it.
- The lists in the matrix are regenerated, which is the first time since M7 that they say what the
  record says.

## Impact

- **Specification**: `openspec/specs/delivery-roadmap/spec.md`, one added requirement.
- **Code**: `tools/roadmap/record.py`, `tools/roadmap/roadmap.py`.
- **Documentation**: `docs/roadmap/capability-matrix.md`'s three lists become generated content
  between markers.
- **Gates**: no new gate. `roadmap-status` is already a permanent gate and already runs in every
  milestone ledger; what changes is that it now checks one more thing it was always reading.
