# Design: blend spaces, sync groups, foot placement and aim

## The program carries the space; the runtime finds a triangle

```
pose.blend2d ──compile_pose──► BlendSpace {dims, sync, x/y/phase params, samples, triangles}
   s0..s15                     BlendSample {x, y, PoseValue}   (sorted along x in 1D)
                               BlendTriangle {3 sample indices} (Delaunay, cook time)
                                       │
            blend_space_weights ◄──────┘  one weighting for the reference evaluator, evaluate(),
                                          RootWalker, events and sync
```

The triangulation is Bowyer-Watson in doubles with a relative in-circle tolerance, because authored
spaces are grids whose quads are co-circular: an exact test would choose a diagonal by rounding.
With the tolerance the diagonal is the one the insertion order reaches first — a function of the
authored order. At run time the weight of a point is the barycentric coordinate of the first
triangle at the least distance (zero inside): a point on a shared edge is weighted by the first
triangle, and a point outside the hull by the nearest point of its edge, so no weight is negative.

## Sync in marker space, not on a leader

A leader-follower scheme re-expresses the phase whenever the leader changes, and a jump of the
leader is a jump of every follower. Marker space has no leader: the phase `p` is a segment index and
a fraction, and every clip maps it independently,

    t_i(p) = k·d_i + s_ij + α·(s_i,j+1 − s_ij),   k = ⌊p/n⌋, j = ⌊p⌋ mod n, α = p − ⌊p⌋

with the segment starts `s_ij` rotated at bind so segment `j` starts at the same marker in every
clip. The phase advances through segment `j` at `Σ w_i · len_ij` seconds per segment (weights
normalised over the synchronised samples). Each tick a clip's clock is set to its mapped time,
re-anchored to the previous clock and forward only, so rounding never accumulates into drift and
root motion and events read a forward interval. A clip whose markers do not correspond, a held
clip, or a sample that is not a clip keeps its own clock.

## Foot placement asks for the ground

The runtime names no physics server and no terrain. `GroundProbe::cast_down` is the host's, const and
thread-safe because the frame system calls it from every job worker. The solve, in model space:
each ankle to the world, a ray from `probe_above` over it; the offset is the ground's height over
the model's ground plane plus what the tilt adds to the ankle (`h/n.y − h`); the pelvis moves by the
lower offset; each leg is solved to its animated ankle plus its offset; each foot gets the
animated model orientation turned from up to the (capped) normal. The offsets and normals follow
`1 − e^(−elapsed/τ)` of the way each solve over SIMULATION time, so a frame rate does not change the
motion. Zero weight returns before touching the pose.

## Off is the frame before

A program without the new nodes compiles, digests and cooks exactly as before (the digest hashes
nothing for empty tables; the cooked record stays version 1). A `pose.foot_ik` at zero weight, at a
tier that skips modifiers, or with no ground copies its input: integration.animation_system compares
the published matrices of the three against a program without the node, byte for byte.

## ABI 1.14

`animation_aim_at` takes a world point into model space through the entity's propagated
`WorldTransform`, checks all three target parameters exist before writing any, and is simulation
(`[N F]`). `animation_foot_placement` reports a solve (`CyFootPlacement`, 68 bytes) and is
presentation (`[N U]`). Both are non-pure virtuals on `AnimationBackend`, so a backend written
against 1.13 answers `UNAVAILABLE`.
