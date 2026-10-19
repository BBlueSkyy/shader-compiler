// SPDX-License-Identifier: GPL-2.0-or-later
// Executes production EmitSPIRV output; no replacement GLSL or handwritten SPIR-V.
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t Lanes = 64;
constexpr uint32_t TicCount = 6;
constexpr uint32_t TscCount = 8;
constexpr uint32_t MapOffset = 9 + TscCount;
constexpr uint32_t MetadataWords = MapOffset + TicCount;
using Pixel = std::array<uint32_t, 4>;
std::atomic_uint ValidationErrors{};
VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++ValidationErrors;
    std::cerr << "Vulkan validation: " << data->pMessage << '\n';
    return VK_FALSE;
}
bool ValidationRequested() {
    const char* layers = std::getenv("VK_INSTANCE_LAYERS");
    return layers && std::strstr(layers, "VK_LAYER_KHRONOS_validation");
}
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string{operation} + ": VkResult " + std::to_string(result));
    }
}
#define CHECK_VK(expression) Check((expression), #expression)
struct Skip : std::runtime_error { using std::runtime_error::runtime_error; };
struct Buffer { VkBuffer object{}; VkDeviceMemory memory{}; void* mapped{}; };
struct Image { VkImage object{}; VkDeviceMemory memory{}; VkImageView view{}; };

Pixel Expected(uint32_t component, uint32_t slot, uint32_t mip) {
    Pixel result{};
    for (uint32_t channel = 0; channel < 4; ++channel) {
        if (component == 0) {
            result[channel] = std::bit_cast<uint32_t>(10.25f + float(slot * 100 + mip * 1000 + channel));
        } else if (component == 1) {
            result[channel] = 100 + slot * 100 + mip * 1000 + channel;
        } else {
            result[channel] = static_cast<uint32_t>(-100 - int32_t(slot * 100 + mip * 1000 + channel));
        }
    }
    return result;
}

class Fixture {
public:
    VkInstance instance{};
    VkDebugUtilsMessengerEXT debug_messenger{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::array<Buffer, 3> buffers{}; // handles, output, metadata
    std::array<Image, 6> images{}; // component * 2 + compact slot
    std::array<VkSampler, 3> samplers{};
    std::array<VkDescriptorSetLayout, 2> set_layouts{};
    std::array<VkDescriptorSet, 2> sets{};
    VkDescriptorPool descriptor_pool{};
    VkPipelineLayout pipeline_layout{};
    VkShaderModule module{};
    VkPipeline pipeline{};
    VkCommandPool command_pool{};
    VkCommandBuffer command{};
    bool remap{};
    bool via_header{};
    bool float_supports_linear{};
    bool linear_sampler_enabled{};
    uint32_t assertions{};
    std::string operation{"sample"};
    bool Samplerless() const { return operation == "fetch" || operation == "query"; }

    ~Fixture() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (module) vkDestroyShaderModule(device, module, nullptr);
            if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            for (auto layout : set_layouts) if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
            for (auto sampler : samplers) if (sampler) vkDestroySampler(device, sampler, nullptr);
            for (auto image : images) {
                if (image.view) vkDestroyImageView(device, image.view, nullptr);
                if (image.object) vkDestroyImage(device, image.object, nullptr);
                if (image.memory) vkFreeMemory(device, image.memory, nullptr);
            }
            for (auto buffer : buffers) {
                if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
                if (buffer.object) vkDestroyBuffer(device, buffer.object, nullptr);
                if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (debug_messenger) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance, debug_messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) {
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        }
        throw Skip("No compatible memory type");
    }
    Buffer MakeBuffer(VkDeviceSize size) {
        Buffer buffer{};
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = size;
        create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        CHECK_VK(vkCreateBuffer(device, &create, nullptr, &buffer.object));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer.object, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CHECK_VK(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory));
        CHECK_VK(vkBindBufferMemory(device, buffer.object, buffer.memory, 0));
        CHECK_VK(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
        return buffer;
    }
    void InitDevice() {
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Production dynamic texture handle regression";
        application.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &application;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = ValidationCallback;
        const char* debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const char* validation_layer = "VK_LAYER_KHRONOS_validation";
        if (ValidationRequested()) {
            create.pNext = &debug;
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &debug_extension;
            create.enabledLayerCount = 1;
            create.ppEnabledLayerNames = &validation_layer;
        }
        const auto result = vkCreateInstance(&create, nullptr, &instance);
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) throw Skip("No Vulkan 1.2-capable loader/ICD available");
        Check(result, "vkCreateInstance");
        if (ValidationRequested()) {
            const auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!make) throw std::runtime_error("Validation requested but debug messenger unavailable");
            CHECK_VK(make(instance, &debug, nullptr, &debug_messenger));
        }
        uint32_t count{};
        CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count) throw Skip("No Vulkan physical device available");
        std::vector<VkPhysicalDevice> devices(count);
        CHECK_VK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDeviceDescriptorIndexingFeatures supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        for (auto candidate : devices) {
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &supported;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (features.features.shaderSampledImageArrayDynamicIndexing &&
                features.features.shaderImageGatherExtended &&
                supported.shaderSampledImageArrayNonUniformIndexing && supported.runtimeDescriptorArray &&
                supported.descriptorBindingPartiallyBound) { physical = candidate; break; }
        }
        if (!physical) throw Skip("Required sampled image nonuniform indexing/runtime arrays/partially-bound/gather features unavailable");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::cout << "Device: " << properties.deviceName << "; api " << VK_VERSION_MAJOR(properties.apiVersion)
                  << '.' << VK_VERSION_MINOR(properties.apiVersion) << '.' << VK_VERSION_PATCH(properties.apiVersion)
                  << "; driver " << properties.driverVersion << '\n';
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        if (family == count) throw Skip("No compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo queue_create{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex = family;
        queue_create.queueCount = 1;
        queue_create.pQueuePriorities = &priority;
        VkPhysicalDeviceDescriptorIndexingFeatures enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        enable.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enable.runtimeDescriptorArray = VK_TRUE;
        enable.descriptorBindingPartiallyBound = VK_TRUE;
        VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_create.pNext = &enable;
        VkPhysicalDeviceFeatures core{};
        core.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
        // The production backend declares ImageGatherExtended for every module.
        core.shaderImageGatherExtended = VK_TRUE;
        device_create.pEnabledFeatures = &core;
        device_create.queueCreateInfoCount = 1;
        device_create.pQueueCreateInfos = &queue_create;
        const char* extension = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
        if (properties.apiVersion < VK_API_VERSION_1_2) {
            device_create.enabledExtensionCount = 1;
            device_create.ppEnabledExtensionNames = &extension;
        }
        CHECK_VK(vkCreateDevice(physical, &device_create, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        buffers[0] = MakeBuffer(Lanes * sizeof(uint32_t));
        buffers[1] = MakeBuffer(Lanes * sizeof(Pixel));
        buffers[2] = MakeBuffer(MetadataWords * sizeof(uint32_t));
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family;
        CHECK_VK(vkCreateCommandPool(device, &pool, nullptr, &command_pool));
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        CHECK_VK(vkAllocateCommandBuffers(device, &allocate, &command));
    }
    void Begin() {
        CHECK_VK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK_VK(vkBeginCommandBuffer(command, &begin));
    }
    void Submit() {
        CHECK_VK(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        CHECK_VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK_VK(vkQueueWaitIdle(queue));
    }
    void InitResources() {
        constexpr std::array<VkFormat, 3> formats{
            VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT};
        Begin();
        for (uint32_t component = 0; component < 3; ++component) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, formats[component], &properties);
            if (component == 0) float_supports_linear =
                (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
            if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) throw Skip("Required RGBA32 sampled format unavailable");
            for (uint32_t slot = 0; slot < 2; ++slot) {
                auto& image = images[component * 2 + slot];
                VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                create.imageType = VK_IMAGE_TYPE_2D;
                create.format = formats[component];
                create.extent = {2u << slot, 2u << slot, 1};
                create.mipLevels = 2;
                create.arrayLayers = 1;
                create.samples = VK_SAMPLE_COUNT_1_BIT;
                create.tiling = VK_IMAGE_TILING_OPTIMAL;
                create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                CHECK_VK(vkCreateImage(device, &create, nullptr, &image.object));
                VkMemoryRequirements requirements{};
                vkGetImageMemoryRequirements(device, image.object, &requirements);
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocate.allocationSize = requirements.size;
                allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, 0);
                CHECK_VK(vkAllocateMemory(device, &allocate, nullptr, &image.memory));
                CHECK_VK(vkBindImageMemory(device, image.object, image.memory, 0));
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = image.object;
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = formats[component];
                view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1};
                CHECK_VK(vkCreateImageView(device, &view, nullptr, &image.view));
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.object;
                barrier.subresourceRange = view.subresourceRange;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
                for (uint32_t mip = 0; mip < 2; ++mip) {
                    VkClearColorValue clear{};
                    const auto expected = Expected(component, slot, mip);
                    std::memcpy(clear.uint32, expected.data(), sizeof(clear));
                    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
                    vkCmdClearColorImage(command, image.object, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
                }
            }
        }
        VkMemoryBarrier transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        transfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        transfer.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &transfer, 0, nullptr, 0, nullptr);
        Submit();
        for (uint32_t i = 0; i < samplers.size(); ++i) {
            VkSamplerCreateInfo create{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            create.magFilter = create.minFilter = i == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            create.addressModeU = create.addressModeV = create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            create.minLod = create.maxLod = i == 2 ? 0.0f : float(i);
            CHECK_VK(vkCreateSampler(device, &create, nullptr, &samplers[i]));
        }
    }
    void InitPipeline(const char* path) {
        const std::array<VkDescriptorSetLayoutBinding, 2> ordinary{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        const std::array<VkDescriptorSetLayoutBinding, 5> dynamic{{
            {0, VK_DESCRIPTOR_TYPE_SAMPLER, TscCount, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = uint32_t(ordinary.size());
        layout.pBindings = ordinary.data();
        CHECK_VK(vkCreateDescriptorSetLayout(device, &layout, nullptr, &set_layouts[0]));
        const std::array<VkDescriptorBindingFlags, 5> flags{
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        binding_flags.bindingCount = uint32_t(flags.size());
        binding_flags.pBindingFlags = flags.data();
        layout.pNext = &binding_flags;
        layout.bindingCount = uint32_t(dynamic.size());
        layout.pBindings = dynamic.data();
        CHECK_VK(vkCreateDescriptorSetLayout(device, &layout, nullptr, &set_layouts[1]));
        VkPipelineLayoutCreateInfo pipeline_create{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_create.setLayoutCount = uint32_t(set_layouts.size());
        pipeline_create.pSetLayouts = set_layouts.data();
        CHECK_VK(vkCreatePipelineLayout(device, &pipeline_create, nullptr, &pipeline_layout));
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12},
            {VK_DESCRIPTOR_TYPE_SAMPLER, TscCount}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 2;
        pool.poolSizeCount = uint32_t(sizes.size());
        pool.pPoolSizes = sizes.data();
        CHECK_VK(vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptor_pool;
        allocate.descriptorSetCount = uint32_t(set_layouts.size());
        allocate.pSetLayouts = set_layouts.data();
        CHECK_VK(vkAllocateDescriptorSets(device, &allocate, sets.data()));
        const std::array<VkDescriptorBufferInfo, 3> buffer_info{{
            {buffers[0].object, 0, Lanes * sizeof(uint32_t)},
            {buffers[1].object, 0, Lanes * sizeof(Pixel)},
            {buffers[2].object, 0, MetadataWords * sizeof(uint32_t)}}};
        std::array<VkDescriptorImageInfo, 6> image_info{};
        for (uint32_t i = 0; i < images.size(); ++i) image_info[i] = {VK_NULL_HANDLE, images[i].view, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkDescriptorImageInfo, TscCount> sampler_info{};
        for (uint32_t i = 0; i < TscCount; ++i) sampler_info[i].sampler = samplers[i % 2];
        std::array<VkWriteDescriptorSet, 7> writes{};
        auto write_buffer = [&](uint32_t index, uint32_t set, uint32_t binding, uint32_t buffer) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = sets[set];
            writes[index].dstBinding = binding;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &buffer_info[buffer];
        };
        write_buffer(0, 0, 0, 0);
        write_buffer(1, 0, 1, 1);
        write_buffer(2, 1, 1, 2);
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = sets[1];
        writes[3].dstBinding = 0;
        writes[3].descriptorCount = TscCount;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[3].pImageInfo = sampler_info.data();
        for (uint32_t component = 0; component < 3; ++component) {
            auto& write = writes[4 + component];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = sets[1];
            write.dstBinding = 2 + component;
            write.descriptorCount = 2; // slots 2,3 intentionally unwritten and not accessed
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &image_info[2 * component];
        }
        vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        if (!input) throw std::runtime_error(std::string{"Cannot open production module: "} + path);
        const auto length = input.tellg();
        if (length <= 0 || length % 4 != 0) throw std::runtime_error("Invalid SPIR-V file size");
        std::vector<uint32_t> words(static_cast<size_t>(length) / 4);
        input.seekg(0);
        input.read(reinterpret_cast<char*>(words.data()), length);
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = words.size() * 4;
        shader.pCode = words.data();
        CHECK_VK(vkCreateShaderModule(device, &shader, nullptr, &module));
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = module;
        compute.stage.pName = "main";
        compute.layout = pipeline_layout;
        CHECK_VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline));
    }
    uint32_t* Metadata() { return static_cast<uint32_t*>(buffers[2].mapped); }
    uint32_t* Handles() { return static_cast<uint32_t*>(buffers[0].mapped); }
    void ResetMetadata(bool remapped = false, bool via = false) {
        remap = remapped;
        via_header = via;
        auto* words = Metadata();
        std::memset(words, 0, MetadataWords * sizeof(uint32_t));
        words[0] = TicCount;
        words[1] = TscCount;
        words[2] = via ? 1 : 0;
        words[5] = 1;
        words[8] = MapOffset;
        for (uint32_t i = 0; i < TscCount; ++i) words[9 + i] = 1;
        for (uint32_t i = 0; i < TicCount; ++i) {
            const uint32_t compact = (i / 3) ^ (remapped ? 1u : 0u);
            words[MapOffset + i] = (compact << 2) | (i % 3);
        }
    }
    void Dispatch() {
        std::memset(buffers[1].mapped, 0xcd, Lanes * sizeof(Pixel));
        Begin();
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
            0, uint32_t(sets.size()), sets.data(), 0, nullptr);
        vkCmdDispatch(command, 1, 1, 1);
        VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        readback.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        readback.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &readback, 0, nullptr, 0, nullptr);
        Submit();
    }
    void Verify(const std::string& name, uint32_t fault = 0, uint32_t fault_handle = 0) {
        const auto* output = static_cast<const Pixel*>(buffers[1].mapped);
        for (uint32_t lane = 0; lane < Lanes; ++lane) {
            const uint32_t handle = Handles()[lane];
            const uint32_t tic = handle & 0xfffff;
            const uint32_t tsc = via_header ? tic : handle >> 20;
            const uint32_t compact_slot = (tic / 3) ^ (remap ? 1u : 0u);
            const Pixel expected = fault ? Pixel{} : operation == "query" ?
                Pixel{2u << compact_slot, 2u << compact_slot, 0, 2} :
                Expected(tic % 3, compact_slot, Samplerless() || (linear_sampler_enabled && tsc == 7) ? 0 : tsc % 2);
            for (uint32_t channel = 0; channel < 4; ++channel) {
                if (output[lane][channel] != expected[channel]) {
                    throw std::runtime_error(name + ": lane " + std::to_string(lane) + " channel " +
                        std::to_string(channel) + " got " + std::to_string(output[lane][channel]) +
                        " expected " + std::to_string(expected[channel]));
                }
                ++assertions;
            }
        }
        if (Metadata()[3] != fault) throw std::runtime_error(name + ": wrong fault code " + std::to_string(Metadata()[3]));
        if (fault && Metadata()[4] != fault_handle) throw std::runtime_error(name + ": wrong captured fault handle");
        ++assertions;
        std::cout << "PASS " << name << ": 64 lanes x 4 channels; fault=" << fault << '\n';
    }
    void Run() {
        ResetMetadata();
        for (uint32_t lane = 0; lane < Lanes; ++lane) Handles()[lane] = lane % TicCount | (((lane / TicCount) % TscCount) << 20);
        Dispatch();
        Verify("divergent Float/Uint/Sint " + operation);
        ResetMetadata(true);
        Dispatch();
        Verify("TIC mappings changed between dispatches with same pipeline/descriptors");
        ResetMetadata(false, true);
        for (uint32_t lane = 0; lane < Lanes; ++lane) Handles()[lane] = lane % TicCount | (4095u << 20);
        Dispatch();
        Verify("via-header selects sampler from TIC and ignores TSC bits");
        const std::array<const char*, 5> names{
            "TIC bounds guard", "TSC bounds guard", "invalid sampler guard", "invalid TIC mapping guard", "shape group guard"};
        for (uint32_t fault = 1; fault <= 5; ++fault) {
            if (Samplerless() && (fault == 2 || fault == 3)) continue;
            ResetMetadata();
            uint32_t handle = 0;
            if (fault == 1) handle = TicCount;
            if (fault == 2) handle = TscCount << 20;
            if (fault == 3) Metadata()[9] = 0;
            if (fault == 4) Metadata()[MapOffset] = 0xffffffff;
            if (fault == 5) Metadata()[5] = 0;
            for (uint32_t lane = 0; lane < Lanes; ++lane) Handles()[lane] = handle;
            Dispatch();
            Verify(names[fault - 1], fault, handle);
        }
        if (!Samplerless()) {
            // Index 7 is replaced only after the previous submission completed.
            VkDescriptorImageInfo info{};
            info.sampler = samplers[2];
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = sets[1];
            write.dstBinding = 0;
            write.dstArrayElement = 7;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            linear_sampler_enabled = true;
            for (uint32_t tic : {1u, 0u}) {
                ResetMetadata();
                Metadata()[9 + 7] = 3; // valid | linear filtering
                const uint32_t handle = tic | (7u << 20);
                for (uint32_t lane = 0; lane < Lanes; ++lane) Handles()[lane] = handle;
                Dispatch();
                Verify(tic ? "integer image with linear sampler guard" :
                    "unfilterable float image with linear sampler guard", 6, handle);
            }
            if (float_supports_linear) {
                ResetMetadata();
                Metadata()[9 + 7] = 3;
                Metadata()[MapOffset] |= 0x80000000u;
                for (uint32_t lane = 0; lane < Lanes; ++lane) Handles()[lane] = 7u << 20;
                Dispatch();
                Verify("filterable float image accepts linear sampler and masks high map flag");
            } else {
                std::cout << "SKIP linear float positive: RGBA32_SFLOAT is not linearly filterable\n";
            }
            ResetMetadata();
            Metadata()[9 + 2] = 0;
            Metadata()[9 + 7] = 3;
            Metadata()[MapOffset + 1] = 0xffffffffu;
            const std::array<std::array<uint32_t, 2>, 5> faults{{
                {1, TicCount}, {2, TscCount << 20}, {3, 2u << 20},
                {4, 1u | (1u << 20)}, {6, 2u | (7u << 20)}}};
            for (uint32_t lane = 0; lane < Lanes; ++lane) Handles()[lane] = faults[lane % faults.size()][1];
            Dispatch();
            bool matched = false;
            for (const auto& pair : faults) {
                matched |= Metadata()[3] == pair[0] && Metadata()[4] == pair[1];
            }
            if (!matched) throw std::runtime_error("Mixed-lane first-fault code/handle pair is inconsistent");
            Verify("mixed invalid lanes retain one consistent first-fault code/handle", Metadata()[3], Metadata()[4]);
        }
        if (Samplerless()) {
            ResetMetadata();
            Metadata()[1] = 0;
            for (uint32_t i = 0; i < TscCount; ++i) Metadata()[9 + i] = 0;
            for (uint32_t lane = 0; lane < Lanes; ++lane) Handles()[lane] = lane % TicCount | (4095u << 20);
            Dispatch();
            Verify("samplerless operation ignores invalid sampler pool and TSC bits");
        }
        std::cout << "PASS production compiler Vulkan execution: " << assertions << " assertions\n";
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) { std::cerr << "Usage: dynamic_vulkan_runner COMPILER_GENERATED.spv [sample|fetch|query]\n"; return 2; }
    try {
        {
        Fixture fixture;
        if (argc == 3) fixture.operation = argv[2];
        if (fixture.operation != "sample" && fixture.operation != "fetch" && fixture.operation != "query")
            throw std::runtime_error("Unknown operation expectation");
        fixture.InitDevice();
        fixture.InitResources();
        fixture.InitPipeline(argv[1]);
        fixture.Run();
        }
        if (ValidationErrors) throw std::runtime_error("Vulkan validation reported " +
            std::to_string(ValidationErrors.load()) + " errors");
        if (ValidationRequested()) std::cout << "PASS VK_LAYER_KHRONOS_validation: 0 errors\n";
        return 0;
    } catch (const Skip& reason) {
        std::cout << "SKIP: " << reason.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
