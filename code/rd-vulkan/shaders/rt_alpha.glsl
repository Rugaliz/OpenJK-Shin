// The alpha test of the surfaces that are cut out by one (grates, foliage) for ray queries (vk_rt.cpp): their triangles
// are not opaque, so a ray query hands them to the shader as candidates, and these say whether the texture is there
// where the ray goes through. Included after RT_ALPHA_SET, RT_ALPHA_ATLAS and RT_ALPHA_DATA are defined (the set and
// the bindings of the atlas of the textures' alpha and of the buffer of materials and triangles).

struct RtMaterial {
	vec4 rect;		// the texture's tile in the atlas: corner, size
	uvec4 test;		// x: 1 alpha > 0, 2 alpha < 0.5, 3 alpha >= 0.5, 4 alpha >= 0.75
};

struct RtTriangle {
	vec4 uv01;		// the texture coordinates of the first two corners
	vec2 uv2;		// and of the third
	uint material;
	uint pad;
};

layout(set = RT_ALPHA_SET, binding = RT_ALPHA_ATLAS) uniform sampler2D uAlphaAtlas;
layout(std430, set = RT_ALPHA_SET, binding = RT_ALPHA_DATA) readonly buffer RtAlpha {
	RtMaterial materials[256];	// (RT_ALPHA_MATERIALS)
	RtTriangle triangles[];
} rtAlpha;

// the candidate triangle (the instance's custom index is where the triangles of its structure start) is solid where the
// ray goes through it
bool rtAlphaSolid(uint first, uint primitive, vec2 barycentrics)
{
	RtTriangle t = rtAlpha.triangles[first + primitive];
	vec2 uv = t.uv01.xy * (1.0 - barycentrics.x - barycentrics.y) + t.uv01.zw * barycentrics.x + t.uv2 * barycentrics.y;
	RtMaterial m = rtAlpha.materials[t.material];
	float a = textureLod(uAlphaAtlas, m.rect.xy + fract(uv) * m.rect.zw, 0.0).r;
	uint test = m.test.x;
	return (test == 1u && a > 0.0) || (test == 2u && a < 0.5) || (test == 3u && a >= 0.5) || (test == 4u && a >= 0.75);
}

// runs the ray query q to the end, the cut out parts of alpha tested triangles letting it through
#define RT_PROCEED(q) \
	while (rayQueryProceedEXT(q)) { \
		if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && \
			rtAlphaSolid(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)), \
				uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), rayQueryGetIntersectionBarycentricsEXT(q, false))) \
			rayQueryConfirmIntersectionEXT(q); \
	}
