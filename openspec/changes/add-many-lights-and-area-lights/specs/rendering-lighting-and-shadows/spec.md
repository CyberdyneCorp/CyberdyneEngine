## ADDED Requirements

### Requirement: Area lights are lights the frame carries
The frame SHALL carry rect, disc, sphere and tube lights as lights of their own kinds, each with a
size in metres, a luminance in nits and, for a rect or a disc, an optional two-sided flag. The
forward pass SHALL shade an area light's diffuse term as the form factor of its emitting surface
and, when the frame names a linearly-transformed-cosine table, its specular term as that table's
integral over the surface; without a table it SHALL shade the diffuse term only. The device's
shading SHALL agree with the engine's host evaluation of the same light at the same surface. An
area light SHALL reach CyberGI as emitters radiating its flux. A frame with no area light and no
table SHALL be the frame as it was before area lights existed.

#### Scenario: No area light is the frame before
- **WHEN** a frame is drawn with no area light and no table
- **THEN** it SHALL be byte-identical to the frame drawn by the frame shader as it was before area
  lights existed

#### Scenario: The device shades as the host evaluates
- **WHEN** a floor is lit by a rect, a disc, a sphere and a tube light with the table named
- **THEN** every lit floor pixel SHALL match the host's area shading of the same light, diffuse and
  specular, within the stated tolerance

#### Scenario: Energy against a reference
- **WHEN** an area light's diffuse and specular terms are compared with a Monte-Carlo integration
  over its surface
- **THEN** the diffuse term SHALL agree within one per cent for a rect and the stated tolerance for
  the other shapes, and the specular term within the stated mean and worst-case bounds

### Requirement: Light mobility reaches the frame
A light SHALL carry a `Static`, `Stationary` or `Movable` mobility, `Movable` by default. A static
light SHALL be in no cluster list and no stochastic candidate set and SHALL change no pixel of the
frame; its index SHALL remain, so the lightmap's channels and the stochastic path's history keep
their identities. A stationary or movable light SHALL be shaded directly.

#### Scenario: A static light costs the frame nothing
- **WHEN** a frame is drawn with a light marked `Static`
- **THEN** no texel SHALL differ from the frame without that light and no cluster list SHALL name it
- **AND** the same light marked `Stationary` or `Movable` SHALL light the frame

### Requirement: Cluster lists are assigned on the device
When the device light assignment is enabled, the frame's cluster lists SHALL be built by a compute
pass from the frame's light records: each cluster's list SHALL be the lights of the host's list for
that cluster whose bounds reach the cluster's tile frustum, nearest first, bounded per cluster, with
the overflow counted. A thousand dynamic lights at 1920×1080 SHALL be assigned and shaded inside a
stated budget on the reference device.

#### Scenario: The device's lists are the host's, less the lights that miss the tile
- **WHEN** a frame of hundreds of lights is assigned on the device and on the host
- **THEN** no device list SHALL name a light the host's does not, every light the host's names that
  reaches the tile SHALL be in the device's, and the two frames SHALL be byte-identical

#### Scenario: A thousand lights inside the budget
- **WHEN** a thousand lights light a 1920×1080 frame
- **THEN** the device assignment SHALL take under 0.5 ms and the depth prepass's end to the opaque
  pass's end under 1.5 ms on the reference device, with no list overflowing

### Requirement: The stochastic many-light path is a device frame
The stochastic path SHALL draw, per pixel, light candidates in proportion to their power, resample
them into a reservoir, reuse reservoirs temporally with a capped history and spatially with
weights that keep the estimate unbiased, trace one visibility ray for the selected sample, and
denoise the diffuse result with the engine's denoiser. Its estimate SHALL be unbiased against a sum
over every light, and with reuse its bias SHALL stay inside a stated bound; its denoised signal's
noise and frame-to-frame change SHALL stay inside stated bounds. The choice between the clustered
and the stochastic path SHALL be made each frame from the cluster bound's drops and the frame
arbiter's shading allocation, leaving at once and returning only after a dwell.

#### Scenario: Unbiased against every light
- **WHEN** the stochastic estimate without reuse is averaged over many frames
- **THEN** its mean over the open floor SHALL be within one per cent of the exact sum over every
  light

#### Scenario: The denoiser is the engine's
- **WHEN** the path's denoised output is compared with `Denoiser::denoise` over the device's own
  noisy signal and reprojection
- **THEN** the two SHALL agree within the stated tolerance

#### Scenario: Either path, the same content
- **WHEN** a scene with no shadowing is drawn clustered and stochastically
- **THEN** the mean difference SHALL be within five per cent and the mean absolute per-pixel
  difference within twenty per cent of the clustered picture

#### Scenario: The arbiter's choice
- **WHEN** a frame's clusters drop lights at their bound
- **THEN** the selection SHALL move the frame to the stochastic path that frame
- **AND** once the clustered lighting fits the arbiter's allocation again, it SHALL return only
  after the dwell

### Requirement: Lights a game spawns for a moment
The engine SHALL provide a fixed pool of point, spot and area lights a game spawns with a lifetime
and a fade, addressed by generational handles, with no allocation per spawn; the live lights SHALL
be appended to the frame's lights each frame, faded, and a light whose lifetime ended SHALL be
freed. A directional light, a negative lifetime and a spawn into a full pool SHALL be refused, and
the refusal counted.

#### Scenario: A flash fades and is freed
- **WHEN** a light with a lifetime of 0.1 s and a quadratic fade is spawned and advanced 0.05 s
- **THEN** it SHALL contribute a quarter of its intensity, and after its lifetime SHALL be gone
  from the frame and its handle SHALL no longer answer
