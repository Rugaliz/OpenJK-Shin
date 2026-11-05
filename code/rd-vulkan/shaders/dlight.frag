#version 450

layout(push_constant) uniform Constants
{
	mat4 mvp;
	vec4 light;
	vec4 colorTextured;
	vec4 alphaTest;
	vec4 unused;
} pc;

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;

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
