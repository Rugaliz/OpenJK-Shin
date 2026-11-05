#version 450

// The view with the effects on it.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: 1 if the occlusion is on, 1 to show it alone, how much bloom is added, the exposure of the eye adaptation

layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uAO;
layout(set = 0, binding = 2) uniform sampler2D uBloom;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
	vec3 colour = texture(uScene, vUV).rgb;
	if (p.a.x > 0.5) {
		float ao = texture(uAO, vUV).r;
		colour = p.a.y > 0.5 ? vec3(ao) : colour * ao;
	}
	colour += texture(uBloom, vUV).rgb * p.a.z * (1.0 - colour);
	if (p.a.w > 0.0 && p.a.w != 1.0) {
		colour = 1.0 - pow(clamp(1.0 - colour, 0.0, 1.0), vec3(p.a.w));	// (black stays black, white white)
	}
	outColor = vec4(colour, 1.0);
}
