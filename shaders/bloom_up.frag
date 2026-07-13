#version 450 core
// 9-tap tent upsample, additively blended onto the previous mip (GL_ONE, GL_ONE).
in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texSrc;
layout(std140, binding = 15) uniform U {
    float uRadius;
};

void main() {
    vec2 t = uRadius / vec2(textureSize(texSrc, 0));
    vec3 col = texture(texSrc, vUV).rgb * 4.0;
    col += texture(texSrc, vUV + vec2(-t.x, 0)).rgb * 2.0;
    col += texture(texSrc, vUV + vec2( t.x, 0)).rgb * 2.0;
    col += texture(texSrc, vUV + vec2(0, -t.y)).rgb * 2.0;
    col += texture(texSrc, vUV + vec2(0,  t.y)).rgb * 2.0;
    col += texture(texSrc, vUV + vec2(-t.x, -t.y)).rgb;
    col += texture(texSrc, vUV + vec2( t.x, -t.y)).rgb;
    col += texture(texSrc, vUV + vec2(-t.x,  t.y)).rgb;
    col += texture(texSrc, vUV + vec2( t.x,  t.y)).rgb;
    fragColor = vec4(col / 16.0, 1.0);
}
