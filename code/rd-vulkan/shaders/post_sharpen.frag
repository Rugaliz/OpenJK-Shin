#version 450

// Contrast adaptive sharpening (r_sharpen), the method of AMD's FidelityFX CAS written from its description (see
// tr_post.h): each pixel is pulled away from its four neighbours by an amount that depends on how much room the 3 x 3
// pixels around it leave before they would clip.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: the weight of the neighbours at full amount (negative)

layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 at(ivec2 base, int x, int y)
{
	const ivec2 size = textureSize(uScene, 0);
	return texelFetch(uScene, clamp(base + ivec2(x, y), ivec2(0), size - 1), 0).rgb;
}

void main()
{
	const ivec2 base = ivec2(gl_FragCoord.xy);
	vec3 a = at(base, -1, -1), b = at(base, 0, -1), c = at(base, 1, -1);
	vec3 d = at(base, -1, 0), e = at(base, 0, 0), f = at(base, 1, 0);
	vec3 g = at(base, -1, 1), h = at(base, 0, 1), i = at(base, 1, 1);
	vec3 mn = min(min(min(d, e), min(f, b)), h);
	vec3 mx = max(max(max(d, e), max(f, b)), h);
	mn += min(min(min(mn, a), min(g, c)), i);	// (the cross and the whole square, soft)
	mx += max(max(max(mx, a), max(g, c)), i);
	vec3 amount = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(0.0001)), 0.0, 1.0));
	vec3 w = amount * p.a.x;
	outColor = vec4(clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
}
