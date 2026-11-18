// SPDX-License-Identifier: GPL-2.0-or-later
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
static unsigned errors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++errors; std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
}
static void Check(VkResult r) { if (r != VK_SUCCESS) { std::fprintf(stderr,"VkResult=%d\n",r); std::exit(2); } }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    unsigned runs{}, bounds_runs{};
    for (uint32_t api : {VK_API_VERSION_1_1, VK_API_VERSION_1_2}) {
        const char* layer="VK_LAYER_KHRONOS_validation";
        uint32_t n{}; Check(vkEnumerateInstanceLayerProperties(&n,nullptr));
        std::vector<VkLayerProperties> layers(n); Check(vkEnumerateInstanceLayerProperties(&n,layers.data()));
        bool exact{}; for(auto& l:layers) if(!std::strcmp(l.layerName,layer)) {
            std::printf("Validation layer %u.%u.%u\n",VK_VERSION_MAJOR(l.specVersion),VK_VERSION_MINOR(l.specVersion),VK_VERSION_PATCH(l.specVersion));
            exact=l.specVersion==VK_MAKE_API_VERSION(0,1,4,363);
        }
        if (!exact) { std::fprintf(stderr,"Requires SDK 1.4.363.1 validation layer\n"); return 2; }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=api;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT; debug.pfnUserCallback=Message;
        VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT}; validation.pNext=&debug;
        validation.enabledValidationFeatureCount=1; validation.pEnabledValidationFeatures=&sync;
        const char* debugExt=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo=&app;
        ii.enabledLayerCount=1; ii.ppEnabledLayerNames=&layer; ii.enabledExtensionCount=1; ii.ppEnabledExtensionNames=&debugExt; ii.pNext=&validation;
        VkInstance instance; Check(vkCreateInstance(&ii,nullptr,&instance));
        auto createDebug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
        auto destroyDebug=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerEXT messenger; Check(createDebug(instance,&debug,nullptr,&messenger));
        Check(vkEnumeratePhysicalDevices(instance,&n,nullptr)); assert(n);
        std::vector<VkPhysicalDevice> physicals(n); Check(vkEnumeratePhysicalDevices(instance,&n,physicals.data()));
        auto physical=physicals[0]; VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical,&properties);
        std::printf("API 1.%u: %s\n",VK_VERSION_MINOR(api),properties.deviceName);
        VkPhysicalDeviceRobustness2FeaturesEXT robustness{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDevice16BitStorageFeatures s16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES}; s16.pNext=&robustness;
        VkPhysicalDevice8BitStorageFeatures s8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES}; s8.pNext=&s16;
        VkPhysicalDeviceShaderFloat16Int8Features arithmetic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES}; arithmetic.pNext=&s8;
        VkPhysicalDeviceUniformBufferStandardLayoutFeatures standard{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES}; standard.pNext=&arithmetic;
        VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; available.pNext=&standard; vkGetPhysicalDeviceFeatures2(physical,&available);
        if (!robustness.nullDescriptor || !arithmetic.shaderInt8 || !available.features.shaderInt16 || !s8.storageBuffer8BitAccess || !s8.uniformAndStorageBuffer8BitAccess || !s16.storageBuffer16BitAccess || !s16.uniformAndStorageBuffer16BitAccess || !standard.uniformBufferStandardLayout) {
            std::fprintf(stderr,"Test ICD must expose native features for A/B execution\n"); return 2;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);
        std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
        uint32_t family{}; while(family<n && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family; assert(family<n);
        for (unsigned mask : {64u,67u,107u,87u,127u,63u}) {
            VkPhysicalDeviceRobustness2FeaturesEXT er{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT}; er.nullDescriptor=VK_TRUE;
            VkPhysicalDevice16BitStorageFeatures e16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES}; e16.pNext=&er;
            e16.storageBuffer16BitAccess=(mask&8)!=0; e16.uniformAndStorageBuffer16BitAccess=(mask&32)!=0;
            VkPhysicalDevice8BitStorageFeatures e8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES}; e8.pNext=&e16;
            e8.storageBuffer8BitAccess=(mask&4)!=0; e8.uniformAndStorageBuffer8BitAccess=(mask&16)!=0;
            VkPhysicalDeviceShaderFloat16Int8Features ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES}; ei.pNext=&e8; ei.shaderInt8=(mask&1)!=0;
            VkPhysicalDeviceUniformBufferStandardLayoutFeatures es{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES}; es.pNext=&ei; es.uniformBufferStandardLayout=VK_TRUE;
            VkPhysicalDeviceFeatures enabled{}; enabled.shaderImageGatherExtended=available.features.shaderImageGatherExtended; enabled.shaderInt16=(mask&2)!=0; enabled.robustBufferAccess=VK_FALSE;
            float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
            std::vector<const char*> extensions{VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};
            if(api==VK_API_VERSION_1_1) {
                extensions.push_back(VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
                if(mask & 1) extensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
                if(mask & 20) extensions.push_back(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
            }
            if(api==VK_API_VERSION_1_1 && !(mask & 20)) ei.pNext=&e16;
            VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext=&es; di.pEnabledFeatures=&enabled; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
            di.enabledExtensionCount=extensions.size();di.ppEnabledExtensionNames=extensions.data();
            VkDevice device; Check(vkCreateDevice(physical,&di,nullptr,&device)); VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
            constexpr VkDeviceSize stride=65536, total=3*stride;
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=total; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VkBuffer buffer; Check(vkCreateBuffer(device,&bi,nullptr,&buffer));
            VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer,&req);
            VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
            uint32_t mt{}; constexpr auto wanted=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            while(mt<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<mt)) || (mp.memoryTypes[mt].propertyFlags&wanted)!=wanted)) ++mt; assert(mt<mp.memoryTypeCount);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=mt;
            VkDeviceMemory memory; Check(vkAllocateMemory(device,&ai,nullptr,&memory)); Check(vkBindBufferMemory(device,buffer,memory,0));
            void* mapped; Check(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped));
            VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex=family;
            VkCommandPool pool; Check(vkCreateCommandPool(device,&cpi,nullptr,&pool));
            for(unsigned kind : {0u,1u,2u,3u,4u,5u,6u,7u}) {
                const bool ubo=kind==4||kind==5;
                std::vector<VkDescriptorSetLayoutBinding> bindings;
                const unsigned ubos = kind == 5 ? 14u : ubo ? 1u : 0u;
                for (unsigned i = 0; i < ubos; ++i) bindings.push_back({i,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
                bindings.push_back({static_cast<uint32_t>(bindings.size()),VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
                if(kind==7) bindings.push_back({1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
                VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=bindings.size(); li.pBindings=bindings.data();
                VkDescriptorSetLayout layout; Check(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout));
                VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&layout;
                VkPipelineLayout pipelineLayout; Check(vkCreatePipelineLayout(device,&pli,nullptr,&pipelineLayout));
                auto path=std::filesystem::path(argv[1])/("65536-"+std::to_string(mask)+"-"+std::to_string(kind)+".spv");
                std::ifstream f(path,std::ios::binary|std::ios::ate); assert(f); size_t bytes=f.tellg(); f.seekg(0);
                std::vector<uint32_t> code(bytes/4); f.read(reinterpret_cast<char*>(code.data()),bytes); assert(f.good());
                VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize=bytes; smi.pCode=code.data();
                VkShaderModule module; Check(vkCreateShaderModule(device,&smi,nullptr,&module));
                VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pci.layout=pipelineLayout;
                pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pci.stage.module=module;pci.stage.pName="main";
                VkPipeline pipeline; Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
                if(errors)return 1;
                const bool fallback8 = (mask & 69) != 69;
                const bool fallback16 = (mask & 74) != 74;
                const bool both_fallback = fallback8 && fallback16;
                // Preserve all valid-data tests, then exercise OOB and tiny/null
                // descriptors with robustness disabled, solely on CAS paths.
                for (unsigned variant=0; variant<(kind==0 && both_fallback ? 6u : 1u); ++variant) {
                    if(kind==6 && !both_fallback) continue;
                    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,14},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
                    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=1;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;
                    VkDescriptorPool dp; Check(vkCreateDescriptorPool(device,&dpi,nullptr,&dp));
                    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsi.descriptorPool=dp;dsi.descriptorSetCount=1;dsi.pSetLayouts=&layout;
                    VkDescriptorSet set;Check(vkAllocateDescriptorSets(device,&dsi,&set));
                    std::vector<VkDescriptorBufferInfo> infos; for(unsigned i=0;i<ubos;++i)infos.push_back({buffer,0,stride});
                    const VkDeviceSize range = variant==1 ? 3 : variant==2 ? 8191 : variant>=4 ? 4 : 8192;
                    const VkDeviceSize shift = variant==5 ? std::max<VkDeviceSize>(4, properties.limits.minStorageBufferOffsetAlignment) : 0;
                    const VkDeviceSize base = stride + shift;
                    infos.push_back({variant==3 ? VK_NULL_HANDLE : buffer, variant==3 ? 0 : base,
                                     variant==3 ? VK_WHOLE_SIZE : range});
                    if(kind==7)infos.push_back({buffer,2*stride,8192});
                    std::vector<VkWriteDescriptorSet> writes;
                    for(unsigned i=0;i<bindings.size();++i) {VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=bindings[i].descriptorType;w.pBufferInfo=&infos[i];writes.push_back(w);}
                    vkUpdateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);
                    auto* memoryBytes=static_cast<uint8_t*>(mapped);std::memset(mapped,0xa5,total);
                    uint32_t input=0x8123ef80;std::memcpy(memoryBytes,&input,4);
                    uint8_t src[]{0x7f,0x80,0x34,0x12,0x01,0x80};std::memcpy(memoryBytes+stride+6144,src,6);
                    std::array<uint32_t,2048> expected;expected.fill(0xa5a5a5a5);
                    if(kind<=2)for(unsigned lane=0;lane<256;++lane) {
                        if(kind!=2) {
                            reinterpret_cast<uint8_t*>(expected.data())[lane]=0x40+(lane&3);
                            reinterpret_cast<uint8_t*>(expected.data())[256+lane]=0x80+(lane&3);
                            expected[512+lane]=0xa5a580a5;expected[1792+lane]=0xefa5a5a5;
                        }
                        if(kind!=1) {
                            auto* halfs=reinterpret_cast<uint16_t*>(expected.data());halfs[256+lane]=0x1234+(lane&1);halfs[512+lane]=0x8000+(lane&1);
                            expected[768+lane]=0xbeefa5a5;expected[1024+lane]=0x8123a5a5;
                        }
                    }
                    std::memcpy(reinterpret_cast<uint8_t*>(expected.data())+6144,src,6);
                    if(kind==3||ubo)for(unsigned lane=0;lane<256;++lane) {
                        expected[4*lane]=ubo?0x80:0x7f;expected[4*lane+1]=0xffffff80;
                        expected[4*lane+2]=ubo?0xef80:0x1234;expected[4*lane+3]=ubo?0xffffef80:0xffff8001;
                    }
                    if(kind==7)for(unsigned lane=0;lane<256;++lane)expected[lane]=0x8765a5a5;
                    if(variant==2) expected[2047]=0xa5a5a5a5;
                    if(variant==1 || variant>=3) {
                        expected.fill(0xa5a5a5a5);
                        std::memcpy(reinterpret_cast<uint8_t*>(expected.data())+6144,src,6);
                        if(variant==4 || variant==5) expected[shift/4]=0x43424140;
                    }
                    const bool bounds_case = variant || kind==6;
                    const auto errors_before = errors;
                    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
                    VkCommandBuffer command;Check(vkAllocateCommandBuffers(device,&ca,&command));
                    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(command,&begin));
                    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);vkCmdDispatch(command,1,1,1);
                    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);Check(vkEndCommandBuffer(command));
                    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
                    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;Check(vkCreateFence(device,&fi,nullptr,&fence));
                    Check(vkQueueSubmit(queue,1,&submit,fence));Check(vkWaitForFences(device,1,&fence,VK_TRUE,30'000'000'000));
                    auto* actual=reinterpret_cast<uint32_t*>(memoryBytes+stride);
                    for(unsigned i=0;i<expected.size();++i)if(actual[i]!=expected[i]){std::fprintf(stderr,"mask=%u kind=%u variant=%u word=%u actual=%08X expected=%08X\n",mask,kind,variant,i,actual[i],expected[i]);return 1;}
                    if(kind==7)for(unsigned i=0;i<64;++i)assert(reinterpret_cast<uint32_t*>(memoryBytes+2*stride)[i]==0x42424242);
                    vkDestroyFence(device,fence,nullptr);vkFreeCommandBuffers(device,pool,1,&command);vkDestroyDescriptorPool(device,dp,nullptr);++runs;
                    if(bounds_case) { ++bounds_runs; assert(errors==errors_before); }
                }
                vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,module,nullptr);vkDestroyPipelineLayout(device,pipelineLayout,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);
                if(errors)return 1;
            }
            vkDestroyCommandPool(device,pool,nullptr);vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);vkDestroyDevice(device,nullptr);
        }
        destroyDebug(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
    }
    assert(runs==120 && bounds_runs==36);
    std::printf("PASS: %u dispatches (84 preserved + %u bounds/null/tiny cases), 96 pipelines, robustness disabled, %u validation errors\n",runs,bounds_runs,errors);
    return errors?1:0;
}
