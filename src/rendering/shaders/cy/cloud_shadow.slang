// The cloud shadow, read from the field the sky publishes. Part of the engine's Slang standard
// library.
//
// `atmosphere-sky-and-clouds` — "Cloud shadows": the cloud shadow "SHALL be published as an
// environment field ... and consumed by terrain, foliage, water, and illumination". Four consumers,
// all four of them shading, and all four of them wanting the same one line of arithmetic — so the
// line lives here once rather than four times.
//
// `CloudShadowField` in src/rendering/sky/ is the producer. It declares the field
// `Presentation`-classified and `UNorm8` over [0, 1], with a DECLARED DEFAULT OF ONE — full sun —
// because a sample outside resident data must return the declared default rather than block, and a
// default of zero would put a black world under an unstreamed sky. That default is why the
// functions below need no "is it resident" branch: an unwritten region multiplies sunlight by one.
//
// ================================================================================================
// WHY THIS IS NOT A SHADOW MAP
// ================================================================================================
//
// The requirement's own justification: a cloud's shadow edge is softened to tens of metres by the
// sun's own disc, so the field's finest level is declared no finer than 64 m and
// `cloud_shadow_declaration()` refuses anything finer, naming `virtual-shadows` as where shadow maps
// belong. A consumer that wanted a sharp edge here is asking the wrong subsystem.

module cloud_shadow;

import cy.field;

/// The fraction of direct sunlight reaching this position through the cloud layers, read from an
/// image a pass bound itself: 1 in full sun, 0 under a storm. `x`/`z` are metres relative to the
/// image's origin — `cy/field.slang`'s rule.
///
/// The form a frame without the GPU scene's bindless table uses — `samples/10-world` binds its
/// packed field images directly — and the one `cyCloudShadowAt` below is built on, so the two
/// binding paths share one line of arithmetic and cannot answer differently.
public float cyCloudShadowAtImage(CyFieldImage image, float x, float y, float z)
{
    // SATURATED, and not because the producer is untrusted. `UNorm8` over a declared range decodes
    // inside [0, 1] by construction; what this guards is a consumer binding a slot that holds a
    // DIFFERENT field, whose range is something else entirely. A transmittance above one brightens
    // a surface in sunlight, which looks like a lighting bug three subsystems away.
    return saturate(cyFieldSample(image, x, y, z).value.x);
}

/// The fraction of direct sunlight reaching this position through the cloud layers: 1 in full sun,
/// 0 under a storm. `slot` is the cloud shadow field's slot in the GPU scene's bindless table, and
/// `x`/`z` are metres relative to that image's origin — `cy/field.slang`'s rule.
public float cyCloudShadowAt(uint slot, float x, float y, float z)
{
    return cyCloudShadowAtImage(cyFieldFromScene(slot), x, y, z);
}

/// Direct sunlight, attenuated by the cloud deck. The whole of what the four consumers the
/// requirement names do with this field.
///
/// MULTIPLICATIVE AND APPLIED TO THE DIRECT TERM ONLY: the field is the fraction of DIRECT sunlight
/// that got through, and the sky's own ambient term is already computed from the same cloud
/// reconstruction. Multiplying ambient by it as well would darken an overcast day twice.
public float3 cyCloudShadowAttenuate(float3 directSunlight, uint slot, float x, float y, float z)
{
    return directSunlight * cyCloudShadowAt(slot, x, y, z);
}

/// The same attenuation through a directly bound image.
public float3 cyCloudShadowAttenuateImage(float3 directSunlight, CyFieldImage image, float x,
                                          float y, float z)
{
    return directSunlight * cyCloudShadowAtImage(image, x, y, z);
}
