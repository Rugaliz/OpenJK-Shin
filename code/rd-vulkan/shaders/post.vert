#version 450

// One triangle that covers the target; vUV runs 0..1 from the top left corner, like the rows of the images.

layout(location = 0) out vec2 vUV;

void main()
{
	const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	vUV = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
