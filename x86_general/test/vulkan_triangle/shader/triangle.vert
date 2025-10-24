#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;

layout (binding = 0) uniform UBO 
{
	mat4 projectionMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
} ubo;

mat4 mvp = mat4 (
    0.974279, 0.000000, 0.000000, 0.000000,
    0.000000, 1.732051, 0.000000, 0.000000,
    0.000000, 0.000000,-1.003922,-1.000000,
    0.000000, 0.000000, 1.505882, 2.500000
);

layout (location = 0) out vec3 outColor;

out gl_PerVertex 
{
    vec4 gl_Position;   
};


void main() 
{
	outColor = inColor;
	gl_Position = mvp * vec4(inPos.xyz, 1.0);
}
