#version 450

// Eye adaptation (r_eyeAdaptation): the Vulkan version of the measuring program in tr_post.h, two passes (c.x): 0 the
// view shrunk to 64 square (EYE_LUM_SIZE), each texel the average log2 brightness of four pixels as 0 to 1 over -10 to
// 2 (EYE_LOG_MIN, EYE_LOG_RANGE); 1 that averaged into one pixel.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float encoded(vec2 uv)
{
	float lum = dot(texture(uTex, uv).rgb, vec3(0.2126, 0.7152, 0.0722));
	return clamp((log2(lum + 0.001) - (-10.0)) / 12.0, 0.0, 1.0);
}

void main()
{
	float sum = 0.0;
	if (p.c.x < 0.5) {
		float d = 0.25 / 64.0;
		sum = 0.25 * (encoded(vUV + vec2(-d, -d)) + encoded(vUV + vec2(d, -d)) + encoded(vUV + vec2(-d, d)) + encoded(vUV + vec2(d, d)));
	} else {
		for (int y = 0; y < 32; y++) {
			for (int x = 0; x < 32; x++) {
				sum += texture(uTex, (vec2(float(x), float(y)) * 2.0 + 1.0) / 64.0).r;
			}
		}
		sum /= 1024.0;
	}
	outColor = vec4(sum, sum, sum, 1.0);
}
