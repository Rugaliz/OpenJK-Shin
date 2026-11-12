#version 460

// Built twice (shaders/build.py): as it is, and with RAY_QUERY as dlight_rt.frag for graphics cards with ray tracing,
// where a ray from the pixel to the light through the level's ray tracing structure (vk_rt.cpp) can keep the light off
// what is behind a wall (the shadows of dynamic lights, r_rayTracing).
#ifdef RAY_QUERY
#extension GL_EXT_ray_query : require
#endif

layout(push_constant) uniform Constants
{
	mat4 mvp;
	vec4 light;
	vec4 colorTextured;
	vec4 alphaTest;
	vec4 lightHalf;		// xyz: a light along a line, from light.xyz to an end (0 for a light at a point)
#ifdef RAY_QUERY
	vec4 toWorld0, toWorld1, toWorld2;	// from the space of the surface to the world (vkDlightShadow_t)
	vec4 shadow;						// x: 1 for the rays, y: the quality (2 High: more rays)
#endif
} pc;

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;
#ifdef RAY_QUERY
layout(set = 1, binding = 0) uniform accelerationStructureEXT uScene;
#define RT_ALPHA_SET 1
#define RT_ALPHA_ATLAS 1
#define RT_ALPHA_DATA 2
#include "rt_alpha.glsl"
#endif

layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

// alphaTest.y: how deep the bumps of the surface are (r_dlightGLSL 2), in units for the brightest texel, 0 flat
float height(vec2 st) { return dot(texture(uDiffuse, st).rgb, vec3(0.299, 0.587, 0.114)) * pc.alphaTest.y; }

#ifdef RAY_QUERY
// A ray query must get numbers (no NaN, no infinity), a direction that is not zero, and an end beyond its start:
// otherwise the GPU can hang (device lost). A NaN comes from a model with a broken point, or a direction of length 0.
bool rayOk(vec3 origin, vec3 dir, float tMin, float tMax)
{
	return !any(isnan(origin)) && !any(isinf(origin)) && !any(isnan(dir)) && !any(isinf(dir)) && dot(dir, dir) > 1e-12 &&
		tMax > tMin && !isinf(tMax);	// (false for a NaN tMax too)
}

// 1 if a ray from wPos (on a surface of normal n, 0 when unknown) gets to target, 0 if something is in between
float seenFrom(vec3 wPos, vec3 n, vec3 target, float tMin)
{
	vec3 d = target - wPos;
	float dist = length(d);
	if (dist - 12.0 <= tMin + 1.0) {
		return 1.0;	// (nothing in between worth a ray; and a ray query must not end before it starts: the GPU can hang)
	}
	vec3 origin = wPos + (dot(n, d) < 0.0 ? -n : n);
	if (!rayOk(origin, d / dist, tMin, dist - 12.0)) return 1.0;
	rayQueryEXT query;
	rayQueryInitializeEXT(query, uScene, gl_RayFlagsTerminateOnFirstHitEXT, 0x07, origin, tMin, d / dist, dist - 12.0);
	RT_PROCEED(query)
	return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
#endif

void main()
{
	float t = clamp(dot(vPos - pc.light.xyz, pc.lightHalf.xyz) / max(dot(pc.lightHalf.xyz, pc.lightHalf.xyz), 1e-6), -1.0, 1.0);
	vec3 toLight = pc.light.xyz + pc.lightHalf.xyz * t - vPos;	// (from the nearest point of the line)
	float dist2 = dot(toLight, toLight);
	float atten = clamp(1.0 - dist2 / (pc.light.w * pc.light.w), 0.0, 1.0);
	float facing = 1.0;
	float nlen2 = dot(vNormal, vNormal);
	// the bumps: the surface texture read as a height, its slope across the pixel tilting the normal (the surface
	// gradient of Mikkelsen, "Bump Mapping Unparametrized Surfaces on the GPU", 2010: no tangents needed)
	vec2 stX = dFdx(vTexCoord), stY = dFdy(vTexCoord);
	vec3 posX = dFdx(vPos), posY = dFdy(vPos);
	float h = 0.0, hX = 0.0, hY = 0.0;
	if (pc.alphaTest.y > 0.0 && pc.colorTextured.a > 0.5) {	// (read outside the branch below, which can differ between pixels)
		h = height(vTexCoord); hX = height(vTexCoord + stX) - h; hY = height(vTexCoord + stY) - h;
	}
	if (nlen2 > 0.0001) {	// the terrain has no normals, it is lit like the old passes did
		vec3 n = vNormal * inversesqrt(nlen2);
		vec3 r1 = cross(posY, n), r2 = cross(n, posX);
		float det = dot(posX, r1);
		vec3 bumpedN = normalize(max(abs(det), 1e-8) * n - sign(det) * (hX * r1 + hY * r2));
		float ndl = dot(n, toLight) * inversesqrt(max(dist2, 0.0001));
		float bumped = dot(bumpedN, toLight) * inversesqrt(max(dist2, 0.0001));
		facing = smoothstep(0.0, 0.25, ndl) * mix(1.0, max(bumped, 0.0), 0.6);
	}
	vec3 light = pc.colorTextured.rgb * (atten * facing);
	vec4 diffuse = vec4(1.0);
	if (pc.colorTextured.a > 0.5) {
		diffuse = texture(uDiffuse, vTexCoord);
	}
#ifdef RAY_QUERY
	// rays from the pixel to the light, in the world, against the level, its brush models and the characters (as they
	// were the frame before, mask 0x04): one to a light at a point, several spread along a light along a line (the
	// soft shadows a long light makes). They stop short of the light, so that what it sits on (an explosion on a wall,
	// the hand on a saber's hilt) does not hide it; from a character (shadow.x 2) they start further out, so that the
	// frame before's copy of it does not shadow it where it has moved since.
	vec4 p4 = vec4(vPos, 1.0), l4 = vec4(pc.light.xyz, 1.0);
	vec3 wPos = vec3(dot(pc.toWorld0, p4), dot(pc.toWorld1, p4), dot(pc.toWorld2, p4));
	vec3 wN = cross(dFdx(wPos), dFdy(wPos));	// (outside the branch: derivatives need every pixel of the quad)
#endif
	if (pc.colorTextured.a > 0.5) {
		float at = pc.alphaTest.x;
		if ((at > 0.5 && at < 1.5 && diffuse.a <= 0.0) ||
			(at > 1.5 && at < 2.5 && diffuse.a >= 0.5) ||
			(at > 2.5 && at < 3.5 && diffuse.a < 0.5) ||
			(at > 3.5 && diffuse.a < 0.75)) {
			discard;	// the stage is cut out here, so it gets no light here (and sends no rays)
		}
		light *= diffuse.rgb;
	}
#ifdef RAY_QUERY
	if (pc.shadow.x > 0.5 && max(light.r, max(light.g, light.b)) > 0.001) {	// (no rays for a light too faint to see)
		vec3 wLight = vec3(dot(pc.toWorld0, l4), dot(pc.toWorld1, l4), dot(pc.toWorld2, l4));
		vec3 wHalf = vec3(dot(pc.toWorld0.xyz, pc.lightHalf.xyz), dot(pc.toWorld1.xyz, pc.lightHalf.xyz), dot(pc.toWorld2.xyz, pc.lightHalf.xyz));
		float tMin = pc.shadow.x > 1.5 ? 8.0 : 0.5;
		vec3 n = dot(wN, wN) > 1e-12 ? normalize(wN) : vec3(0.0);
		float seen;
		if (dot(wHalf, wHalf) > 1e-6) {
			// a line: points spread along it (four, eight at High), moved by a different amount at every pixel. Every
			// other one goes first, over the whole line; only where they disagree (the edge of a shadow) are the ones
			// between them traced too, as elsewhere the whole line is seen, or hidden, from the pixel: half the rays
			// for most of what is lit
			const int count = pc.shadow.y > 1.5 ? 8 : 4;
			float turn = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
			vec3 start = wLight - wHalf, step = wHalf * (2.0 / float(count));
			float first = 0.0;
			for (int i = 0; i < count; i += 2) first += seenFrom(wPos, n, start + step * (float(i) + turn), tMin);
			if (first == 0.0 || first == float(count / 2)) {
				seen = first / float(count / 2);
			} else {
				seen = first;
				for (int i = 1; i < count; i += 2) seen += seenFrom(wPos, n, start + step * (float(i) + turn), tMin);
				seen /= float(count);
			}
		} else {
			seen = seenFrom(wPos, n, wLight, tMin);
		}
		light *= seen;
	}
#endif
	outColor = vec4(light, 1.0);
}
