#include <vulkan/vulkan.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cassert>

using namespace std;

/* ================= CPU 数据结构 ================= */

struct float3 {
    float x, y, z;
};

struct TriFromMatrixCPU {
    vector<float3>   vertices;
    vector<uint32_t> indices;
    vector<float>    values;
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
            continue;
        }

        float x, y, val;
        istringstream iss(line);
        if (!(iss >> x >> y >> val))
            continue;

        float dx[3] = {-0.5f, 0.0f, 0.5f};
        float dy[3] = {0.0f, 0.5f, 0.0f};

        for (int i = 0; i < 3; i++) {
            output.vertices.push_back({x + dx[i], y + dy[i], 0.0f});
            output.indices.push_back(index++);
        }

        output.values.push_back(val);
    }

    return output;
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

/* ================= 主程序 ================= */

int main()
{
    /* ---------- 1. Vulkan Instance ---------- */

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

    VkPhysicalDeviceBufferDeviceAddressFeatures addrFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
        nullptr,
        VK_TRUE
    };

    VkDeviceCreateInfo devInfo{
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        &addrFeat,
        0,
        1,
        &qinfo
    };

    VkDevice device;
    vkCreateDevice(physicalDevice, &devInfo, nullptr, &device);
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    /* ---------- 4. 读取矩阵并生成三角形 ---------- */

    string matrixFile =
        "/home/zjw/SPMSPM/optix/RTSpMSpM/optixSpMSpM/src/data/web-Google/web-Google_small.mtx";

    TriFromMatrixCPU data = Data_transform_T1(matrixFile);

    int count = data.vertices.size();
    cout << "Vertices: " << data.vertices.size()
         << " Indices: " << data.indices.size() << endl;

    /* ---------- 5. 创建 Vertex Buffer ---------- */

    VkBuffer vertexBuffer;  //缓存资源
    VkDeviceSize vertexBytes = sizeof(float3) * data.vertices.size();
    VkDeviceSize indexBytes  = sizeof(uint32_t) * data.indices.size();
    VkBufferCreateInfo bufInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        vertexBytes + indexBytes ,
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
    vkUnmapMemory(device, vertexMem);

    /* ---------- 7. 获取 VkDeviceAddress ---------- */

    VkBufferDeviceAddressInfo addrInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        vertexBuffer
    };

    VkDeviceAddress addr = vkGetBufferDeviceAddress(device, &addrInfo);
    VkDeviceAddress indexAddr = addr + vertexBytes;

    cout << "Vertex Buffer Device Address: 0x"
         << hex << addr << dec << endl;

    if (addr == 0) {
        cerr << "ERROR: Device address is zero\n";
    } else {
        cout << "SUCCESS: Vulkan-sim + Mesa device address works\n";
    }


//     /* ---------- 9. 构建加速结构 ----------*/

//     VkPhysicalDeviceAccelerationStructureFeaturesKHR bvh;
//     VkBuffer AS_bvh_Buffer;  // 保存加速结构
    
//     VkAccelerationStructureKHR AS_bvh_KHR;

// /*
//     9.1 设置数据信息
// */
//     VkAccelerationStructureGeometryTrianglesDataKHR triangles{
//         .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
//         .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
//         .vertexData = {.deviceAddress = addr},
//         .vertexStride = sizeof(float3),
//         .maxVertex = (uint32_t)count-1,
//         .indexType = VK_INDEX_TYPE_UINT32,
//         .indexData = {.deviceAddress = indexAddr},
//     };

//     VkAccelerationStructureGeometryKHR geometry{
//         .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
//         .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
//         .geometry = {.triangles = triangles}, // union传参
//         .flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR | VK_GEOMETRY_OPAQUE_BIT_KHR,
//     };

//     const auto triangleCount = static_cast<uint32_t>(count / 3U);
//     VkAccelerationStructureBuildRangeInfoKHR rangeInfo{
//         .primitiveCount = triangleCount
//     };
// /*
//     9.2 构建信息设置
//     创建VkAccelerationStructureBuildGeometryInfoKHR包含几何数据的结构
//     设置加速结构类型（BLAS 或 TLAS）、构建标志和模式
// */
//     VkAccelerationStructureBuildGeometryInfoKHR asBuildInfo{
//         .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
//         .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
//         .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
//         .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
//         .geometryCount = 1,
//         .pGeometries = &geometry,
//     };

// /*
//     9.3 尺寸计算
//     vkGetAccelerationStructureBuildSizesKHR需要确定 以下事项：
//     加速结构本身所需的尺寸
//     暂存缓冲区（临时工作内存）所需大小
//     更新暂存缓冲区所需大小（用于未来的更新）
// */
//     VkAccelerationStructureBuildSizesInfoKHR asBuildSize{
//         .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
//     };

//     std::vector<uint32_t> maxPrimCount(1);
//     maxPrimCount[0] = rangeInfo.primitiveCount;
//     vkGetAccelerationStructureBuildSizesKHR(device,
//                                 VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
//                                 &asBuildInfo,
//                                 maxPrimCount.data(),
//                                 &asBuildSize);

// /*
//     9.4  创建buffer

// */

// //9.4.1 创建 buffer（只给用途，不放数据）
//     VkBufferCreateInfo asBufInfo{
//         VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
//         nullptr,
//         0,
//         asBuildSize.accelerationStructureSize,
//         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
//         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
//         VK_SHARING_MODE_EXCLUSIVE
//     };

//     vkCreateBuffer(device, &asBufInfo, nullptr, &AS_bvh_Buffer);

// //9.4.2 分配并绑定内存（必须有 DEVICE_ADDRESS）

//     VkMemoryRequirements asMemReq;
//     vkGetBufferMemoryRequirements(device, AS_bvh_Buffer, &asMemReq);

//     VkMemoryAllocateFlagsInfo asAllocFlags{
//         VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
//         nullptr,
//         VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
//     };

//     VkMemoryAllocateInfo asAllocInfo{
//         VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
//         &asAllocFlags,
//         asMemReq.size,
//         findMemoryType(physicalDevice,
//                     asMemReq.memoryTypeBits,
//                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
//     };

//     VkDeviceMemory asMemory;
//     vkAllocateMemory(device, &asAllocInfo, nullptr, &asMemory);
//     vkBindBufferMemory(device, AS_bvh_Buffer, asMemory, 0);

// //9.4.3 创建 VkAccelerationStructureKHR 对象

//     VkAccelerationStructureCreateInfoKHR asCreateInfo{
//     VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
//     nullptr,
//     0,
//     AS_bvh_Buffer,
//     0,
//     asBuildSize.accelerationStructureSize,
//     VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
//     };

//     vkCreateAccelerationStructureKHR(
//         device,
//         &asCreateInfo,
//         nullptr,
//         &AS_bvh_KHR
//     );

// // 9.4.4 创建 Scratch Buffer（构建时的临时内存）

// // 创建 scratch buffer
//     VkBuffer scratchBuffer;

//     VkBufferCreateInfo scratchBufInfo{
//         VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
//         nullptr,
//         0,
//         asBuildSize.buildScratchSize,
//         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
//         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
//         VK_SHARING_MODE_EXCLUSIVE
//     };

//     vkCreateBuffer(device, &scratchBufInfo, nullptr, &scratchBuffer);
// // 分配 + 绑定
//     VkMemoryRequirements scratchReq;
//     vkGetBufferMemoryRequirements(device, scratchBuffer, &scratchReq);

//     VkMemoryAllocateFlagsInfo scratchFlags{
//         VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
//         nullptr,
//         VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
//     };

//     VkMemoryAllocateInfo scratchAlloc{
//         VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
//         &scratchFlags,
//         scratchReq.size,
//         findMemoryType(physicalDevice,
//                     scratchReq.memoryTypeBits,
//                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
//     };

//     VkDeviceMemory scratchMem;
//     vkAllocateMemory(device, &scratchAlloc, nullptr, &scratchMem);
//     vkBindBufferMemory(device, scratchBuffer, scratchMem, 0);
// // 获取 scratch device address
//     VkBufferDeviceAddressInfo scratchAddrInfo{
//         VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
//         nullptr,
//         scratchBuffer
//     };

//     VkDeviceAddress scratchAddr =
//         vkGetBufferDeviceAddress(device, &scratchAddrInfo);

// // 9.4.5 真正构建 BLAS（命令缓冲）

//     asBuildInfo.dstAccelerationStructure = AS_bvh_KHR;
//     asBuildInfo.scratchData.deviceAddress = scratchAddr;

// // 记录命令

//     VkCommandPool commandPool;

//     VkCommandPoolCreateInfo poolInfo{
//         VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
//         nullptr,
//         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
//         0   // queueFamilyIndex = 0，与 device queue 一致
//     };

//     vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);


//     VkCommandBufferAllocateInfo cmdAlloc{
//         VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
//         nullptr,
//         commandPool,
//         VK_COMMAND_BUFFER_LEVEL_PRIMARY,
//         1
//     };

//     VkCommandBuffer cmd;
//     vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

//     VkCommandBufferBeginInfo beginInfo{
//         VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
//     };

//     vkBeginCommandBuffer(cmd, &beginInfo);

//     const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

//     vkCmdBuildAccelerationStructuresKHR(
//         cmd,
//         1,
//         &asBuildInfo,
//         &pRangeInfo
//     );

//     vkEndCommandBuffer(cmd);

// // 提交并等待完成
//     VkSubmitInfo submit{};
//     submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
//     submit.commandBufferCount = 1;
//     submit.pCommandBuffers = &cmd;

//     vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
//     vkQueueWaitIdle(queue);


// // 测试构建结构

//     cout<<"测试加速结构构建结构"<<endl;
//     VkAccelerationStructureDeviceAddressInfoKHR asAddrInfo{
//         VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
//         nullptr,
//         AS_bvh_KHR
//     };

//     VkDeviceAddress blasAddr =
//         vkGetAccelerationStructureDeviceAddressKHR(device, &asAddrInfo);

//     std::cout << "BLAS device address: 0x"
//             << std::hex << blasAddr << std::dec << std::endl;

//     if (blasAddr == 0) {
//         std::cerr << "ERROR: BLAS device address is zero\n";
//     } else {
//         std::cout << "SUCCESS: BLAS was built correctly\n";
//     }


    /* ---------- 8. 清理 ---------- */

    vkDestroyBuffer(device, vertexBuffer, nullptr);
    vkFreeMemory(device, vertexMem, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    return 0;
}