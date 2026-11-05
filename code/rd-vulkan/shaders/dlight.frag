#version 460

// Built twice (shaders/build.py): as it is, and with RAY_QUERY as dlight_rt.frag for graphics cards with ray tracing,
// where a ray from the pixel to the light through the level's ray tracing structure (vk_rt.cpp) can keep the light off
// what is behind a wall (the shadows of dynamic lights, cg_shadows 5).
#ifdef RAY_QUERY
#extension GL_EXT_ray_query : require
#endif

layout(push_constant) uniform Constants
{
	mat4 mvp;
	vec4 light;
	vec4 colorTextured;
	vec4 alphaTest;
	vec4 unused;
#ifdef RAY_QUERY
	vec4 toWorld0, toWorld1, toWorld2;	// from the space of the surface to the world (vkDlightShadow_t)
	vec4 shadow;						// x: 1 for the rays
#endif
} pc;

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;
#ifdef RAY_QUERY
layout(set = 1, binding = 0) uniform accelerationStructureEXT uScene;
#endif

layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

// alphaTest.y: how deep the bumps of the surface are (r_dlightGLSL 2), in units for the brightest texel, 0 flat
float height(vec2 st) { return dot(texture(uDiffuse, st).rgb, vec3(0.299, 0.587, 0.114)) * pc.alphaTest.y; }

void main()
{
	vec3 toLight = pc.light.xyz - vPos;
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
#ifdef RAY_QUERY
	// a ray from the pixel to the light, in the world, against the level and its brush models (mask 0x03); it stops
	// short of the light, so that what the light sits on (an explosion on a wall) does not hide it
	vec4 p4 = vec4(vPos, 1.0), l4 = vec4(pc.light.xyz, 1.0);
	vec3 wPos = vec3(dot(pc.toWorld0, p4), dot(pc.toWorld1, p4), dot(pc.toWorld2, p4));
	vec3 wN = cross(dFdx(wPos), dFdy(wPos));	// (outside the branch: derivatives need every pixel of the quad)
	if (pc.shadow.x > 0.5 && atten * facing > 0.0) {
		vec3 wLight = vec3(dot(pc.toWorld0, l4), dot(pc.toWorld1, l4), dot(pc.toWorld2, l4));
		vec3 d = wLight - wPos;
		float dist = length(d);
		vec3 n = dot(wN, wN) > 1e-12 ? normalize(wN) : vec3(0.0);
		if (dot(n, d) < 0.0) n = -n;
		if (dist > 16.0) {
			rayQueryEXT query;
			rayQueryInitializeEXT(query, uScene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x03,
				wPos + n * 1.0, 0.5, d / dist, dist - 12.0);
			while (rayQueryProceedEXT(query)) {}
			if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) light = vec3(0.0);
		}
	}
#endif
	if (pc.colorTextured.a > 0.5) {
		vec4 diffuse = texture(uDiffuse, vTexCoord);
		float at = pc.alphaTest.x;
		if ((at > 0.5 && at < 1.5 && diffuse.a <= 0.0) ||
			(at > 1.5 && at < 2.5 && diffuse.a >= 0.5) ||
			(at > 2.5 && at < 3.5 && diffuse.a < 0.5) ||
			(at > 3.5 && diffuse.a < 0.75)) {
			discard;	// the stage is cut out here, so it gets no light here
		}
		light *= diffuse.rgb;
	}
	outColor = vec4(light, 1.0);
}
