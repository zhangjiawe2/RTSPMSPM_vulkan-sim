#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_atomic_float : require

layout(set = 0, binding = 2) buffer ResultBuffer {
    float result[];
};

layout(location = 0) rayPayloadInEXT vec3 payload;

void main()
{
    uint primID = gl_PrimitiveID;

    result[0]=payload.x;
}
