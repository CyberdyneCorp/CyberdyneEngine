// Light evaluation. Part of the engine's Slang standard library (`shader-system`).
//
// PHYSICAL UNITS, which is what `rendering-lights-and-shadows` requires: a punctual light carries
// luminous intensity in candela and an area light luminance in nits. The inverse-square falloff is
// the physical one, with a smooth window rather than a hard cut so a light does not pop as it
// leaves its range.

module light;

public static const uint kLightDirectional = 0;
public static const uint kLightPoint = 1;
public static const uint kLightSpot = 2;
/// A spot that falls off as the cosine from its axis, not its square: a small Lambertian emitter
/// (a GI emitter promoted to a light of the frame). `kGpuLightEmitter` on the CPU.
public static const uint kLightEmitter = 3;
/// The four area kinds. `kGpuLightRect` and on, in `rendering/lighting/lights.h`.
public static const uint kLightRect = 4;
public static const uint kLightDisc = 5;
public static const uint kLightSphere = 6;
public static const uint kLightTube = 7;
/// A static light's placeholder: baked whole, shaded by no loop. `kGpuLightBaked` on the CPU.
public static const uint kLightBaked = 8;
/// `kind`'s low byte is the kind; the bits above it are flags no punctual light sets.
public static const uint kLightKindMask = 0xFFu;
/// A rect or a disc that emits from both faces.
public static const uint kLightTwoSided = 0x100u;

/// One light, as the GPU scene publishes it.
///
/// CAMERA-RELATIVE (design.md §3): `position` is relative to the camera, never world space. That is
/// a decision this struct records rather than a comment somewhere else, because a world-space
/// position here is precisely the field that would work at the origin and jitter a million units
/// out.
public struct Light
{
    public float3 positionRelativeToCamera;
    public float range;
    public float3 direction;
    public float intensity;      // candela for punctual lights
    public float3 color;         // linear, unit luminance
    public uint kind;
    /// A spot's smooth cone attenuation, precomputed on the CPU. An area light's two half-extents.
    public float2 spotScaleBias;
    /// An area light's width axis, octahedral (`encode_area_tangent`). A punctual light's shadow
    /// slot, which no shader reads.
    public uint shapeWord;
    /// The light channels this light reaches: read by the assignment, never by the shading loop.
    public uint layerMask;
};

/// The sample one light contributes at a shading point.
public struct LightSample
{
    public float3 direction;    // towards the light, normalised
    public float3 illuminance;  // colour times attenuated intensity
    public float attenuation;
};

/// Inverse-square falloff windowed to `range`, so the light reaches zero at its bound rather than
/// being clipped there.
float distanceAttenuation(float distanceSquared, float range)
{
    let factor = distanceSquared / max(range * range, 1e-6);
    let smoothWindow = saturate(1.0 - factor * factor);
    return (smoothWindow * smoothWindow) / max(distanceSquared, 1e-4);
}

public LightSample evaluateLight(Light light, float3 surfaceRelativeToCamera)
{
    LightSample result;
    if (light.kind == kLightDirectional)
    {
        result.direction = -light.direction;
        result.attenuation = 1.0;
        result.illuminance = light.color * light.intensity;
        return result;
    }

    let toLight = light.positionRelativeToCamera - surfaceRelativeToCamera;
    let distanceSquared = dot(toLight, toLight);
    result.direction = toLight * rsqrt(max(distanceSquared, 1e-8));
    result.attenuation = distanceAttenuation(distanceSquared, light.range);

    if (light.kind == kLightSpot)
    {
        let cosine = dot(-result.direction, light.direction);
        let cone = saturate(cosine * light.spotScaleBias.x + light.spotScaleBias.y);
        result.attenuation *= cone * cone;
    }
    else if (light.kind == kLightEmitter)
    {
        let cosine = dot(-result.direction, light.direction);
        result.attenuation *= saturate(cosine * light.spotScaleBias.x + light.spotScaleBias.y);
    }
    result.illuminance = light.color * (light.intensity * result.attenuation);
    return result;
}

// ================================================================================================
// AREA LIGHTS — the device half of `rendering/lighting/area.h` and `area_shading.h`
// ================================================================================================
//
// THE C++ IS THE REFERENCE, AND THIS IS A TRANSCRIPTION OF IT IN ITS ORDER: the quad an emitter
// presents to the shading point (`area_light_quad`), the horizon clip and the closed-form cosine
// integral over a spherical polygon (`signed_cosine_polygon`), the two-sided rule, and the window
// that ends the light at its reach. `render.area_lights` holds a device frame to
// `shade_area_light` pixel for pixel; a change here is a change there.
//
// The LTC table is the frame's (a texture slot `cy/frame.slang` reads), so what this module takes
// is the entry already fetched: `AreaLtc`, the inverse transform's four free terms and the split
// sum's two.

/// Whether a record's kind is one of the four area kinds.
public bool isAreaLight(uint kind)
{
    let base = kind & kLightKindMask;
    return base >= kLightRect && base <= kLightTube;
}

float signNotZero(float value)
{
    return value >= 0.0 ? 1.0 : -1.0;
}

/// `decode_area_tangent`: two signed 16-bit halves, octahedral.
public float3 decodeAreaTangent(uint word)
{
    let x = max(float(int(word << 16) >> 16) / 32767.0, -1.0);
    let y = max(float(int(word) >> 16) / 32767.0, -1.0);
    var unit = float3(x, y, 1.0 - abs(x) - abs(y));
    if (unit.z < 0.0)
    {
        let foldedX = (1.0 - abs(unit.y)) * signNotZero(unit.x);
        let foldedY = (1.0 - abs(unit.x)) * signNotZero(unit.y);
        unit.x = foldedX;
        unit.y = foldedY;
    }
    let lengthSquared = dot(unit, unit);
    return lengthSquared > 0.0 ? unit * rsqrt(lengthSquared) : float3(1.0, 0.0, 0.0);
}

/// One fetched LTC entry: `Minv = [a 0 b; 0 c 0; d 0 1]` in the shading frame, and the lobe's
/// energy with a Fresnel of one.
public struct AreaLtc
{
    public float4 transform;  // a, b, c, d
    public float magnitude;
};

/// The emitter as a quad about the shading point: four corners relative to it, and whether the
/// shading point is on the emitting side.
public struct AreaQuad
{
    public float3 corner0;
    public float3 corner1;
    public float3 corner2;
    public float3 corner3;
    public bool lit;
};

float3 unitOr(float3 value, float3 fallback)
{
    let lengthSquared = dot(value, value);
    return lengthSquared > 1e-20 ? value * rsqrt(lengthSquared) : fallback;
}

/// `facing_perpendicular`: perpendicular to `axis`, preferring the one perpendicular to `towards`.
float3 facingPerpendicular(float3 axis, float3 towards)
{
    let fallback = abs(axis.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    return unitOr(cross(axis, towards), normalize(cross(axis, fallback)));
}

/// `area_light_from_record` followed by `area_light_quad`.
public AreaQuad areaLightQuad(Light light, float3 surfaceRelativeToCamera)
{
    static const float kDiscToQuad = 0.8862269255;  // sqrt(pi) / 2: equal AREA
    let kind = light.kind & kLightKindMask;
    let center = light.positionRelativeToCamera - surfaceRelativeToCamera;
    let normal = unitOr(light.direction, float3(0.0, -1.0, 0.0));
    var axisX = float3(1.0, 0.0, 0.0);
    var axisY = float3(0.0, 1.0, 0.0);
    var extentX = light.spotScaleBias.x;
    var extentY = light.spotScaleBias.y;
    var singleSided = (light.kind & kLightTwoSided) == 0u;
    if (kind == kLightRect || kind == kLightDisc)
    {
        let tangent = decodeAreaTangent(light.shapeWord);
        axisX = unitOr(tangent - normal * dot(tangent, normal), facingPerpendicular(normal, center));
        axisY = cross(normal, axisX);
        if (kind == kLightDisc)
        {
            extentX *= kDiscToQuad;
            extentY *= kDiscToQuad;
        }
    }
    else if (kind == kLightTube)
    {
        // The silhouette: `spotScaleBias` is (half length, radius), and the quad is the radius
        // across and the half length along the axis, turned to face the point.
        extentX = light.spotScaleBias.y;
        extentY = light.spotScaleBias.x;
        axisY = normal;
        axisX = facingPerpendicular(normal, center);
        singleSided = false;
    }
    else
    {
        // A sphere presents its silhouette disc, facing the point.
        extentX *= kDiscToQuad;
        extentY = extentX;
        let forward = unitOr(center, float3(0.0, 0.0, 1.0));
        axisX = unitOr(cross(float3(0.0, 0.0, 1.0), forward), float3(1.0, 0.0, 0.0));
        axisY = cross(forward, axisX);
        singleSided = false;
    }
    AreaQuad quad;
    quad.corner0 = center - axisX * extentX - axisY * extentY;
    quad.corner1 = center + axisX * extentX - axisY * extentY;
    quad.corner2 = center + axisX * extentX + axisY * extentY;
    quad.corner3 = center - axisX * extentX + axisY * extentY;
    // `ltc_evaluate_diffuse`'s single-sided test: on the emitter's normal against the direction to
    // the shading point, never on the surface's normal.
    quad.lit = !singleSided || dot(cross(axisX, axisY), center) <= 0.0;
    return quad;
}

/// `edge_vector`: the angle between two unit vectors along their normalised cross product.
float3 edgeVector(float3 a, float3 b)
{
    let cosine = clamp(dot(a, b), -1.0, 1.0);
    let theta = acos(cosine);
    let axis = cross(a, b);
    let sine = length(axis);
    let ratio = sine > 1e-6 ? theta / sine : 1.0;
    return axis * ratio;
}

/// `integrate_cosine_polygon_vector` over four corners already in the integration frame (+Z the
/// lobe's axis): normalise, clip to z > 0, normalise again, and sum the edge vectors. `integral`
/// is the form factor whichever way the corners wind; `direction` the mean direction they are seen
/// in.
public struct PolygonIntegral
{
    public float integral;
    public float3 direction;
};

public PolygonIntegral cosinePolygonVector(float3 c0, float3 c1, float3 c2, float3 c3)
{
    PolygonIntegral result;
    result.integral = 0.0;
    result.direction = float3(0.0, 0.0, 1.0);
    float3 input[4] = { unitOr(c0, float3(0.0, 0.0, 1.0)), unitOr(c1, float3(0.0, 0.0, 1.0)),
                        unitOr(c2, float3(0.0, 0.0, 1.0)), unitOr(c3, float3(0.0, 0.0, 1.0)) };
    float3 clipped[5];
    uint written = 0u;
    for (uint index = 0u; index < 4u; ++index)
    {
        let current = input[index];
        let next = input[(index + 1u) % 4u];
        let currentAbove = current.z > 0.0;
        let nextAbove = next.z > 0.0;
        if (currentAbove)
        {
            clipped[written] = current;
            written += 1u;
        }
        if (currentAbove != nextAbove)
        {
            let t = current.z / (current.z - next.z);
            clipped[written] = current + (next - current) * t;
            written += 1u;
        }
    }
    if (written < 3u)
    {
        return result;
    }
    float3 sum = float3(0.0);
    for (uint index = 0u; index < written; ++index)
    {
        let a = unitOr(clipped[index], float3(0.0, 0.0, 1.0));
        let b = unitOr(clipped[(index + 1u) % written], float3(0.0, 0.0, 1.0));
        sum += edgeVector(a, b);
    }
    if (sum.z < 0.0)
    {
        sum = -sum;
    }
    result.integral = sum.z / (2.0 * 3.14159265358979);
    result.direction = unitOr(sum, float3(0.0, 0.0, 1.0));
    return result;
}

/// The form factor alone.
public float cosinePolygonIntegral(float3 c0, float3 c1, float3 c2, float3 c3)
{
    return cosinePolygonVector(c0, c1, c2, c3).integral;
}

/// `ltc_evaluate_diffuse`: the clamped-cosine integral in a frame built from the normal alone.
public float areaFormFactor(AreaQuad quad, float3 normal)
{
    if (!quad.lit)
    {
        return 0.0;
    }
    let up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    let tangent = unitOr(cross(up, normal), float3(1.0, 0.0, 0.0));
    let bitangent = cross(normal, tangent);
    return cosinePolygonIntegral(
        float3(dot(quad.corner0, tangent), dot(quad.corner0, bitangent), dot(quad.corner0, normal)),
        float3(dot(quad.corner1, tangent), dot(quad.corner1, bitangent), dot(quad.corner1, normal)),
        float3(dot(quad.corner2, tangent), dot(quad.corner2, bitangent), dot(quad.corner2, normal)),
        float3(dot(quad.corner3, tangent), dot(quad.corner3, bitangent), dot(quad.corner3, normal)));
}

/// The LTC's polygon integral, and the direction the GGX lobe gathers the light from in the
/// shading space: `shade_area_light`'s specular half up to the Fresnel. The frame whose +X is the
/// view's tangential part.
public PolygonIntegral areaSpecularIntegral(AreaQuad quad, float3 normal, float3 view, AreaLtc ltc)
{
    PolygonIntegral result;
    result.integral = 0.0;
    result.direction = normal;
    if (!quad.lit)
    {
        return result;
    }
    let cosTheta = clamp(dot(normal, view), 1e-3, 1.0);
    let tangent = unitOr(view - normal * cosTheta, float3(1.0, 0.0, 0.0));
    let bitangent = cross(normal, tangent);
    let m = ltc.transform;
    float3 local[4];
    float3 corners[4] = { quad.corner0, quad.corner1, quad.corner2, quad.corner3 };
    for (uint index = 0u; index < 4u; ++index)
    {
        let corner = corners[index];
        let shading = float3(dot(corner, tangent), dot(corner, bitangent), dot(corner, normal));
        // Minv's columns are (a, 0, d), (0, c, 0), (b, 0, 1).
        local[index] = float3(m.x * shading.x + m.y * shading.z, m.z * shading.y,
                              m.w * shading.x + shading.z);
    }
    let polygon = cosinePolygonVector(local[0], local[1], local[2], local[3]);
    // `lobe_direction`: back through M = Minv^-1, in closed form for the sparse Minv.
    var gathered = polygon.direction;
    let determinant = m.x - m.y * m.w;
    if (abs(determinant) >= 1e-8 && abs(m.z) >= 1e-8)
    {
        gathered = unitOr(float3((gathered.x - m.y * gathered.z) / determinant, gathered.y / m.z,
                                 (m.x * gathered.z - m.w * gathered.x) / determinant),
                          gathered);
    }
    result.integral = polygon.integral;
    result.direction =
        unitOr(tangent * gathered.x + bitangent * gathered.y + normal * gathered.z, normal);
    return result;
}

/// `area_light_window`.
public float areaLightWindow(Light light, float3 surfaceRelativeToCamera)
{
    let offset = light.positionRelativeToCamera - surfaceRelativeToCamera;
    let factor = dot(offset, offset) / max(light.range * light.range, 1e-6);
    let window = saturate(1.0 - factor * factor);
    return window * window;
}

/// What one area light adds: `shade_area_light`'s two terms.
public struct AreaShading
{
    public float3 diffuse;
    public float3 specular;
};

/// `shade_area_light`. `hasLtc` false shades the diffuse term only, as an unbuilt table does.
public AreaShading shadeAreaLight(Light light, float3 surfaceRelativeToCamera, float3 normal,
                                  float3 view, float3 albedo, float roughness, float metallic,
                                  bool hasLtc, AreaLtc ltc)
{
    AreaShading result;
    result.diffuse = float3(0.0);
    result.specular = float3(0.0);
    let window = areaLightWindow(light, surfaceRelativeToCamera);
    if (window <= 0.0)
    {
        return result;
    }
    let quad = areaLightQuad(light, surfaceRelativeToCamera);
    let radiance = light.color * (light.intensity * window);
    result.diffuse = albedo * (1.0 - metallic) * radiance * areaFormFactor(quad, normal);
    if (!hasLtc)
    {
        return result;
    }
    // Fresnel where the lobe gathers this light from, not the hemisphere's split sum: see
    // `area_shading.h`.
    let polygon = areaSpecularIntegral(quad, normal, view, ltc);
    let half = unitOr(view + polygon.direction, normal);
    let schlick = pow(1.0 - saturate(dot(view, half)), 5.0);
    let f0 = lerp(float3(0.04), albedo, metallic);
    let fresnel = f0 + (1.0 - f0) * schlick;
    result.specular = fresnel * radiance * (polygon.integral * ltc.magnitude);
    return result;
}

/// `light_irradiance`: the unshadowed irradiance, in lux, a surface facing `normal` receives from
/// one local light — `pi L` times the form factor for an area light, the windowed inverse square
/// and the cone for a punctual one. Zero for a directional light and a baked placeholder. The
/// stochastic path's target function.
public float3 lightIrradiance(Light light, float3 surfaceRelativeToCamera, float3 normal)
{
    let kind = light.kind & kLightKindMask;
    if (kind == kLightDirectional || kind == kLightBaked)
    {
        return float3(0.0);
    }
    if (isAreaLight(light.kind))
    {
        let window = areaLightWindow(light, surfaceRelativeToCamera);
        let quad = areaLightQuad(light, surfaceRelativeToCamera);
        return light.color * (light.intensity * 3.14159265358979 * areaFormFactor(quad, normal) *
                              window);
    }
    let sample = evaluateLight(light, surfaceRelativeToCamera);
    return sample.illuminance * saturate(dot(normal, sample.direction));
}

/// `sample_light_point`: a point on an area light's surface for two uniform numbers, the silhouette
/// a sphere or a tube presents to `towards`; a punctual light's position.
public float3 sampleLightPoint(Light light, float3 towards, float u, float v)
{
    let center = light.positionRelativeToCamera;
    if (!isAreaLight(light.kind))
    {
        return center;
    }
    let kind = light.kind & kLightKindMask;
    let su = 2.0 * u - 1.0;
    let sv = 2.0 * v - 1.0;
    let toward = center - towards;
    let normal = unitOr(light.direction, float3(0.0, -1.0, 0.0));
    if (kind == kLightRect)
    {
        let tangent = decodeAreaTangent(light.shapeWord);
        let axisX = unitOr(tangent - normal * dot(tangent, normal), facingPerpendicular(normal, toward));
        let axisY = cross(normal, axisX);
        return center + axisX * (su * light.spotScaleBias.x) + axisY * (sv * light.spotScaleBias.y);
    }
    if (kind == kLightTube)
    {
        let across = facingPerpendicular(normal, toward);
        return center + across * (su * light.spotScaleBias.y) + normal * (sv * light.spotScaleBias.x);
    }
    var axisX = float3(1.0, 0.0, 0.0);
    var axisY = float3(0.0, 1.0, 0.0);
    if (kind == kLightDisc)
    {
        let tangent = decodeAreaTangent(light.shapeWord);
        axisX = unitOr(tangent - normal * dot(tangent, normal), facingPerpendicular(normal, toward));
        axisY = cross(normal, axisX);
    }
    else
    {
        let facing = unitOr(towards - center, float3(0.0, 0.0, 1.0));
        axisX = facingPerpendicular(facing, float3(0.0, 1.0, 0.0));
        axisY = cross(facing, axisX);
    }
    let radius = light.spotScaleBias.x * sqrt(u);
    let angle = 6.28318530717959 * v;
    return center + axisX * (radius * cos(angle)) + axisY * (radius * sin(angle));
}
