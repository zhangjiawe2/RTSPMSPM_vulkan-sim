#include <vulkan/vulkan.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cstring>

#define VK_CHECK(x) assert((x) == VK_SUCCESS)

std::vector<char> readFile(const char* filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    size_t size = file.tellg();
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

int main() {
    /* 1. Instance */
    VkInstance instance;
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    /* 2. Physical device */
    uint32_t physCount = 0;
    vkEnumeratePhysicalDevices(instance, &physCount, nullptr);
    assert(physCount > 0);
    VkPhysicalDevice phys;
    vkEnumeratePhysicalDevices(instance, &physCount, &phys);

    /* 3. Queue family */
    uint32_t qCount;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qCount, qprops.data());

    uint32_t computeQ = 0;
    for (uint32_t i = 0; i < qCount; i++)
        if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            computeQ = i;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = computeQ;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    /* 4. Device */
    VkDevice device;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));

    VkQueue queue;
    vkGetDeviceQueue(device, computeQ, 0, &queue);

    /* 5. Buffer */
    int hostData[4] = {0, 1, 2, 3};
    VkBuffer buffer;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(hostData);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffer));

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, buffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    uint32_t memIndex = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            memIndex = i;
            break;
        }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = memIndex;

    VkDeviceMemory memory;
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &memory));
    vkBindBufferMemory(device, buffer, memory, 0);

    void* mapped;
    vkMapMemory(device, memory, 0, sizeof(hostData), 0, &mapped);
    memcpy(mapped, hostData, sizeof(hostData));
    vkUnmapMemory(device, memory);

    /* 6. Descriptor */
    VkDescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 1;
    dlci.pBindings = &bind;

    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(device, &dlci, nullptr, &dsl);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;

    VkPipelineLayout pipelineLayout;
    vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout);

    /* 7. Pipeline */
    auto spv = readFile("add.comp.spv");
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(spv.data());

    VkShaderModule shader;
    vkCreateShaderModule(device, &smci, nullptr, &shader);

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr, 0,
        VK_SHADER_STAGE_COMPUTE_BIT,
        shader, "main"
    };
    cpci.layout = pipelineLayout;

    VkPipeline pipeline;
    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);

    /* 8. Descriptor set */
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;

    VkDescriptorPool pool;
    vkCreateDescriptorPool(device, &dpci, nullptr, &pool);

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;

    VkDescriptorSet set;
    vkAllocateDescriptorSets(device, &dsai, &set);

    VkDescriptorBufferInfo dbi{buffer, 0, sizeof(hostData)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &dbi;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    /* 9. Command buffer */
    VkCommandPoolCreateInfo cpci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci2.queueFamilyIndex = computeQ;

    VkCommandPool cmdPool;
    vkCreateCommandPool(device, &cpci2, nullptr, &cmdPool);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cbai, &cmd);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    /* 10. Read back */
    vkMapMemory(device, memory, 0, sizeof(hostData), 0, &mapped);
    int* out = static_cast<int*>(mapped);
    std::cout << out[0] << " " << out[1] << " "
              << out[2] << " " << out[3] << std::endl;
    vkUnmapMemory(device, memory);

    return 0;
}
