#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require

// The shadows of the characters, ray traced (r_rayTracing), all of them in one pass: at Medium at half the size of the
// view, at High at its full size. Each character has a few entries (tr_shadow_volumes.h, RB_ShadowRtEntries): one for
// each lamp of the level that lights it, or one for the light grid's direction where there are none, and a contact
// shadow. A pixel is tested against the area an entry's shadow can fall in (the character seen from its light,
// padded), and only for those it is in, rays go against the triangles of the characters (vk_rt.cpp, instance mask
// 0x04): towards the light, spread over its size (two at Medium, four at High, and as many more between them where
// those disagree: the edge of a shadow), or for the contact shadow a few short ones all around the surface. The result
// is the factor the picture is multiplied by; the rays are few, a blur along the surfaces follows (post_ssao_blur.frag).

struct Caster {
	vec4 rowS, rowT;	// from the eye to where the character is seen from the light
	vec4 area;			// that area: its corners (s, t)
	vec4 light;			// a direction: towards it (world), how wide the rays spread (a tangent); a lamp: its position,
						// its radius; the contact shadow: the middle of the character, how far its rays go
	vec4 params;		// the floor under it, darkness, the type (0 direction, 1 lamp, 2 contact), for the contact
						// shadow how far from the middle it reaches, for a lamp how far the character is from it
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Casters { Caster c[]; };

layout(push_constant) uniform P {
	vec4 a; vec4 b; vec4 c; vec4 row0; vec4 row1; vec4 row2;
	Casters casters; uint count; uint quality;
} p;
// a: proj [0] [5] [8] [9]; b: proj [10] [14], depth uv scale; c: depth uv bias; row0-2: from the eye to the world

layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 3) uniform accelerationStructureEXT uScene;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float depthAt(vec2 t) { return texture(uDepth, t * p.b.zw + p.c.xy).r; }
float viewZ(float depth) { return -p.b.y / (depth * 2.0 - 1.0 + p.b.x); }

// A ray query must get numbers (no NaN, no infinity), a direction that is not zero, and an end beyond its start:
// otherwise the GPU can hang (device lost). A NaN comes from a model with a broken point, or a direction of length 0.
bool rayOk(vec3 origin, vec3 dir, float tMin, float tMax)
{
	return !any(isnan(origin)) && !any(isinf(origin)) && !any(isnan(dir)) && !any(isinf(dir)) && dot(dir, dir) > 1e-12 &&
		tMax > tMin && !isinf(tMax);	// (false for a NaN tMax too)
}

// 1 if the ray from origin along dir hits a character before tMax; blockedTowards: the ray r of rays (a spiral over a
// disc around the light's direction l, as wide as spread, turned by turn)
float blocked(vec3 origin, vec3 dir, float tMax)
{
	if (!rayOk(origin, dir, 0.5, tMax)) return 0.0;
	rayQueryEXT query;
	rayQueryInitializeEXT(query, uScene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x04, origin, 0.5, dir, tMax);
	while (rayQueryProceedEXT(query)) {}
	return rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
// whether the level (its surfaces and brush models, masks 0x01 and 0x02) is between origin and the first character
// along dir (or tMax when there is none)
bool levelBefore(vec3 origin, vec3 dir, float tMax)
{
	if (!rayOk(origin, dir, 0.5, max(tMax, 0.6))) return false;
	rayQueryEXT query;
	rayQueryInitializeEXT(query, uScene, gl_RayFlagsOpaqueEXT, 0x04, origin, 0.5, dir, max(tMax, 0.6));
	while (rayQueryProceedEXT(query)) {}
	if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
		tMax = rayQueryGetIntersectionTEXT(query, true);
	}
	rayQueryEXT level;
	rayQueryInitializeEXT(level, uScene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x03, origin, 0.5, dir, max(tMax, 0.6));
	while (rayQueryProceedEXT(level)) {}
	return rayQueryGetIntersectionTypeEXT(level, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}
float blockedTowards(vec3 origin, vec3 l, vec3 side, vec3 up, float turn, int r, int rays, float spread, float tMax)
{
	float angle = turn + float(r) * 2.3999632;
	float radius = sqrt((float(r) + 0.5) / float(rays)) * spread;
	return blocked(origin, normalize(l + (cos(angle) * side + sin(angle) * up) * radius), tMax);
}

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
		if (any(lessThan(st, c.area.xy)) || any(greaterThan(st, c.area.zw))) continue;
		int type = int(c.params.z + 0.5);
		// below the floor under the character (down a step, off a ledge) its shadow goes on where the level is not
		// between them (not on the floor of the room under it, which is what the rays alone would darken)
		bool below = world.z < c.params.x;
		if (below && world.z < c.params.x - 128.0) continue;	// (no further down than a few steps)
		if (type == 2) {
			if (below) continue;
			// the contact shadow: short rays all around the surface (cosine weighted, as the light from all around
			// falls on it), and how many of them a character stops; few, as it is soft (the blur does the rest), and
			// only near the character
			if (distance(world, c.light.xyz) > c.params.w) continue;
			int rays = p.quality > 1u ? 4 : 3;
			vec3 side = normalize(abs(n.z) < 0.9 ? cross(n, vec3(0.0, 0.0, 1.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
			vec3 up = cross(side, n);
			float hits = 0.0;
			for (int r = 0; r < rays; r++) {
				float angle = turn + float(r) * 2.3999632, radius = sqrt((float(r) + 0.5) / float(rays));
				vec3 dir = (cos(angle) * side + sin(angle) * up) * radius + n * sqrt(1.0 - radius * radius);
				hits += blocked(world + n * 0.5, dir, c.light.w);
			}
			f *= 1.0 - c.params.y * hits / float(rays);
			continue;
		}

		// towards the light: a direction, or a lamp (whose size, seen from here, the rays spread over)
		vec3 l = normalize(c.light.xyz);
		float spread = c.light.w, tMax = 4096.0, darkness = c.params.y;
		if (type == 1) {
			vec3 d = c.light.xyz - world;
			float dist = length(d);
			if (dist <= c.light.w + 2.0) continue;
			l = d / dist;
			spread = c.light.w / dist;
			tMax = dist - c.light.w;
			// further from the lamp than the character, it has less of the lamp's light to lose: the shadow fades
			darkness *= min(c.params.w * c.params.w / (dist * dist), 1.0);
		}
		if (dot(n, l) <= 0.0) continue;	// facing away from the light: dark in the level's own lighting already

		// the rays: around the light's direction in a disc, turned by a different amount at every pixel; every other
		// one first, the ones between them only where those disagree (the edge of a shadow)
		vec3 side = normalize(abs(l.z) < 0.9 ? cross(l, vec3(0.0, 0.0, 1.0)) : cross(l, vec3(1.0, 0.0, 0.0)));
		vec3 up = cross(side, l);
		int rays = p.quality > 1u ? 8 : 4;
		float hidden = 0.0;
		for (int r = 0; r < rays; r += 2) {
			hidden += blockedTowards(world + n * 0.5, l, side, up, turn, r, rays, spread, tMax);
		}
		if (hidden > 0.0 && hidden < float(rays / 2)) {
			for (int r = 1; r < rays; r += 2) {
				hidden += blockedTowards(world + n * 0.5, l, side, up, turn, r, rays, spread, tMax);
			}
		} else {
			hidden *= 2.0;	// (those traced agree: as if every ray was)
		}
		if (below && hidden > 0.0 && levelBefore(world + n * 0.5, l, tMax)) continue;
		f *= 1.0 - darkness * hidden / float(rays);
	}
	outColor = vec4(f, f, f, 1.0);
}
