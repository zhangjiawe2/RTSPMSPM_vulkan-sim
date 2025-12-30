#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cassert>
#define VK_NO_PROTOTYPES
#include <volk.h>
#include <vulkan/vulkan.h>
#include <array>
#include <glm/glm.hpp>
#include <fstream> 
using namespace std;

/* ================= CPU 数据结构 ================= */

struct float3 {
    float x, y, z;
};

struct TriFromMatrixCPU {
    vector<float3>   vertices;
    vector<uint32_t> indices;
    vector<float>    values;
    int rows;
    int cols;
};

struct HostRay {
    glm::vec4 payload;
};
/* ================= Matrix → 三角形 ================= */

TriFromMatrixCPU Data_transform_T1(const string& filename)
{
    TriFromMatrixCPU output;

    ifstream file(filename);
    if (!file.is_open()) {
        cerr << "Failed to open matrix file\n";
        return output;
    }

    bool isFirstDataLine = true;
    int rows = 0, cols = 0;
    uint64_t nonZeros = 0;
    uint32_t index = 0;
    string line;

    while (getline(file, line)) {
        if (line.empty() || line[0] == '%')
            continue;

        if (isFirstDataLine) {
            istringstream iss(line);
            iss >> rows >> cols >> nonZeros;
            isFirstDataLine = false;
            output.rows = rows;
            output.cols = cols;
            continue;
        }

        float x, y, val;
        istringstream iss(line);
        if (!(iss >> x >> y >> val))
            continue;

        float dx[3] = {-0.5f, 0.0f, 0.5f};
        float dy[3] = {0.0f, 0.5f, 0.0f};
        float dz[3] = {-0.5f,0.5f,-0.5f};

        for (int i = 0; i < 3; i++) {
            output.vertices.push_back({x + dx[i], y + dy[i], 0.0f+dz[i]});
            output.indices.push_back(index++);
        }

        output.values.push_back(val);
    }

    return output;
}

/* -----------------读取光线数据------------------- -----*/
void loadMatrixAsRays(const string& filename,vector<HostRay>& rays,int & row) {
    ifstream file(filename);
    if (!file.is_open()) {
        cerr << "Failed to open matrix file\n";
        return ;
    }

    string line;
    uint32_t rows, cols;
    uint64_t nonZeros = 0;
    float value;
    bool isFirstDataLine = true;
    while (getline(file, line)) {
        if (line.empty() || line[0] == '%')
            continue;

        if (isFirstDataLine) {
            istringstream iss(line);
            iss >> rows >> cols >> nonZeros;
            row = rows;
            isFirstDataLine = false;
            continue;
        }

        uint32_t row, col;
        float value;
        istringstream iss(line);
        if (!(iss >> row >> col >> value))
            continue;
        
        HostRay r{};
        r.payload.x = static_cast<float>(row);
        r.payload.y = static_cast<float>(col);
        r.payload.z = value;      // 核心要求：value -> z
        r.payload.w = 0.0f;
        rays.push_back(r);
    }
}

/* ================= Vulkan 工具函数 ================= */

uint32_t findMemoryType(VkPhysicalDevice phys,
                        uint32_t typeFilter,
                        VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    assert(false && "No suitable memory type");
    return 0;
}

static std::vector<char> readFile(const std::string& filename) {
    // 1. 以二进制模式打开文件，并定位到文件末尾 (std::ios::ate)
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("failed to open file: " + filename);
    }

    // 2. 利用此时的文件指针位置（在末尾）获取文件大小
    size_t fileSize = (size_t) file.tellg();
    std::vector<char> buffer(fileSize);

    // 3. 回到文件开头，读取所有字节到缓冲区
    file.seekg(0);
    file.read(buffer.data(), fileSize);

    file.close();
    return buffer;
}
VkShaderModule createShaderModule(string filename,VkDevice device){
    VkShaderModuleCreateInfo createInfo{};
    vector<char> shaderCode = readFile(filename);
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.codeSize = shaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode.data());
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shader module!");
    }

    cout<<"*******************success build shaderModule********"<<endl;
    return shaderModule;
}


// =================== 1. SBT 结构体 ===================
struct SbtRecordHeader {
    uint8_t header[64]; // 填充 shader group handle
};

template<typename T>
struct SbtRecord {
    SbtRecordHeader header;
    T data; // Payload 数据，如矩阵指针、尺寸、结果 buffer 等
};

struct SphereData {
    float* sphereColor;
    float* result;
    int resultNumRow;
    int resultNumCol;
    uint32_t matrix1size;
    uint32_t matrix2size;
};

struct CloseHitData {
    float* hitColor;
    float* result;
    int resultNumRow;
    int resultNumCol;
    uint32_t matrix1size;
    uint32_t matrix2size;
};

struct RayData {
    glm::vec4* originVec;
    uint32_t size;
};

struct MissData {
    float r, g, b;
};

struct VulkanSBTRegions {
    VkStridedDeviceAddressRegionKHR raygenRegion{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR closeHitRegion{}; // 新增
    VkStridedDeviceAddressRegionKHR callableRegion{};
};

// =================== 2. Helper: memory type ===================
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

// =================== 3. SBT 创建函数 ===================
void createShaderBindingTable(
    VkDevice device,
    VkPipeline rtPipeline,
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rtProperties,
    const std::vector<VkPipelineShaderStageCreateInfo>& stages,
    VulkanSBTRegions& sbtRegions,
    SbtRecord<RayData>* raygenPayload,
    SbtRecord<MissData>* missPayload,
    SbtRecord<SphereData>* hitPayload,
    SbtRecord<CloseHitData>* closeHitPayload,
    VkBuffer& sbtBuffer,
    VkDeviceMemory& sbtMemory,
    VkPhysicalDevice physicalDevice
) {
    uint32_t handleSize      = rtProperties.shaderGroupHandleSize;
    uint32_t handleAlignment = rtProperties.shaderGroupHandleAlignment;
    uint32_t baseAlignment   = rtProperties.shaderGroupBaseAlignment;
    uint32_t groupCount      = 4; // raygen, miss, hitgroup, closehit

    // 1. 获取 shader group handles
    size_t dataSize = handleSize * groupCount;
    std::vector<uint8_t> shaderHandles(dataSize);
    vkGetRayTracingShaderGroupHandlesKHR(device, rtPipeline, 0, groupCount, dataSize, shaderHandles.data());

    // 2. 计算每个 SBT record 的对齐大小
    auto alignUp = [](size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); };

    size_t raygenSize   = alignUp(sizeof(SbtRecord<RayData>), handleAlignment);
    size_t missSize     = alignUp(sizeof(SbtRecord<MissData>), handleAlignment);
    size_t hitSize      = alignUp(sizeof(SbtRecord<SphereData>), handleAlignment);
    size_t closeHitSize = alignUp(sizeof(SbtRecord<CloseHitData>), handleAlignment);

    size_t raygenOffset = 0;
    size_t missOffset   = alignUp(raygenOffset + raygenSize, baseAlignment);
    size_t hitOffset    = alignUp(missOffset + missSize, baseAlignment);
    size_t closeHitOffset = alignUp(hitOffset + hitSize, baseAlignment);
    size_t sbtBufferSize = closeHitOffset + closeHitSize;

    // 3. 创建 Vulkan buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sbtBufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vkCreateBuffer(device, &bufferInfo, nullptr, &sbtBuffer);

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, sbtBuffer, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(device, &allocInfo, nullptr, &sbtMemory);
    vkBindBufferMemory(device, sbtBuffer, sbtMemory, 0);

    // 4. 填充 SBT buffer
    uint8_t* pData = nullptr;
    vkMapMemory(device, sbtMemory, 0, sbtBufferSize, 0, (void**)&pData);

    memcpy(raygenPayload->header.header, shaderHandles.data() + 0 * handleSize, handleSize);
    memcpy(pData + raygenOffset, raygenPayload, sizeof(SbtRecord<RayData>));

    memcpy(missPayload->header.header, shaderHandles.data() + 1 * handleSize, handleSize);
    memcpy(pData + missOffset, missPayload, sizeof(SbtRecord<MissData>));

    memcpy(hitPayload->header.header, shaderHandles.data() + 2 * handleSize, handleSize);
    memcpy(pData + hitOffset, hitPayload, sizeof(SbtRecord<SphereData>));

    memcpy(closeHitPayload->header.header, shaderHandles.data() + 3 * handleSize, handleSize);
    memcpy(pData + closeHitOffset, closeHitPayload, sizeof(SbtRecord<CloseHitData>));

    vkUnmapMemory(device, sbtMemory);

    // 5. 填充 VkStridedDeviceAddressRegionKHR
    VkBufferDeviceAddressInfo addrInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        sbtBuffer
    };
    VkDeviceAddress sbtAddr = vkGetBufferDeviceAddress(device, &addrInfo);

    sbtRegions.raygenRegion.deviceAddress = sbtAddr + raygenOffset;
    sbtRegions.raygenRegion.stride        = raygenSize;
    sbtRegions.raygenRegion.size          = raygenSize;

    sbtRegions.missRegion.deviceAddress = sbtAddr + missOffset;
    sbtRegions.missRegion.stride        = missSize;
    sbtRegions.missRegion.size          = missSize;

    sbtRegions.hitRegion.deviceAddress = sbtAddr + hitOffset;
    sbtRegions.hitRegion.stride        = hitSize;
    sbtRegions.hitRegion.size          = hitSize;

    sbtRegions.closeHitRegion.deviceAddress = sbtAddr + closeHitOffset;
    sbtRegions.closeHitRegion.stride        = closeHitSize;
    sbtRegions.closeHitRegion.size          = closeHitSize;

    sbtRegions.callableRegion.deviceAddress = 0;
    sbtRegions.callableRegion.stride        = 0;
    sbtRegions.callableRegion.size          = 0;
}

// 传递信息
struct ShaderNeedInfo{
    // 结果矩阵维度
    int ResultRow;
    int ResultCol;
    // 光线数量
    uint32_t RayCount;
};
// =================== 6. Shader Groups 配置 ===================
// 在创建 pipeline 时添加 CloseHit shader group
// Raygen: 0
// Miss: 1
// AnyHit (Sphere): 2
// CloseHit: 3

/* ================= 主程序 ================= */

int main()
{
    /* ---------- 1. Vulkan Instance ---------- */
    if (volkInitialize() != VK_SUCCESS) {
        std::cerr << "Failed to initialize volk\n";
        return -1;
    }
    VkApplicationInfo appInfo{
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        "vulkan-sim-test",
        1,
        nullptr,
        0,
        VK_API_VERSION_1_2
    };

    VkInstanceCreateInfo instInfo{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr,
        0,
        &appInfo
    };

    VkInstance instance;
    vkCreateInstance(&instInfo, nullptr, &instance);
    volkLoadInstance(instance);
    /* ---------- 2. Physical Device ---------- */

    uint32_t physCount = 0;
    vkEnumeratePhysicalDevices(instance, &physCount, nullptr); // 获取支持vulkan的物理设备数量
    assert(physCount > 0);

    vector<VkPhysicalDevice> phys(physCount);
    vkEnumeratePhysicalDevices(instance, &physCount, phys.data());

    VkPhysicalDevice physicalDevice = phys[0];

    /* ---------- 3. Logical Device ---------- */

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qinfo{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        nullptr,
        0,
        0,
        1,
        &prio
    };

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
    };
    asFeat.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
    };
    rtFeat.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceBufferDeviceAddressFeatures addrFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
    };
    addrFeat.bufferDeviceAddress = VK_TRUE;

    // 链接 pNext
    asFeat.pNext  = &rtFeat;
    rtFeat.pNext  = &addrFeat;

    const char* deviceExtensions[] = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    };

    uint32_t deviceExtensionCount = sizeof(deviceExtensions) / sizeof(deviceExtensions[0]);
    VkDeviceCreateInfo devInfo{
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        &asFeat,
        0,
        1,
        &qinfo,
        0,
        nullptr,
        deviceExtensionCount,
        deviceExtensions
    };

    VkDevice device;
    vkCreateDevice(physicalDevice, &devInfo, nullptr, &device);
    volkLoadDevice(device);
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // ---------- 获取 Ray Tracing Pipeline Properties ----------
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{};
    rtProperties.sType =VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 physProps2{};
    physProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    physProps2.pNext = &rtProperties;

    vkGetPhysicalDeviceProperties2(physicalDevice, &physProps2);

    /* ---------- 4. 读取矩阵并生成三角形 ---------- */

    int Resultrow;
    int Resultcow; 
    string matrixFile =
        "/home/zjw/SPMSPM/optix/RTSpMSpM/optixSpMSpM/src/data/mydata/2.mtx";

    TriFromMatrixCPU data = Data_transform_T1(matrixFile);
    Resultcow = data.cols;
    int count = data.vertices.size();
    cout << "Vertices: " << data.vertices.size()
         << " Indices: " << data.indices.size() 
         << " values: "<< data.values.size()<<endl;

    /* ---------- 5. 创建 Vertex Buffer ---------- */

    VkBuffer vertexBuffer;  //缓存资源
    VkDeviceSize vertexBytes = sizeof(float3) * data.vertices.size();
    VkDeviceSize indexBytes  = sizeof(uint32_t) * data.indices.size();
    VkDeviceSize valueBytes = sizeof(float) * data.values.size();
    VkBufferCreateInfo bufInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        vertexBytes + indexBytes + valueBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufInfo, nullptr, &vertexBuffer);

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, vertexBuffer, &memReq);

    VkMemoryAllocateFlagsInfo flagsInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &flagsInfo,
        memReq.size,
        findMemoryType(physicalDevice,
                       memReq.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    VkDeviceMemory vertexMem;
    vkAllocateMemory(device, &allocInfo, nullptr, &vertexMem);
    vkBindBufferMemory(device, vertexBuffer, vertexMem, 0); //资源与设备内存绑定

    /* ---------- 6. 拷贝 CPU 数据 ---------- */

    void* mapped = nullptr;
    vkMapMemory(device, vertexMem, 0, VK_WHOLE_SIZE, 0, &mapped); //内存映射
    memcpy(mapped, data.vertices.data(),
           sizeof(float3) * data.vertices.size());
    memcpy((char*)mapped + vertexBytes,
           data.indices.data(),indexBytes);
    memcpy((char*)mapped + vertexBytes + indexBytes,
           data.values.data(),valueBytes);
    vkUnmapMemory(device, vertexMem);

    /* ---------- 7. 获取 VkDeviceAddress ---------- */

    VkBufferDeviceAddressInfo addrInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        vertexBuffer
    };

    VkDeviceAddress addr = vkGetBufferDeviceAddress(device, &addrInfo);
    VkDeviceAddress indexAddr = addr + vertexBytes;
    VkDeviceAddress valueAddr = indexAddr + indexBytes;

    cout << "Vertex Buffer Device Address: 0x"
         << hex << addr << dec << endl;

    if (addr == 0) {
        cerr << "ERROR: Device address is zero\n";
    } else {
        cout << "SUCCESS: Vulkan-sim + Mesa device address works\n";
    }

    /* -----------保存光线数据--------------------*/

    vector<HostRay> hostRays;
    loadMatrixAsRays("/home/zjw/SPMSPM/optix/RTSpMSpM/optixSpMSpM/src/data/mydata/1.mtx", hostRays,Resultrow);

    uint32_t rayCount = static_cast<uint32_t>(hostRays.size());

    cout<<"Ray Count :"<<" "<<rayCount<<endl;

    VkBuffer rayBuffer;  //缓存资源
    VkDeviceSize rayBytes = sizeof(HostRay)*rayCount;
    VkBufferCreateInfo bufInfo_ray{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        rayBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufInfo_ray, nullptr, &rayBuffer);

    VkMemoryRequirements memReq_ray;
    vkGetBufferMemoryRequirements(device, rayBuffer, &memReq_ray);

    VkMemoryAllocateFlagsInfo flagsInfo_ray{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo_ray{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &flagsInfo_ray,
        memReq_ray.size,
        findMemoryType(physicalDevice,
                       memReq_ray.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    VkDeviceMemory rayMem;
    vkAllocateMemory(device, &allocInfo_ray, nullptr, &rayMem);
    vkBindBufferMemory(device, rayBuffer, rayMem, 0); //资源与设备内存绑定

    

    /* ---------- 6. 拷贝 CPU 数据 ---------- */

    void* mapped_ray = nullptr;
    vkMapMemory(device, rayMem, 0, VK_WHOLE_SIZE, 0, &mapped_ray); //内存映射
    memcpy(mapped_ray, hostRays.data(),
           sizeof(HostRay) * rayCount);
    vkUnmapMemory(device, rayMem);

    /* ---------- 7. 获取 VkDeviceAddress ---------- */

    VkBufferDeviceAddressInfo addrInfo_ray{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        rayBuffer
    };

    VkDeviceAddress addr_ray = vkGetBufferDeviceAddress(device, &addrInfo_ray);

    cout << "Ray Buffer Device Address: 0x"
         << hex << addr << dec << endl;

    /* -----------分配Result 空间---------------------------*/

    VkBuffer ResultBuffer;  //缓存资源
    VkDeviceSize ResultBytes = sizeof(float)*Resultrow*Resultcow;
    VkBufferCreateInfo bufInfo_Result{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        ResultBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufInfo_Result, nullptr, &ResultBuffer);

    VkMemoryRequirements memReq_Result;
    vkGetBufferMemoryRequirements(device, ResultBuffer, &memReq_Result);

    VkMemoryAllocateFlagsInfo flagsInfo_Result{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo_Result{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &flagsInfo_Result,
        memReq_Result.size,
        findMemoryType(physicalDevice,
                       memReq_Result.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    VkDeviceMemory ResultMem;
    vkAllocateMemory(device, &allocInfo_Result, nullptr, &ResultMem);
    vkBindBufferMemory(device, ResultBuffer, ResultMem, 0); //资源与设备内存绑定
    VkBufferDeviceAddressInfo addrInfo_Result{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        ResultBuffer
    };

    VkDeviceAddress addr_Result = vkGetBufferDeviceAddress(device, &addrInfo_Result);

    /* ---------- 9. 构建加速结构 ----------*/


    VkBuffer AS_bvh_Buffer;  // 保存加速结构
    
    VkAccelerationStructureKHR AS_bvh_KHR;

/*
    9.1 设置数据信息
*/
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .vertexData = {.deviceAddress = addr},
        .vertexStride = sizeof(float3),
        .maxVertex = (uint32_t)count-1,
        .indexType = VK_INDEX_TYPE_UINT32,
        .indexData = {.deviceAddress = indexAddr},
    };

    VkAccelerationStructureGeometryKHR geometry{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry = {.triangles = triangles}, // union传参
        .flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR | VK_GEOMETRY_OPAQUE_BIT_KHR,
    };

    const auto triangleCount = static_cast<uint32_t>(count / 3U);
    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{
        .primitiveCount = triangleCount
    };
/*
    9.2 构建信息设置
    创建VkAccelerationStructureBuildGeometryInfoKHR包含几何数据的结构
    设置加速结构类型（BLAS 或 TLAS）、构建标志和模式
*/
    VkAccelerationStructureBuildGeometryInfoKHR asBuildInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };

/*
    9.3 尺寸计算
    vkGetAccelerationStructureBuildSizesKHR需要确定 以下事项：
    加速结构本身所需的尺寸
    暂存缓冲区（临时工作内存）所需大小
    更新暂存缓冲区所需大小（用于未来的更新）
*/
    VkAccelerationStructureBuildSizesInfoKHR asBuildSize{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };

    std::vector<uint32_t> maxPrimCount(1);
    maxPrimCount[0] = rangeInfo.primitiveCount;
    vkGetAccelerationStructureBuildSizesKHR(device,
                                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                &asBuildInfo,
                                maxPrimCount.data(),
                                &asBuildSize);

/*
    9.4  创建buffer

*/

//9.4.1 创建 buffer（只给用途，不放数据）
    VkBufferCreateInfo asBufInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        asBuildSize.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &asBufInfo, nullptr, &AS_bvh_Buffer);

//9.4.2 分配并绑定内存（必须有 DEVICE_ADDRESS）

    VkMemoryRequirements asMemReq;
    vkGetBufferMemoryRequirements(device, AS_bvh_Buffer, &asMemReq);

    VkMemoryAllocateFlagsInfo asAllocFlags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo asAllocInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &asAllocFlags,
        asMemReq.size,
        findMemoryType(physicalDevice,
                    asMemReq.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };

    VkDeviceMemory asMemory;
    vkAllocateMemory(device, &asAllocInfo, nullptr, &asMemory);
    vkBindBufferMemory(device, AS_bvh_Buffer, asMemory, 0);

//9.4.3 创建 VkAccelerationStructureKHR 对象

    VkAccelerationStructureCreateInfoKHR asCreateInfo{
    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    nullptr,
    0,
    AS_bvh_Buffer,
    0,
    asBuildSize.accelerationStructureSize,
    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    };

    vkCreateAccelerationStructureKHR(
        device,
        &asCreateInfo,
        nullptr,
        &AS_bvh_KHR
    );

// 9.4.4 创建 Scratch Buffer（构建时的临时内存）

// 创建 scratch buffer
    VkBuffer scratchBuffer;

    VkBufferCreateInfo scratchBufInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        asBuildSize.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &scratchBufInfo, nullptr, &scratchBuffer);
// 分配 + 绑定
    VkMemoryRequirements scratchReq;
    vkGetBufferMemoryRequirements(device, scratchBuffer, &scratchReq);

    VkMemoryAllocateFlagsInfo scratchFlags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo scratchAlloc{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &scratchFlags,
        scratchReq.size,
        findMemoryType(physicalDevice,
                    scratchReq.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };

    VkDeviceMemory scratchMem;
    vkAllocateMemory(device, &scratchAlloc, nullptr, &scratchMem);
    vkBindBufferMemory(device, scratchBuffer, scratchMem, 0);
// 获取 scratch device address
    VkBufferDeviceAddressInfo scratchAddrInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        scratchBuffer
    };

    VkDeviceAddress scratchAddr =
        vkGetBufferDeviceAddress(device, &scratchAddrInfo);

// 9.4.5 真正构建 BLAS（命令缓冲）

    asBuildInfo.dstAccelerationStructure = AS_bvh_KHR;
    asBuildInfo.scratchData.deviceAddress = scratchAddr;

// 记录命令

    VkCommandPool commandPool;

    VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        0   // queueFamilyIndex = 0，与 device queue 一致
    };

    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);


    VkCommandBufferAllocateInfo cmdAlloc{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr,
        commandPool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        1
    };

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };

    vkBeginCommandBuffer(cmd, &beginInfo);

    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

    vkCmdBuildAccelerationStructuresKHR(
        cmd,
        1,
        &asBuildInfo,
        &pRangeInfo
    );

    vkEndCommandBuffer(cmd);

// 提交并等待完成
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);


    /* ------------- TLAS 构建 ------------------------*/

/* ------------- TLAS 构建（变量名已修改） ------------------------*/

    // 1. 创建实例数组 (这里只有一个 BLAS)
    VkAccelerationStructureInstanceKHR tlasInstance{};
    tlasInstance.transform = VkTransformMatrixKHR{{ // 单位矩阵
        {1,0,0,0},
        {0,1,0,0},
        {0,0,1,0}
    }};
    tlasInstance.instanceCustomIndex = 0; // shader record offset
    tlasInstance.mask = 0xFF;
    tlasInstance.instanceShaderBindingTableRecordOffset = 0;
    tlasInstance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

    // 获取 BLAS device address
    VkAccelerationStructureDeviceAddressInfoKHR blasAddrInfoTLAS{};
    blasAddrInfoTLAS.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    blasAddrInfoTLAS.accelerationStructure = AS_bvh_KHR;
    tlasInstance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(device, &blasAddrInfoTLAS);

    // 2. 创建实例 buffer
    VkBuffer tlasInstanceBuffer;
    VkDeviceMemory tlasInstanceMemory;
    VkDeviceSize tlasInstanceBufferSize = sizeof(VkAccelerationStructureInstanceKHR);

    VkBufferCreateInfo tlasInstanceBufInfo{};
    tlasInstanceBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    tlasInstanceBufInfo.size = tlasInstanceBufferSize;
    tlasInstanceBufInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | 
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    tlasInstanceBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &tlasInstanceBufInfo, nullptr, &tlasInstanceBuffer);

    VkMemoryRequirements tlasInstMemReq;
    vkGetBufferMemoryRequirements(device, tlasInstanceBuffer, &tlasInstMemReq);

    VkMemoryAllocateFlagsInfo tlasInstAllocFlags{};
    tlasInstAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    tlasInstAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo tlasInstAllocInfo{};
    tlasInstAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    tlasInstAllocInfo.pNext = &tlasInstAllocFlags;
    tlasInstAllocInfo.allocationSize = tlasInstMemReq.size;
    tlasInstAllocInfo.memoryTypeIndex = findMemoryType(physicalDevice, tlasInstMemReq.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(device, &tlasInstAllocInfo, nullptr, &tlasInstanceMemory);
    vkBindBufferMemory(device, tlasInstanceBuffer, tlasInstanceMemory, 0);

    // 3. 拷贝实例数据到 buffer
    void* mappedTlasInst = nullptr;
    vkMapMemory(device, tlasInstanceMemory, 0, tlasInstanceBufferSize, 0, &mappedTlasInst);
    memcpy(mappedTlasInst, &tlasInstance, sizeof(tlasInstance));
    vkUnmapMemory(device, tlasInstanceMemory);

    // 4. 获取 instance buffer device address
    VkBufferDeviceAddressInfo tlasInstAddrInfo{};
    tlasInstAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    tlasInstAddrInfo.buffer = tlasInstanceBuffer;
    VkDeviceAddress tlasInstanceBufferAddr = vkGetBufferDeviceAddress(device, &tlasInstAddrInfo);

    // 5. TLAS geometry info
    VkAccelerationStructureGeometryKHR tlasGeom{};
    tlasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkAccelerationStructureGeometryInstancesDataKHR tlasInstancesData{};
    tlasInstancesData.arrayOfPointers = VK_FALSE;
    tlasInstancesData.data.deviceAddress = tlasInstanceBufferAddr;
    tlasGeom.geometry.instances = tlasInstancesData;

    // 6. TLAS build range
    VkAccelerationStructureBuildRangeInfoKHR tlasRangeInfo{};
    tlasRangeInfo.primitiveCount = 1; // 一个实例
    tlasRangeInfo.primitiveOffset = 0;
    tlasRangeInfo.firstVertex = 0;
    tlasRangeInfo.transformOffset = 0;

    // 7. TLAS build info
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfo{};
    tlasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuildInfo.geometryCount = 1;
    tlasBuildInfo.pGeometries = &tlasGeom;
    tlasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    // 8. 查询 build size
    VkAccelerationStructureBuildSizesInfoKHR tlasBuildSizes{};
    tlasBuildSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    uint32_t tlasPrimitiveCounts[1] = {1};
    vkGetAccelerationStructureBuildSizesKHR(device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &tlasBuildInfo,
        tlasPrimitiveCounts,
        &tlasBuildSizes);

    // 9. 创建 TLAS buffer
    VkBuffer tlasBuffer;
    VkDeviceMemory tlasMemory;

    VkBufferCreateInfo tlasBufInfo{};
    tlasBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    tlasBufInfo.size = tlasBuildSizes.accelerationStructureSize;
    tlasBufInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    tlasBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &tlasBufInfo, nullptr, &tlasBuffer);

    VkMemoryRequirements tlasMemReq;
    vkGetBufferMemoryRequirements(device, tlasBuffer, &tlasMemReq);

    VkMemoryAllocateFlagsInfo tlasAllocFlags{};
    tlasAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    tlasAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo tlasAllocInfo{};
    tlasAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    tlasAllocInfo.pNext = &tlasAllocFlags;
    tlasAllocInfo.allocationSize = tlasMemReq.size;
    tlasAllocInfo.memoryTypeIndex = findMemoryType(physicalDevice, tlasMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkAllocateMemory(device, &tlasAllocInfo, nullptr, &tlasMemory);
    vkBindBufferMemory(device, tlasBuffer, tlasMemory, 0);

    // 10. 创建 TLAS 对象
    VkAccelerationStructureKHR tlas;
    VkAccelerationStructureCreateInfoKHR tlasCreateInfo{};
    tlasCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlasCreateInfo.buffer = tlasBuffer;
    tlasCreateInfo.size = tlasBuildSizes.accelerationStructureSize;
    tlasCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    vkCreateAccelerationStructureKHR(device, &tlasCreateInfo, nullptr, &tlas);

    // 11. 创建 TLAS scratch buffer
    VkBuffer tlasScratchBuffer;
    VkDeviceMemory tlasScratchMemory;

    VkBufferCreateInfo tlasScratchBufInfo{};
    tlasScratchBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    tlasScratchBufInfo.size = tlasBuildSizes.buildScratchSize;
    tlasScratchBufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    tlasScratchBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &tlasScratchBufInfo, nullptr, &tlasScratchBuffer);

    VkMemoryRequirements tlasScratchReq;
    vkGetBufferMemoryRequirements(device, tlasScratchBuffer, &tlasScratchReq);

    VkMemoryAllocateFlagsInfo tlasScratchFlags{};
    tlasScratchFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    tlasScratchFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo tlasScratchAlloc{};
    tlasScratchAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    tlasScratchAlloc.pNext = &tlasScratchFlags;
    tlasScratchAlloc.allocationSize = tlasScratchReq.size;
    tlasScratchAlloc.memoryTypeIndex = findMemoryType(physicalDevice, tlasScratchReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &tlasScratchAlloc, nullptr, &tlasScratchMemory);
    vkBindBufferMemory(device, tlasScratchBuffer, tlasScratchMemory, 0);

    // 12. 获取 scratch device address
    VkBufferDeviceAddressInfo tlasScratchAddrInfo{};
    tlasScratchAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    tlasScratchAddrInfo.buffer = tlasScratchBuffer;
    VkDeviceAddress tlasScratchAddr = vkGetBufferDeviceAddress(device, &tlasScratchAddrInfo);

    // 13. TLAS build info 更新
    tlasBuildInfo.dstAccelerationStructure = tlas;
    tlasBuildInfo.scratchData.deviceAddress = tlasScratchAddr;

    // 14. 提交构建命令
    vkResetCommandBuffer(cmd, 0);  
    VkCommandBufferBeginInfo tlasCmdBeginInfo{};
    tlasCmdBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &tlasCmdBeginInfo);

    const VkAccelerationStructureBuildRangeInfoKHR* pTlasRangeInfo = &tlasRangeInfo;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuildInfo, &pTlasRangeInfo);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo tlasSubmit{};
    tlasSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    tlasSubmit.commandBufferCount = 1;
    tlasSubmit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &tlasSubmit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    std::cout << "*****************TLAS built successfully!***********" << std::endl;
    /* ---------------分配valueBuffer和 ShaderNeedInfoBuffer*/
    ShaderNeedInfo shaderneedinfo{
        .ResultRow = Resultrow,
        .ResultCol = Resultcow,
        .RayCount = rayCount,
    }; 
    VkBuffer ValueBuffer;  //缓存资源
    VkBufferCreateInfo bufInfo_Value{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        valueBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufInfo_Value, nullptr, &ValueBuffer);

    VkMemoryRequirements memReq_Value;
    vkGetBufferMemoryRequirements(device, ValueBuffer, &memReq_Value);

    VkMemoryAllocateFlagsInfo flagsInfo_Value{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo_Value{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &flagsInfo_Value,
        memReq_Value.size,
        findMemoryType(physicalDevice,
                       memReq_Value.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    VkDeviceMemory ValueMem;
    vkAllocateMemory(device, &allocInfo_Value, nullptr, &ValueMem);
    vkBindBufferMemory(device, ValueBuffer, ValueMem, 0); //资源与设备内存绑定
    VkBufferDeviceAddressInfo addrInfo_Value{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        ValueBuffer
    };

    void* mapped_value = nullptr;
    vkMapMemory(device, ValueMem, 0, VK_WHOLE_SIZE, 0, &mapped_value); //内存映射
    memcpy(mapped_value, data.values.data(),
           sizeof(float) * data.values.size());
    vkUnmapMemory(device, ValueMem);

    // ---------------- ShaderNeedInfoBuffer ----------------
    VkBuffer ShaderNeedInfoBuffer;

    VkDeviceSize shaderNeedInfoBytes = sizeof(ShaderNeedInfo);

    VkBufferCreateInfo bufInfo_ShaderNeedInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        shaderNeedInfoBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufInfo_ShaderNeedInfo, nullptr, &ShaderNeedInfoBuffer);

    // memory requirements
    VkMemoryRequirements memReq_ShaderNeedInfo;
    vkGetBufferMemoryRequirements(device, ShaderNeedInfoBuffer, &memReq_ShaderNeedInfo);

    // device address flag（和你 ValueBuffer 保持一致）
    VkMemoryAllocateFlagsInfo flagsInfo_ShaderNeedInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo_ShaderNeedInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &flagsInfo_ShaderNeedInfo,
        memReq_ShaderNeedInfo.size,
        findMemoryType(
            physicalDevice,
            memReq_ShaderNeedInfo.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        )
    };

    VkDeviceMemory ShaderNeedInfoMem;
    vkAllocateMemory(device, &allocInfo_ShaderNeedInfo, nullptr, &ShaderNeedInfoMem);

    // bind
    vkBindBufferMemory(device, ShaderNeedInfoBuffer, ShaderNeedInfoMem, 0);

    // （可选）device address，如果你后面要用
    VkBufferDeviceAddressInfo addrInfo_ShaderNeedInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        ShaderNeedInfoBuffer
    };

    void* mapped_shaderinfo = nullptr;
    vkMapMemory(device, ShaderNeedInfoMem, 0, VK_WHOLE_SIZE, 0, &mapped_shaderinfo);
    memcpy(mapped_shaderinfo, &shaderneedinfo, sizeof(ShaderNeedInfo));
    vkUnmapMemory(device, ShaderNeedInfoMem);


    /* ----------10. 创建基本光线追踪管线结构----------*/

    /*  10.1 创建Pipeline Layout        */

/* ----------10.1 创建 Descriptor Set Layout ---------- */
    VkDescriptorSetLayoutBinding tlasBinding{};
    tlasBinding.binding = 0; // GLSL binding = 0
    tlasBinding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    tlasBinding.descriptorCount = 1;
    tlasBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutBinding rayBinding{};
    rayBinding.binding = 1; // GLSL binding = 1
    rayBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    rayBinding.descriptorCount = 1;
    rayBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutBinding resultBinding{};
    resultBinding.binding = 2; // GLSL binding = 2
    resultBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resultBinding.descriptorCount = 1;
    resultBinding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutBinding vertexBinding{};
    vertexBinding.binding = 3; // GLSL binding = 2
    vertexBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    vertexBinding.descriptorCount = 1;
    vertexBinding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutBinding valueBinding{};
    valueBinding.binding = 4; // GLSL binding = 2
    valueBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    valueBinding.descriptorCount = 1;
    valueBinding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutBinding NeedInfoBinding{};
    NeedInfoBinding.binding = 5; // GLSL binding = 2
    NeedInfoBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    NeedInfoBinding.descriptorCount = 1;
    NeedInfoBinding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    std::array<VkDescriptorSetLayoutBinding, 6> bindings = {tlasBinding, rayBinding, resultBinding ,vertexBinding ,valueBinding , NeedInfoBinding };

    VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
    descLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descLayoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout descriptorSetLayout;
    vkCreateDescriptorSetLayout(device, &descLayoutInfo, nullptr, &descriptorSetLayout);

    /* ----------10.2 创建 Pipeline Layout ---------- */
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 0;
    layoutInfo.pPushConstantRanges = nullptr;

    VkPipelineLayout pipelineLayout;
    vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

    /* ----------10.3 创建 Descriptor Pool ---------- */
 

    std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            1
        },
        {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            5   // ray buffer + result buffer + value/matrix buffer
        }
    }};

    VkDescriptorPoolCreateInfo poolInfo2{};
    poolInfo2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo2.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo2.pPoolSizes = poolSizes.data();
    poolInfo2.maxSets = 1;
    
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkResult res = vkCreateDescriptorPool(device, &poolInfo2, nullptr, &descriptorPool);
    assert(res == VK_SUCCESS);


    /* ----------10.4 分配 Descriptor Set ---------- */
    VkDescriptorSetAllocateInfo allocInfo2{};
    allocInfo2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo2.descriptorPool = descriptorPool;
    allocInfo2.descriptorSetCount = 1;
    allocInfo2.pSetLayouts = &descriptorSetLayout;

    VkDescriptorSet descriptorSet;
    vkAllocateDescriptorSets(device, &allocInfo2, &descriptorSet);

    /* ----------10.5 更新 Descriptor Set (绑定 TLAS) ---------- */

    VkDescriptorBufferInfo rayBufferInfo{};
    rayBufferInfo.buffer = rayBuffer;
    rayBufferInfo.offset = 0;
    rayBufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo resultBufferInfo{};
    resultBufferInfo.buffer = ResultBuffer;
    resultBufferInfo.offset = 0;
    resultBufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo vertexBufferInfo{};
    vertexBufferInfo.buffer = vertexBuffer;
    vertexBufferInfo.offset = 0;
    vertexBufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo valueBufferInfo{};
    valueBufferInfo.buffer = ValueBuffer;
    valueBufferInfo.offset = 0;
    valueBufferInfo.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo NeedInfoBufferInfo{};
    NeedInfoBufferInfo.buffer = ShaderNeedInfoBuffer;
    NeedInfoBufferInfo.offset = 0;
    NeedInfoBufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSetAccelerationStructureKHR descriptorAS{};
    descriptorAS.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    descriptorAS.accelerationStructureCount = 1;
    descriptorAS.pAccelerationStructures = &tlas;

    std::array<VkWriteDescriptorSet, 6> descriptorWrites{};

    // TLAS
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = descriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    descriptorWrites[0].pNext = &descriptorAS;

    // 光线 buffer
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = descriptorSet;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[1].pBufferInfo = &rayBufferInfo;

    // 结果 buffer
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].dstSet = descriptorSet;
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[2].pBufferInfo = &resultBufferInfo;

    //vertex buffer
    descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[3].dstSet = descriptorSet;
    descriptorWrites[3].dstBinding = 3;
    descriptorWrites[3].descriptorCount = 1;
    descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[3].pBufferInfo = &vertexBufferInfo;

    descriptorWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[4].dstSet = descriptorSet;
    descriptorWrites[4].dstBinding = 4;
    descriptorWrites[4].descriptorCount = 1;
    descriptorWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[4].pBufferInfo = &valueBufferInfo;

    descriptorWrites[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[5].dstSet = descriptorSet;
    descriptorWrites[5].dstBinding = 5;
    descriptorWrites[5].descriptorCount = 1;
    descriptorWrites[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[5].pBufferInfo = &NeedInfoBufferInfo;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);


    /*  10.2 读取着色器文件              */
    
    // 1. Shader 模块加载
    string raygen_filename = "/home/zjw/SPMSPM/vulkan-sim/SPMSPM/GLSL/raygen.spv";
    VkShaderModule raygenModule = createShaderModule(raygen_filename, device);

    string miss_filename = "/home/zjw/SPMSPM/vulkan-sim/SPMSPM/GLSL/miss.spv";
    VkShaderModule missModule = createShaderModule(miss_filename, device);

    string anyhit_filename = "/home/zjw/SPMSPM/vulkan-sim/SPMSPM/GLSL/anyhit.spv";
    VkShaderModule anyhitModule = createShaderModule(anyhit_filename, device);

    string closehit_filename = "/home/zjw/SPMSPM/vulkan-sim/SPMSPM/GLSL/closehit.spv";
    VkShaderModule closeHitModule = createShaderModule(closehit_filename, device);

    // 2. 创建 Shader Stage
    std::vector<VkPipelineShaderStageCreateInfo> stages;

    auto makeStage = [&](VkShaderModule sm, VkShaderStageFlagBits stage) {
        VkPipelineShaderStageCreateInfo s{};
        s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage  = stage;
        s.module = sm;
        s.pName  = "main";
        return s;
    };

    stages.push_back(makeStage(raygenModule, VK_SHADER_STAGE_RAYGEN_BIT_KHR));
    stages.push_back(makeStage(missModule,   VK_SHADER_STAGE_MISS_BIT_KHR));
    stages.push_back(makeStage(anyhitModule, VK_SHADER_STAGE_ANY_HIT_BIT_KHR));
    stages.push_back(makeStage(closeHitModule, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)); // 新增

    // 3. Shader Groups 配置
    vector<VkRayTracingShaderGroupCreateInfoKHR> shader_groups;

    // Raygen Group
    VkRayTracingShaderGroupCreateInfoKHR raygenGroup{};
    raygenGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    raygenGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    raygenGroup.generalShader = 0; // stages[0] -> raygen
    raygenGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
    raygenGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
    raygenGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
    shader_groups.push_back(raygenGroup);

    // Miss Group
    VkRayTracingShaderGroupCreateInfoKHR missGroup{};
    missGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    missGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    missGroup.generalShader = 1; // stages[1] -> miss
    missGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
    missGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
    missGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
    shader_groups.push_back(missGroup);

    // AnyHit Group (Triangles Hit Group)
    VkRayTracingShaderGroupCreateInfoKHR hitGroup{};
    hitGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    hitGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    hitGroup.anyHitShader = 2;      // stages[2] -> anyhit
    hitGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
    hitGroup.generalShader = VK_SHADER_UNUSED_KHR;
    hitGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
    shader_groups.push_back(hitGroup);

    // CloseHit Group
    VkRayTracingShaderGroupCreateInfoKHR closeHitGroup{};
    closeHitGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    closeHitGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    closeHitGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
    closeHitGroup.closestHitShader = 3; // stages[3] -> closehit
    closeHitGroup.generalShader = VK_SHADER_UNUSED_KHR;
    closeHitGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
    shader_groups.push_back(closeHitGroup);

    // 4. 创建 Ray Tracing Pipeline
    VkRayTracingPipelineCreateInfoKHR rtInfo{};
    rtInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtInfo.stageCount = static_cast<uint32_t>(stages.size());
    rtInfo.pStages = stages.data();
    rtInfo.groupCount = static_cast<uint32_t>(shader_groups.size());
    rtInfo.pGroups = shader_groups.data();
    rtInfo.maxPipelineRayRecursionDepth = 1;
    rtInfo.layout = pipelineLayout;

    VkPipeline rtPipeline;
    VkResult result = vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtInfo, nullptr, &rtPipeline);
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create Ray Tracing Pipeline: " << result << std::endl;
    } else {
        std::cout << "Ray Tracing Pipeline created successfully!" << std::endl;
    }

    // 5. 创建 SBT 并增加 CloseHit
    SbtRecord<RayData> rgSbt{};
    rgSbt.data.originVec = (glm::vec4*)addr_ray;
    rgSbt.data.size = rayCount;

    SbtRecord<MissData> msSbt{};
    msSbt.data = {0.3f, 0.1f, 0.2f};

    SbtRecord<SphereData> hgSbt{};
    hgSbt.data.sphereColor = (float*)valueAddr;
    hgSbt.data.resultNumRow = Resultrow;
    hgSbt.data.resultNumCol = Resultcow;
    hgSbt.data.result = (float*)addr_Result;

    SbtRecord<CloseHitData> chSbt{}; // 新增 CloseHit payload
    chSbt.data.hitColor = (float*)valueAddr; // 或者单独定义
    chSbt.data.resultNumRow = Resultrow;
    chSbt.data.resultNumCol = Resultcow;
    chSbt.data.result = (float*)addr_Result;

    VulkanSBTRegions sbtRegions{};
    VkBuffer sbtBuffer;
    VkDeviceMemory sbtMemory;

    createShaderBindingTable(device, rtPipeline, rtProperties, stages, sbtRegions,
                            &rgSbt, &msSbt, &hgSbt, &chSbt, sbtBuffer, sbtMemory, physicalDevice);

    assert(sbtRegions.raygenRegion.deviceAddress != 0);
    assert(sbtRegions.missRegion.deviceAddress != 0);
    assert(sbtRegions.hitRegion.deviceAddress != 0);
    assert(sbtRegions.closeHitRegion.deviceAddress != 0);

    std::cout << "*************success: SBT table binding with CloseHit*************" << std::endl;

    /*  ----------12 Launch ----------------------*/

  // 假设你已经有一个 VkCommandBuffer cmdBuffer，并在 recording 状态

    // 1. 准备 SBT 信息
    VkStridedDeviceAddressRegionKHR raygenSBT{};
    VkStridedDeviceAddressRegionKHR missSBT{};
    VkStridedDeviceAddressRegionKHR hitSBT{};
    VkStridedDeviceAddressRegionKHR callableSBT{};
    VkStridedDeviceAddressRegionKHR closeSBT{};
    // 设置 SBT 地址和 stride
    // 假设你已经创建好 SBT Buffer 并获得 deviceAddress
    raygenSBT=sbtRegions.raygenRegion;
    missSBT=sbtRegions.missRegion;
    hitSBT=sbtRegions.hitRegion;
    closeSBT=sbtRegions.closeHitRegion;
    callableSBT=sbtRegions.callableRegion;


    // 2. Launch 光线追踪
    vkResetCommandBuffer(cmd, 0);  
    VkCommandBufferBeginInfo beginInfo1{};
    beginInfo1.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo1.flags = 0; // 或 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    beginInfo1.pInheritanceInfo = nullptr;

    VkResult result1 = vkBeginCommandBuffer(cmd, &beginInfo1);
    if (result1 != VK_SUCCESS) {
        // 错误处理
    }
    vkCmdBindPipeline(
        cmd,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        rtPipeline
    );

    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        pipelineLayout,
        0,
        1, &descriptorSet,
        0, nullptr
    );

    vkCmdTraceRaysKHR(
        cmd,
        &raygenSBT,
        &missSBT,
        &hitSBT,
        &callableSBT,
        rayCount,   // 图像宽度
        Resultcow,  // 图像高度
        1        // depth
    );

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    float* mappedResult;
    vkMapMemory(device, ResultMem, 0, VK_WHOLE_SIZE, 0, (void**)&mappedResult);
    cout<<"*****结果******"<<mappedResult[0]<<endl;
    cout<<"*****结果******"<<mappedResult[1]<<endl;
    cout<<"*****结果******"<<mappedResult[2]<<endl;


    // mappedResult[i] 就是 any-hit shader atomicAdd 的结果
    vkUnmapMemory(device, ResultMem);



    
    /* ---------- 8. 清理 ---------- */

    // vkDestroyBuffer(device, vertexBuffer, nullptr);
    // vkFreeMemory(device, vertexMem, nullptr);
    // vkDestroyDevice(device, nullptr);
    // vkDestroyInstance(instance, nullptr);
/* ---------- 10. 清理 ---------- */

// 1. 确保 GPU 完全执行完毕
    vkDeviceWaitIdle(device);

    // 2. 销毁加速结构（先销毁对象，再销毁 backing buffer）
    if (AS_bvh_KHR != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, AS_bvh_KHR, nullptr);
    }

    // 3. 销毁 scratch buffer + memory
    if (scratchBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, scratchBuffer, nullptr);
    }
    if (scratchMem != VK_NULL_HANDLE) {
        vkFreeMemory(device, scratchMem, nullptr);
    }

    // 4. 销毁 AS buffer + memory
    if (AS_bvh_Buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, AS_bvh_Buffer, nullptr);
    }
    if (asMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, asMemory, nullptr);
    }

    // 5. 销毁 vertex/index buffer + memory
    if (vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vertexBuffer, nullptr);
    }
    if (vertexMem != VK_NULL_HANDLE) {
        vkFreeMemory(device, vertexMem, nullptr);
    }

    // 6. 销毁 command pool（会自动释放 command buffer）
    if (commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool, nullptr);
    }

    // 7. 销毁 logical device
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }

    // 8. 销毁 instance
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }

    return 0;
}
