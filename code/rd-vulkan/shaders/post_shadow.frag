#version 450

// The shadow of one character from its shadow map (cg_shadows 4): the Vulkan version of the shader in tr_post.h, with
// the view's positions found the way post_ssao.frag finds them ("t" from the top left corner). The result is the factor
// the picture is multiplied by.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 rowS; vec4 rowT; vec4 rowN; vec4 rowZ; vec4 h; } p;
// a: proj [0] [5] [8] [9]; b: proj [10] [14], depth uv scale; c: depth uv bias, the floor, darkness; h: bias, softness,
// the corner of the caster's tile in the atlas

layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler2D uAtlas;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float texel = 1.0 / 512.0;	// (SHADOWMAP_ATLAS, SHADOWMAP_TILES, SHADOWMAP_TILE)

float depthAt(vec2 t) { return texture(uDepth, t * p.b.zw + p.c.xy).r; }
float viewZ(float depth) { return -p.b.y / (depth * 2.0 - 1.0 + p.b.x); }
vec3 viewPos(vec2 t)
{
	float z = viewZ(depthAt(t));
	vec2 ndc = vec2(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0);
	return vec3(-(ndc.x + p.a.z) * z / p.a.x, -(ndc.y + p.a.w) * z / p.a.y, z);
}

float casterAt(vec2 st, vec2 lo, vec2 hi)
{
	float r = texture(uAtlas, clamp(st, lo, hi)).r * 255.0;
	return r < 0.5 ? -1.0 : (r - 1.0) / 254.0;
}

void main()
{
	vec2 uv = vUV;
	float depth = depthAt(uv);
	if (depth >= 1.0 || depth < 0.3) { outColor = vec4(1.0); return; }	// the sky, the weapon
	vec4 e = vec4(viewPos(uv), 1.0);
	if (dot(p.rowZ, e) < p.c.z) { outColor = vec4(1.0); return; }
	vec2 st = vec2(dot(p.rowS, e), dot(p.rowT, e));
	vec2 lo = p.h.zw + 0.5 * texel;
	vec2 hi = p.h.zw + (128.0 - 0.5) * texel;
	if (st.x < lo.x || st.y < lo.y || st.x > hi.x || st.y > hi.y) { outColor = vec4(1.0); return; }
	float n = dot(p.rowN, e);
	float blocker = 0.0, found = 0.0;
	for (int i = 0; i < 5; i++) {
		vec2 o = vec2(float(i == 1) - float(i == 2), float(i == 3) - float(i == 4)) * 6.0;
		float c = casterAt(st + o * texel, lo, hi);
		if (c >= 0.0 && n < c - p.h.x) { blocker += c; found += 1.0; }
	}
	if (found < 0.5) { outColor = vec4(1.0); return; }
	float radius = clamp(0.75 + (blocker / found - n) * p.h.y, 0.75, 10.0);
	float shadow = 0.0;
	for (int i = 0; i < 12; i++) {
		float an = float(i) * 2.39996323, r = sqrt((float(i) + 0.5) / 12.0) * radius;
		float c = casterAt(st + vec2(cos(an), sin(an)) * r * texel, lo, hi);
		if (c >= 0.0 && n < c - p.h.x) shadow += 1.0;
	}
	float f = 1.0 - p.c.w * shadow / 12.0;
	outColor = vec4(f, f, f, 1.0);
}
