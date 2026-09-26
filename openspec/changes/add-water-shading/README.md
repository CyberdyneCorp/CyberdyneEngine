# add-water-shading

The sea in `samples/10-world` is shaded as water: the bed shows through the shallows and fades into
the channel by Beer-Lambert over the path to it, with the body's own absorption and in-scatter; the
surface reflects the sky and the land through a planar mirror, weighted by Fresnel; the shore foams
where the column is thin; and the sun is focused on the shallow bed by the curvature of the ocean's
own wave trains.

![the fjord, mid-morning](../../../docs/design/images/water-shading-day-on.png)
![the same frame before](../../../docs/design/images/water-shading-day-off.png)
![the shallows](../../../docs/design/images/water-shading-shore-on.png)
![the clouds in the lake at night](../../../docs/design/images/water-shading-night-on.png)

With `--no-water-shading` the take is the one 0f1dfd1 drew, 192 of 192 frames byte-identical
(`evidence/frame-identity.txt`). Every case of `render.world_water` was seen red under a mutation
(`evidence/falsification.txt`).
