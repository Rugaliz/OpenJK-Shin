#version 450

// Dynamic lights as a shader pass (the Vulkan version of ProjectDlightGLSL in tr_shade.cpp).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec3 inNormal;

layout(push_constant) uniform Constants
{
	mat4 mvp;
	vec4 light;			// xyz: position in the space of the surface, w: radius
	vec4 colorTextured;	// rgb: colour of the light, a: 1 if the surface texture is used
	vec4 alphaTest;		// x: 0 none, 1 alpha > 0, 2 alpha < 0.5, 3 alpha >= 0.5, 4 alpha >= 0.75
	vec4 unused;
} pc;

layout(location = 0) out vec3 vPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;

void main()
{
	vPos = inPosition;
	vNormal = inNormal;
	vTexCoord = inTexCoord0;
	gl_Position = pc.mvp * vec4(inPosition, 1.0);
}
