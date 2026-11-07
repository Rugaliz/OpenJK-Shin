#version 460
#extension GL_EXT_ray_query : require

// r_rtDebug: from every pixel of the view, a ray towards the sun of the level through the ray tracing structures of
// the level (vk_rt.cpp). Where it is blocked, or the surface faces away from the sun, the pixel is darkened: these
// shadows should be where the level's own lighting has them. 1 darkens the view, 2 shows the result on its own.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 row0; vec4 row1; vec4 row2; vec4 sun; vec4 unused; } p;
// a: proj [0] [5] [8] [9]; b: proj [10] [14], depth uv scale; c: depth uv bias; row0-2: from the eye to the world;
// sun: the direction to the sun in the world

layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 3) uniform accelerationStructureEXT uScene;
#define RT_ALPHA_SET 0
#define RT_ALPHA_ATLAS 4
#define RT_ALPHA_DATA 5
#include "rt_alpha.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float depthAt(vec2 t) { return texture(uDepth, t * p.b.zw + p.c.xy).r; }
float viewZ(float depth) { return -p.b.y / (depth * 2.0 - 1.0 + p.b.x); }

void main()
{
	float depth = depthAt(vUV);
	if (depth >= 1.0 || depth < 0.3) { outColor = vec4(1.0); return; }	// the sky, the weapon
	float z = viewZ(depth);
	vec2 ndc = vec2(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0);
	vec4 eye = vec4(-(ndc.x + p.a.z) * z / p.a.x, -(ndc.y + p.a.w) * z / p.a.y, z, 1.0);
	vec3 world = vec3(dot(p.row0, eye), dot(p.row1, eye), dot(p.row2, eye));

	// the surface, from how the position changes across the pixel, towards the eye
	vec3 n = cross(dFdx(world), dFdy(world));
	n = dot(n, n) > 1e-12 ? normalize(n) : vec3(0.0, 0.0, 1.0);	// (no NaN into a ray query: the GPU can hang)
	vec3 toEye = vec3(p.row0.w, p.row1.w, p.row2.w) - world;
	if (dot(n, toEye) < 0.0) n = -n;

	float lit = 0.0;
	if (dot(n, p.sun.xyz) > 0.0) {
		rayQueryEXT query;
		rayQueryInitializeEXT(query, uScene, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF,
			world + n * 1.0, 0.5, p.sun.xyz, 32768.0);
		RT_PROCEED(query)
		lit = rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
	}
	float f = p.unused.x > 1.5 ? mix(0.15, 1.0, lit) : mix(0.45, 1.0, lit);
	outColor = vec4(f, f, f, 1.0);
}
