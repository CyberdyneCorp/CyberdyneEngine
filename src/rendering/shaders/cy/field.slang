// An environment field, sampled on the device. Part of the engine's Slang standard library.
//
// `environment-fields` — "CPU and GPU access": fields "SHALL be samplable from both CPU code and
// GPU shaders, through interfaces that produce the same value for the same position and
// resolution", and GPU access "SHALL be through BINDLESS RESOURCES reachable from the GPU scene, so
// a shader can sample a field without per-draw binding".
//
// `src/environment/include/cy/environment/gpu.h` fixes the buffer's layout and `sample_field_image()`
// in src/environment/src/gpu.cpp is that layout's sampler written on the processor. This module is
// the THIRD implementation of the same algorithm and it is written against the second, expression
// by expression. Until M11.a it did not exist, and `m10:fields-sampled-on-a-device` measured the
// number of `.slang` modules in this tree that sampled an environment field at ZERO.
//
// ================================================================================================
// WHY IT IS WRITTEN AGAINST gpu.cpp AND NOT AGAINST THE STORE
// ================================================================================================
//
// gpu.cpp's own header says why it exists: it is the shader's algorithm on the processor, sharing
// no code with `FieldStore`, so that "the same value for the same position" is a MEASUREMENT
// between independent implementations rather than a tautology. This file keeps that property by
// being a transcription of gpu.cpp rather than a re-derivation of it, including the associations a
// shader author would naturally change:
//
//   * the lattice coordinate is `(x / cellMetres) - 0.5`, not `(x - 0.5 * cellMetres) / cellMetres`.
//     The two differ in the last bit and the agreement is measured in ULPs;
//   * the bilinear accumulation runs dy, then dz, then dx, with `weight = wx * wz * wy` in THAT
//     order, because floating-point addition is not associative and the comparison is bit-exact;
//   * `cyFieldFloorDiv` is the C++ function's two-case form and not `floor(a / b)`, which for
//     negative operands is a different number once the division rounds.
//
// A "simplification" here is a disagreement there. M11.a's spike measured what that is worth: the
// device reproduces `sample_field_image()` to within 3 ULP over 1 843 968 comparisons, while the
// STORE and `sample_field_image()` already differ from each other by 77 ULP on the processor —
// so a drift introduced here is not lost in the noise, it is four and a half times the noise.
//
// ================================================================================================
// THE POSITION ARRIVES ALREADY LOCAL, AND THAT IS MEASURED RATHER THAN ASSUMED
// ================================================================================================
//
// `x` and `z` are metres RELATIVE to the image's origin corner — `environment::image_local()` does
// the f64 subtraction once, on the processor — and `y` is absolute metres, because a world's
// vertical extent is thin enough for f32 to be exact there.
//
// gpu.h stated that as a convention. M11.a's spike measured the alternative: a shader doing its own
// f32 subtraction lands on a different representable number for 13.7% of positions, and the answer
// is still inside every field's declared precision — but with 35x of margin on a 1536 m world,
// which will not survive a continental one. The licence exists and is too narrow to spend, so this
// module has no entry point that takes an absolute position: the GPU scene carries the local one.
//
// ================================================================================================
// WHAT THIS MODULE'S SUITE STILL OWES
// ================================================================================================
//
// A VOLUMETRIC CASE. Every field M11.a's spike measured is planar — `verticalCells == 1` — so
// `cyFieldVerticalWeight` returns exactly 1.0 and the vertical tap never varies. Reassociating the
// bilinear weight produced a BYTE-IDENTICAL report there: the mutation could not fail. Weather's
// `wind` is Vec3/F32 with four vertical cells and is the case that discriminates; until a suite
// samples it on a device, the blend-order claim three paragraphs up is untested for `dy != 0`.

module field;

/// `CYFD`, and the layout version. A shader compiled against an older layout must fail loudly, so
/// the number it checks is in the buffer — gpu.h's rule, kept.
public static const uint kFieldImageMagic = 0x43594644u;
public static const uint kFieldImageVersion = 1u;
public static const uint kFieldImageHeaderWords = 16u;
public static const uint kFieldImageEntryWords = 4u;
/// `cy::environment::kTileCells`, store.h.
public static const uint kFieldTileCells = 16u;

/// `cy::environment::FieldEncoding`.
public static const uint kFieldEncodingF32 = 0u;
public static const uint kFieldEncodingUNorm8 = 1u;
public static const uint kFieldEncodingUNorm16 = 2u;
public static const uint kFieldEncodingUint8 = 3u;
public static const uint kFieldEncodingUint16 = 4u;

/// `cy::environment::FieldInterpolation::Linear`.
public static const uint kFieldInterpolationLinear = 1u;

/// `cy::environment::FieldLayerRule`.
public static const uint kFieldRuleReplace = 0u;
public static const uint kFieldRuleAdd = 1u;
public static const uint kFieldRuleMultiply = 2u;
public static const uint kFieldRuleMax = 3u;
public static const uint kFieldRuleMin = 4u;

// --- The bindless table -------------------------------------------------------------------------
//
// SET 0, for the reason `cy/material.slang` gives at its own table: `cy/backends/shader/reflection.h`
// requires a bindless global table to appear only in set 0, because "this is a per-frame resource"
// is a fact about the resource rather than about the draw. Binding 3 — 0 is `cyGlobals`, 1 and 2 are
// the material texture table and its sampler.
//
// A `StructuredBuffer<uint>` and NOT a typed one: the payload's element type is a property of the
// DECLARATION, read out of the header at run time, and a shader that had to know it before it read
// the header would need one variant of this module per encoding.
[[vk::binding(3, 0)]]
public StructuredBuffer<uint> cyEnvironmentFields[];

/// One field's image. Constructed from the bindless table by index, or from a buffer a pass bound
/// itself — the arithmetic below is identical either way, which is what "without per-draw binding"
/// buys and what makes this module testable from a compute dispatch that binds one buffer.
public struct CyFieldImage
{
    public StructuredBuffer<uint> words;
};

/// The image at one slot of the GPU scene's field table.
public CyFieldImage cyFieldFromScene(uint slot)
{
    CyFieldImage image;
    image.words = cyEnvironmentFields[slot];
    return image;
}

/// The header, as the sampler reads it. `ImageHeader` in gpu.cpp, field for field.
public struct CyFieldHeader
{
    public uint components;
    public uint encoding;
    public uint interpolation;
    public uint rule;
    public uint verticalCells;
    public float cellMetres;
    public float verticalMetres;
    public float verticalOrigin;
    public float rangeMin;
    public float rangeMax;
    public float defaults[4];
    public uint entries;
    public int originTileX;
    public int originTileZ;
};

/// What one sample answered, and whether any layer held it.
///
/// `resolved` is NOT in the processor sampler's return — `sample_field_image()` answers the declared
/// default and says nothing — and it is here because a consumer that has to tell "the tile said the
/// default" from "there was no tile" cannot do it from the value. A consumer that ignores it reads
/// exactly what the processor reads.
public struct CyFieldSample
{
    public float4 value;
    public bool resolved;
};

public bool cyFieldIsImage(CyFieldImage image)
{
    return image.words[0] == kFieldImageMagic && image.words[1] == kFieldImageVersion;
}

public CyFieldHeader cyFieldReadHeader(CyFieldImage image)
{
    CyFieldHeader header;
    const uint packed = image.words[2];
    header.components = packed & 0xFFu;
    header.encoding = (packed >> 8u) & 0xFFu;
    header.interpolation = (packed >> 16u) & 0xFFu;
    header.rule = (packed >> 24u) & 0xFFu;
    header.verticalCells = image.words[3];
    header.cellMetres = asfloat(image.words[4]);
    header.verticalMetres = asfloat(image.words[5]);
    header.verticalOrigin = asfloat(image.words[6]);
    header.rangeMin = asfloat(image.words[7]);
    header.rangeMax = asfloat(image.words[8]);
    [unroll] for (uint index = 0u; index < 4u; ++index)
    {
        header.defaults[index] = asfloat(image.words[9u + index]);
    }
    header.entries = image.words[13];
    header.originTileX = asint(image.words[14]);
    header.originTileZ = asint(image.words[15]);
    return header;
}

public uint cyFieldEncodingStride(uint encoding)
{
    if (encoding == kFieldEncodingF32)
    {
        return 4u;
    }
    if (encoding == kFieldEncodingUNorm16 || encoding == kFieldEncodingUint16)
    {
        return 2u;
    }
    if (encoding == kFieldEncodingUNorm8 || encoding == kFieldEncodingUint8)
    {
        return 1u;
    }
    return 4u;
}

// --- Byte addressing over a word buffer ---------------------------------------------------------
//
// `decode_point()` in gpu.cpp does `memcpy` from a `const u8*` over the same words, so these three
// are that memcpy on a little-endian machine spelled out. The straddling case in `cyFieldLoadU16` is
// unreachable for every encoding the engine declares — a tile payload is word-aligned and a 16-bit
// component sits at an even offset — and it is written anyway, because a sampler whose correctness
// depends on an alignment nothing asserts is a sampler that breaks the first time a `Vec3`/`Uint8`
// field is declared.

public uint cyFieldLoadByte(CyFieldImage image, uint byteOffset)
{
    return (image.words[byteOffset >> 2u] >> ((byteOffset & 3u) * 8u)) & 0xFFu;
}

public uint cyFieldLoadU16(CyFieldImage image, uint byteOffset)
{
    const uint shift = (byteOffset & 3u) * 8u;
    if (shift <= 16u)
    {
        return (image.words[byteOffset >> 2u] >> shift) & 0xFFFFu;
    }
    return cyFieldLoadByte(image, byteOffset) | (cyFieldLoadByte(image, byteOffset + 1u) << 8u);
}

public uint cyFieldLoadU32(CyFieldImage image, uint byteOffset)
{
    const uint shift = (byteOffset & 3u) * 8u;
    if (shift == 0u)
    {
        return image.words[byteOffset >> 2u];
    }
    const uint low = image.words[byteOffset >> 2u] >> shift;
    return low | (image.words[(byteOffset >> 2u) + 1u] << (32u - shift));
}

/// `decode_point()`. One stored lattice point, with the IMAGE's own encoding and range — never a
/// declaration's, because a shader has no declaration.
public float4 cyFieldDecodePoint(CyFieldImage image, CyFieldHeader header, uint byteOffset)
{
    float4 out = float4(0.0, 0.0, 0.0, 0.0);
    const uint stride = cyFieldEncodingStride(header.encoding);
    const float span = header.rangeMax - header.rangeMin;
    for (uint index = 0u; index < header.components; ++index)
    {
        const uint offset = byteOffset + (index * stride);
        // THE ONE PLACE THIS MODULE CANNOT BE BIT-EXACT WITH THE PROCESSOR, AND IT IS THE DEVICE
        // CONFORMING RATHER THAN DISAGREEING. A quantised branch below divides, and **Vulkan
        // specifies `OpFDiv` to 2.5 ULP rather than to correct rounding**. Measured at a lattice
        // centre, where there is no interpolation arithmetic at all: a stored `UNorm8` byte of 51
        // decodes to 0x3E4CCCCD on the processor and 0x3E4CCCCE on an RTX 5060. One ULP, from the
        // division and from nothing else — `precise` on this variable was tried and changed not one
        // value, which is what identified it. `render.environment_field` therefore bounds the
        // quantised encodings at three ULP at lattice centres and asserts BIT-IDENTITY for `F32`,
        // which decodes with no arithmetic at all.
        float value = 0.0;
        if (header.encoding == kFieldEncodingF32)
        {
            value = asfloat(cyFieldLoadU32(image, offset));
        }
        else if (header.encoding == kFieldEncodingUNorm8)
        {
            value = header.rangeMin + ((float(cyFieldLoadByte(image, offset)) / 255.0) * span);
        }
        else if (header.encoding == kFieldEncodingUNorm16)
        {
            // 65535.0 AND NOT 65536.0. M11.a's spike mutated exactly this constant: every value in
            // the report changed and EVERY FIELD STAYED INSIDE ITS DECLARED PRECISION, so the
            // tolerance bound alone could not see it. Only the bit-exact columns caught it, which is
            // why `render.environment_field` compares ULPs rather than a tolerance.
            value = header.rangeMin + ((float(cyFieldLoadU16(image, offset)) / 65535.0) * span);
        }
        else if (header.encoding == kFieldEncodingUint8)
        {
            value = float(cyFieldLoadByte(image, offset));
        }
        else
        {
            value = float(cyFieldLoadU16(image, offset));
        }
        out[index] = value;
    }
    return out;
}

/// `find_entry()`: a binary search over entries sorted by (layer, z, x), which is what the builder
/// sorts for.
public bool cyFieldFindEntry(CyFieldImage image, CyFieldHeader header, uint layer, int tileX,
                             int tileZ, out uint payloadWords)
{
    payloadWords = 0u;
    uint low = 0u;
    uint high = header.entries;
    while (low < high)
    {
        const uint middle = low + ((high - low) / 2u);
        const uint base = kFieldImageHeaderWords + (middle * kFieldImageEntryWords);
        const uint entryLayer = image.words[base];
        const int entryX = asint(image.words[base + 1u]);
        const int entryZ = asint(image.words[base + 2u]);
        const bool before = (entryLayer < layer) || (entryLayer == layer && entryZ < tileZ) ||
                            (entryLayer == layer && entryZ == tileZ && entryX < tileX);
        if (before)
        {
            low = middle + 1u;
            continue;
        }
        if (entryLayer == layer && entryZ == tileZ && entryX == tileX)
        {
            payloadWords = image.words[base + 3u];
            return true;
        }
        high = middle;
    }
    return false;
}

/// `floor_div_i32()`. NOT `int(floor(float(a) / float(b)))`: that is a different number for large
/// operands once the division rounds, and the tile a corner belongs to is not a place to round.
public int cyFieldFloorDiv(int value, int divisor)
{
    const int quotient = value / divisor;
    return ((value % divisor != 0) && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

public int cyFieldIfloor(float value)
{
    return int(floor(value));
}

public int cyFieldClampI32(int value, int low, int high)
{
    if (value < low)
    {
        return low;
    }
    if (high < value)
    {
        return high;
    }
    return value;
}

/// `vertical_weight()`. A field one cell deep has no vertical axis to blend along, so its single tap
/// carries the whole sample.
public float cyFieldVerticalWeight(uint taps, uint dy, float fy)
{
    if (taps == 1u)
    {
        return 1.0;
    }
    return (dy == 0u) ? (1.0 - fy) : fy;
}

/// `ImageLattice` / `image_lattice()`.
public struct CyFieldLattice
{
    public int i0;
    public int k0;
    public int j0;
    public float fx;
    public float fz;
    public float fy;
    public bool linear;
};

public CyFieldLattice cyFieldLattice(CyFieldHeader header, float x, float y, float z)
{
    CyFieldLattice lattice;
    lattice.i0 = 0;
    lattice.k0 = 0;
    lattice.j0 = 0;
    lattice.fx = 0.0;
    lattice.fz = 0.0;
    lattice.fy = 0.0;
    lattice.linear = header.interpolation == kFieldInterpolationLinear;
    if (lattice.linear)
    {
        // Values sit at cell centres, so the continuous coordinate is offset by half a cell. The
        // expression is the processor's: divide first, then subtract 0.5. Deleting the half-cell
        // offset was one of M11.a's spike mutations — bit-identical agreement went from 23.29% to
        // 0.00% and every field left its declared precision, which is what a real mutation looks
        // like.
        const float u = (x / header.cellMetres) - 0.5;
        const float w = (z / header.cellMetres) - 0.5;
        lattice.i0 = cyFieldIfloor(u);
        lattice.k0 = cyFieldIfloor(w);
        lattice.fx = u - float(lattice.i0);
        lattice.fz = w - float(lattice.k0);
    }
    else
    {
        lattice.i0 = cyFieldIfloor(x / header.cellMetres);
        lattice.k0 = cyFieldIfloor(z / header.cellMetres);
    }

    const float vertical = (y - header.verticalOrigin) / header.verticalMetres;
    if (lattice.linear && header.verticalCells > 1u)
    {
        const float v = vertical - 0.5;
        lattice.j0 = cyFieldIfloor(v);
        lattice.fy = v - float(lattice.j0);
    }
    else
    {
        lattice.j0 = cyFieldIfloor(vertical);
    }
    return lattice;
}

/// `ImageTile`: which tile answered, so a corner that falls outside it is clamped back into it.
public struct CyFieldTile
{
    public uint layer;
    public int x;
    public int z;
    public uint payload;
};

/// `read_image_point()`. The whole of the missing-neighbour rule: a corner whose own tile is not in
/// the table is clamped into the tile that answered, so a sample never faults and never blends
/// against a default it happens to sit next to.
public float4 cyFieldReadPoint(CyFieldImage image, CyFieldHeader header, CyFieldTile centre, int gi,
                               int gk, int gj)
{
    const int cells = int(kFieldTileCells);
    const int j = cyFieldClampI32(gj, 0, int(header.verticalCells) - 1);
    int tileX = cyFieldFloorDiv(gi, cells);
    int tileZ = cyFieldFloorDiv(gk, cells);
    uint payload = centre.payload;
    if (tileX != centre.x || tileZ != centre.z)
    {
        uint found = 0u;
        if (cyFieldFindEntry(image, header, centre.layer, tileX, tileZ, found))
        {
            payload = found;
        }
        else
        {
            payload = centre.payload;
            tileX = centre.x;
            tileZ = centre.z;
        }
    }
    const int lx = cyFieldClampI32(gi - (tileX * cells), 0, cells - 1);
    const int lz = cyFieldClampI32(gk - (tileZ * cells), 0, cells - 1);
    const uint point = (uint(j) * kFieldTileCells * kFieldTileCells) + (uint(lz) * kFieldTileCells) +
                       uint(lx);
    const uint stride = cyFieldEncodingStride(header.encoding) * header.components;
    return cyFieldDecodePoint(image, header, (payload * 4u) + (point * stride));
}

/// `sample_image_layer()`.
public bool cyFieldSampleLayer(CyFieldImage image, CyFieldHeader header, uint layer, float x,
                               float y, float z, out float4 result)
{
    result = float4(0.0, 0.0, 0.0, 0.0);
    const int cells = int(kFieldTileCells);
    CyFieldTile centre;
    centre.layer = layer;
    centre.x = cyFieldFloorDiv(cyFieldIfloor(x / header.cellMetres), cells);
    centre.z = cyFieldFloorDiv(cyFieldIfloor(z / header.cellMetres), cells);
    centre.payload = 0u;
    uint payload = 0u;
    if (!cyFieldFindEntry(image, header, layer, centre.x, centre.z, payload))
    {
        return false;
    }
    centre.payload = payload;

    const CyFieldLattice lattice = cyFieldLattice(header, x, y, z);
    if (!lattice.linear)
    {
        result = cyFieldReadPoint(image, header, centre, lattice.i0, lattice.k0, lattice.j0);
        return true;
    }

    float4 accumulated = float4(0.0, 0.0, 0.0, 0.0);
    const uint verticalTaps = (header.verticalCells > 1u) ? 2u : 1u;
    // dy, then dz, then dx — the processor sampler's order. Summation order is the answer's last
    // bit, and the comparison this module is judged by is bit-exact.
    for (uint dy = 0u; dy < verticalTaps; ++dy)
    {
        const float wy = cyFieldVerticalWeight(verticalTaps, dy, lattice.fy);
        for (uint dz = 0u; dz < 2u; ++dz)
        {
            const float wz = (dz == 0u) ? (1.0 - lattice.fz) : lattice.fz;
            for (uint dx = 0u; dx < 2u; ++dx)
            {
                const float wx = (dx == 0u) ? (1.0 - lattice.fx) : lattice.fx;
                const float4 corner = cyFieldReadPoint(image, header, centre,
                                                       lattice.i0 + int(dx), lattice.k0 + int(dz),
                                                       lattice.j0 + int(dy));
                const float weight = wx * wz * wy;
                for (uint index = 0u; index < header.components; ++index)
                {
                    accumulated[index] += corner[index] * weight;
                }
            }
        }
    }
    result = accumulated;
    return true;
}

/// `combine_in_image()`. The rule byte out of the header, which is what a shader has.
public float4 cyFieldCombine(uint rule, float4 base, float4 delta, uint components)
{
    float4 out = base;
    for (uint index = 0u; index < components; ++index)
    {
        const float a = base[index];
        const float b = delta[index];
        if (rule == kFieldRuleReplace)
        {
            out[index] = b;
        }
        else if (rule == kFieldRuleAdd)
        {
            out[index] = a + b;
        }
        else if (rule == kFieldRuleMultiply)
        {
            out[index] = a * b;
        }
        else if (rule == kFieldRuleMax)
        {
            out[index] = (a > b) ? a : b;
        }
        else if (rule == kFieldRuleMin)
        {
            out[index] = (a < b) ? a : b;
        }
    }
    return out;
}

/// `sample_field_image()`: the whole of it.
///
/// `x` and `z` are metres RELATIVE to the image's origin — see this file's header — and `y` is
/// absolute metres.
public CyFieldSample cyFieldSample(CyFieldImage image, float x, float y, float z)
{
    CyFieldSample answer;
    answer.value = float4(0.0, 0.0, 0.0, 0.0);
    answer.resolved = false;
    if (!cyFieldIsImage(image))
    {
        return answer;
    }
    const CyFieldHeader header = cyFieldReadHeader(image);
    [unroll] for (uint index = 0u; index < 4u; ++index)
    {
        answer.value[index] = header.defaults[index];
    }

    float4 base = float4(0.0, 0.0, 0.0, 0.0);
    float4 delta = float4(0.0, 0.0, 0.0, 0.0);
    const bool hasBase = cyFieldSampleLayer(image, header, 0u, x, y, z, base);
    const bool hasDelta = cyFieldSampleLayer(image, header, 1u, x, y, z, delta);
    if (!hasBase && !hasDelta)
    {
        return answer;
    }
    answer.resolved = true;
    if (!hasBase)
    {
        [unroll] for (uint index = 0u; index < 4u; ++index)
        {
            base[index] = header.defaults[index];
        }
    }
    if (!hasDelta)
    {
        answer.value = base;
        return answer;
    }
    answer.value = cyFieldCombine(header.rule, base, delta, header.components);
    return answer;
}

/// The same sample through the GPU scene's bindless table — the form `environment-fields` asks for,
/// and the only one a draw uses: "a shader can sample a field WITHOUT PER-DRAW BINDING".
public CyFieldSample cyFieldSampleScene(uint slot, float x, float y, float z)
{
    return cyFieldSample(cyFieldFromScene(slot), x, y, z);
}

/// The scalar convenience every consumer of a Scalar field wants, so that a shader reading wetness
/// does not spell `.value.x` and a shader reading wind cannot use this by accident.
public float cyFieldSampleScalar(CyFieldImage image, float x, float y, float z)
{
    return cyFieldSample(image, x, y, z).value.x;
}
