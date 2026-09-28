## ADDED Requirements

### Requirement: Decals are applied in the forward frame before lighting
When a frame is given decals, the assembly SHALL rank them in application order — ascending sort
order, ties on the decal's identifier — and SHALL assign them to clusters in the same assignment as
the lights, as their own element type, each bounded by its oriented box and filtered by its
channels, with every cluster list in ascending rank. The forward pass SHALL apply the decals of a
fragment's cluster, in rank order, to the receiver's albedo, roughness, metallic, emission and
shading normal BEFORE the light loop, so that a decal is lit, shadowed and occluded as its receiver
is. A decal SHALL change no pixel outside its projected box. A frame given no decal table SHALL
shade exactly as it did before decals existed.

#### Scenario: No decal table is the frame as it was
- **WHEN** a frame is drawn with no decal table named in its view block
- **THEN** it SHALL be byte-identical to the frame drawn by the frame shader as it was before
  decals existed

#### Scenario: A decal layer of none costs nothing
- **WHEN** a decal's channels are empty, or intersect no channel of the receiver it covers
- **THEN** the frame SHALL be byte-identical to the frame without that decal
- **AND** a decal with empty channels SHALL be assigned to no cluster

#### Scenario: A decal is confined to its box
- **WHEN** a decal is applied to a receiver
- **THEN** every pixel it changes SHALL see a point inside its projected box, and every pixel well
  inside the box on a receiver facing the projector SHALL change

#### Scenario: A decal is lit like its receiver
- **WHEN** a decal covers a receiver partly in a directional light's umbra
- **THEN** where the receiver is in umbra the decalled pixel SHALL be exactly what it is with that
  light switched off, and where the receiver is lit it SHALL NOT be

#### Scenario: Order and assignment are the decals' own
- **WHEN** two decals overlap
- **THEN** the one later in application order SHALL cover the earlier, swapping their sort orders
  SHALL swap which covers, and handing them to the frame in another array order SHALL draw the
  same bytes
- **AND** the frame through the cluster lists SHALL draw the same bytes as the frame that applies
  every decal to every pixel

#### Scenario: Many decals cost no draw call
- **WHEN** tens of thousands of decals are handed to a frame
- **THEN** the frame SHALL record the same number of draws as the frame without them
