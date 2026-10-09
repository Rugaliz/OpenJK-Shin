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

void main()
{
	vec3 toLight = pc.light.xyz - vPos;
	float dist2 = dot(toLight, toLight);
	float atten = clamp(1.0 - dist2 / (pc.light.w * pc.light.w), 0.0, 1.0);
	float facing = 1.0;
	float nlen2 = dot(vNormal, vNormal);
	if (nlen2 > 0.0001) {	// the terrain has no normals, it is lit like the old passes did
		float ndl = dot(vNormal, toLight) * inversesqrt(nlen2 * max(dist2, 0.0001));
		facing = smoothstep(0.0, 0.25, ndl) * mix(1.0, ndl, 0.6);
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
