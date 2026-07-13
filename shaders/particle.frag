#version 450 core
// Premultiplied particle shading. Textured batches sample the sprite (with
// flipbook UVs already applied); untextured ones use a procedural soft radial
// disc. Soft fade reads the opaque depth and fades the quad out across the
// last uParams.y meters before it intersects geometry (no hard clip lines in
// smoke/fog). Additive batches blend ONE/ONE; alpha ONE/ONE_MINUS_SRC_ALPHA.
// HDR colors feed the bloom chain.
layout(binding = 0) uniform sampler2D texSprite;
layout(binding = 1) uniform sampler2D texDepth;

layout(std140, binding = 15) uniform U {
    mat4 uViewProj;
    vec4 uParams; // x = textured, y = soft-fade meters, z/w = proj A/B
};

in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;

// Positive view-space distance from hardware depth (see ssao.frag).
float linearDepth(float d) {
    float ndc = d * 2.0 - 1.0;
    return -(uParams.w / (-ndc - uParams.z));
}

void main() {
    vec3 rgb = vColor.rgb;
    float alpha = vColor.a;
    if (uParams.x > 0.5) {
        vec4 s = texture(texSprite, vUV);
        rgb *= s.rgb;
        alpha *= s.a;
    } else {
        float d = length(vUV * 2.0 - 1.0);
        float mask = smoothstep(1.0, 0.25, d);
        mask *= mask; // softer core falloff
        alpha *= mask;
    }

    if (uParams.y > 0.0) {
        float sceneD = linearDepth(texelFetch(texDepth, ivec2(gl_FragCoord.xy), 0).r);
        float fragD = linearDepth(gl_FragCoord.z);
        alpha *= clamp((sceneD - fragD) / uParams.y, 0.0, 1.0);
    }

    fragColor = vec4(rgb * alpha, alpha);
}
