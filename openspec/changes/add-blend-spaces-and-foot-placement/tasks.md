# Tasks

- [x] `pose.blend1d` / `pose.blend2d`, `PoseOp::BlendSpace`, the three tables, the cook-time
      triangulation, `blend_space_weights`, refusals, dependency analysis, digest, assembly checks,
      the reference evaluator; integration.graph_compiler cases.
- [x] Runtime blend spaces: running-weight pose blend, root motion and events by weight, marker-space
      sync bound at `AnimationRig::bind`; integration.animation_runtime cases (barycentric pose,
      sync alignment, cadence, root motion and events, determinism, partial mask weight).
- [x] `foot_placement.h` and `solve_aim`; the two-bone solver in model space (regression case);
      `pose.foot_ik` and `pose.aim` with joints by name; unit.animation cases.
- [x] `AnimationSystem::set_ground_probe`, placement and elapsed time per instance, reports;
      integration.animation_system cases (slope, off byte-identical, workers).
- [x] `cy::animation-ground` with integration.animation_ground.
- [x] Cooked program version 2; integration.animation_assets case.
- [x] ABI 1.14 (`animation_aim_at`, `animation_foot_placement`), thunks, adapter, baseline, Swift
      overlay, Rust SDK; unit.abi, integration.game_backend_animation, CyberdyneKit tests.
- [x] Editor catalogue, vector literals, preview ground; editor backend and Rust tests.
- [x] `--course` in samples/09b-animated-character and `just capture-foot-placement`; images and
      video.
- [x] Prove every new case red by a mutation (`evidence/falsification.txt`).
- [x] READMEs, `docs/guides/animation.md`, `docs/guides/swift.md`, the requirements map and the
      living specifications; validate this change.
