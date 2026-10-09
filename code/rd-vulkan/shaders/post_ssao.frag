#version 450

// Ambient occlusion from the depth of the view (the Vulkan version of the shader in tr_post.cpp; "t" is a position in
// the view with the origin at the top left, the maths is the one of the OpenGL renderer with its Y up).

layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; vec4 d; } p;
// a: proj [0] [5] [8] [9]; b: proj [10] [14], size of the view; c: radius, strength, bias; d: depth uv scale, bias

layout(set = 0, binding = 0) uniform sampler2D uDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float depthAt(vec2 t) { return texture(uDepth, t * p.d.xy + p.d.zw).r; }
float viewZ(float depth) { return -p.b.y / (depth * 2.0 - 1.0 + p.b.x); }
vec3 viewPos(vec2 t)
{
	float z = viewZ(depthAt(t));
	vec2 ndc = vec2(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0);
	return vec3(-(ndc.x + p.a.z) * z / p.a.x, -(ndc.y + p.a.w) * z / p.a.y, z);
}

void main()
{
	const vec2 size = p.b.zw;
	vec2 uv = vUV;
	float depth = depthAt(uv);
	// below 0.3 is the weapon in front of the player, 1 is the sky
	if (depth >= 1.0 || depth < 0.3) {
		outColor = vec4(1.0);
		return;
	}
	vec3 c = viewPos(uv);
	vec2 px = 1.0 / size;
	vec3 pr = viewPos(uv + vec2(px.x, 0.0));
	vec3 pl = viewPos(uv - vec2(px.x, 0.0));
	vec3 pu = viewPos(uv - vec2(0.0, px.y));	// up on the screen
	vec3 pd = viewPos(uv + vec2(0.0, px.y));
	vec3 dx = abs(pr.z - c.z) < abs(c.z - pl.z) ? pr - c : c - pl;
	vec3 dy = abs(pu.z - c.z) < abs(c.z - pd.z) ? pu - c : c - pd;
	vec3 n = normalize(cross(dx, dy));
	if (dot(n, c) > 0.0) {
		n = -n;
	}
	float radius = p.c.x;
	float radiusPx = clamp(radius * p.a.x * 0.5 * size.x / -c.z, 3.0, 0.25 * size.y);
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float occlusion = 0.0;
	for (int i = 0; i < 16; i++) {
		float angle = noise * 6.2831853 + float(i) * 2.3999632;
		float dist = sqrt((float(i) + 0.5) / 16.0) * radiusPx;
		vec2 suv = uv + vec2(cos(angle), -sin(angle)) * dist / size;
		if (depthAt(suv) < 0.3) {
			continue;
		}
		vec3 v = viewPos(suv) - c;
		float vv = dot(v, v);
		float falloff = clamp(1.0 - vv / (radius * radius), 0.0, 1.0);
		occlusion += falloff * max(dot(v, n) * inversesqrt(vv + 0.0001) - p.c.z, 0.0);
	}
	outColor = vec4(vec3(clamp(1.0 - p.c.y * occlusion / 16.0, 0.0, 1.0)), 1.0);
}
