// Direct SPIR-V replay of the real D2D text glyph shader pair against the
// panvk Windows ICD, to localise the D2D text "ink=0" failure.
//
// The replayed modules are byte-identical dumps taken from the failing D2D
// pipeline (DXVK dumpSpv, pre-linkage; the pair's interfaces are aligned so
// linkage makes no changes):
//   vs.fdce93f5112c6d3fc3003dca9968ee8e.spv  SHA256 e6c823eb...55c79699
//   fs.8ed1f4d24a53953766a02d7852726c7c.spv  SHA256 f7c76857...01e7c90c1
// The FS loads a u32 texel buffer (5-tap coverage sums) and a f32 2D LUT
// (three fetches) and writes one BGRA8 pixel; the VS takes five vertex
// attributes (D2D packs small integers into raw f32 bit patterns), one UBO
// (cb0, set 1 binding 0) and produces SV_Position plus the FS interface.
//
// Items:
//   sanity_const  handcrafted trivial VS/FS pair writing a constant colour;
//                proves instance/device/pipeline/renderpass/readback on the
//                ICD independent of the D2D modules.
//   clear_only   the BGRA8 target cleared (R=10,G=200,B=20,A=255) and copied
//                back without any draw.
//   replay_zero  the real D2D pair with the u32 texel buffer all zero. The
//                LUT row is designed so a correctly running FS reproduces
//                the clear colour in RGB and alpha=200 in A, which the
//                clear itself never produces (clear alpha is 255).
//   replay_ramp  the real D2D pair with the u32 texel buffer set to
//                min(i,60). A correctly running FS must produce a
//                horizontal ink gradient (all pixels differ from both the
//                clear signature and the zero signature).
//
// Verdict (printed as LAYER=...):
//   sanity PASS + ramp ink + zero reproduces the zero signature
//     -> the real FS executes correctly when fed directly; the D2D failure
//        sits in the DXVK descriptor/pipeline mapping layer.
//   sanity PASS + both replays equal the clear signature everywhere
//     -> the FS does not execute as compiled; suspicion moves to SPIR-V
//        compilation / PanVK execution (texel-buffer fetch, bit-op chain).
//   anything else -> HARNESS SUSPECT, raw values are printed for review.
//
// Build (d2d_text_vs_spv.h / d2d_text_fs_spv.h / sanity_*_spv.h are checked
// in; no glslang step is needed for this test):
//   cl /nologo /EHsc /O2 /std:c++17 /I third_party\vulkan\include ^
//      test_vk_d2d_replay.cpp /Fe:vk_d2d_replay_arm64.exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#define VK_NO_PROTOTYPES
#include "third_party/vulkan/include/vulkan/vulkan_core.h"
#include "d2d_text_vs_spv.h"
#include "d2d_text_fs_spv.h"
#include "sanity_vs_spv.h"
#include "sanity_fs_spv.h"
#include "sanity_novb_vs_spv.h"

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
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
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

static bool find_memory_type(const VkPhysicalDeviceMemoryProperties &mp, uint32_t bits,
                             VkMemoryPropertyFlags required, uint32_t *out) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;
        if ((mp.memoryTypes[i].propertyFlags & required) == required) {
            *out = i;
            return true;
        }
    }
    return false;
}

static float int_bits_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
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
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkWaitForFences WaitForFences;
};

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
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCreateBufferView CreateBufferView;
    PFN_vkDestroyBufferView DestroyBufferView;
};

static int load_dev_api(DevApi &a, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, VkInstance instance) {
    a = DevApi{};
    int missing = 0;
#define LOAD(field)                                                                       \
    do {                                                                                  \
        a.field = dev_proc<PFN_vk##field>(gdpa, dev, instance, "vk" #field);               \
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
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdBindPipeline);
    LOAD(CmdBindDescriptorSets);
    LOAD(CmdBindVertexBuffers);
    LOAD(CmdCopyBuffer);
    LOAD(CmdPipelineBarrier);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(QueueSubmit);
    LOAD(WaitForFences);
#undef LOAD
    return missing;
}

static bool load_rt_api(RtApi &a, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, VkInstance inst) {
    a = RtApi{};
    int missing = 0;
#define LOADRT(field)                                                                     \
    do {                                                                                  \
        a.field = dev_proc<PFN_vk##field>(gdpa, dev, inst, "vk" #field);                  \
        if (!a.field) {                                                                   \
            std::printf("missing_entrypoint: vk%s\n", #field);                           \
            missing++;                                                                   \
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
    LOADRT(DestroyPipeline);
    LOADRT(CmdBeginRenderPass);
    LOADRT(CmdEndRenderPass);
    LOADRT(CmdDraw);
    LOADRT(CmdCopyImageToBuffer);
    LOADRT(CmdCopyBufferToImage);
    LOADRT(CreateBufferView);
    LOADRT(DestroyBufferView);
#undef LOADRT
    return missing == 0;
}

static bool create_buffer_mem(const DevApi &api, VkDevice dev,
                              const VkPhysicalDeviceMemoryProperties &mp,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkBuffer *out_buf, VkDeviceMemory *out_mem) {
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
    if (!find_memory_type(mp, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &type_index)) {
        std::printf("errors: no HOST_VISIBLE|COHERENT memory type for usage=0x%x\n", (unsigned)usage);
        return false;
    }
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

static bool fill_host_buffer(const DevApi &api, VkDevice dev, VkDeviceMemory mem,
                             const void *data, VkDeviceSize size) {
    void *map = nullptr;
    if (!ok("vkMapMemory", api.MapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map)))
        return false;
    std::memcpy(map, data, (size_t)size);
    api.UnmapMemory(dev, mem);
    return true;
}

static bool create_image_dev_local(const RtApi &rt, const DevApi &api, VkDevice dev,
                                   const VkPhysicalDeviceMemoryProperties &mp,
                                   uint32_t w, uint32_t h, VkFormat fmt,
                                   VkImageUsageFlags usage,
                                   VkImage *out_img, VkDeviceMemory *out_mem) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ok("vkCreateImage", rt.CreateImage(dev, &ici, nullptr, out_img)))
        return false;
    VkMemoryRequirements req{};
    rt.GetImageMemoryRequirements(dev, *out_img, &req);
    uint32_t mtype = 0;
    if (!find_memory_type(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mtype)) {
        std::printf("errors: no DEVICE_LOCAL memory type for image\n");
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mtype;
    if (!ok("vkAllocateMemory(image)", api.AllocateMemory(dev, &mai, nullptr, out_mem)))
        return false;
    if (!ok("vkBindImageMemory", rt.BindImageMemory(dev, *out_img, *out_mem, 0)))
        return false;
    return true;
}

// One 256x256 BGRA8 frame: image + view + renderpass + framebuffer with the
// standard clear colour (R=10, G=200, B=20, A=255)/255.
struct Frame {
    static const uint32_t W = 256, H = 256;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    bool init(const RtApi &rt, const DevApi &api, VkDevice dev,
              const VkPhysicalDeviceMemoryProperties &mp) {
        if (!create_image_dev_local(rt, api, dev, mp, W, H, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    &image, &imem))
            return false;
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_B8G8R8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!ok("vkCreateImageView", rt.CreateImageView(dev, &vci, nullptr, &view)))
            return false;
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_B8G8R8A8_UNORM;
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
            return false;
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rp;
        fci.attachmentCount = 1;
        fci.pAttachments = &view;
        fci.width = W;
        fci.height = H;
        fci.layers = 1;
        if (!ok("vkCreateFramebuffer", rt.CreateFramebuffer(dev, &fci, nullptr, &fb)))
            return false;
        return true;
    }
    void destroy(const RtApi &rt, const DevApi &api, VkDevice dev) {
        if (fb) rt.DestroyFramebuffer(dev, fb, nullptr);
        if (rp) rt.DestroyRenderPass(dev, rp, nullptr);
        if (view) rt.DestroyImageView(dev, view, nullptr);
        if (image) rt.DestroyImage(dev, image, nullptr);
        if (imem) api.FreeMemory(dev, imem, nullptr);
    }
};

// Pixel classification over a 256x256 BGRA readback (bytes B,G,R,A).
// clear  = {20,200,10,255}  the renderpass clear colour
// zero   = {20,200,10,200}  what the real FS must write with an all-zero u32
//                           texel buffer if it executes correctly
// sanity = {191,128,64,255} what the sanity FS constant write must produce
struct PixStats {
    uint32_t n_clear = 0, n_zero = 0, n_other = 0;
    uint32_t first_bgra[4] = {0, 0, 0, 0};
    uint32_t first_x = 0, first_y = 0;
    uint8_t samples[5][4]{};
};
static const uint8_t SIG_CLEAR[4] = {20, 200, 10, 255};
static const uint8_t SIG_ZERO[4] = {20, 200, 10, 200};
static const uint8_t SIG_SANITY[4] = {191, 128, 64, 255};

static PixStats classify(const uint8_t *map) {
    PixStats s;
    const uint32_t W = Frame::W, H = Frame::H;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t *p = map + (size_t)(y * W + x) * 4;
            if (!std::memcmp(p, SIG_CLEAR, 4)) {
                s.n_clear++;
            } else if (!std::memcmp(p, SIG_ZERO, 4)) {
                s.n_zero++;
            } else {
                s.n_other++;
                if (s.n_other == 1) {
                    std::memcpy(s.first_bgra, p, 4);
                    s.first_x = x;
                    s.first_y = y;
                }
            }
        }
    }
    static const uint32_t pts[5][2] = {{8, 8}, {247, 8}, {8, 247}, {247, 247}, {128, 128}};
    for (int i = 0; i < 5; ++i) {
        const uint8_t *p = map + (size_t)(pts[i][1] * W + pts[i][0]) * 4;
        std::memcpy(s.samples[i], p, 4);
    }
    return s;
}

static void print_stats(const char *item, const PixStats &s) {
    std::printf("points bgra: (8,8)=%u,%u,%u,%u (247,8)=%u,%u,%u,%u (8,247)=%u,%u,%u,%u"
                " (247,247)=%u,%u,%u,%u (128,128)=%u,%u,%u,%u\n",
                s.samples[0][0], s.samples[0][1], s.samples[0][2], s.samples[0][3],
                s.samples[1][0], s.samples[1][1], s.samples[1][2], s.samples[1][3],
                s.samples[2][0], s.samples[2][1], s.samples[2][2], s.samples[2][3],
                s.samples[3][0], s.samples[3][1], s.samples[3][2], s.samples[3][3],
                s.samples[4][0], s.samples[4][1], s.samples[4][2], s.samples[4][3]);
    std::printf("counts clear=%u zero=%u other=%u total=%u first_other=%u,%u bgra=%u,%u,%u,%u\n",
                s.n_clear, s.n_zero, s.n_other, Frame::W * Frame::H,
                s.first_x, s.first_y,
                s.first_bgra[0], s.first_bgra[1], s.first_bgra[2], s.first_bgra[3]);
}

// Renders one frame on `frame` and copies the result back into `rb` (a
// HOST_VISIBLE buffer of W*H*4 bytes).  When `pre` is non-null it runs after
// vkBeginCommandBuffer and BEFORE CmdBeginRenderPass — upload/copy work must
// never sit inside a renderpass instance.  When `draw` is null the command
// buffer only clears; otherwise it is invoked between renderpass begin/end.
static bool render_and_readback(
    const char *label, VkDevice dev, VkQueue queue,
    const DevApi &api, const RtApi &rt,
    const Frame &frame, VkBuffer rb, VkDeviceMemory rbmem,
    void (*draw)(VkCommandBuffer, void *), void *draw_ctx,
    void (*pre)(VkCommandBuffer, void *), void *pre_ctx) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool good = false;
    do {
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
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!ok("vkBeginCommandBuffer", api.BeginCommandBuffer(cmd, &cbbi)))
            break;
        if (pre)
            pre(cmd, pre_ctx);
        VkClearValue clear{};
        clear.color.float32[0] = 10.0f / 255.0f;
        clear.color.float32[1] = 200.0f / 255.0f;
        clear.color.float32[2] = 20.0f / 255.0f;
        clear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = frame.rp;
        rpbi.framebuffer = frame.fb;
        rpbi.renderArea.extent = {Frame::W, Frame::H};
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        rt.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        if (draw)
            draw(cmd, draw_ctx);
        rt.CmdEndRenderPass(cmd);
        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = frame.image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &ib);
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {Frame::W, Frame::H, 1};
        rt.CmdCopyImageToBuffer(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1,
                                &bic);
        if (!ok("vkEndCommandBuffer", api.EndCommandBuffer(cmd)))
            break;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!ok("vkCreateFence", api.CreateFence(dev, &fci, nullptr, &fence)))
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
        good = true;
    } while (0);
    if (fence) api.DestroyFence(dev, fence, nullptr);
    if (pool) api.DestroyCommandPool(dev, pool, nullptr);
    return good;
}

static bool readback_pixels(VkDevice dev, const DevApi &api, VkDeviceMemory rbmem,
                           PixStats *out) {
    void *map = nullptr;
    if (!ok("vkMapMemory", api.MapMemory(dev, rbmem, 0, VK_WHOLE_SIZE, 0, &map)))
        return false;
    *out = classify(static_cast<const uint8_t *>(map));
    api.UnmapMemory(dev, rbmem);
    return true;
}

// ---------------------------------------------------------------------------
// sanity_const: handcrafted modules, no descriptors, one f32vec2 attribute.
// ---------------------------------------------------------------------------
static bool run_sanity(VkDevice dev, VkQueue queue,
                       const DevApi &api, const RtApi &rt,
                       const VkPhysicalDeviceMemoryProperties &mp,
                       PixStats *out) {
    Frame frame;
    VkBuffer vb = VK_NULL_HANDLE, rb = VK_NULL_HANDLE;
    VkDeviceMemory vbmem = VK_NULL_HANDLE, rbmem = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool pass = false;
    do {
        if (!frame.init(rt, api, dev, mp))
            break;
        if (!create_buffer_mem(api, dev, mp, 3 * 8,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vbmem))
            break;
        const float tri[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
        if (!fill_host_buffer(api, dev, vbmem, tri, sizeof tri))
            break;
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)Frame::W * Frame::H * 4,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rbmem))
            break;

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sanity_vs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(sanity_vs_spv);
        if (!ok("vkCreateShaderModule(sanity vs)",
                api.CreateShaderModule(dev, &smci, nullptr, &vs)))
            break;
        smci.codeSize = sanity_fs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(sanity_fs_spv);
        if (!ok("vkCreateShaderModule(sanity fs)",
                api.CreateShaderModule(dev, &smci, nullptr, &fs)))
            break;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!ok("vkCreatePipelineLayout", api.CreatePipelineLayout(dev, &plci, nullptr, &pl)))
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
        VkVertexInputBindingDescription bind{};
        bind.binding = 0;
        bind.stride = 8;
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attr{};
        attr.location = 0;
        attr.binding = 0;
        attr.format = VK_FORMAT_R32G32_SFLOAT;
        attr.offset = 0;
        VkPipelineVertexInputStateCreateInfo vist{};
        vist.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vist.vertexBindingDescriptionCount = 1;
        vist.pVertexBindingDescriptions = &bind;
        vist.vertexAttributeDescriptionCount = 1;
        vist.pVertexAttributeDescriptions = &attr;
        VkPipelineInputAssemblyStateCreateInfo iast{};
        iast.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        iast.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{};
        vp.width = (float)Frame::W;
        vp.height = (float)Frame::H;
        vp.maxDepth = 1.0f;
        VkRect2D sc{};
        sc.extent = {Frame::W, Frame::H};
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
        gpci.renderPass = frame.rp;
        gpci.subpass = 0;
        if (!ok("vkCreateGraphicsPipelines(sanity)",
                rt.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe)))
            break;

        struct Ctx {
            const RtApi *rt;
            const DevApi *api;
            VkPipeline pipe;
            VkBuffer vb;
        } ctx{&rt, &api, pipe, vb};
        auto draw = [](VkCommandBuffer cmd, void *ud) {
            Ctx *c = static_cast<Ctx *>(ud);
            c->api->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
            VkDeviceSize off = 0;
            c->api->CmdBindVertexBuffers(cmd, 0, 1, &c->vb, &off);
            c->rt->CmdDraw(cmd, 3, 1, 0, 0);
        };
        if (!render_and_readback("sanity", dev, queue, api, rt, frame, rb, rbmem, draw, &ctx,
                                 nullptr, nullptr))
            break;
        if (!readback_pixels(dev, api, rbmem, out))
            break;
        // Every pixel must be the constant colour (191,128,64,255) in BGRA.
        pass = (out->n_other == 0 && out->n_clear == 0 && out->n_zero == 0);
    } while (0);

    if (pipe) rt.DestroyPipeline(dev, pipe, nullptr);
    if (pl) api.DestroyPipelineLayout(dev, pl, nullptr);
    if (fs) api.DestroyShaderModule(dev, fs, nullptr);
    if (vs) api.DestroyShaderModule(dev, vs, nullptr);
    if (rb) api.DestroyBuffer(dev, rb, nullptr);
    if (rbmem) api.FreeMemory(dev, rbmem, nullptr);
    if (vb) api.DestroyBuffer(dev, vb, nullptr);
    if (vbmem) api.FreeMemory(dev, vbmem, nullptr);
    frame.destroy(rt, api, dev);
    std::printf("MODE=vk-d2d-replay ITEM=sanity_const RESULT=%s\n", pass ? "PASS" : "FAIL");
    print_stats("sanity_const", *out);
    return pass;
}

// ---------------------------------------------------------------------------
// sanity_novb: the proven-on-device control shape (same as test_vk_uint_rt):
// gl_VertexIndex fullscreen triangle, constant FS, NO vertex buffer, NO
// descriptors, NO CPU->GPU upload of any kind. If this paints while
// sanity_const does not, the draw path itself is healthy and the failure
// isolates to CPU-uploaded data (vertex fetch / upload direction).
// ---------------------------------------------------------------------------
static bool run_sanity_novb(VkDevice dev, VkQueue queue,
                             const DevApi &api, const RtApi &rt,
                             const VkPhysicalDeviceMemoryProperties &mp,
                             PixStats *out) {
    Frame frame;
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rbmem = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool pass = false;
    do {
        if (!frame.init(rt, api, dev, mp))
            break;
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)Frame::W * Frame::H * 4,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rbmem))
            break;

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sanity_novb_vs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(sanity_novb_vs_spv);
        if (!ok("vkCreateShaderModule(sanity_novb vs)",
                api.CreateShaderModule(dev, &smci, nullptr, &vs)))
            break;
        smci.codeSize = sanity_fs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(sanity_fs_spv);
        if (!ok("vkCreateShaderModule(sanity_novb fs)",
                api.CreateShaderModule(dev, &smci, nullptr, &fs)))
            break;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!ok("vkCreatePipelineLayout", api.CreatePipelineLayout(dev, &plci, nullptr, &pl)))
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
        vp.width = (float)Frame::W;
        vp.height = (float)Frame::H;
        vp.maxDepth = 1.0f;
        VkRect2D sc{};
        sc.extent = {Frame::W, Frame::H};
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
        gpci.renderPass = frame.rp;
        gpci.subpass = 0;
        if (!ok("vkCreateGraphicsPipelines(sanity_novb)",
                rt.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe)))
            break;

        struct Ctx {
            const RtApi *rt;
            VkPipeline pipe;
        } ctx{&rt, pipe};
        auto draw = [](VkCommandBuffer cmd, void *ud) {
            Ctx *c = static_cast<Ctx *>(ud);
            c->rt->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
            c->rt->CmdDraw(cmd, 3, 1, 0, 0);
        };
        if (!render_and_readback("sanity_novb", dev, queue, api, rt, frame, rb, rbmem,
                                 draw, &ctx, nullptr, nullptr))
            break;
        if (!readback_pixels(dev, api, rbmem, out))
            break;
        pass = (out->n_other == 0 && out->n_clear == 0 && out->n_zero == 0);
    } while (0);

    if (pipe) rt.DestroyPipeline(dev, pipe, nullptr);
    if (pl) api.DestroyPipelineLayout(dev, pl, nullptr);
    if (fs) api.DestroyShaderModule(dev, fs, nullptr);
    if (vs) api.DestroyShaderModule(dev, vs, nullptr);
    if (rb) api.DestroyBuffer(dev, rb, nullptr);
    if (rbmem) api.FreeMemory(dev, rbmem, nullptr);
    frame.destroy(rt, api, dev);
    std::printf("MODE=vk-d2d-replay ITEM=sanity_novb RESULT=%s\n", pass ? "PASS" : "FAIL");
    print_stats("sanity_novb", *out);
    return pass;
}

// ---------------------------------------------------------------------------
// vb_roundtrip: CPU-write -> GPU-read direction probe without any draw.
// (a) fill a buffer through the mapped blob, re-map and compare (CPU echo);
// (b) CmdCopyBuffer it GPU-side into a second HOST_VISIBLE buffer and read
// that back (GPU read). (a) failing means the blob mapping does not even
// hold CPU writes; (b) failing with (a) passing means the GPU cannot see
// CPU-written memory — the upload path is lost.
// ---------------------------------------------------------------------------
static bool run_vb_roundtrip(VkDevice dev, VkQueue queue,
                              const DevApi &api,
                              const VkPhysicalDeviceMemoryProperties &mp) {
    const uint32_t words[6] = {0xBF800000u, 0xBF800000u, 0x40400000u,
                               0xBF800000u, 0xBF800000u, 0x40400000u};
    bool cpu_echo = false, gpu_copy = false;
    uint32_t back_cpu[6] = {}, back_gpu[6] = {};
    VkBuffer vb = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    VkDeviceMemory vbmem = VK_NULL_HANDLE, dstmem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    do {
        if (!create_buffer_mem(api, dev, mp, sizeof words,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                &vb, &vbmem))
            break;
        if (!fill_host_buffer(api, dev, vbmem, words, sizeof words))
            break;
        {
            void *map = nullptr;
            if (!ok("vkMapMemory(vb echo)", api.MapMemory(dev, vbmem, 0, VK_WHOLE_SIZE, 0, &map)))
                break;
            std::memcpy(back_cpu, map, sizeof back_cpu);
            api.UnmapMemory(dev, vbmem);
            cpu_echo = (std::memcmp(back_cpu, words, sizeof words) == 0);
        }
        if (!create_buffer_mem(api, dev, mp, sizeof words,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst, &dstmem))
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
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!ok("vkBeginCommandBuffer", api.BeginCommandBuffer(cmd, &cbbi)))
            break;
        api.CmdCopyBuffer(cmd, vb, dst, sizeof words);
        if (!ok("vkEndCommandBuffer", api.EndCommandBuffer(cmd)))
            break;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!ok("vkCreateFence", api.CreateFence(dev, &fci, nullptr, &fence)))
            break;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (!ok("vkQueueSubmit", api.QueueSubmit(queue, 1, &si, fence)))
            break;
        if (!ok("vkWaitForFences", api.WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX)))
            break;
        {
            void *map = nullptr;
            if (!ok("vkMapMemory(vb gpu copy)", api.MapMemory(dev, dstmem, 0, VK_WHOLE_SIZE, 0, &map)))
                break;
            std::memcpy(back_gpu, map, sizeof back_gpu);
            api.UnmapMemory(dev, dstmem);
            gpu_copy = (std::memcmp(back_gpu, words, sizeof words) == 0);
        }
    } while (0);

    if (fence) api.DestroyFence(dev, fence, nullptr);
    if (pool) api.DestroyCommandPool(dev, pool, nullptr);
    if (dst) api.DestroyBuffer(dev, dst, nullptr);
    if (dstmem) api.FreeMemory(dev, dstmem, nullptr);
    if (vb) api.DestroyBuffer(dev, vb, nullptr);
    if (vbmem) api.FreeMemory(dev, vbmem, nullptr);

    std::printf("vb_roundtrip words=%08x %08x %08x %08x %08x %08x\n",
                words[0], words[1], words[2], words[3], words[4], words[5]);
    std::printf("vb_roundtrip cpu_echo=%d back=%08x %08x %08x %08x %08x %08x\n",
                cpu_echo ? 1 : 0,
                back_cpu[0], back_cpu[1], back_cpu[2], back_cpu[3], back_cpu[4], back_cpu[5]);
    std::printf("vb_roundtrip gpu_copy=%d back=%08x %08x %08x %08x %08x %08x\n",
                gpu_copy ? 1 : 0,
                back_gpu[0], back_gpu[1], back_gpu[2], back_gpu[3], back_gpu[4], back_gpu[5]);
    const bool pass = cpu_echo && gpu_copy;
    std::printf("MODE=vk-d2d-replay ITEM=vb_roundtrip RESULT=%s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// ---------------------------------------------------------------------------
// clear_only: no draw, just the clear.
// ---------------------------------------------------------------------------
static bool run_clear_only(VkDevice dev, VkQueue queue,
                           const DevApi &api, const RtApi &rt,
                           const VkPhysicalDeviceMemoryProperties &mp,
                           PixStats *out) {
    Frame frame;
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rbmem = VK_NULL_HANDLE;
    bool pass = false;
    do {
        if (!frame.init(rt, api, dev, mp))
            break;
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)Frame::W * Frame::H * 4,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rbmem))
            break;
        if (!render_and_readback("clear_only", dev, queue, api, rt, frame, rb, rbmem,
                                 nullptr, nullptr, nullptr, nullptr))
            break;
        if (!readback_pixels(dev, api, rbmem, out))
            break;
        pass = (out->n_clear == Frame::W * Frame::H);
    } while (0);
    if (rb) api.DestroyBuffer(dev, rb, nullptr);
    if (rbmem) api.FreeMemory(dev, rbmem, nullptr);
    frame.destroy(rt, api, dev);
    std::printf("MODE=vk-d2d-replay ITEM=clear_only RESULT=%s\n", pass ? "PASS" : "FAIL");
    if (pass) print_stats("clear_only", *out);
    return pass;
}

// ---------------------------------------------------------------------------
// replay of the real D2D pair. dataset_zero selects the all-zero u32 texel
// buffer; otherwise the buffer is min(i,60).
// ---------------------------------------------------------------------------
static bool run_replay(const char *label, bool dataset_zero, VkDevice dev, VkQueue queue,
                       const DevApi &api, const RtApi &rt,
                       const VkPhysicalDeviceMemoryProperties &mp,
                       PixStats *out) {
    Frame frame;
    VkBuffer vb = VK_NULL_HANDLE, rb = VK_NULL_HANDLE;
    VkBuffer ubo = VK_NULL_HANDLE, t0buf = VK_NULL_HANDLE, stage = VK_NULL_HANDLE;
    VkDeviceMemory vbmem = VK_NULL_HANDLE, rbmem = VK_NULL_HANDLE, ubomem = VK_NULL_HANDLE,
                  t0mem = VK_NULL_HANDLE, stagemem = VK_NULL_HANDLE;
    VkBufferView t0view = VK_NULL_HANDLE;
    VkImage t1 = VK_NULL_HANDLE;
    VkDeviceMemory t1mem = VK_NULL_HANDLE;
    VkImageView t1view = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_empty = VK_NULL_HANDLE, dsl_set1 = VK_NULL_HANDLE,
                          dsl_set2 = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dsets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipe = VK_NULL_HANDLE;

    const uint32_t LUT_W = 1024, LUT_H = 4, T0_TEXELS = 1024;

    do {
        if (!frame.init(rt, api, dev, mp))
            break;

        // Vertex data: 5 attributes, 36 bytes/vertex, D2D int-in-f32-bits.
        // v0 (0,0) v1 (256,0) v2 (0,256) as integer values packed in f32;
        // delta=(1,1); ATTRIB12.x bits 0x8001 -> FS brush=+1.0039, LUT row 1;
        // stride=0; ATTRIB16=0. cb0=(1/64,1/64,-1,-1) -> fullscreen triangle.
        float verts[3][9];
        for (int v = 0; v < 3; ++v) {
            float *f = verts[v];
            f[0] = int_bits_float(v == 1 ? 256u : 0u);
            f[1] = int_bits_float(v == 2 ? 256u : 0u);
            f[2] = 0.0f;
            f[3] = 0.0f;
            f[4] = int_bits_float(1u);
            f[5] = int_bits_float(1u);
            f[6] = int_bits_float(0x8001u);
            f[7] = 0.0f;
            f[8] = 0.0f;
        }
        if (!create_buffer_mem(api, dev, mp, sizeof verts,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vbmem))
            break;
        if (!fill_host_buffer(api, dev, vbmem, verts, sizeof verts))
            break;

        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)Frame::W * Frame::H * 4,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rbmem))
            break;

        const float cb0[4] = {1.0f / 64.0f, 1.0f / 64.0f, -1.0f, -1.0f};
        if (!create_buffer_mem(api, dev, mp, sizeof cb0, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               &ubo, &ubomem))
            break;
        if (!fill_host_buffer(api, dev, ubomem, cb0, sizeof cb0))
            break;

        std::vector<uint32_t> t0(T0_TEXELS);
        if (!dataset_zero) {
            for (uint32_t i = 0; i < T0_TEXELS; ++i)
                t0[i] = i < 60u ? i : 60u;
        }
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)T0_TEXELS * 4,
                               VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, &t0buf, &t0mem))
            break;
        if (!fill_host_buffer(api, dev, t0mem, t0.data(), (VkDeviceSize)t0.size() * 4))
            break;
        VkBufferViewCreateInfo bvci{};
        bvci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        bvci.buffer = t0buf;
        bvci.format = VK_FORMAT_R32_UINT;
        bvci.offset = 0;
        bvci.range = (VkDeviceSize)T0_TEXELS * 4;
        if (!ok("vkCreateBufferView(t0)", rt.CreateBufferView(dev, &bvci, nullptr, &t0view)))
            break;

        // LUT rows 0 and 1 identical: the zero-dataset fetches land exactly on
        // the clear components (x=0, 217, 434); the ramp dataset lands inside
        // the gradient bands.
        std::vector<float> lut((size_t)LUT_W * LUT_H * 4, 0.0f);
        for (uint32_t row = 0; row <= 1; ++row) {
            for (uint32_t x = 0; x < LUT_W; ++x) {
                float v = 0.0f;
                if (x <= 216) {
                    v = (x == 0) ? 10.0f / 255.0f : 0.05f + 0.85f * ((float)x / 216.0f);
                } else if (x <= 433) {
                    v = (x == 217) ? 200.0f / 255.0f
                                   : 200.0f / 255.0f + 0.15f * ((float)(x - 217) / 216.0f);
                } else if (x <= 650) {
                    v = (x == 434) ? 20.0f / 255.0f
                                   : 0.15f + 0.5f * ((float)(x - 434) / 216.0f);
                }
                float *texel = &lut[((size_t)row * LUT_W + x) * 4];
                texel[0] = v;
            }
        }
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)lut.size() * 4,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage, &stagemem))
            break;
        if (!fill_host_buffer(api, dev, stagemem, lut.data(),
                              (VkDeviceSize)lut.size() * 4))
            break;
        if (!create_image_dev_local(rt, api, dev, mp, LUT_W, LUT_H,
                                   VK_FORMAT_R32G32B32A32_SFLOAT,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   &t1, &t1mem))
            break;
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = t1;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!ok("vkCreateImageView(t1)", rt.CreateImageView(dev, &ivci, nullptr, &t1view)))
            break;

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = d2d_text_vs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(d2d_text_vs_spv);
        if (!ok("vkCreateShaderModule(d2d vs)",
                api.CreateShaderModule(dev, &smci, nullptr, &vs)))
            break;
        smci.codeSize = d2d_text_fs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(d2d_text_fs_spv);
        if (!ok("vkCreateShaderModule(d2d fs)",
                api.CreateShaderModule(dev, &smci, nullptr, &fs)))
            break;

        // Set layouts: set0 empty placeholder, set1 = cb0 UBO (vertex),
        // set2 = t0 texel buffer + t1 sampled image (fragment).
        VkDescriptorSetLayoutCreateInfo eci{};
        eci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (!ok("vkCreateDescriptorSetLayout(empty)",
                api.CreateDescriptorSetLayout(dev, &eci, nullptr, &dsl_empty)))
            break;
        VkDescriptorSetLayoutBinding ubo_bind{};
        ubo_bind.binding = 0;
        ubo_bind.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo_bind.descriptorCount = 1;
        ubo_bind.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo c1{};
        c1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        c1.bindingCount = 1;
        c1.pBindings = &ubo_bind;
        if (!ok("vkCreateDescriptorSetLayout(set1)",
                api.CreateDescriptorSetLayout(dev, &c1, nullptr, &dsl_set1)))
            break;
        VkDescriptorSetLayoutBinding img_binds[2]{};
        img_binds[0].binding = 0;
        img_binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        img_binds[0].descriptorCount = 1;
        img_binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        img_binds[1].binding = 1;
        img_binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        img_binds[1].descriptorCount = 1;
        img_binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo c2{};
        c2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        c2.bindingCount = 2;
        c2.pBindings = img_binds;
        if (!ok("vkCreateDescriptorSetLayout(set2)",
                api.CreateDescriptorSetLayout(dev, &c2, nullptr, &dsl_set2)))
            break;
        VkDescriptorSetLayout layouts[3] = {dsl_empty, dsl_set1, dsl_set2};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 3;
        plci.pSetLayouts = layouts;
        if (!ok("vkCreatePipelineLayout", api.CreatePipelineLayout(dev, &plci, nullptr, &pl)))
            break;

        VkDescriptorPoolSize pool_sizes[3]{};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_sizes[0].descriptorCount = 1;
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        pool_sizes[1].descriptorCount = 1;
        pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        pool_sizes[2].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 4;
        dpci.poolSizeCount = 3;
        dpci.pPoolSizes = pool_sizes;
        if (!ok("vkCreateDescriptorPool", api.CreateDescriptorPool(dev, &dpci, nullptr, &dpool)))
            break;
        VkDescriptorSetLayout want[2] = {dsl_set1, dsl_set2};
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = dpool;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = want;
        if (!ok("vkAllocateDescriptorSets", api.AllocateDescriptorSets(dev, &dsai, dsets)))
            break;

        VkDescriptorBufferInfo ubo_info{};
        ubo_info.buffer = ubo;
        ubo_info.offset = 0;
        ubo_info.range = 16;
        VkDescriptorImageInfo img_info{};
        img_info.imageView = t1view;
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = dsets[0];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &ubo_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = dsets[1];
        writes[1].dstBinding = 0;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        writes[1].pTexelBufferView = &t0view;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = dsets[1];
        writes[2].dstBinding = 1;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[2].pImageInfo = &img_info;
        api.UpdateDescriptorSets(dev, 3, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkVertexInputBindingDescription bind{};
        bind.binding = 0;
        bind.stride = 36;
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[5]{};
        for (int i = 0; i < 4; ++i) {
            attrs[i].location = (uint32_t)i;
            attrs[i].binding = 0;
            attrs[i].format = VK_FORMAT_R32G32_SFLOAT;
            attrs[i].offset = (uint32_t)(8 * i);
        }
        attrs[4].location = 4;
        attrs[4].binding = 0;
        attrs[4].format = VK_FORMAT_R32_SFLOAT;
        attrs[4].offset = 32;
        VkPipelineVertexInputStateCreateInfo vist{};
        vist.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vist.vertexBindingDescriptionCount = 1;
        vist.pVertexBindingDescriptions = &bind;
        vist.vertexAttributeDescriptionCount = 5;
        vist.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo iast{};
        iast.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        iast.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{};
        vp.width = (float)Frame::W;
        vp.height = (float)Frame::H;
        vp.maxDepth = 1.0f;
        VkRect2D sc{};
        sc.extent = {Frame::W, Frame::H};
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
        gpci.renderPass = frame.rp;
        gpci.subpass = 0;
        if (!ok("vkCreateGraphicsPipelines(d2d)",
                rt.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe)))
            break;

        // t1: staging -> image (TRANSFER_DST), then to SHADER_READ_ONLY.  This
        // is COPY work, so it must run BEFORE CmdBeginRenderPass (a renderpass
        // instance may not contain copy commands); it goes through the `pre`
        // callback.  Binding + draw stay in the `draw` callback.
        struct Ctx {
            const RtApi *rt;
            const DevApi *api;
            VkPipeline pipe;
            VkPipelineLayout pl;
            VkDescriptorSet *dsets;
            VkBuffer vb;
            VkBuffer stage;
            VkImage t1;
        } ctx{&rt, &api, pipe, pl, dsets, vb, stage, t1};
        auto pre = [](VkCommandBuffer cmd, void *ud) {
            Ctx *c = static_cast<Ctx *>(ud);
            VkImageMemoryBarrier to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcAccessMask = 0;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.image = c->t1;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            c->api->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                       nullptr, 1, &to_dst);
            VkBufferImageCopy cp{};
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.imageExtent = {1024, 4, 1};
            c->rt->CmdCopyBufferToImage(cmd, c->stage, c->t1,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            VkImageMemoryBarrier to_shader{};
            to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.image = c->t1;
            to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            c->api->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                                       0, nullptr, 1, &to_shader);
        };
        auto draw = [](VkCommandBuffer cmd, void *ud) {
            Ctx *c = static_cast<Ctx *>(ud);
            c->api->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
            VkDeviceSize off = 0;
            c->api->CmdBindVertexBuffers(cmd, 0, 1, &c->vb, &off);
            c->api->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl,
                                          1, 2, c->dsets, 0, nullptr);
            c->rt->CmdDraw(cmd, 3, 1, 0, 0);
        };
        if (!render_and_readback(label, dev, queue, api, rt, frame, rb, rbmem, draw, &ctx,
                                 pre, &ctx))
            break;
        if (!readback_pixels(dev, api, rbmem, out))
            break;
    } while (0);

    if (pipe) rt.DestroyPipeline(dev, pipe, nullptr);
    if (t1view) rt.DestroyImageView(dev, t1view, nullptr);
    if (t1) rt.DestroyImage(dev, t1, nullptr);
    if (t1mem) api.FreeMemory(dev, t1mem, nullptr);
    if (dpool) api.DestroyDescriptorPool(dev, dpool, nullptr);
    if (pl) api.DestroyPipelineLayout(dev, pl, nullptr);
    if (dsl_set2) api.DestroyDescriptorSetLayout(dev, dsl_set2, nullptr);
    if (dsl_set1) api.DestroyDescriptorSetLayout(dev, dsl_set1, nullptr);
    if (dsl_empty) api.DestroyDescriptorSetLayout(dev, dsl_empty, nullptr);
    if (fs) api.DestroyShaderModule(dev, fs, nullptr);
    if (vs) api.DestroyShaderModule(dev, vs, nullptr);
    if (t0view) rt.DestroyBufferView(dev, t0view, nullptr);
    if (stage) api.DestroyBuffer(dev, stage, nullptr);
    if (stagemem) api.FreeMemory(dev, stagemem, nullptr);
    if (t0buf) api.DestroyBuffer(dev, t0buf, nullptr);
    if (t0mem) api.FreeMemory(dev, t0mem, nullptr);
    if (ubo) api.DestroyBuffer(dev, ubo, nullptr);
    if (ubomem) api.FreeMemory(dev, ubomem, nullptr);
    if (rb) api.DestroyBuffer(dev, rb, nullptr);
    if (rbmem) api.FreeMemory(dev, rbmem, nullptr);
    if (vb) api.DestroyBuffer(dev, vb, nullptr);
    if (vbmem) api.FreeMemory(dev, vbmem, nullptr);
    frame.destroy(rt, api, dev);

    // Judgement.
    bool pass;
    if (dataset_zero) {
        // Zero dataset: FS must have run everywhere (no clear pixels) and
        // reproduce the zero signature on essentially every pixel.
        pass = (out->n_clear == 0 && out->n_zero >= Frame::W * Frame::H - 1024);
    } else {
        // Ramp dataset: ink everywhere; neither the clear nor the zero
        // signature may dominate.
        pass = (out->n_clear == 0 && out->n_other >= Frame::W * Frame::H - 1024);
    }
    std::printf("MODE=vk-d2d-replay ITEM=%s RESULT=%s\n", label, pass ? "PASS" : "FAIL");
    print_stats(label, *out);
    return pass;
}

int main(int argc, char **argv) {
    // The device ICD logs to stderr unbuffered and has abnormal-exit paths; keep
    // our own stdout unbuffered so diagnostics are never lost on early death.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char *dll = "C:\\panvk\\dev_vulkan_virtio.dll";
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0')
            dll = argv[i];
    }
    std::printf("probe=vk_d2d_replay\n");
    std::printf("icd_path=%s\n", dll);
    std::printf("d2d_vs_spv_len=%u d2d_fs_spv_len=%u sanity_vs_spv_len=%u sanity_fs_spv_len=%u "
                "sanity_novb_vs_spv_len=%u\n",
                d2d_text_vs_spv_len, d2d_text_fs_spv_len, sanity_vs_spv_len,
                sanity_fs_spv_len, sanity_novb_vs_spv_len);

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
    app.pApplicationName = "vk_d2d_replay";
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
        proc<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qf =
        proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceProperties get_props =
        proc<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkCreateDevice create_device = proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
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
    if (get_props) {
        VkPhysicalDeviceProperties props{};
        get_props(pd, &props);
        std::printf("physical_device=%s api=%u.%u.%u\n", props.deviceName,
                    VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion),
                    VK_API_VERSION_PATCH(props.apiVersion));
    }

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

    PixStats st_sanity{}, st_novb{}, st_clear{}, st_zero{}, st_ramp{};
    // Order matters: cheapest base controls first so an early death still
    // leaves the most diagnostic evidence in the log.
    bool novb = run_sanity_novb(dev, queue, api, rt, mp, &st_novb);
    bool upload_ok = run_vb_roundtrip(dev, queue, api, mp);
    bool sanity = run_sanity(dev, queue, api, rt, mp, &st_sanity);
    bool clear_ok = run_clear_only(dev, queue, api, rt, mp, &st_clear);
    bool zero_ok = run_replay("replay_zero", true, dev, queue, api, rt, mp, &st_zero);
    bool ramp_ok = run_replay("replay_ramp", false, dev, queue, api, rt, mp, &st_ramp);

    std::printf("MODE=vk-d2d-replay SUMMARY items=6 novb=%d upload=%d sanity=%d clear=%d "
                "zero=%d ramp=%d\n",
                novb ? 1 : 0, upload_ok ? 1 : 0, sanity ? 1 : 0, clear_ok ? 1 : 0,
                zero_ok ? 1 : 0, ramp_ok ? 1 : 0);

    int exit_code = 2;
    if (!novb) {
        std::printf("LAYER=HARNESS_SUSPECT (base no-upload draw paints nothing: ICD draw "
                    "path broken at a level below any upload question; last_step=%s "
                    "result=%d)\n", g_last_step, (int)g_last_result);
        std::printf("RESULT: SUSPECT\n");
    } else if (!upload_ok) {
        std::printf("LAYER=UPLOAD_BROKEN (draws themselves are fine per sanity_novb, but "
                    "CPU-written mapped memory is not seen by the GPU: the D2D replay "
                    "cannot be fed through this path; next step is a compute-filled "
                    "replay with zero CPU->GPU uploads)\n");
        std::printf("RESULT: SUSPECT\n");
    } else if (!sanity) {
        std::printf("LAYER=VERTEX_FETCH_BROKEN (uploads verified by vb_roundtrip, base "
                    "draw verified by sanity_novb, but a draw fed from a CPU-filled "
                    "vertex buffer paints nothing: PanVK/ICD vertex fetch suspect; the "
                    "D2D pair cannot be replayed with a host-filled vertex buffer)\n");
        std::printf("RESULT: SUSPECT\n");
    } else if (zero_ok && ramp_ok) {
        std::printf("LAYER=DXVK_MAPPING (real D2D FS executes correctly when fed directly; "
                    "text ink failure suspect sits in DXVK descriptor/pipeline mapping)\n");
        std::printf("RESULT: VERDICT\n");
        exit_code = 0;
    } else if (!zero_ok && !ramp_ok && st_zero.n_clear == Frame::W * Frame::H &&
               st_ramp.n_clear == Frame::W * Frame::H) {
        std::printf("LAYER=SPIRV_EXEC (both datasets render as pure clear; FS does not "
                    "execute as compiled - suspect SPIR-V compile or PanVK execution of the "
                    "texel-buffer fetch / bit-op chain)\n");
        std::printf("RESULT: VERDICT\n");
        exit_code = 0;
    } else {
        std::printf("LAYER=HARNESS_SUSPECT (replay output inconsistent; raw values above; "
                    "last_step=%s result=%d)\n", g_last_step, (int)g_last_result);
        std::printf("RESULT: SUSPECT\n");
    }

    api.DeviceWaitIdle(dev);
    api.DestroyDevice(dev, nullptr);
    return exit_code;
}
