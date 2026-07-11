#version 450 core
// Compact FXAA (quality preset) operating on sRGB input with luma in alpha.
in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texSrc;

const float EDGE_THRESHOLD_MIN = 0.0312;
const float EDGE_THRESHOLD_MAX = 0.125;
const float SUBPIXEL_QUALITY = 0.75;
const int ITERATIONS = 8;

float lumaAt(vec2 uv) { return texture(texSrc, uv).a; }

void main() {
    vec2 texel = 1.0 / vec2(textureSize(texSrc, 0));
    vec3 colorCenter = texture(texSrc, vUV).rgb;

    float lumaCenter = lumaAt(vUV);
    float lumaDown  = lumaAt(vUV + vec2(0, -texel.y));
    float lumaUp    = lumaAt(vUV + vec2(0, texel.y));
    float lumaLeft  = lumaAt(vUV + vec2(-texel.x, 0));
    float lumaRight = lumaAt(vUV + vec2(texel.x, 0));

    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD_MAX)) {
        fragColor = vec4(colorCenter, 1.0);
        return;
    }

    float lumaDL = lumaAt(vUV + vec2(-texel.x, -texel.y));
    float lumaUR = lumaAt(vUV + vec2(texel.x, texel.y));
    float lumaUL = lumaAt(vUV + vec2(-texel.x, texel.y));
    float lumaDR = lumaAt(vUV + vec2(texel.x, -texel.y));

    float lumaDownUp = lumaDown + lumaUp;
    float lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners = lumaDL + lumaUL;
    float lumaDownCorners = lumaDL + lumaDR;
    float lumaRightCorners = lumaDR + lumaUR;
    float lumaUpCorners = lumaUR + lumaUL;

    float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) +
                           abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 +
                           abs(-2.0 * lumaRight + lumaRightCorners);
    float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) +
                         abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 +
                         abs(-2.0 * lumaDown + lumaDownCorners);
    bool isHorizontal = edgeHorizontal >= edgeVertical;

    float luma1 = isHorizontal ? lumaDown : lumaLeft;
    float luma2 = isHorizontal ? lumaUp : lumaRight;
    float gradient1 = luma1 - lumaCenter;
    float gradient2 = luma2 - lumaCenter;
    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength = isHorizontal ? texel.y : texel.x;
    float lumaLocalAverage;
    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }

    vec2 currentUv = vUV;
    if (isHorizontal) currentUv.y += stepLength * 0.5;
    else currentUv.x += stepLength * 0.5;

    vec2 offset = isHorizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uv1 = currentUv - offset;
    vec2 uv2 = currentUv + offset;

    float lumaEnd1 = lumaAt(uv1) - lumaLocalAverage;
    float lumaEnd2 = lumaAt(uv2) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;

    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    for (int i = 2; i < ITERATIONS && !(reached1 && reached2); ++i) {
        if (!reached1) { lumaEnd1 = lumaAt(uv1) - lumaLocalAverage; reached1 = abs(lumaEnd1) >= gradientScaled; }
        if (!reached2) { lumaEnd2 = lumaAt(uv2) - lumaLocalAverage; reached2 = abs(lumaEnd2) >= gradientScaled; }
        float q = 1.0 + float(i) * 0.5;
        if (!reached1) uv1 -= offset * q;
        if (!reached2) uv2 += offset * q;
    }

    float distance1 = isHorizontal ? (vUV.x - uv1.x) : (vUV.y - uv1.y);
    float distance2 = isHorizontal ? (uv2.x - vUV.x) : (uv2.y - vUV.y);
    bool isDirection1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);
    float edgeThickness = distance1 + distance2;

    bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
    bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
    float pixelOffset = correctVariation ? (-distanceFinal / edgeThickness + 0.5) : 0.0;

    // Sub-pixel anti-aliasing.
    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) +
                        lumaLeftCorners + lumaRightCorners);
    float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
    float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * SUBPIXEL_QUALITY;
    pixelOffset = max(pixelOffset, subPixelOffsetFinal);

    vec2 finalUv = vUV;
    if (isHorizontal) finalUv.y += pixelOffset * stepLength;
    else finalUv.x += pixelOffset * stepLength;

    fragColor = vec4(texture(texSrc, finalUv).rgb, 1.0);
}
