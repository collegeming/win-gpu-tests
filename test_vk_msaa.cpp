// Vulkan-level multisample probe for the Windows ARM64 panvk ICD.
//
// The D3D11 probe shows that on this stack a multisampled render target takes
// the clear but loses every pixel the draw produces, while a single-sampled
// target works. That leaves one question: does the loss happen inside the
// UMD's render pass handling, or below it in the ICD/KMD?
//
// This probe asks the same question with no D3D11 in the way: render a triangle
// covering half a four-sample attachment, resolve into a single-sampled image,
// copy it back and look at the diagonal. A hard two-level edge means the
// coverage was lost below the UMD; a multi-level fringe means the Vulkan path is
// sound and the UMD is at fault.
//
// The single-sampled run at the top is the control: same shaders, same readback,
// and it must show ink before the multisample results mean anything.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#include "third_party/vulkan/include/vulkan/vulkan_core.h"
#include "msaa_diag_vert_spirv.h"
#include "msaa_green_frag_spirv.h"

typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_icdGetInstanceProcAddr_t)(VkInstance, const char *);

static PFN_icdGetInstanceProcAddr_t g_icd_gipa = nullptr;
static uint32_t g_qf_index = 0;

static PFN_vkVoidFunction gipa(VkInstance instance, const char *name) {
    return g_icd_gipa ? g_icd_gipa(instance, name) : nullptr;
}

template <typename T>
static T proc(VkInstance instance, const char *name) {
    return reinterpret_cast<T>(gipa(instance, name));
}

template <typename T>
static T dev_proc(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, VkInstance instance,
                  const char *name) {
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
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "VK_ERROR_OTHER";
    }
}

static bool ok(const char *step, VkResult r) {
    if (r == VK_SUCCESS)
        return true;
    std::printf("errors: %s -> %s (%d)\n", step, vkres(r), (int)r);
    return false;
}

struct DevApi {
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBindPipeline CmdBindPipeline;
};

static bool load_dev_api(DevApi &a, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev,
                         VkInstance inst) {
    a = DevApi{};
    int missing = 0;
#define LOAD(field)                                                                  \
    do {                                                                             \
        a.field = dev_proc<PFN_vk##field>(gdpa, dev, inst, "vk" #field);             \
        if (!a.field) {                                                              \
            std::printf("missing_entrypoint: vk%s\n", #field);                       \
            missing++;                                                               \
        }                                                                            \
    } while (0)
    LOAD(GetDeviceQueue);
    LOAD(DeviceWaitIdle);
    LOAD(DestroyDevice);
    LOAD(CreateShaderModule);
    LOAD(DestroyShaderModule);
    LOAD(CreatePipelineLayout);
    LOAD(DestroyPipelineLayout);
    LOAD(CreateGraphicsPipelines);
    LOAD(DestroyPipeline);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(QueueSubmit);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(CreateBuffer);
    LOAD(DestroyBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(UnmapMemory);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdBindPipeline);
#undef LOAD
    return missing == 0;
}

struct RtApi {
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
};

static bool load_rt_api(RtApi &a, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev,
                        VkInstance inst, PFN_vkGetPhysicalDeviceFormatProperties fprops) {
    a = RtApi{};
    int missing = 0;
#define LOADRT(field)                                                                \
    do {                                                                             \
        a.field = dev_proc<PFN_vk##field>(gdpa, dev, inst, "vk" #field);             \
        if (!a.field) {                                                              \
            std::printf("missing_entrypoint: vk%s\n", #field);                       \
            missing++;                                                               \
        }                                                                            \
    } while (0)
    LOADRT(CreateImage);
    LOADRT(DestroyImage);
    LOADRT(GetImageMemoryRequirements);
    LOADRT(BindImageMemory);
    LOADRT(CreateImageView);
    LOADRT(DestroyImageView);
    LOADRT(CreateRenderPass);
    LOADRT(DestroyRenderPass);
    LOADRT(CreateFramebuffer);
    LOADRT(DestroyFramebuffer);
    LOADRT(CmdBeginRenderPass);
    LOADRT(CmdEndRenderPass);
    LOADRT(CmdDraw);
    LOADRT(CmdCopyImageToBuffer);
#undef LOADRT
    a.GetPhysicalDeviceFormatProperties = fprops;
    return missing == 0;
}

static bool find_memory_type(const VkPhysicalDeviceMemoryProperties &mp, uint32_t bits,
                             VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid,
                             uint32_t *out) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i)))
            continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & want) == want && (f & avoid) == 0) {
            *out = i;
            return true;
        }
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i)))
            continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) {
            *out = i;
            return true;
        }
    }
    return false;
}

struct CaseStats {
    bool clear_only = false;
    int distinct_green = 0;
    int min_green = 255;
    int max_green = 255;
    int intermediate = 0;
    int corner_r = 0, corner_g = 0;
    char midrow[64] = {0};
    bool created_msaa = false;
    VkResult msaa_result = VK_SUCCESS;
};

// Renders (or only clears) one case at the given sample count and reads the
// resolved image back. The green channel carries the fill, magenta the clear.
static bool run_case(const char *label, VkSampleCountFlagBits samples, bool draw,
                     const unsigned char *vs_code, unsigned vs_len,
                     const unsigned char *fs_code, unsigned fs_len, VkInstance inst,
                     VkDevice dev, VkQueue queue, const DevApi &api, const RtApi &rt,
                     const VkPhysicalDeviceMemoryProperties &mp, CaseStats *stats) {
    const uint32_t W = 64, H = 64;
    const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    bool pass = false;

    VkImage msaa_img = VK_NULL_HANDLE;
    VkDeviceMemory msaa_mem = VK_NULL_HANDLE;
    VkImageView msaa_view = VK_NULL_HANDLE;
    VkImage res_img = VK_NULL_HANDLE;
    VkDeviceMemory res_mem = VK_NULL_HANDLE;
    VkImageView res_view = VK_NULL_HANDLE;
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

    std::printf("stage=%s begin samples=%d draw=%d\n", label, (int)samples, draw ? 1 : 0);
    do {
        // Multisampled colour attachment.
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = {W, H, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = samples;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = rt.CreateImage(dev, &ici, nullptr, &msaa_img);
        stats->msaa_result = r;
        if (!ok("vkCreateImage(msaa)", r))
            break;
        stats->created_msaa = true;

        VkMemoryRequirements req{};
        rt.GetImageMemoryRequirements(dev, msaa_img, &req);
        uint32_t mtype = 0;
        if (!find_memory_type(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                              &mtype)) {
            std::printf("errors: no DEVICE_LOCAL memory for msaa image\n");
            break;
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mtype;
        if (!ok("vkAllocateMemory(msaa)", api.AllocateMemory(dev, &mai, nullptr, &msaa_mem)))
            break;
        if (!ok("vkBindImageMemory(msaa)", rt.BindImageMemory(dev, msaa_img, msaa_mem, 0)))
            break;

        std::printf("stage=%s image_ok\n", label);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = msaa_img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!ok("vkCreateImageView(msaa)", rt.CreateImageView(dev, &vci, nullptr, &msaa_view)))
            break;

        // Single-sampled destination, both the resolve target and the copy source.
        VkImageCreateInfo rici = ici;
        rici.samples = VK_SAMPLE_COUNT_1_BIT;
        rici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!ok("vkCreateImage(resolve)", rt.CreateImage(dev, &rici, nullptr, &res_img)))
            break;
        rt.GetImageMemoryRequirements(dev, res_img, &req);
        if (!find_memory_type(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                              &mtype)) {
            std::printf("errors: no DEVICE_LOCAL memory for resolve image\n");
            break;
        }
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mtype;
        if (!ok("vkAllocateMemory(resolve)", api.AllocateMemory(dev, &mai, nullptr, &res_mem)))
            break;
        if (!ok("vkBindImageMemory(resolve)", rt.BindImageMemory(dev, res_img, res_mem, 0)))
            break;
        VkImageViewCreateInfo rvci = vci;
        rvci.image = res_img;
        if (!ok("vkCreateImageView(resolve)", rt.CreateImageView(dev, &rvci, nullptr, &res_view)))
            break;

        // Render pass. With more than one sample the colour attachment is
        // resolved into the single-sampled image inside the same pass; at one
        // sample a resolve attachment is illegal, so the pass renders straight
        // into the image that gets copied out.
        const bool use_resolve = samples != VK_SAMPLE_COUNT_1_BIT;
        VkAttachmentDescription att[2]{};
        att[0].format = fmt;
        att[0].samples = samples;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (use_resolve) {
            att[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // resolved, not stored
            att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att[1].format = fmt;
            att[1].samples = VK_SAMPLE_COUNT_1_BIT;
            att[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            att[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        } else {
            att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolve_ref{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &color_ref;
        sub.pResolveAttachments = use_resolve ? &resolve_ref : nullptr;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        std::printf("stage=%s views_ok\n", label);
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = use_resolve ? 2 : 1;
        rpci.pAttachments = att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        VkSubpassDependency dep2{};
        dep2.srcSubpass = 0;
        dep2.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep2.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep2.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkSubpassDependency deps[2] = {dep, dep2};

        rpci.dependencyCount = 2;
        rpci.pDependencies = deps;
        if (!ok("vkCreateRenderPass", rt.CreateRenderPass(dev, &rpci, nullptr, &rp)))
            break;

        VkImageView views[2] = {msaa_view, res_view};
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rp;
        fci.attachmentCount = use_resolve ? 2 : 1;
        fci.pAttachments = use_resolve ? views : &views[1];
        fci.width = W;
        fci.height = H;
        fci.layers = 1;
        if (!ok("vkCreateFramebuffer", rt.CreateFramebuffer(dev, &fci, nullptr, &fb)))
            break;

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = vs_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(vs_code);
        if (!ok("vkCreateShaderModule(vs)",
                api.CreateShaderModule(dev, &smci, nullptr, &vs)))
            break;
        smci.codeSize = fs_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(fs_code);
        if (!ok("vkCreateShaderModule(fs)",
                api.CreateShaderModule(dev, &smci, nullptr, &fs)))
            break;

        std::printf("stage=%s framebuffer_ok\n", label);
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        // No dynamic state is enabled, so the viewport and scissor have to be
        // part of the pipeline itself.
        VkViewport static_vp{0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f};
        VkRect2D static_sc{{0, 0}, {W, H}};
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.pViewports = &static_vp;
        vp.scissorCount = 1;
        vp.pScissors = &static_sc;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = samples;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xf;
        cba.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!ok("vkCreatePipelineLayout",
                api.CreatePipelineLayout(dev, &plci, nullptr, &pl)))
            break;

        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pColorBlendState = &cb;
        gpci.layout = pl;
        gpci.renderPass = rp;
        gpci.subpass = 0;
        if (!ok("vkCreateGraphicsPipelines",
                api.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe)))
            break;

        std::printf("stage=%s pipeline_ok\n", label);
        // Host visible readback buffer: W*H*4 bytes.
        const VkDeviceSize bytes = VkDeviceSize(W) * H * 4;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!ok("vkCreateBuffer", api.CreateBuffer(dev, &bci, nullptr, &buf)))
            break;
        api.GetBufferMemoryRequirements(dev, buf, &req);
        mtype = 0;
        if (!find_memory_type(mp, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              0, &mtype)) {
            std::printf("errors: no HOST_VISIBLE memory for readback\n");
            break;
        }
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mtype;
        if (!ok("vkAllocateMemory(buffer)", api.AllocateMemory(dev, &mai, nullptr, &bmem)))
            break;
        if (!ok("vkBindBufferMemory", api.BindBufferMemory(dev, buf, bmem, 0)))
            break;

        std::printf("stage=%s pool_begin\n", label);
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = g_qf_index;
        if (!ok("vkCreateCommandPool", api.CreateCommandPool(dev, &pci, nullptr, &pool)))
            break;
        std::printf("stage=%s pool_ok\n", label);
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (!ok("vkAllocateCommandBuffers",
                api.AllocateCommandBuffers(dev, &cbai, &cmd)))
            break;

        std::printf("stage=%s cb_alloc_ok\n", label);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!ok("vkBeginCommandBuffer", api.BeginCommandBuffer(cmd, &bi)))
            break;

        // The attachment starts UNDEFINED and the pass clears it, so no barrier
        // is needed before begin; the resolve attachment's layout comes from the
        // pass itself.
        std::printf("stage=%s begin_ok\n", label);
        VkClearValue clear{};
        clear.color.float32[0] = 1.0f;  // magenta, so the clear is unmistakable
        clear.color.float32[1] = 0.0f;
        clear.color.float32[2] = 1.0f;
        clear.color.float32[3] = 1.0f;

        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = rp;
        rbi.framebuffer = fb;
        rbi.renderArea = {{0, 0}, {W, H}};
        rbi.clearValueCount = 1;
        rbi.pClearValues = &clear;
        rt.CmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        std::printf("stage=%s rp_begun\n", label);
        api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        std::printf("stage=%s pipe_bound\n", label);
        if (draw) {
            std::printf("stage=%s cmd_draw_enter\n", label);
            rt.CmdDraw(cmd, 3, 1, 0, 0);
            std::printf("stage=%s cmd_draw_returned\n", label);
        }
        std::printf("stage=%s draw_ok\n", label);
        rt.CmdEndRenderPass(cmd);

        // Resolve image is TRANSFER_SRC_OPTIMAL after the pass; copy it out.
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W, H, 1};
        rt.CmdCopyImageToBuffer(cmd, res_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                                &region);

        std::printf("stage=%s copy_ok\n", label);
        if (!ok("vkEndCommandBuffer", api.EndCommandBuffer(cmd)))
            break;

        std::printf("stage=%s recorded\n", label);
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
                api.WaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull))) {
            std::printf("errors: fence wait timed out or failed for %s\n", label);
            break;
        }

        std::printf("stage=%s submitted\n", label);
        void *mapped = nullptr;
        if (!ok("vkMapMemory", api.MapMemory(dev, bmem, 0, bytes, 0, &mapped)))
            break;

        const unsigned char *px = static_cast<const unsigned char *>(mapped);
        int distinct[256] = {0};
        int distinct_count = 0;
        int mn = 255, mx = 0;
        int intermediate = 0;
        for (uint32_t y = 4; y < H - 4; ++y) {
            for (uint32_t x = 4; x < W - 4; ++x) {
                const int g = px[(y * W + x) * 4 + 1];
                if (!distinct[g]++) distinct_count++;
                if (g < mn) mn = g;
                if (g > mx) mx = g;
                if (g > 8 && g < 247) ++intermediate;
            }
        }
        stats->distinct_green = distinct_count;
        stats->min_green = mn;
        stats->max_green = mx;
        stats->intermediate = intermediate;
        stats->clear_only = !draw;
        {
            const size_t corner = (1 * W + 1) * 4;
            stats->corner_r = px[corner + 2];
            stats->corner_g = px[corner + 1];
            const uint32_t my = H / 2;
            int len = 0;
            for (uint32_t x = W / 2 - 8; x <= W / 2 + 8 && len < 60; ++x) {
                const int r = px[(my * W + x) * 4 + 2];
                const int g = px[(my * W + x) * 4 + 1];
                stats->midrow[len++] = (r < 8 && g < 8) ? 'B'
                                    : (r > 247 && g < 8) ? 'M'
                                    : (r < 8 && g > 247) ? 'G'
                                    : (r > 8 && g > 8) ? '?'
                                                       : '.';
            }
            stats->midrow[len] = 0;
        }
        api.UnmapMemory(dev, bmem);

        // A single-sample target has to show both the clear and the fill; more
        // samples must additionally show intermediate coverage on the diagonal.
        const bool drew = (mx > 247) && (mn < 8);
        const bool needs_fringe = samples != VK_SAMPLE_COUNT_1_BIT && draw;
        pass = draw ? (drew && (!needs_fringe || intermediate >= 1))
                    : (stats->corner_r > 247 && mx < 8);
    } while (false);

    std::printf(
        "MODE=vk-msaa ITEM=%s samples=%d draw=%d created=%d create_result=%s "
        "corner_rg=%d,%d midrow=[%s] distinct_green=%d min=%d max=%d intermediate=%d RESULT=%s\n",
        label, (int)samples, draw ? 1 : 0, stats->created_msaa ? 1 : 0,
        vkres(stats->msaa_result), stats->corner_r, stats->corner_g, stats->midrow,
        stats->distinct_green, stats->min_green, stats->max_green,
        stats->intermediate,
        pass ? "PASS" : "FAIL");

    if (pipe) api.DestroyPipeline(dev, pipe, nullptr);
    if (pl) api.DestroyPipelineLayout(dev, pl, nullptr);
    if (vs) api.DestroyShaderModule(dev, vs, nullptr);
    if (fs) api.DestroyShaderModule(dev, fs, nullptr);
    if (fence) api.DestroyFence(dev, fence, nullptr);
    if (pool) api.DestroyCommandPool(dev, pool, nullptr);
    if (buf) api.DestroyBuffer(dev, buf, nullptr);
    if (bmem) api.FreeMemory(dev, bmem, nullptr);
    if (fb) rt.DestroyFramebuffer(dev, fb, nullptr);
    if (rp) rt.DestroyRenderPass(dev, rp, nullptr);
    if (msaa_view) rt.DestroyImageView(dev, msaa_view, nullptr);
    if (res_view) rt.DestroyImageView(dev, res_view, nullptr);
    if (msaa_img) rt.DestroyImage(dev, msaa_img, nullptr);
    if (res_img) rt.DestroyImage(dev, res_img, nullptr);
    if (msaa_mem) api.FreeMemory(dev, msaa_mem, nullptr);
    if (res_mem) api.FreeMemory(dev, res_mem, nullptr);
    return pass;
}

int main(int argc, char **argv) {
    // Unbuffered: when the driver hangs we still want the stages we reached.
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *dll = "C:\\panvk\\umd_c1\\vulkan_virtio.dll";
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0')
            dll = argv[i];
    }
    std::printf("probe=vk_msaa\n");
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
    app.pApplicationName = "vk_msaa";
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
    PFN_vkGetPhysicalDeviceFormatProperties get_fprops =
        proc<PFN_vkGetPhysicalDeviceFormatProperties>(
            instance, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkCreateDevice create_device =
        proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa =
        proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    if (!enum_pd || !get_mem_props || !get_qf || !create_device || !gdpa || !get_fprops) {
        std::printf("ERROR: instance entry points missing\n");
        return 1;
    }

    uint32_t pd_count = 0;
    enum_pd(instance, &pd_count, nullptr);
    std::vector<VkPhysicalDevice> pds(pd_count ? pd_count : 1);
    if (pd_count) enum_pd(instance, &pd_count, pds.data());
    if (!pd_count) {
        std::printf("ERROR: no physical devices\n");
        return 1;
    }
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceMemoryProperties mp{};
    get_mem_props(pd, &mp);

    VkFormatProperties fp{};
    get_fprops(pd, VK_FORMAT_B8G8R8A8_UNORM, &fp);
    std::printf("format=B8G8R8A8_UNORM optimal=0x%08x linear=0x%08x buffer=0x%08x\n",
                (unsigned)fp.optimalTilingFeatures, (unsigned)fp.linearTilingFeatures,
                (unsigned)fp.bufferFeatures);

    uint32_t qf_count = 0;
    get_qf(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count ? qf_count : 1);
    if (qf_count) get_qf(pd, &qf_count, qf.data());
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

    g_qf_index = qf_index;
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
    if (!load_dev_api(api, gdpa, dev, instance)) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    api.GetDeviceQueue(dev, qf_index, 0, &queue);

    RtApi rt;
    if (!load_rt_api(rt, gdpa, dev, instance, get_fprops)) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    int failed = 0;
    CaseStats s{};
    // Control first: same geometry, single sampled. If this has no ink the rest
    // of the run says nothing.
    if (!run_case("msaa1_draw", VK_SAMPLE_COUNT_1_BIT, true, msaa_diag_vert_spirv,
                  msaa_diag_vert_spirv_len, msaa_green_frag_spirv,
                  msaa_green_frag_spirv_len, instance, dev, queue, api, rt, mp, &s))
        failed++;
    CaseStats s2{};
    if (!run_case("msaa4_draw", VK_SAMPLE_COUNT_4_BIT, true, msaa_diag_vert_spirv,
                  msaa_diag_vert_spirv_len, msaa_green_frag_spirv,
                  msaa_green_frag_spirv_len, instance, dev, queue, api, rt, mp, &s2))
        failed++;
    CaseStats s3{};
    // Same target and pass, no draw: the clear alone must reach the resolve image.
    if (!run_case("msaa4_clear", VK_SAMPLE_COUNT_4_BIT, false, msaa_diag_vert_spirv,
                  msaa_diag_vert_spirv_len, msaa_green_frag_spirv,
                  msaa_green_frag_spirv_len, instance, dev, queue, api, rt, mp, &s3))
        failed++;

    std::printf("MODE=vk-msaa SUMMARY items=3 failed=%d\n", failed);
    std::printf("RESULT: %s\n", failed ? "FAIL" : "PASS");
    api.DeviceWaitIdle(dev);
    api.DestroyDevice(dev, nullptr);
    return failed ? 1 : 0;
}
