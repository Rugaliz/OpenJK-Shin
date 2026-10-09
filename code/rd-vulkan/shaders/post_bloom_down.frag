#version 450

// Shrinks the source to half size with 13 taps. On the first level only what is brighter than the threshold goes on.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: texel size of the source, threshold, knee; b: 1 on the first level

layout(set = 0, binding = 0) uniform sampler2D uSource;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 tap(vec2 offset)
{
	vec3 c = texture(uSource, vUV + offset * p.a.xy).rgb;
	if (p.b.x > 0.5) {
		float brightness = max(c.r, max(c.g, c.b));
		float soft = clamp(brightness - p.a.z + p.a.w, 0.0, 2.0 * p.a.w);
		soft = soft * soft / (4.0 * p.a.w + 0.0001);
		c *= max(soft, brightness - p.a.z) / max(brightness, 0.0001);
	}
	return c;
}

void main()
{
	vec3 a = tap(vec2(-2.0, -2.0)), b = tap(vec2(0.0, -2.0)), c = tap(vec2(2.0, -2.0));
	vec3 d = tap(vec2(-2.0, 0.0)), e = tap(vec2(0.0, 0.0)), f = tap(vec2(2.0, 0.0));
	vec3 g = tap(vec2(-2.0, 2.0)), h = tap(vec2(0.0, 2.0)), i = tap(vec2(2.0, 2.0));
	vec3 j = tap(vec2(-1.0, -1.0)), k = tap(vec2(1.0, -1.0));
	vec3 l = tap(vec2(-1.0, 1.0)), m = tap(vec2(1.0, 1.0));
	vec3 colour = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
	outColor = vec4(colour, 1.0);
}
