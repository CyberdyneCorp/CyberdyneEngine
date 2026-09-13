## ADDED Requirements

### Requirement: A declared signal has a producer, or it is not declared
`denoising` names five signals — indirect diffuse, indirect specular, ray-traced shadows, ambient
occlusion and stochastic direct lighting — and requires each to declare its configuration. A signal
that is declared, configured and **fed by nothing** is a table entry, and a framework measured only
on the signals that happen to have a producer is a framework whose per-signal requirements are
untested for the rest.

Every signal the framework declares SHALL have a producer that routes through the framework, or SHALL
be removed from the declaration. Where a signal is deliberately deferred, it SHALL be recorded as a
deferral with the rung at which it re-enters, in one place, rather than existing as an enumerator
nobody drives.

The check SHALL name which signals have producers and which do not, so that "the framework handles
five signals" cannot be read off an enumerator.

#### Scenario: A declared signal with no producer is a finding
- **WHEN** a signal kind is declared and no module in the tree routes that signal through the
  denoiser
- **THEN** the check SHALL fail naming the signal, rather than passing because the enumerator exists

#### Scenario: A visibility term is denoised as occlusion
- **WHEN** ray-traced shadows or ambient occlusion are produced and denoised
- **THEN** they SHALL be filtered as occlusion rather than as radiance, and the case SHALL be
  exercised by the producer rather than by a synthetic buffer written in the denoiser's own test
