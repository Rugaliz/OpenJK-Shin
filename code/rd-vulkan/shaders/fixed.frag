#version 450

// The fixed function fragment stage of the original renderer: up to two texture units with their texture
// environment mode, fog, alpha test and the clip plane. What a draw uses is fixed by specialization constants, so
// every combination is a pipeline of its own. (docs/vulkan-renderer.md)

layout(constant_id = 0) const int UNIT_MASK = 1;		// bit n: texture unit n is on
layout(constant_id = 1) const int ENV0 = 0;			// 0 modulate, 1 replace, 2 add, 3 decal
layout(constant_id = 2) const int ENV1 = 0;
layout(constant_id = 3) const int ALPHA_FUNC = 0;	// 0 off, 1 less, 2 equal, 3 lequal, 4 greater, 5 notequal, 6 gequal
layout(constant_id = 4) const float ALPHA_REF = 0.0;
layout(constant_id = 5) const int FOG_MODE = 0;		// 0 off, 1 linear, 2 exp2
layout(constant_id = 6) const int CLIP = 0;

layout(push_constant) uniform Constants
{
	mat4 mvp;
	vec4 clipPlane;
	vec4 eyeZ;
	vec4 fogColorDensity;
	vec4 fogRange;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord0;
layout(location = 2) in vec2 vTexCoord1;
layout(location = 3) in float vFogDepth;
layout(location = 4) in float vClip;

layout(location = 0) out vec4 outColor;

vec4 applyEnv(int env, vec4 previous, vec4 texel)
{
	if (env == 1) {			// replace
		return texel;
	}
	if (env == 2) {			// add: colour adds, alpha multiplies
		return vec4(previous.rgb + texel.rgb, previous.a * texel.a);
	}
	if (env == 3) {			// decal
		return vec4(mix(previous.rgb, texel.rgb, texel.a), previous.a);
	}
	return previous * texel;	// modulate
}

void main()
{
	if (CLIP != 0 && vClip < 0.0) {
		discard;
	}

	vec4 color = clamp(vColor, 0.0, 1.0);
	if ((UNIT_MASK & 1) != 0) {
		color = clamp(applyEnv(ENV0, color, texture(tex0, vTexCoord0)), 0.0, 1.0);
	}
	if ((UNIT_MASK & 2) != 0) {
		color = clamp(applyEnv(ENV1, color, texture(tex1, vTexCoord1)), 0.0, 1.0);
	}

	if (FOG_MODE == 1) {
		float f = clamp((pc.fogRange.y - vFogDepth) / (pc.fogRange.y - pc.fogRange.x), 0.0, 1.0);
		color.rgb = mix(pc.fogColorDensity.rgb, color.rgb, f);
	} else if (FOG_MODE == 2) {
		float d = pc.fogColorDensity.a * vFogDepth;
		float f = clamp(exp(-d * d), 0.0, 1.0);
		color.rgb = mix(pc.fogColorDensity.rgb, color.rgb, f);
	}

	if (ALPHA_FUNC == 1 && !(color.a < ALPHA_REF)) discard;
	if (ALPHA_FUNC == 2 && !(color.a == ALPHA_REF)) discard;
	if (ALPHA_FUNC == 3 && !(color.a <= ALPHA_REF)) discard;
	if (ALPHA_FUNC == 4 && !(color.a > ALPHA_REF)) discard;
	if (ALPHA_FUNC == 5 && !(color.a != ALPHA_REF)) discard;
	if (ALPHA_FUNC == 6 && !(color.a >= ALPHA_REF)) discard;

	outColor = color;
}
