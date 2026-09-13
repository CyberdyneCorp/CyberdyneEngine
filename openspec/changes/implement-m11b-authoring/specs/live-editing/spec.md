## ADDED Requirements

### Requirement: A play mode is reachable, or its absence is declared and refused by name
The three play modes are already required. What is not required, and what this adds, is that **the
set of modes the engine claims SHALL be enumerable, and every entry in it SHALL be either driveable
end to end or declared absent with the rung that closes it.**

`grep -rniI 'SeparateProcess\|RemoteDevice\|InEditor' src/ editor/ tools/` returns nothing across
this tree, so all three modes are claimed by a specification and named by no code. A mode in that
state is a claim nothing can check.

Selecting a mode that is not available SHALL **refuse, naming the mode and the reason**. It SHALL
NOT fall back to another mode. A request for remote play that quietly runs in-editor produces a
green result over a feature that does not exist, and is worse than a refusal in every respect: the
test passes, the feature is absent, and nothing in the record says so.

Where a mode is available but a capability of it is not — single-frame step on a runtime that cannot
provide it, for instance — the **capability** SHALL be declared per mode and queried, rather than
the mode being silently downgraded or the capability silently ignored. The specification already
permits this: play mode supports stepping *"in every mode where the runtime permits"*, and "where
the runtime permits" SHALL be answerable by a query rather than by trying it.

A mode declared absent SHALL name the milestone or rung at which it is due, so that "not yet" is a
recorded decision rather than the absence of a check.

#### Scenario: An unavailable mode refuses rather than substituting
- **WHEN** a play mode that has no implementation on this configuration is selected
- **THEN** the request SHALL fail naming the mode and the reason, and no other mode SHALL start

#### Scenario: The refusal is load-bearing
- **WHEN** the refusal is replaced by a fallback to the default mode
- **THEN** the test that claims per-mode behaviour SHALL go red, and that SHALL be demonstrated
  rather than asserted

#### Scenario: A claimed mode with no implementation is a finding
- **WHEN** the engine enumerates its play modes
- **THEN** a mode that is neither driveable through the live bridge nor declared absent with a rung
  SHALL fail the check, naming the mode
