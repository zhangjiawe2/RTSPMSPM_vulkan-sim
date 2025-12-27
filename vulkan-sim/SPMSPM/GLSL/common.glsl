// vec4 数组（Ray 数据）
layout(buffer_reference, scalar) buffer Vec4Buffer {
    vec4 data[];
};

// float 数组（matrix value / result）
layout(buffer_reference, scalar) buffer FloatBuffer {
    float data[];
};

/* ================= SBT Payload 结构 ================= */

struct RayData {
    Vec4Buffer originVec;   // VkDeviceAddress → vec4[]
    uint32_t   size;
};

struct SphereData {
    FloatBuffer sphereColor;
    FloatBuffer result;
    int         resultNumRow;
    int         resultNumCol;
    uint32_t    matrix1size;
    uint32_t    matrix2size;
};

struct ShaderNeedInfo{
    // 结果矩阵维度
    int ResultRow;
    int ResultCol;
    // 光线数量
    int RayCount;
};

/* ================= Descriptor / AS ================= */


// 普通 UBO（如果需要）
layout(set = 0, binding = 1) uniform ParamsUBO {
    uint matrixDimK;
    uint matrixDimN;
} params;
