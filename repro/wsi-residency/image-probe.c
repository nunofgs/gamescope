#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(call) do { VkResult result = (call); if (result != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #call, result); exit(1); } } while (0)
#define WSI_MEMORY_TYPE ((VkStructureType)1000001003)
struct legacy_wsi { VkStructureType sType; const void *pNext; bool implicit_sync; };
struct fixed_wsi { VkStructureType sType; const void *pNext; bool implicit_sync; bool dma_buf_sync_file; };
struct padded_wsi { VkStructureType sType; const void *pNext; bool implicit_sync; bool dma_buf_sync_file; bool reserved[6]; };

static VkDevice device;
static VkPhysicalDevice physical;

static VkDeviceAddress allocate(const void *hint, VkBuffer *buffer, VkDeviceMemory *memory)
{
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo create = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(device, &create, NULL, buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    }
    if (type == UINT32_MAX) { fprintf(stderr, "No device-local memory type\n"); exit(1); }
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = hint, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkExportMemoryAllocateInfo export = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &flags, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryDedicatedAllocateInfo dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &export, .buffer = *buffer };
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated, .allocationSize = requirements.size, .memoryTypeIndex = type };
    CHECK(vkAllocateMemory(device, &allocation, NULL, memory));
    CHECK(vkBindBufferMemory(device, *buffer, *memory, 0));
    VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = *buffer };
    return vkGetBufferDeviceAddress(device, &address);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "Usage: image-probe legacy|field|padding|none shader.spv\n"); return 2; }
    struct legacy_wsi legacy;
    struct fixed_wsi field;
    struct padded_wsi padding;
    memset(&legacy, 0, sizeof(legacy)); legacy.sType = WSI_MEMORY_TYPE;
    memset(&field, 0, sizeof(field)); field.sType = WSI_MEMORY_TYPE;
    memset(&padding, 0, sizeof(padding)); padding.sType = WSI_MEMORY_TYPE;
    const void *hint = NULL;
    if (!strcmp(argv[1], "legacy")) hint = &legacy;
    else if (!strcmp(argv[1], "field")) hint = &field;
    else if (!strcmp(argv[1], "padding")) hint = &padding;
    else if (strcmp(argv[1], "none")) return 2;
    VkApplicationInfo application = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "wsi-residency-probe", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo instanceInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application };
    VkInstance instance;
    CHECK(vkCreateInstance(&instanceInfo, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
        vkGetPhysicalDeviceProperties2(devices[i], &properties);
        if (driver.driverID == VK_DRIVER_ID_MESA_RADV) { physical = devices[i]; strcpy(name, properties.properties.deviceName); break; }
    }
    free(devices);
    if (!physical) { fprintf(stderr, "No RADV device\n"); return 1; }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    free(families);
    if (family == UINT32_MAX) return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    VkPhysicalDeviceBufferDeviceAddressFeatures features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .bufferDeviceAddress = VK_TRUE };
    const char *extensions[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME };
    VkDeviceCreateInfo deviceInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions };
    CHECK(vkCreateDevice(physical, &deviceInfo, NULL, &device));

    VkImage image;
    VkExternalMemoryImageCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkImageCreateInfo imageInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1,1,1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(device, &imageInfo, NULL, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { memoryType = i; break; }
    }
    if (memoryType == UINT32_MAX) return 1;
    VkExportMemoryAllocateInfo export = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = hint, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryDedicatedAllocateInfo dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &export, .image = image };
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated, .allocationSize = requirements.size, .memoryTypeIndex = memoryType };
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    uint64_t allocationStart = (uint64_t)timestamp.tv_sec * 1000000000 + timestamp.tv_nsec;
    VkDeviceMemory imageMemory;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &imageMemory));
    CHECK(vkBindImageMemory(device, image, imageMemory, 0));
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    uint64_t allocationEnd = (uint64_t)timestamp.tv_sec * 1000000000 + timestamp.tv_nsec;
    VkBuffer buffer; VkDeviceMemory bufferMemory;
    VkDeviceAddress control = allocate(NULL, &buffer, &bufferMemory);
    VkImageViewCreateInfo viewInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = imageInfo.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} };
    VkImageView view; CHECK(vkCreateImageView(device, &viewInfo, NULL, &view));
    VkSamplerCreateInfo samplerInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler sampler; CHECK(vkCreateSampler(device, &samplerInfo, NULL, &sampler));
    VkDescriptorSetLayoutBinding bindings[] = {
        {.binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}
    };
    VkDescriptorSetLayoutCreateInfo setInfo = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=2, .pBindings=bindings };
    VkDescriptorSetLayout setLayout; CHECK(vkCreateDescriptorSetLayout(device, &setInfo, NULL, &setLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo descriptorPoolInfo = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets=1, .poolSizeCount=2, .pPoolSizes=sizes };
    VkDescriptorPool descriptorPool; CHECK(vkCreateDescriptorPool(device, &descriptorPoolInfo, NULL, &descriptorPool));
    VkDescriptorSetAllocateInfo setAllocation = { .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool=descriptorPool, .descriptorSetCount=1, .pSetLayouts=&setLayout };
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &setAllocation, &set));
    VkDescriptorImageInfo sampled = { .sampler=sampler, .imageView=view, .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo output = { .buffer=buffer, .offset=0, .range=4096 };
    VkWriteDescriptorSet writes[] = {
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo=&sampled},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set, .dstBinding=1, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo=&output}
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    FILE *shaderFile = fopen(argv[2], "rb");
    if (!shaderFile) return 1;
    fseek(shaderFile, 0, SEEK_END); long shaderSize=ftell(shaderFile); rewind(shaderFile);
    if (shaderSize <= 0 || shaderSize % 4) return 1;
    uint32_t *code=malloc(shaderSize);
    if (fread(code,1,shaderSize,shaderFile) != (size_t)shaderSize) return 1;
    fclose(shaderFile);
    VkShaderModuleCreateInfo shaderInfo = { .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=shaderSize, .pCode=code };
    VkShaderModule shader; CHECK(vkCreateShaderModule(device,&shaderInfo,NULL,&shader)); free(code);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&setLayout };
    VkPipelineLayout pipelineLayout; CHECK(vkCreatePipelineLayout(device,&pipelineLayoutInfo,NULL,&pipelineLayout));
    VkComputePipelineCreateInfo pipelineInfo = { .sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_COMPUTE_BIT, .module=shader, .pName="main"}, .layout=pipelineLayout };
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,NULL,&pipeline));
    VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    VkCommandPool pool; CHECK(vkCreateCommandPool(device, &poolInfo, NULL, &pool));
    VkCommandBufferAllocateInfo commandInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &commandInfo, &command));
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier = { .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED, .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .image=image, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} };
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkClearColorValue color = { .float32={0.25f,0.5f,0.75f,1.0f} };
    vkCmdClearColorImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
    CHECK(vkEndCommandBuffer(command));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command };
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    CHECK(vkResetCommandBuffer(command,0)); CHECK(vkBeginCommandBuffer(command,&begin));
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,NULL);
    vkCmdDispatch(command,1,1,1);
    CHECK(vkEndCommandBuffer(command));
    printf("{\"case\":\"%s\",\"device\":\"%s\",\"control_va\":\"0x%llx\",\"image_bytes\":%llu,\"allocation_start_ns\":%llu,\"allocation_end_ns\":%llu,\"compute_submissions\":1}\n",argv[1],name,(unsigned long long)control,(unsigned long long)requirements.size,(unsigned long long)allocationStart,(unsigned long long)allocationEnd);
    fflush(stdout);
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    vkDestroyCommandPool(device,pool,NULL); vkDestroyPipeline(device,pipeline,NULL);
    vkDestroyPipelineLayout(device,pipelineLayout,NULL); vkDestroyShaderModule(device,shader,NULL);
    vkDestroyDescriptorPool(device,descriptorPool,NULL); vkDestroyDescriptorSetLayout(device,setLayout,NULL);
    vkDestroySampler(device,sampler,NULL); vkDestroyImageView(device,view,NULL);
    vkDestroyImage(device,image,NULL); vkFreeMemory(device,imageMemory,NULL);
    vkDestroyBuffer(device,buffer,NULL); vkFreeMemory(device,bufferMemory,NULL);
    vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    return 0;
}
