// The terrain's substrate shading, on the device. Part of the engine's Slang standard library.
//
// `m10:world-frame-budget` names three bands that make `samples/10-world` cost about 122 ms a
// frame, and the largest is "the environment substrate re-sampled at every terrain vertex" —
// 63.0 ms, `World::shade_terrain()` in samples/10-world/world.cpp, four `sample_deterministic()`
// calls at each of 152 000 vertices, every frame. The gap's own text says it closes "when those
// shaders exist, not by optimising the CPU loop". THIS IS THAT SHADER'S SUBSTRATE HALF: the four
// samples and the colour they produce, as one function of a position.
//
// M11.a's spike measured the band before it was scoped: 81.90 ms for the shipped per-vertex loop
// against 0.121-0.150 ms for the whole band — four samples AND the colour — as one dispatch. The
// decomposition is what makes that number believable rather than impressive: per-sample bookkeeping
// 8.6%, the store's indirection over the flat buffer 45.9%, the sampling arithmetic 45.4%. What the
// device deletes is the first two; what it accelerates is the third.
//
// ================================================================================================
// THE ARITHMETIC IS `World::shade_terrain()`'s, EXPRESSION FOR EXPRESSION
// ================================================================================================
//
// A band that got cheap by doing less is not the band. The colour ramps, the thresholds and the
// order of the three decisions — shore, then slope, then altitude — are the sample's, and the four
// field names are the ones four different producers claim: `water-distance` from `cy::water`'s
// shoreline, `wetness` and `snow-depth` from `cy::weather`, `vegetation` from `cy::foliage`.
// Nothing here knows which module wrote which, which is the whole point of a substrate.
//
// ================================================================================================
// WHAT THE CALLER OWES IT
// ================================================================================================
//
// FOUR SLOTS OF THE GPU SCENE'S FIELD TABLE, and a position ALREADY MADE LOCAL to those images —
// see `cy/field.slang`'s header for why the f64 subtraction stays on the processor. The four images
// share an origin when they share a residency level, which is what lets one local position serve
// all four; a caller mixing levels passes the local position of each.

module terrain_shade;

import cy.field;

/// Which slot of the bindless field table each of the four fields is at, plus the vertex's own
/// geometric inputs. A struct rather than four parameters because a draw carries this per view and
/// not per vertex, and because a caller that got two slots the wrong way round would otherwise be
/// spelling four bare integers at the call site.
public struct CyTerrainFields
{
    public uint waterDistance;
    public uint wetness;
    public uint snowDepth;
    public uint vegetation;
};

/// The palette. Public and a struct rather than five constants, because `terrain`'s own material
/// tiers are allowed to change it and a shader standard library that baked one project's colours in
/// would be a library with an art direction.
public struct CyTerrainPalette
{
    public float3 rock;
    public float3 sand;
    public float3 grass;
    public float3 alpine;
    public float3 snow;
};

/// `samples/10-world`'s own palette, so the artefact's picture is reproduced by this path rather
/// than approximated by it.
public CyTerrainPalette cyTerrainDefaultPalette()
{
    CyTerrainPalette palette;
    palette.rock = float3(0.33, 0.31, 0.30);
    palette.sand = float3(0.62, 0.56, 0.42);
    palette.grass = float3(0.24, 0.34, 0.17);
    palette.alpine = float3(0.44, 0.43, 0.40);
    palette.snow = float3(0.86, 0.88, 0.92);
    return palette;
}

/// What the four samples said, kept separately from the colour so that a caller measuring the
/// AGREEMENT of the two paths compares four values rather than one colour — a colour folds four
/// samples into three numbers and hides which of the four disagreed.
public struct CyTerrainSubstrate
{
    public float waterDistanceMetres;
    public bool shoreResolved;
    public float wetness;
    public float snowDepthMetres;
    public float vegetation;
};

/// The four samples, at one position. `x` and `z` are metres relative to the field images' origin;
/// `y` is absolute metres.
public CyTerrainSubstrate cyTerrainSampleSubstrate(CyTerrainFields fields, float x, float y,
                                                   float z)
{
    CyTerrainSubstrate substrate;
    const CyFieldSample shore = cyFieldSampleScene(fields.waterDistance, x, y, z);
    substrate.waterDistanceMetres = shore.value.x;
    // RESOLVED, NOT "SMALL". The processor path reads `FieldSample::resolved` before it believes a
    // shore distance, because an unwritten region answers the declared default and a default that
    // happened to be small would paint a beach in the middle of a continent.
    substrate.shoreResolved = shore.resolved;
    substrate.wetness = cyFieldSampleScene(fields.wetness, x, y, z).value.x;
    substrate.snowDepthMetres = cyFieldSampleScene(fields.snowDepth, x, y, z).value.x;
    substrate.vegetation = cyFieldSampleScene(fields.vegetation, x, y, z).value.x;
    return substrate;
}

/// The same substrate sample for a pass that binds four field images directly. Integrated sample
/// passes use this form before their fields have been published into the GPU scene's bindless
/// table; the sampling and shading arithmetic remains shared with the material path above.
public CyTerrainSubstrate cyTerrainSampleSubstrateImages(
    CyFieldImage waterDistance, CyFieldImage wetness, CyFieldImage snowDepth,
    CyFieldImage vegetation, float x, float y, float z)
{
    CyTerrainSubstrate substrate;
    const CyFieldSample shore = cyFieldSample(waterDistance, x, y, z);
    substrate.waterDistanceMetres = shore.value.x;
    substrate.shoreResolved = shore.resolved;
    substrate.wetness = cyFieldSample(wetness, x, y, z).value.x;
    substrate.snowDepthMetres = cyFieldSample(snowDepth, x, y, z).value.x;
    substrate.vegetation = cyFieldSample(vegetation, x, y, z).value.x;
    return substrate;
}

/// The colour those four samples produce, given the vertex's slope and altitude.
///
/// `slope` is `1 - normal.y`, which is the sample's own definition; `altitudeMetres` is the vertex's
/// height and `alpineMetres` the altitude above which bare rock gives way to alpine ground.
public float3 cyTerrainShade(CyTerrainPalette palette, CyTerrainSubstrate substrate, float slope,
                             float altitudeMetres, float alpineMetres)
{
    // THE BEACH IS THE WATER-DISTANCE FIELD, not a height band: `cy::water`'s shoreline publishes a
    // two-pass chamfer transform whose answer is the distance from whichever side the water is on,
    // and reading it here is what makes sand follow a coastline into every inlet instead of
    // following a contour line.
    float3 base = palette.grass;
    if (substrate.shoreResolved && substrate.waterDistanceMetres < 14.0)
    {
        base = lerp(palette.sand, palette.grass,
                    saturate((substrate.waterDistanceMetres - 4.0) / 10.0));
    }
    else if (slope > 0.45)
    {
        base = palette.rock;
    }
    else if (altitudeMetres > alpineMetres)
    {
        base = palette.alpine;
    }

    // Wet ground is darker and a little more saturated, and the three coefficients differ per
    // channel because water darkens the red end of a surface more than the blue.
    const float wet = saturate(substrate.wetness);
    float3 shaded = float3(base.x * (1.0 - (wet * 0.45)), base.y * (1.0 - (wet * 0.42)),
                           base.z * (1.0 - (wet * 0.30)));
    shaded = lerp(shaded, lerp(shaded, palette.grass, 0.5), saturate(substrate.vegetation) * 0.4);

    // Snow LIES rather than tints: it covers the flat before the steep, and it takes a real depth to
    // cover at all — so a few centimetres dusts and half a metre buries.
    const float cover = saturate(substrate.snowDepthMetres * 1.2) * saturate(1.3 - (slope * 2.6));
    return lerp(shaded, palette.snow, cover);
}

/// Both halves at once, which is what a terrain vertex or material actually calls.
public float3 cyTerrainShadeAt(CyTerrainFields fields, CyTerrainPalette palette, float x, float y,
                               float z, float slope, float altitudeMetres, float alpineMetres)
{
    const CyTerrainSubstrate substrate = cyTerrainSampleSubstrate(fields, x, y, z);
    return cyTerrainShade(palette, substrate, slope, altitudeMetres, alpineMetres);
}

public float3 cyTerrainShadeAtImages(
    CyFieldImage waterDistance, CyFieldImage wetness, CyFieldImage snowDepth,
    CyFieldImage vegetation, CyTerrainPalette palette, float x, float y, float z, float slope,
    float altitudeMetres, float alpineMetres)
{
    const CyTerrainSubstrate substrate = cyTerrainSampleSubstrateImages(
        waterDistance, wetness, snowDepth, vegetation, x, y, z);
    return cyTerrainShade(palette, substrate, slope, altitudeMetres, alpineMetres);
}
