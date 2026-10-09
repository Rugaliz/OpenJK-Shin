#version 450

// Grows the source to twice the size with a tent filter; blended (added) on top of what is in the target.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: texel size of the source, scale

layout(set = 0, binding = 0) uniform sampler2D uSource;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
	vec2 t = p.a.xy;
	vec3 colour = texture(uSource, vUV).rgb * 4.0;
	colour += (texture(uSource, vUV + vec2(t.x, 0.0)).rgb + texture(uSource, vUV - vec2(t.x, 0.0)).rgb +
			   texture(uSource, vUV + vec2(0.0, t.y)).rgb + texture(uSource, vUV - vec2(0.0, t.y)).rgb) * 2.0;
	colour += texture(uSource, vUV + t).rgb + texture(uSource, vUV - t).rgb +
			  texture(uSource, vUV + vec2(t.x, -t.y)).rgb + texture(uSource, vUV + vec2(-t.x, t.y)).rgb;
	outColor = vec4(colour * (p.a.z / 16.0), 1.0);
}
