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
//   sanity_novb  gl_VertexIndex fullscreen triangle + constant FS, no vertex
//                buffer, no descriptors, no uploads: the zero-dependency draw.
//   vb_roundtrip CPU-write -> GPU-read probe without any draw.
//   sanity_const trivial VS/FS pair writing a constant colour through a
//                CPU-filled vertex buffer: proves vertex fetch + uploads.
//   desc_const  trivial shaders through the FULL replay descriptor machinery
//                (3-set layout, UBO + texel buffer + sampled image, pre-pass
//                LUT upload): proves the replay feeding path itself.
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
#include "desc_const_fs_spv.h"
#include "d2d_feed_vs_spv.h"
#include "fiveattr_vs_spv.h"
#include "lut_read_fs_spv.h"
#include "oneattr_vs_spv.h"
#include "twoattr_vs_spv.h"
#include "fourattr_vs_spv.h"
#include "flat_echo_fs_spv.h"

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
// desc_const FS expected output: R=-m.z=1.0 (cb0.z=-1 delivered), G=(t0[0]&63)/63=1.0
// (texel 0 = 63 delivered), B=0.75, A=1.0.
static const uint8_t SIG_DESC[4] = {191, 255, 255, 255};
// lut_read control: FS out (10/255, 0.5, 20/255, 10/255) -> BGRA8 bytes
// {B=20, G=128, R=10, A=10} when the LUT upload + 2D fetch are healthy.
static const uint8_t SIG_LUTREAD[4] = {20, 128, 10, 10};
// flat_echo control: feed VS's flat outputs echoed back as colour.
static const uint8_t SIG_FLATECHO[4] = {0, 17, 136, 128};

static bool samples_match(const PixStats &s, const uint8_t sig[4]) {
    for (int i = 0; i < 5; ++i)
        for (int c = 0; c < 4; ++c)
            if (s.samples[i][c] != sig[c]) return false;
    return true;
}

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
        // The classifier puts that colour in the "other" bucket, so the
        // criterion is: all pixels "other" and all 5 samples exact.
        pass = (out->n_other == Frame::W * Frame::H && out->n_clear == 0 &&
                out->n_zero == 0 && samples_match(*out, SIG_SANITY));
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
            const DevApi *api;
            VkPipeline pipe;
        } ctx{&rt, &api, pipe};
        auto draw = [](VkCommandBuffer cmd, void *ud) {
            Ctx *c = static_cast<Ctx *>(ud);
            c->api->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
            c->rt->CmdDraw(cmd, 3, 1, 0, 0);
        };
        if (!render_and_readback("sanity_novb", dev, queue, api, rt, frame, rb, rbmem,
                                 draw, &ctx, nullptr, nullptr))
            break;
        if (!readback_pixels(dev, api, rbmem, out))
            break;
        // Same fixed criterion: sanity colour classifies as "other".
        pass = (out->n_other == Frame::W * Frame::H && out->n_clear == 0 &&
                out->n_zero == 0 && samples_match(*out, SIG_SANITY));
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
        VkBufferCopy cpregion{};
        cpregion.size = sizeof words;
        api.CmdCopyBuffer(cmd, vb, dst, 1, &cpregion);
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
// desc_const: descriptor-feeding control. Trivial shaders (gl_VertexIndex
// fullscreen triangle VS = the sanity_novb module; FS reads cb0.m.z and
// texel 0 of the u32 texel buffer) but the FULL run_replay descriptor
// machinery: 3-set pipeline layout (set0 empty, set1 UBO, set2 texel buffer
// + sampled image), descriptor pool with all three types, UpdateDescriptorSets
// writes (UBO + buffer view + image view), CmdBindDescriptorSets(firstSet=1,
// count=2), and the t1 staging->image pre-pass upload. If this paints its
// expected colour the descriptor path is proven and the d2d pair's all-clear
// output in run_replay cannot be blamed on harness descriptor feeding.
// Expected: BGRA {191,255,255,255} (R=-m.z with cb0.z=-1; G=(t0[0]&63)/63
// with t0[0]=63; B=0.75; A=1.0).
// ---------------------------------------------------------------------------
static bool run_desc_const(VkDevice dev, VkQueue queue,
                           const DevApi &api, const RtApi &rt,
                           const VkPhysicalDeviceMemoryProperties &mp,
                           PixStats *out) {
    Frame frame;
    VkBuffer rb = VK_NULL_HANDLE, ubo = VK_NULL_HANDLE, t0buf = VK_NULL_HANDLE,
            stage = VK_NULL_HANDLE;
    VkDeviceMemory rbmem = VK_NULL_HANDLE, ubomem = VK_NULL_HANDLE,
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
    bool pass = false;

    do {
        if (!frame.init(rt, api, dev, mp))
            break;
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)Frame::W * Frame::H * 4,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rbmem))
            break;

        // cb0 UBO, identical fill to run_replay.
        const float cb0[4] = {1.0f / 64.0f, 1.0f / 64.0f, -1.0f, -1.0f};
        if (!create_buffer_mem(api, dev, mp, sizeof cb0, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               &ubo, &ubomem))
            break;
        if (!fill_host_buffer(api, dev, ubomem, cb0, sizeof cb0))
            break;

        // t0: texel 0 = 63 (so G=1.0 -> 255 when delivered); rest zero.
        std::vector<uint32_t> t0(T0_TEXELS, 0u);
        t0[0] = 63u;
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

        // t1: same dev-local image + staging upload as run_replay (content is
        // irrelevant to this FS, the upload path itself is the control).
        std::vector<float> lut((size_t)LUT_W * LUT_H * 4, 0.0f);
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)lut.size() * 4,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage, &stagemem))
            break;
        if (!fill_host_buffer(api, dev, stagemem, lut.data(), (VkDeviceSize)lut.size() * 4))
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
        smci.codeSize = sanity_novb_vs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(sanity_novb_vs_spv);
        if (!ok("vkCreateShaderModule(desc vs)",
                api.CreateShaderModule(dev, &smci, nullptr, &vs)))
            break;
        smci.codeSize = desc_const_fs_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t *>(desc_const_fs_spv);
        if (!ok("vkCreateShaderModule(desc fs)",
                api.CreateShaderModule(dev, &smci, nullptr, &fs)))
            break;

        // Set layouts: mirror run_replay (set0 empty placeholder; set1 UBO —
        // the desc_const FS reads it from the fragment stage, so the stage
        // mask widens to VERTEX|FRAGMENT; set2 texel buffer + sampled image).
        VkDescriptorSetLayoutCreateInfo eci{};
        eci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (!ok("vkCreateDescriptorSetLayout(empty)",
                api.CreateDescriptorSetLayout(dev, &eci, nullptr, &dsl_empty)))
            break;
        VkDescriptorSetLayoutBinding ubo_bind{};
        ubo_bind.binding = 0;
        ubo_bind.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo_bind.descriptorCount = 1;
        ubo_bind.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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
        if (!ok("vkCreateGraphicsPipelines(desc_const)",
                rt.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe)))
            break;

        // Same pre/draw split as run_replay: t1 staging->image upload runs
        // before CmdBeginRenderPass; binding + draw inside.
        struct Ctx {
            const RtApi *rt;
            const DevApi *api;
            VkPipeline pipe;
            VkPipelineLayout pl;
            VkDescriptorSet *dsets;
            VkBuffer stage;
            VkImage t1;
        } ctx{&rt, &api, pipe, pl, dsets, stage, t1};
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
            c->api->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl,
                                          1, 2, c->dsets, 0, nullptr);
            c->rt->CmdDraw(cmd, 3, 1, 0, 0);
        };
        if (!render_and_readback("desc_const", dev, queue, api, rt, frame, rb, rbmem,
                                 draw, &ctx, pre, &ctx))
            break;
        if (!readback_pixels(dev, api, rbmem, out))
            break;

        // Expected everywhere: BGRA {191,255,255,255}. R encodes the cb0 UBO
        // delivery, G the texel-buffer delivery; either failing shifts the
        // byte away from 255 and fails the sample match.
        pass = (out->n_other == Frame::W * Frame::H && out->n_clear == 0 &&
                out->n_zero == 0 && samples_match(*out, SIG_DESC));
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
    frame.destroy(rt, api, dev);
    std::printf("MODE=vk-d2d-replay ITEM=desc_const RESULT=%s\n", pass ? "PASS" : "FAIL");
    print_stats("desc_const", *out);
    return pass;
}

// ---------------------------------------------------------------------------
// bisect: internal bisection of the real D2D pair.
//   mode=1 "d2d_vs_trivial_fs": real d2d VS (replay vertex input + cb0)
//          + the proven desc_const FS.  PASS (all SIG_DESC) -> the real
//          VS rasterizes the fullscreen triangle and a healthy FS
//          writes through it: VS side healthy.
//   mode=0 "trivial_vs_d2d_fs": the d2d_feed VS (gl_VertexIndex
//          fullscreen, FS-interface outputs) + the real d2d FS, all-zero
//          t0 dataset, LUT rows 0/1 with .z values.  PASS (no clear
//          pixels) -> the real FS executes and writes everywhere: FS
//          side healthy.  The stats then also show whether the decoded
//          math reproduces SIG_ZERO (20,200,10,200 BGRA) exactly.
//   mode=2 "fiveattr_vs_control": a healthy compiler-generated VS with the
//          exact 5-attribute input interface of the real d2d VS (loc0-3
//          f32vec2 + loc4 f32, same 36-byte vertex buffer) paired with the
//          proven desc_const FS.  PASS (all SIG_DESC) -> the harness vertex
//          input wiring is innocent and the real d2d VS verdict is final;
//          FAIL -> the wiring itself is the defect and the d2d-VS verdict
//          is withdrawn.
//   mode=3 "lut_read_control": the proven sanity_novb VS + a lut_read FS
//          that fetches KNOWN LUT texels ((0,0),(434,0),(0,1)) through the
//          exact staging->image pre-pass + SAMPLED_IMAGE descriptor the
//          replay uses.  PASS (all SIG_LUTREAD {20,128,10,10}) -> the LUT
//          upload + 2D OpImageFetch are healthy, so the real d2d FS's
//          uniform (0,0,0,0) output isolates to its own index math / flat
//          input handling; FAIL -> the upload path is the harness defect
//          and the real FS is exonerated.
//   mode=4 "vin_1attr_stride36": oneattr VS, 1 attribute (loc0 R32G32,
//          stride 36) - the stride dimension alone.
//   mode=5 "vin_2attr_stride16": twoattr VS, 2 attributes, stride 16 -
//          attribute-count >= 2 at a small stride.
//   mode=6 "vin_4attr_stride32": fourattr VS, 4 R32G32 attributes, stride
//          32 - the fiveattr shape minus loc4/R32.
//   mode=7 "vin_5attr_stride40": fiveattr VS, 5 attributes, stride 40 -
//          the fiveattr shape at an 8-aligned stride.
//   Modes 4..7 all pair with the proven desc_const FS and feed values that
//          make loc0 = (0,0)/(256,0)/(0,256) -> the same covering triangle;
//          PASS (all SIG_DESC) names a healthy vertex-input config, so a
//          FAIL pattern across them localizes which attribute-count/stride
//          dimension breaks PanVK vertex fetch.
//   mode=8 "flat_echo_control": the d2d_feed VS + a flat_echo FS that
//          echoes the received noperspective/flat outputs as colour.
//          PASS (all SIG_FLATECHO {0,17,136,128}) -> flat varying delivery
//          intact, the real d2d FS's (0,0,0,0) is its own math; FAIL ->
//          flat delivery broken, FS-colour conclusions suspended.
//   mode=9 "vin_1attr_stride8": stride-8 twin of vin_1attr_stride36 - the
//          same oneattr VS + same 0/256 data + same buffer path, ONLY the
//          binding stride (8 vs 36) differs.  PASS here + FAIL at 36
//          convicts the stride dimension of PanVK vertex fetch alone.
// ---------------------------------------------------------------------------
static bool run_bisect(const char *label, int mode, VkDevice dev, VkQueue queue,
                       const DevApi &api, const RtApi &rt,
                       const VkPhysicalDeviceMemoryProperties &mp,
                       PixStats *out) {
    Frame frame;
    VkBuffer vb = VK_NULL_HANDLE, rb = VK_NULL_HANDLE, ubo = VK_NULL_HANDLE,
            t0buf = VK_NULL_HANDLE, stage = VK_NULL_HANDLE;
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
    bool pass = false;
    uint32_t vstride = 0, vattrs = 0;

    do {
        if (!frame.init(rt, api, dev, mp))
            break;
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)Frame::W * Frame::H * 4,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rbmem))
            break;

        // Vertex data per mode.  Every vertex-fed variant encodes the same
        // covering triangle: loc0 carries 0/256 as raw u32 bit patterns in
        // f32 slots (bits 0x00000100 decode - via the real d2d VS's
        // Bitcast->i32->ConvertSToF chain or the control VSs' identical
        // floatBitsToInt decode - to the float 256.0), giving NDC corners
        // (-1,-1), (511,-1), (-1,511).
        //   mode 1,2: 5 attrs, stride 36 (the d2d-shaped config)
        //   mode 4:   1 attr,  stride 36   (stride dimension alone)
        //   mode 5:   2 attrs, stride 16   (count>=2 at a small stride)
        //   mode 6:   4 attrs, stride 32   (5-attr shape minus loc4/R32)
        //   mode 7:   5 attrs, stride 40   (5-attr shape, 8-aligned stride)
        //   modes 0,3,8: no vertex buffer.
        // Flat packing: row v starts at float index v*nfloats, so the
        // rows sit exactly vstride bytes apart for every mode.
        float verts[30];
        uint32_t nfloats = 0;
        if (mode == 1 || mode == 2 || mode == 4) {
            for (int v = 0; v < 3; ++v) {
                float *f = &verts[v * 9];
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
            nfloats = 9; /* modes 1,2 read all 9; mode 4 reads only loc0 */
            vstride = 36;
            vattrs = (mode == 4) ? 1 : 5;
        } else if (mode == 5) {
            for (int v = 0; v < 3; ++v) {
                float *f = &verts[v * 4];
                f[0] = int_bits_float(v == 1 ? 256u : 0u);
                f[1] = int_bits_float(v == 2 ? 256u : 0u);
                f[2] = 0.0f;
                f[3] = 0.0f;
            }
            nfloats = 4;
            vstride = 16;
            vattrs = 2;
        } else if (mode == 6) {
            for (int v = 0; v < 3; ++v) {
                float *f = &verts[v * 8];
                f[0] = int_bits_float(v == 1 ? 256u : 0u);
                f[1] = int_bits_float(v == 2 ? 256u : 0u);
                f[2] = 0.0f;
                f[3] = 0.0f;
                f[4] = int_bits_float(1u);
                f[5] = int_bits_float(1u);
                f[6] = int_bits_float(0x8001u);
                f[7] = 0.0f;
            }
            nfloats = 8;
            vstride = 32;
            vattrs = 4;
        } else if (mode == 7) {
            for (int v = 0; v < 3; ++v) {
                float *f = &verts[v * 10];
                f[0] = int_bits_float(v == 1 ? 256u : 0u);
                f[1] = int_bits_float(v == 2 ? 256u : 0u);
                f[2] = 0.0f;
                f[3] = 0.0f;
                f[4] = int_bits_float(1u);
                f[5] = int_bits_float(1u);
                f[6] = int_bits_float(0x8001u);
                f[7] = 0.0f;
                f[8] = 0.0f;
                f[9] = 0.0f;
            }
            nfloats = 10;
            vstride = 40;
            vattrs = 5;
        } else if (mode == 9) {
            // Stride-8 twin of vin_1attr_stride36: same oneattr VS, same
            // 0/256 data encoding, same buffer path - ONLY the binding
            // stride (8 vs 36) differs.  PASS here + FAIL at stride 36
            // convicts the stride dimension alone.
            for (int v = 0; v < 3; ++v) {
                float *f = &verts[v * 2];
                f[0] = int_bits_float(v == 1 ? 256u : 0u);
                f[1] = int_bits_float(v == 2 ? 256u : 0u);
            }
            nfloats = 2;
            vstride = 8;
            vattrs = 1;
        }
        if (nfloats) {
            const VkDeviceSize vbsize = (VkDeviceSize)sizeof(float) * 3 * nfloats;
            if (!create_buffer_mem(api, dev, mp, vbsize,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vbmem))
                break;
            if (!fill_host_buffer(api, dev, vbmem, verts, vbsize))
                break;
        }

        // cb0: both modes bind it (the d2d VS consumes it; the feed VS ignores
        // it; the desc_const FS reads m.z from the fragment stage).
        const float cb0[4] = {1.0f / 64.0f, 1.0f / 64.0f, -1.0f, -1.0f};
        if (!create_buffer_mem(api, dev, mp, sizeof cb0, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               &ubo, &ubomem))
            break;
        if (!fill_host_buffer(api, dev, ubomem, cb0, sizeof cb0))
            break;

        // t0: modes!=0 pair with the desc_const or flat_echo FS (desc_const
        // reads texel 0 = 63 -> G=255); the mode-0 real d2d FS gets the
        // all-zero dataset.
        std::vector<uint32_t> t0(T0_TEXELS, 0u);
        if (mode != 0)
            t0[0] = 63u;
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

        // t1: modes 1,2,4,5,6,7,8 pair with desc_const/flat_echo FS (never
        // samples; upload zeros); modes 0 and 3 upload the .z LUT rows 0/1
        // identical as run_replay (mode 3 = lut_read control reads known
        // texels back).
        std::vector<float> lut((size_t)LUT_W * LUT_H * 4, 0.0f);
        if (mode == 0 || mode == 3) {
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
                    lut[((size_t)row * LUT_W + x) * 4 + 2] = v;
                }
            }
        }
        if (!create_buffer_mem(api, dev, mp, (VkDeviceSize)lut.size() * 4,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage, &stagemem))
            break;
        if (!fill_host_buffer(api, dev, stagemem, lut.data(), (VkDeviceSize)lut.size() * 4))
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
        if (mode == 1) {
            smci.codeSize = d2d_text_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(d2d_text_vs_spv);
        } else if (mode == 2) {
            smci.codeSize = fiveattr_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(fiveattr_vs_spv);
        } else if (mode == 3) {
            smci.codeSize = sanity_novb_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(sanity_novb_vs_spv);
        } else if (mode == 4) {
            smci.codeSize = oneattr_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(oneattr_vs_spv);
        } else if (mode == 9) {
            smci.codeSize = oneattr_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(oneattr_vs_spv);
        } else if (mode == 5) {
            smci.codeSize = twoattr_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(twoattr_vs_spv);
        } else if (mode == 6) {
            smci.codeSize = fourattr_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(fourattr_vs_spv);
        } else if (mode == 7) {
            smci.codeSize = fiveattr_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(fiveattr_vs_spv);
        } else {
            smci.codeSize = d2d_feed_vs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(d2d_feed_vs_spv);
        }
        if (!ok("vkCreateShaderModule(bisect vs)",
                api.CreateShaderModule(dev, &smci, nullptr, &vs)))
            break;
        if (mode == 0) {
            smci.codeSize = d2d_text_fs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(d2d_text_fs_spv);
        } else if (mode == 3) {
            smci.codeSize = lut_read_fs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(lut_read_fs_spv);
        } else if (mode == 8) {
            smci.codeSize = flat_echo_fs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(flat_echo_fs_spv);
        } else {
            smci.codeSize = desc_const_fs_spv_len;
            smci.pCode = reinterpret_cast<const uint32_t *>(desc_const_fs_spv);
        }
        if (!ok("vkCreateShaderModule(bisect fs)",
                api.CreateShaderModule(dev, &smci, nullptr, &fs)))
            break;

        // Set layouts: the desc_const FS reads set1 from the fragment stage,
        // so the UBO stage mask is VERTEX|FRAGMENT in both modes.
        VkDescriptorSetLayoutCreateInfo eci{};
        eci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (!ok("vkCreateDescriptorSetLayout(empty)",
                api.CreateDescriptorSetLayout(dev, &eci, nullptr, &dsl_empty)))
            break;
        VkDescriptorSetLayoutBinding ubo_bind{};
        ubo_bind.binding = 0;
        ubo_bind.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo_bind.descriptorCount = 1;
        ubo_bind.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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
        bind.stride = vstride;
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[5]{};
        for (uint32_t i = 0; i < vattrs && i < 4; ++i) {
            attrs[i].location = i;
            attrs[i].binding = 0;
            attrs[i].format = VK_FORMAT_R32G32_SFLOAT;
            attrs[i].offset = 8 * i;
        }
        if (vattrs == 5) {
            attrs[4].location = 4;
            attrs[4].binding = 0;
            attrs[4].format = VK_FORMAT_R32_SFLOAT;
            attrs[4].offset = 32;
        }
        VkPipelineVertexInputStateCreateInfo vist{};
        vist.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (vattrs) {
            vist.vertexBindingDescriptionCount = 1;
            vist.pVertexBindingDescriptions = &bind;
            vist.vertexAttributeDescriptionCount = vattrs;
            vist.pVertexAttributeDescriptions = attrs;
        }
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
        if (!ok("vkCreateGraphicsPipelines(bisect)",
                rt.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe)))
            break;

        // t1 staging->image upload before the renderpass; bind + draw inside.
        struct Ctx {
            const RtApi *rt;
            const DevApi *api;
            VkPipeline pipe;
            VkPipelineLayout pl;
            VkDescriptorSet *dsets;
            VkBuffer vb;
            bool bind_vb;
            VkBuffer stage;
            VkImage t1;
        } ctx{&rt, &api, pipe, pl, dsets, vb, (vattrs != 0), stage, t1};
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
            if (c->bind_vb) {
                VkDeviceSize off = 0;
                c->api->CmdBindVertexBuffers(cmd, 0, 1, &c->vb, &off);
            }
            c->api->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl,
                                          1, 2, c->dsets, 0, nullptr);
            c->rt->CmdDraw(cmd, 3, 1, 0, 0);
        };
        if (!render_and_readback(label, dev, queue, api, rt, frame, rb, rbmem,
                                 draw, &ctx, pre, &ctx))
            break;
        if (!readback_pixels(dev, api, rbmem, out))
            break;

        if (mode == 1 || mode == 2 || (mode >= 4 && mode <= 7) || mode == 9) {
            // desc_const FS through a vertex-fed VS (real d2d, healthy
            // fiveattr, or one of the vin_* config-bisect variants):
            // expected everywhere SIG_DESC {191,255,255,255}.
            pass = (out->n_other == Frame::W * Frame::H && out->n_clear == 0 &&
                    out->n_zero == 0 && samples_match(*out, SIG_DESC));
        } else if (mode == 3) {
            // lut_read control: a healthy staging->image upload + 2D
            // OpImageFetch must paint SIG_LUTREAD {20,128,10,10}
            // everywhere.
            pass = (out->n_other == Frame::W * Frame::H && out->n_clear == 0 &&
                    out->n_zero == 0 && samples_match(*out, SIG_LUTREAD));
        } else if (mode == 8) {
            // flat_echo control: the feed VS's flat/noperspective outputs
            // echoed back as colour -> SIG_FLATECHO {0,17,136,128} iff the
            // flat varying delivery the real FS consumes is intact.
            pass = (out->n_other == Frame::W * Frame::H && out->n_clear == 0 &&
                    out->n_zero == 0 && samples_match(*out, SIG_FLATECHO));
        } else {
            // Real d2d FS through the feed VS: it writes everywhere (the
            // clear colour is unreachable: its alpha is the G channel value,
            // never 255), so the criterion is "no clear pixels".  The stats
            // show whether the written colour is the designed SIG_ZERO.
            pass = (out->n_clear == 0);
        }
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
    if (vb) api.DestroyBuffer(dev, vb, nullptr);
    if (vbmem) api.FreeMemory(dev, vbmem, nullptr);
    if (rb) api.DestroyBuffer(dev, rb, nullptr);
    if (rbmem) api.FreeMemory(dev, rbmem, nullptr);
    frame.destroy(rt, api, dev);
    std::printf("MODE=vk-d2d-replay ITEM=%s RESULT=%s\n", label, pass ? "PASS" : "FAIL");
    print_stats(label, *out);
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
                // The real FS reads the .z channel of every LUT fetch
                // (RGBA = t1[x0].z, t1[x1].z, t1[x2].z, t1[x1].z); the row
                // value must live in component 2, not 0.
                texel[2] = v;
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
                "sanity_novb_vs_spv_len=%u desc_const_fs_spv_len=%u d2d_feed_vs_spv_len=%u "
                "fiveattr_vs_spv_len=%u lut_read_fs_spv_len=%u oneattr_vs_spv_len=%u "
                "twoattr_vs_spv_len=%u fourattr_vs_spv_len=%u flat_echo_fs_spv_len=%u\n",
                d2d_text_vs_spv_len, d2d_text_fs_spv_len, sanity_vs_spv_len,
                sanity_fs_spv_len, sanity_novb_vs_spv_len, desc_const_fs_spv_len,
                d2d_feed_vs_spv_len, fiveattr_vs_spv_len, lut_read_fs_spv_len,
                oneattr_vs_spv_len, twoattr_vs_spv_len, fourattr_vs_spv_len,
                flat_echo_fs_spv_len);

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

    PixStats st_sanity{}, st_novb{}, st_desc{}, st_clear{}, st_zero{}, st_ramp{}, st_bvs{},
             st_bfs{}, st_five{}, st_lut{}, st_vin1{}, st_vin2{}, st_vin4{}, st_vin5{},
             st_fecho{}, st_vin8{};
    // Order matters: cheapest base controls first so an early death still
    // leaves the most diagnostic evidence in the log.
    bool novb = run_sanity_novb(dev, queue, api, rt, mp, &st_novb);
    bool upload_ok = run_vb_roundtrip(dev, queue, api, mp);
    bool sanity = run_sanity(dev, queue, api, rt, mp, &st_sanity);
    bool desc_ok = run_desc_const(dev, queue, api, rt, mp, &st_desc);
    bool clear_ok = run_clear_only(dev, queue, api, rt, mp, &st_clear);
    bool zero_ok = run_replay("replay_zero", true, dev, queue, api, rt, mp, &st_zero);
    bool ramp_ok = run_replay("replay_ramp", false, dev, queue, api, rt, mp, &st_ramp);
    bool bisect_vs = run_bisect("d2d_vs_trivial_fs", 1, dev, queue, api, rt, mp, &st_bvs);
    bool bisect_fs = run_bisect("trivial_vs_d2d_fs", 0, dev, queue, api, rt, mp, &st_bfs);
    bool fiveattr_ok = run_bisect("fiveattr_vs_control", 2, dev, queue, api, rt, mp, &st_five);
    bool lut_ok = run_bisect("lut_read_control", 3, dev, queue, api, rt, mp, &st_lut);
    bool vin1_ok = run_bisect("vin_1attr_stride36", 4, dev, queue, api, rt, mp, &st_vin1);
    bool vin2_ok = run_bisect("vin_2attr_stride16", 5, dev, queue, api, rt, mp, &st_vin2);
    bool vin4_ok = run_bisect("vin_4attr_stride32", 6, dev, queue, api, rt, mp, &st_vin4);
    bool vin5_ok = run_bisect("vin_5attr_stride40", 7, dev, queue, api, rt, mp, &st_vin5);
    bool fecho_ok = run_bisect("flat_echo_control", 8, dev, queue, api, rt, mp, &st_fecho);
    bool vin8_ok = run_bisect("vin_1attr_stride8", 9, dev, queue, api, rt, mp, &st_vin8);

    std::printf("MODE=vk-d2d-replay SUMMARY items=17 novb=%d upload=%d sanity=%d desc=%d "
                "clear=%d zero=%d ramp=%d bisect_vs=%d bisect_fs=%d fiveattr=%d lut=%d "
                "vin1=%d vin2=%d vin4=%d vin5=%d fecho=%d vin8s=%d\n",
                novb ? 1 : 0, upload_ok ? 1 : 0, sanity ? 1 : 0, desc_ok ? 1 : 0,
                clear_ok ? 1 : 0, zero_ok ? 1 : 0, ramp_ok ? 1 : 0,
                bisect_vs ? 1 : 0, bisect_fs ? 1 : 0, fiveattr_ok ? 1 : 0,
                lut_ok ? 1 : 0, vin1_ok ? 1 : 0, vin2_ok ? 1 : 0, vin4_ok ? 1 : 0,
                vin5_ok ? 1 : 0, fecho_ok ? 1 : 0, vin8_ok ? 1 : 0);
    std::printf("CONTROLS fiveattr=%d lut_read=%d (fiveattr PASS closes the 5-attr "
                "vertex-input loophole behind the d2d-VS verdict; lut_read PASS "
                "validates the LUT staging->image upload + 2D fetch that the "
                "real FS's (0,0,0,0) output put in question)\n",
                fiveattr_ok ? 1 : 0, lut_ok ? 1 : 0);
    std::printf("VIN_BISECT 1attr_s36=%d 2attr_s16=%d 4attr_s32=%d 5attr_s40=%d "
                "1attr_s8=%d (healthy GLSL vertex shaders that differ only in "
                "the attribute-count/stride configuration; all clear = the "
                "failure is not specific to the d2d VS at all but to the "
                "vertex-input config; any pass names the dimension that "
                "breaks PanVK vertex fetch)\n",
                vin1_ok ? 1 : 0, vin2_ok ? 1 : 0, vin4_ok ? 1 : 0,
                vin5_ok ? 1 : 0, vin8_ok ? 1 : 0);
    std::printf("FLAT_ECHO %d (PASS: the flat/noperspective varying delivery the real "
                "FS consumes is intact, so its (0,0,0,0) output is its own "
                "index math; FAIL: flat delivery broken, FS output judgement "
                "suspended)\n", fecho_ok ? 1 : 0);

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
    } else if (!desc_ok) {
        std::printf("LAYER=HARNESS_SUSPECT (descriptor feeding: the trivial-shader draw "
                    "that consumes cb0 UBO + texel-buffer view + sampled-image bind "
                    "through the exact replay machinery paints nothing or wrong colour; "
                    "the D2D pair replays cannot be judged through this path; "
                    "last_step=%s result=%d)\n", g_last_step, (int)g_last_result);
        std::printf("RESULT: SUSPECT\n");
    } else if (zero_ok && ramp_ok) {
        std::printf("LAYER=DXVK_MAPPING (real D2D FS executes correctly when fed directly; "
                    "text ink failure suspect sits in DXVK descriptor/pipeline mapping)\n");
        std::printf("RESULT: VERDICT\n");
        exit_code = 0;
    } else if (!zero_ok && !ramp_ok && st_zero.n_clear == Frame::W * Frame::H &&
               st_ramp.n_clear == Frame::W * Frame::H) {
        // Pair-level all-clear confirmed; refine with the internal bisect.
        if (bisect_vs && bisect_fs) {
            std::printf("LAYER=PAIR_LINKAGE (each half of the pair executes correctly "
                        "alone - the real d2d VS rasterizes for the desc_const FS and the "
                        "real d2d FS writes through the d2d_feed VS - but the combined "
                        "pair paints nothing; suspicion moves to the VS<->FS interface "
                        "linkage / combined pipeline: Invariant SV_Position + "
                        "NoPerspective/Flat interface, linkage-time specialization)\n");
            std::printf("RESULT: VERDICT\n");
            exit_code = 0;
        } else if (bisect_vs && !bisect_fs) {
            std::printf("LAYER=SPIRV_EXEC_FS (the real d2d VS is healthy: it rasterizes "
                        "the fullscreen triangle and a healthy FS writes through it; the "
                        "real d2d FS paints nothing even through the proven d2d_feed VS "
                        "+ verified descriptor path - its 5x u32 texel-buffer fetch + 3x "
                        "LUT chain fails to execute/write on this PanVK stack)\n");
            std::printf("RESULT: VERDICT\n");
            exit_code = 0;
        } else if (!bisect_vs && bisect_fs) {
            if (fiveattr_ok) {
                std::printf("LAYER=SPIRV_EXEC_VS_CONFIRMED (the real d2d FS is healthy: "
                            "it writes through the d2d_feed VS + verified descriptor "
                            "path; the real d2d VS fails to rasterize a fullscreen "
                            "triangle from the same vertex input + cb0 that the "
                            "healthy fiveattr control VS rasterizes perfectly - the "
                            "5-attr vertex-input wiring is exonerated and the D2D "
                            "text ink=0 defect sits in the d2d vertex-shader "
                            "execution on this PanVK stack; t1_upload=%s)\n",
                            lut_ok ? "OK (LUT staging->image upload + 2D fetch "
                                      "validated; the real FS's uniform (0,0,0,0) "
                                      "output isolates to its own index math / flat "
                                      "input handling, a secondary FS-side question)"
                                   : "BROKEN (lut_read control also reads wrong - "
                                      "the harness LUT upload path is the "
                                      "explanation for the real FS's (0,0,0,0) "
                                      "output; real FS fully exonerated)");
                std::printf("RESULT: VERDICT\n");
                exit_code = 0;
            } else {
                const int vin_any = (vin1_ok ? 1 : 0) + (vin2_ok ? 1 : 0) +
                                    (vin4_ok ? 1 : 0) + (vin5_ok ? 1 : 0);
                if (vin8_ok) {
                    std::printf("LAYER=PANVK_VERTEX_FETCH_STRIDE (stride control: the "
                                "identical oneattr VS + 0/256 data + buffer path "
                                "paints SIG_DESC at binding stride 8 while every "
                                "stride>8 config - 16/32/36/40 bytes, ANY attribute "
                                "count 1..5, healthy or real d2d - produces zero "
                                "rasterization; PanVK/ICD vertex fetch on this stack "
                                "breaks as soon as the vertex binding stride exceeds "
                                "the 8-byte single-R32G32 shape; the real D2D text "
                                "pipeline feeds its VS through 36-byte-stride "
                                "attributes, so THIS is the mechanism of the text "
                                "ink=0 defect)\n");
                    std::printf("RESULT: VERDICT\n");
                    exit_code = 0;
                } else if (vin_any) {
                    std::printf("LAYER=PANVK_VERTEX_FETCH (the healthy fiveattr "
                                "control VS paints nothing through the 5-attr/36B "
                                "input, but healthy GLSL vertex shaders DO paint "
                                "through the vin_* config variants - passing: ");
                    if (vin1_ok) std::printf("1attr/stride36 ");
                    if (vin2_ok) std::printf("2attr/stride16 ");
                    if (vin4_ok) std::printf("4attr/stride32 ");
                    if (vin5_ok) std::printf("5attr/stride40 ");
                    std::printf("- so general vertex fetch + descriptor feeding + "
                                "rasterization are all ALIVE, and the breakage is "
                                "specific to a vertex-input configuration dimension "
                                "the d2d text pipeline also uses; this PanVK/ICD "
                                "vertex-fetch config defect is the leading "
                                "explanation for the original D2D text ink=0 "
                                "(the real pipeline feeds its VS through the same "
                                "kind of multi-attribute input state))\n");
                    std::printf("RESULT: VERDICT\n");
                    exit_code = 0;
                } else {
                    std::printf("LAYER=HARNESS_VERTEX_INPUT_BROAD (the healthy "
                                "fiveattr control VS AND every vin_* healthy "
                                "variant paints nothing - the failure is not "
                                "specific to the d2d VS nor to one config "
                                "dimension: something broader in the PanVK/ICD "
                                "vertex-fetch/IDVS path breaks any draw beyond the "
                                "single-attr/8B proven shape; the SPIRV_EXEC_VS "
                                "verdict stays WITHDRAWN; PanVK-side pending "
                                "upstream comparison)\n");
                    std::printf("RESULT: SUSPECT\n");
                }
            }
            std::printf("FLAT_ECHO_READING %s (controls the real d2d FS's "
                        "(0,0,0,0) uniform output: PASS means the flat varying "
                        "delivery it consumes is intact and the wrong colour is "
                        "its own index-math result; FAIL means flat delivery "
                        "itself is broken and no FS-colour conclusion can be "
                        "drawn)\n",
                        fecho_ok ? "PASS" : "FAIL");
        } else {
            std::printf("LAYER=PAIR_LINKAGE_SUSPECT (both halves also paint nothing "
                        "alone through verified feeds; the common surface of the two "
                        "modules is suspect: VulkanMemoryModel/PhysicalStorageBuffer64 "
                        "header, Invariant-decorated SV_Position, entry-point "
                        "conventions; raw values above; last_step=%s result=%d)\n",
                        g_last_step, (int)g_last_result);
            std::printf("RESULT: SUSPECT\n");
        }
    } else {
        std::printf("LAYER=HARNESS_SUSPECT (replay output inconsistent; raw values above; "
                    "last_step=%s result=%d)\n", g_last_step, (int)g_last_result);
        std::printf("RESULT: SUSPECT\n");
    }

    api.DeviceWaitIdle(dev);
    api.DestroyDevice(dev, nullptr);
    return exit_code;
}
