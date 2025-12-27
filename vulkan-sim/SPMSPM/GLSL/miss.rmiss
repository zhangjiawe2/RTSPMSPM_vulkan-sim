#version 460
#extension GL_EXT_ray_tracing : require


void main()
{
    // OptiX 里 miss 什么都不做
    // Vulkan 这里同样保持 payload 不变
}
