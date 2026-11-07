#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require

// The shadows of the characters, ray traced (cg_shadows 5), all of them in one pass at half the size of the view. A
// pixel is tested against the area each character's shadow can fall in (as for its shadow map: the character seen from
// its light, padded), and only for those it is in, a few rays spread around the direction of that character's light go
// against the triangles of the characters (vk_rt.cpp, instance mask 0x04). The result is the factor the picture is
// multiplied by; the rays are few, a blur along the surfaces follows (post_ssao_blur.frag).

struct Caster {
	vec4 rowS, rowT;	// from the eye to where the character is seen from its light (as in its shadow map)
	vec4 area;			// that area: its corners (s, t)
	vec4 light;			// the direction to the light (world), how wide the rays spread (a tangent)
	vec4 params;		// the floor under it, darkness
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Casters { Caster c[]; };

layout(push_constant) uniform P {
	vec4 a; vec4 b; vec4 c; vec4 row0; vec4 row1; vec4 row2;
	Casters casters; uint count; uint pad;
} p;
// a: proj [0] [5] [8] [9]; b: proj [10] [14], depth uv scale; c: depth uv bias; row0-2: from the eye to the world

layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 3) uniform accelerationStructureEXT uScene;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const int RAYS = 4;

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

	vec3 n = cross(dFdx(world), dFdy(world));
	n = dot(n, n) > 1e-12 ? normalize(n) : vec3(0.0, 0.0, 1.0);	// (no NaN into a ray query: the GPU can hang)
	vec3 toEye = vec3(p.row0.w, p.row1.w, p.row2.w) - world;
	if (dot(n, toEye) < 0.0) n = -n;
	float turn = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

	float f = 1.0;
	for (uint i = 0u; i < p.count; i++) {
		Caster c = p.casters.c[i];
		vec2 st = vec2(dot(c.rowS, eye), dot(c.rowT, eye));
		if (world.z < c.params.x || any(lessThan(st, c.area.xy)) || any(greaterThan(st, c.area.zw))) continue;
		vec3 l = normalize(c.light.xyz);
		if (dot(n, l) <= 0.0) continue;	// facing away from the light: dark in the level's own lighting already

		// the rays: around the light's direction in a disc, turned by a different amount at every pixel
		vec3 side = normalize(abs(l.z) < 0.9 ? cross(l, vec3(0.0, 0.0, 1.0)) : cross(l, vec3(1.0, 0.0, 0.0)));
		vec3 up = cross(side, l);
		float blocked = 0.0;
		for (int r = 0; r < RAYS; r++) {
			float angle = turn + float(r) * 2.3999632;
			float radius = sqrt((float(r) + 0.5) / float(RAYS)) * c.light.w;
			vec3 dir = normalize(l + (cos(angle) * side + sin(angle) * up) * radius);
			rayQueryEXT query;
			rayQueryInitializeEXT(query, uScene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x04,
				world + n * 0.5, 0.5, dir, 4096.0);
			while (rayQueryProceedEXT(query)) {}
			if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) blocked += 1.0;
		}
		f *= 1.0 - c.params.y * blocked / float(RAYS);
	}
	outColor = vec4(f, f, f, 1.0);
}
