#version 450

// The fixed function vertex stage of the original renderer: transform, plus the values the fragment stage needs for
// fog and the clip plane. (docs/vulkan-renderer.md)

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(push_constant) uniform Constants
{
	mat4 mvp;			// projection * modelview, already in Vulkan clip space
	vec4 clipPlane;		// glClipPlane(0), in object space
	vec4 eyeZ;			// third row of the modelview matrix: eye space depth, for fog
	vec4 fogColorDensity;	// rgb, exp2 density
	vec4 fogRange;		// linear start, end
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord0;
layout(location = 2) out vec2 vTexCoord1;
layout(location = 3) out float vFogDepth;
layout(location = 4) out float vClip;

void main()
{
	const vec4 position = vec4(inPosition, 1.0);
	gl_Position = pc.mvp * position;
	vColor = inColor;
	vTexCoord0 = inTexCoord0;
	vTexCoord1 = inTexCoord1;
	vFogDepth = abs(dot(pc.eyeZ, position));
	vClip = dot(pc.clipPlane, position);
}
