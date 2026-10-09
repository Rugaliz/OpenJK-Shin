#version 450

// The view with the effects on it.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: 1 if the occlusion is on, 1 to show it alone, how much bloom is added

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
	outColor = vec4(colour, 1.0);
}
