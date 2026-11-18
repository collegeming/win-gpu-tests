// Vulkan compute round-trip test for the Windows ARM64 panvk ICD.
//
// Loads C:\panvk\dev_vulkan_virtio.dll directly (no Vulkan loader), creates a
// device on queue family 0, uploads 256 uints, runs a local_size_x=64 compute
// shader that adds one to every element, copies the result back and verifies
// it on the host.  The point is to prove real shader execution plus queue
// submission, fence wait and readback - not just vkCreateDevice.
//
// Buffers: a single HOST_VISIBLE|HOST_COHERENT storage buffer when the driver
// exposes one, otherwise a device-local storage buffer plus a host-visible
// staging buffer with vkCmdCopyBuffer in both directions.
//
// Build (compute_spirv.h is generated from compute.comp by the workflow):
//   cl /nologo /EHsc /O2 /std:c++17 /I third_party\vulkan\include ^
//      test_vk_compute.cpp /Fe:vk_compute_arm64.exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#include "third_party/vulkan/include/vulkan/vulkan_core.h"
#include "compute_spirv.h"
#include "compute_nop_spirv.h"
#include "uint_rt_vert_spirv.h"
#include "uint_rt_frag_spirv.h"
#include "float_rt_frag_spirv.h"

typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_icdGetInstanceProcAddr_t)(VkInstance, const char *);

static PFN_icdGetInstanceProcAddr_t g_icd_gipa = nullptr;
static const char *g_last_step = "none";
static VkResult g_last_result = VK_SUCCESS;

static PFN_vkVoidFunction gipa(VkInstance instance, const char *name) {
    return g_icd_gipa ? g_icd_gipa(instance, name) : nullptr;
}

template <typename T>
static T proc(VkInstance instance, const char *name) {
    return reinterpret_cast<T>(gipa(instance, name));
}

template <typename T>
static T dev_proc(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, VkInstance instance, const char *name) {
    T fn = gdpa ? reinterpret_cast<T>(gdpa(device, name)) : nullptr;
    if (!fn)
        fn = proc<T>(instance, name);
    return fn;
}

static const char *vkres(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    default: return "VK_ERROR_OTHER";
    }
}

static bool ok(const char *step, VkResult r) {
    g_last_step = step;
    g_last_result = r;
    std::printf("step=%-42s result=%d (%s)\n", step, (int)r, vkres(r));
    return r == VK_SUCCESS;
}

static void mem_flags(char *buf, size_t n, VkMemoryPropertyFlags f) {
    std::snprintf(buf, n, "0x%08x%s%s%s%s%s", (unsigned)f,
                  (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "|DEVICE_LOCAL" : "",
                  (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "|HOST_VISIBLE" : "",
                  (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "|HOST_COHERENT" : "",
                  (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "|HOST_CACHED" : "",
                  (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "|LAZILY_ALLOCATED" : "");
}

// Pick the memory type in `bits` that has all `required` flags, preferring the
// greatest number of `preferred` flags.
static bool find_memory_type(const VkPhysicalDeviceMemoryProperties &mp, uint32_t bits,
                             VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                             uint32_t *out) {
    bool found = false;
    uint32_t best_score = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) != required)
            continue;
        uint32_t score = 0;
        for (uint32_t b = 1; b != 0x80000000u; b <<= 1) {
            if ((preferred & b) && (f & b))
                score++;
        }
        if (!found || score > best_score) {
            found = true;
            best_score = score;
            *out = i;
        }
    }
    return found;
}

struct DevApi {
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkGetFenceStatus GetFenceStatus;
};

static int load_dev_api(DevApi &a, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, VkInstance instance) {
    a = DevApi{};
    int missing = 0;
#define LOAD(field)                                                                       \
    do {                                                                                  \
        a.field = dev_proc<PFN_vk##field>(gdpa, dev, instance, "vk" #field);              \
        if (!a.field) {                                                                   \
            std::printf("missing_entrypoint: vk%s\n", #field);                            \
            missing++;                                                                    \
        }                                                                                 \
    } while (0)
    LOAD(DestroyDevice);
    LOAD(DeviceWaitIdle);
    LOAD(GetDeviceQueue);
    LOAD(CreateBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(DestroyBuffer);
    LOAD(AllocateMemory);
    LOAD(BindBufferMemory);
    LOAD(FreeMemory);
    LOAD(MapMemory);
    LOAD(UnmapMemory);
    LOAD(FlushMappedMemoryRanges);
    LOAD(InvalidateMappedMemoryRanges);
    LOAD(CreateShaderModule);
    LOAD(DestroyShaderModule);
    LOAD(CreateDescriptorSetLayout);
    LOAD(DestroyDescriptorSetLayout);
    LOAD(CreatePipelineLayout);
    LOAD(DestroyPipelineLayout);
    LOAD(CreateDescriptorPool);
    LOAD(DestroyDescriptorPool);
    LOAD(AllocateDescriptorSets);
    LOAD(UpdateDescriptorSets);
    LOAD(CreateComputePipelines);
    LOAD(DestroyPipeline);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdBindPipeline);
    LOAD(CmdBindDescriptorSets);
    LOAD(CmdDispatch);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdCopyBuffer);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(QueueSubmit);
    LOAD(WaitForFences);
    LOAD(GetFenceStatus);
#undef LOAD
    return missing;
}

static bool create_buffer_mem(const DevApi &api, VkDevice dev, const VkPhysicalDeviceMemoryProperties &mp,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                              VkBuffer *out_buf, VkDeviceMemory *out_mem, uint32_t *out_type) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (!ok("vkCreateBuffer", api.CreateBuffer(dev, &bci, nullptr, out_buf)))
        return false;

    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(dev, *out_buf, &req);

    uint32_t type_index = 0;
    if (!find_memory_type(mp, req.memoryTypeBits, required, preferred, &type_index)) {
        char flags[160];
        mem_flags(flags, sizeof(flags), required);
        std::printf("errors: no memory type with %s for usage=0x%x (size=%llu alignment=%llu)\n",
                    flags, (unsigned)usage, (unsigned long long)req.size,
                    (unsigned long long)req.alignment);
        g_last_step = "vkFindMemoryType";
        g_last_result = VK_ERROR_FEATURE_NOT_PRESENT;
        return false;
    }
    *out_type = type_index;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_index;

    if (!ok("vkAllocateMemory", api.AllocateMemory(dev, &mai, nullptr, out_mem)))
        return false;

    if (!ok("vkBindBufferMemory", api.BindBufferMemory(dev, *out_buf, *out_mem, 0)))
        return false;
    return true;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user_data) {
    (void)type;
    (void)user_data;
    std::printf("[vk-debug] severity=0x%x message=%s\n", (unsigned)severity,
                data && data->pMessage ? data->pMessage : "(null)");
    return VK_FALSE;
}


// Vulkan integer render-target probe.
//
// The D3D11 probe shows that pixel-shader writes into every integer render
// target (R32/R16/R8/RGBA32_UINT) read back as zero on the device, while
// R32_FLOAT and BGRA8 work and the same program passes on WARP. D2D writes its
// coverage masks into uint targets, so this probe asks the layer question
// directly at the Vulkan level: can this ICD render a fullscreen triangle into
// an R32_UINT attachment at all? The R32_FLOAT case is the control that proves
// the harness (image, render pass, pipeline, copy, readback) is sound.

struct RtApi {
    PFN_vkCreateImage CreateImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
};

static bool load_rt_api(RtApi &a, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev,
                        VkInstance inst) {
    a = RtApi{};
    int missing = 0;
#define LOADRT(field)                                                                     \
    do {                                                                                  \
        a.field = dev_proc<PFN_vk##field>(gdpa, dev, inst, "vk" #field);                  \
        if (!a.field) {                                                                   \
            std::printf("missing_entrypoint: vk%s\n", #field);                            \
            missing++;                                                                    \
        }                                                                                 \
    } while (0)
    LOADRT(CreateImage);
    LOADRT(GetImageMemoryRequirements);
    LOADRT(BindImageMemory);
    LOADRT(CreateImageView);
    LOADRT(DestroyImageView);
    LOADRT(DestroyImage);
    LOADRT(CreateRenderPass);
    LOADRT(CreateFramebuffer);
    LOADRT(DestroyFramebuffer);
    LOADRT(DestroyRenderPass);
    LOADRT(CreateGraphicsPipelines);
    LOADRT(CmdBeginRenderPass);
    LOADRT(CmdEndRenderPass);
    LOADRT(CmdDraw);
    LOADRT(CmdCopyImageToBuffer);
#undef LOADRT
    return missing == 0;
}

static int run_rt_case(const char *label, VkFormat fmt, uint32_t expected,
                       const unsigned char *vs_code, unsigned vs_len,
                       const unsigned char *fs_code, unsigned fs_len,
                       VkInstance inst, VkDevice dev, VkQueue queue,
                       const DevApi &api, const RtApi &rt,
                       const VkPhysicalDeviceMemoryProperties &mp) {
    const uint32_t W = 64, H = 64;
    bool pass = false;
    uint32_t mismatches = 0, first_actual = 0;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    do {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = {W, H, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!ok("vkCreateImage", rt.CreateImage(dev, &ici, nullptr, &image)))
            break;

        VkMemoryRequirements req{};
        rt.GetImageMemoryRequirements(dev, image, &req);
        uint32_t mtype = 0;
        if (!find_memory_type(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                              &mtype)) {
            std::printf("errors: no DEVICE_LOCAL memory type for image\n");
            break;
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mtype;
        if (!ok("vkAllocateMemory(image)", api.AllocateMemory(dev, &mai, nullptr, &imem)))
            break;
        if (!ok("vkBindImageMemory", rt.BindImageMemory(dev, image, imem, 0)))
            break;

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!ok("vkCreateImageView", rt.CreateImageView(dev, &vci, nullptr, &view)))
            break;

        VkAttachmentDescription att{};
        att.format = fmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{};
        rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rci.attachmentCount = 1;
        rci.pAttachments = &att;
        rci.subpassCount = 1;
        rci.pSubpasses = &sub;
        rci.dependencyCount = 1;
        rci.pDependencies = &dep;
        if (!ok("vkCreateRenderPass", rt.CreateRenderPass(dev, &rci, nullptr, &rp)))
            break;

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rp;
        fci.attachmentCount = 1;
        fci.pAttachments = &view;
        fci.width = W;
        fci.height = H;
        fci.layers = 1;
        if (!ok("vkCreateFramebuffer", rt.CreateFramebuffer(dev, &fci, nullptr, &fb)))
            break;

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = vs_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(vs_code);
        if (!ok("vkCreateShaderModule(vs)", api.CreateShaderModule(dev, &smci, nullptr, &vs)))
            break;
        smci.codeSize = fs_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(fs_code);
        if (!ok("vkCreateShaderModule(fs)", api.CreateShaderModule(dev, &smci, nullptr, &fs)))
            break;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!ok("vkCreatePipelineLayout",
                api.CreatePipelineLayout(dev, &plci, nullptr, &pl)))
            break;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vist{};
        vist.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo iast{};
        iast.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        iast.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{};
        vp.width = static_cast<float>(W);
        vp.height = static_cast<float>(H);
        vp.maxDepth = 1.0f;
        VkRect2D sc{};
        sc.extent = {W, H};
        VkPipelineViewportStateCreateInfo vst{};
        vst.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vst.viewportCount = 1;
        vst.pViewports = &vp;
        vst.scissorCount = 1;
        vst.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rst{};
        rst.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rst.polygonMode = VK_POLYGON_MODE_FILL;
        rst.cullMode = VK_CULL_MODE_NONE;
        rst.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rst.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo mss{};
        mss.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        mss.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vist;
        gpci.pInputAssemblyState = &iast;
        gpci.pViewportState = &vst;
        gpci.pRasterizationState = &rst;
        gpci.pMultisampleState = &mss;
        gpci.pColorBlendState = &cbs;
        gpci.layout = pl;
        gpci.renderPass = rp;
        gpci.subpass = 0;
        if (!ok("vkCreateGraphicsPipelines",
                rt.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe)))
            break;

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        if (!ok("vkCreateCommandPool", api.CreateCommandPool(dev, &cpci, nullptr, &pool)))
            break;
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (!ok("vkAllocateCommandBuffers", api.AllocateCommandBuffers(dev, &cbai, &cmd)))
            break;

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = static_cast<VkDeviceSize>(W) * H * 4u;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!ok("vkCreateBuffer", api.CreateBuffer(dev, &bci, nullptr, &buf)))
            break;
        VkMemoryRequirements breq{};
        api.GetBufferMemoryRequirements(dev, buf, &breq);
        uint32_t btype = 0;
        if (!find_memory_type(mp, breq.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              0, &btype)) {
            std::printf("errors: no HOST_VISIBLE|COHERENT memory type for buffer\n");
            break;
        }
        VkMemoryAllocateInfo bmai{};
        bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bmai.allocationSize = breq.size;
        bmai.memoryTypeIndex = btype;
        if (!ok("vkAllocateMemory(buffer)", api.AllocateMemory(dev, &bmai, nullptr, &bmem)))
            break;
        if (!ok("vkBindBufferMemory", api.BindBufferMemory(dev, buf, bmem, 0)))
            break;

        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!ok("vkBeginCommandBuffer", api.BeginCommandBuffer(cmd, &cbbi)))
            break;
        VkClearValue clear{};
        clear.color.float32[0] = 0.0f;
        clear.color.float32[1] = 0.0f;
        clear.color.float32[2] = 0.0f;
        clear.color.float32[3] = 0.0f;
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = rp;
        rpbi.framebuffer = fb;
        rpbi.renderArea.extent = {W, H};
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        rt.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        rt.CmdDraw(cmd, 3, 1, 0, 0);
        rt.CmdEndRenderPass(cmd);

        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &ib);
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {W, H, 1};
        rt.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
        if (!ok("vkEndCommandBuffer", api.EndCommandBuffer(cmd)))
            break;

        VkFenceCreateInfo fci2{};
        fci2.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!ok("vkCreateFence", api.CreateFence(dev, &fci2, nullptr, &fence)))
            break;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (!ok("vkQueueSubmit", api.QueueSubmit(queue, 1, &si, fence)))
            break;
        if (!ok("vkWaitForFences",
                api.WaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull)))
            break;

        void *map = nullptr;
        if (!ok("vkMapMemory", api.MapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map)))
            break;
        const uint32_t *got = static_cast<const uint32_t *>(map);
        for (uint32_t i = 0; i < W * H; ++i) {
            if (got[i] != expected) {
                if (mismatches == 0)
                    first_actual = got[i];
                ++mismatches;
            }
        }
        api.UnmapMemory(dev, bmem);
        pass = (mismatches == 0);
    } while (0);

    if (fence) api.DestroyFence(dev, fence, nullptr);
    if (buf) api.DestroyBuffer(dev, buf, nullptr);
    if (bmem) api.FreeMemory(dev, bmem, nullptr);
    if (pool) api.DestroyCommandPool(dev, pool, nullptr);
    if (pipe) api.DestroyPipeline(dev, pipe, nullptr);
    if (pl) api.DestroyPipelineLayout(dev, pl, nullptr);
    if (vs) api.DestroyShaderModule(dev, vs, nullptr);
    if (fs) api.DestroyShaderModule(dev, fs, nullptr);
    if (fb) rt.DestroyFramebuffer(dev, fb, nullptr);
    if (rp) rt.DestroyRenderPass(dev, rp, nullptr);
    if (view) rt.DestroyImageView(dev, view, nullptr);

    std::printf("MODE=vk-uint-rt ITEM=%s RESULT=%s mismatches=%u expected=0x%08x"
                " first_actual=0x%08x\n",
                label, pass ? "PASS" : "FAIL", (unsigned)mismatches, expected,
                first_actual);
    return pass ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *dll = "C:\\panvk\\umd_c1\\vulkan_virtio.dll";
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0')
            dll = argv[i];
    }
    std::printf("probe=vk_uint_rt\n");
    std::printf("icd_path=%s\n", dll);

    HMODULE mod = LoadLibraryA(dll);
    if (!mod) {
        std::printf("ERROR: LoadLibraryA failed err=%lu\n", (unsigned long)GetLastError());
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    g_icd_gipa = reinterpret_cast<PFN_icdGetInstanceProcAddr_t>(
        GetProcAddress(mod, "vk_icdGetInstanceProcAddr"));
    if (!g_icd_gipa) {
        std::printf("ERROR: no vk_icdGetInstanceProcAddr\n");
        return 1;
    }

    PFN_vkCreateInstance create_instance =
        proc<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance) {
        std::printf("ERROR: vkCreateInstance not resolvable\n");
        return 1;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vk_uint_rt";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!ok("vkCreateInstance", create_instance(&ici, nullptr, &instance))) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    PFN_vkEnumeratePhysicalDevices enum_pd =
        proc<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props =
        proc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance,
                                                      "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qf =
        proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create_device =
        proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa =
        proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    if (!enum_pd || !get_mem_props || !get_qf || !create_device || !gdpa) {
        std::printf("ERROR: instance entry points missing\n");
        return 1;
    }

    uint32_t pd_count = 0;
    if (enum_pd(instance, &pd_count, nullptr) != VK_SUCCESS || pd_count == 0) {
        std::printf("ERROR: no physical devices\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    enum_pd(instance, &pd_count, pds.data());
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceMemoryProperties mp{};
    get_mem_props(pd, &mp);

    uint32_t qf_count = 0;
    get_qf(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count);
    if (qf_count)
        get_qf(pd, &qf_count, qf.data());
    uint32_t qf_index = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qf[i].queueCount && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            qf_index = i;
            break;
        }
    }
    if (qf_index == UINT32_MAX) {
        std::printf("ERROR: no graphics queue family\n");
        return 1;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf_index;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    if (!ok("vkCreateDevice", create_device(pd, &dci, nullptr, &dev))) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    DevApi api;
    if (load_dev_api(api, gdpa, dev, instance) != 0) {
        std::printf("errors: device entry points missing\n");
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    api.GetDeviceQueue(dev, qf_index, 0, &queue);

    RtApi rt;
    if (!load_rt_api(rt, gdpa, dev, instance)) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    int failed = 0;
    failed += run_rt_case("r32_uint_65537", VK_FORMAT_R32_UINT, 65537u,
                          uint_rt_vert_spirv, uint_rt_vert_spirv_len, uint_rt_frag_spirv,
                          uint_rt_frag_spirv_len, instance, dev, queue, api, rt, mp);
    failed += run_rt_case("r32_float_0p5", VK_FORMAT_R32_SFLOAT, 0x3F000000u,
                          uint_rt_vert_spirv, uint_rt_vert_spirv_len,
                          float_rt_frag_spirv, float_rt_frag_spirv_len, instance, dev,
                          queue, api, rt, mp);
    std::printf("MODE=vk-uint-rt SUMMARY items=2 failed=%d\n", failed);
    std::printf("RESULT: %s\n", failed ? "FAIL" : "PASS");
    api.DeviceWaitIdle(dev);
    api.DestroyDevice(dev, nullptr);
    return failed ? 1 : 0;
}
