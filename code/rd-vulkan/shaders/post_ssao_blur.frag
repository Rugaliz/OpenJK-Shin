#version 450

// Blurs the occlusion along one axis, across pixels of about the same depth only.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: proj [0] [5] [8] [9]; b: proj [10] [14], size of the view; c: direction in pixels; d: depth uv scale, bias

layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler2D uAO;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float viewZ(vec2 t) { return -p.b.y / (texture(uDepth, t * p.d.xy + p.d.zw).r * 2.0 - 1.0 + p.b.x); }

void main()
{
	const vec2 size = p.b.zw;
	float centreZ = viewZ(vUV);
	float sum = 0.0;
	float weights = 0.0;
	for (int i = -4; i <= 4; i++) {
		vec2 suv = vUV + p.c.xy * float(i) / size;
		float spatial = exp(-float(i * i) / 8.0);
		float dz = abs(viewZ(suv) - centreZ);
		float w = spatial * max(1.0 - dz / (4.0 + 0.02 * abs(centreZ)), 0.0);
		sum += w * texture(uAO, suv).r;
		weights += w;
	}
	outColor = vec4(vec3(sum / max(weights, 0.0001)), 1.0);
}
