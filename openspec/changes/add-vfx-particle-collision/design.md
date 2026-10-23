# Design: particle collision

## A contact is a query; a response is arithmetic over it

A kernel in this domain is an ordered list of attribute writes whose values are pure expressions
(`ir.h`). Collision fits that without a new kind of statement: the world is asked one question a
particle — `Contact(position, radius, shape_a, shape_b) -> float4(normal, separation)` — and every
response is a pure function of the particle's state and that answer:

```
contact  = Contact(p', r, a, b)                    one query, memoised under the node
position = Resolve(p', contact)                    p' + n * max(-separation, 0)
velocity = Bounce(v', contact, (e, mu, stick, rest))
touching = separation < 1 mm ? 1 : 0
began    = max(touching - was_in_contact, 0)
impact   = Impact(v', contact) * began             closing speed on the step a contact began
normal   = (contact.x, contact.y, contact.z)
```

Kill and the event are not operations: they are the kernel's existing kill root and event raise,
wired to `contact` and `impact`. The lowering memoises each output under its pin and the contact
under a pin no author can name, so six wired outputs query the world once — with the optimiser's
sample deduplication on or off.

## Bounce

Touching and closing (`dot(v, n) < 0`): the normal component is reversed and scaled by the
restitution, which is zero below `rest_speed`; the tangent loses at most `friction` times the normal
impulse `(1 + e) |v_n|` — Coulomb friction, so a particle slides when the grip is low and stops when
it is high, and a resting particle under gravity loses `mu g dt` of its sliding speed a step.
`stick` above one half stops it. Not touching, or already leaving: unchanged.

## The colliders

| | Contact |
|---|---|
| plane | `n = normalise(a.xyz)`, separation `dot(n, p) - a.w - r` |
| sphere | away from `a.xyz`, separation `|p - c| - a.w - r` |
| box | axis-aligned: the outside distance and its normal, or the nearest face inside |
| capsule | the sphere about the closest point of the segment |
| sdf | world `p + origin`; one field sample, and if touching the gradient by six samples (a tetrahedron's four at quality 2) |
| depth | project, read the texel, unproject it; normal from the neighbours on the same surface (forward differences at quality 2); separation along it; no contact off screen, behind the camera, on sky or deeper than `thickness` |

## The field is the GI's

`GpuGiScene` already keeps the clipmaps on the device for the GI's traces. `field_binding()` hands
out the page table, the brick pool and the level rows, and the VFX pass binds them; the device code
transcribes `gi_gpu_common.slang`'s `giDistance`, which is itself a transcription of
`DistanceField::distance`. `cy::vfx-gpu` links no GI module: the host converts the binding into a
`GpuCollisionField` (eleven numbers and two handles).

## Off is the frame before

Set 0 is fixed for every effect, so the collision resources are a SECOND descriptor set; only an
emitter whose kernels contain a `Contact` declares it in its pipeline layout, generates the collision
prelude, or widens the generated push block to `GpuPushConstants`' last four words (whose layout was
already 48 bytes; the last word was `reserved` and is now `collision_quality`). A collider-free effect's
dispatch unit, probe unit and cook key are held to `main`'s bytes by `integration.vfx_compiler`.

## Events on the CPU path

`vfx.event_count(channel)` in a Spawn stage reads how many events the channel holds that this
emitter has not spawned from this frame; `vfx.event_payload(channel)` in an Initialise stage reads
the payload of the event at `cursor + spawn_index`. The world keeps a cursor per block and channel,
cleared with the router's channels at the start of a step and advanced by what was spawned — so a
spawn the cap or the controller reduced leaves the rest for the next sub-step rather than consuming
events nothing was spawned for.
