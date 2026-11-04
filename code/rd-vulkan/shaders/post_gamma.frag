#version 450

// The finished picture with the brightness (r_gamma) and the overbright lighting (r_overBrightBits), as the gamma
// ramps of the original games did it: the table of R_SetColorMappings, rounding included (see tr_post.h).

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: 1 / gamma, 2 ^ overbright

layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
	vec3 c = floor(texture(uScene, vUV).rgb * 255.0 + 0.5);
	c = floor(255.0 * pow(c / 255.0, vec3(p.a.x)) + 0.5) * p.a.y;
	outColor = vec4(min(c, vec3(255.0)) / 255.0, 1.0);
}
