#version 450

// Light shafts (r_lightShafts): the Vulkan version of the program in tr_post.h, one shader for its three passes
// (c.x): 0 the sky near the sun, 1 that blurred along the line to the sun, 2 the result as it is, added over the view.
// "t" is from the top left corner.

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 unused; } p;
// a: the sun on the view, the width of the view over its height, strength; b: depth uv scale, bias; c: the pass

layout(set = 0, binding = 0) uniform sampler2D uTex;	// the view (0), the sky near the sun (1), the rays (2)
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
	vec2 uv = vUV;
	if (p.c.x < 0.5) {
		float sky = texture(uDepth, uv * p.b.xy + p.b.zw).r >= 0.99999 ? 1.0 : 0.0;
		vec2 d = (uv - p.a.xy) * vec2(p.a.z, 1.0);
		float nearSun = clamp(1.0 - length(d) / 0.5, 0.0, 1.0);
		vec3 c = max(texture(uTex, uv).rgb - 0.4, 0.0) * 1.6667;	// (only the brighter parts of the sky)
		outColor = vec4(c * c * (sky * nearSun * nearSun), 1.0);
	} else if (p.c.x < 1.5) {
		vec2 stepUv = (p.a.xy - uv) * (0.85 / 48.0);
		float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		vec2 at = uv + stepUv * noise;	// (each pixel starts a little further along, so the steps don't show)
		vec3 sum = vec3(0.0);
		float weight = 1.0;
		for (int i = 0; i < 48; i++) {
			if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0) break;	// (the sun is off the view)
			sum += texture(uTex, at).rgb * weight;
			weight *= 0.97;
			at += stepUv;
		}
		outColor = vec4(sum * (p.a.w * 0.035), 1.0);
	} else {
		outColor = vec4(texture(uTex, uv).rgb, 1.0);
	}
}
