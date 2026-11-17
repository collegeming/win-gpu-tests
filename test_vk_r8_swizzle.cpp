// Independent offscreen probe; reuse the existing loader, VkResult logging and memory helpers.
#define main legacy_compute_main
#include "test_vk_compute.cpp"
#undef main
#include <cmath>
#include <limits>
#include "r8_swizzle_spirv.h"

static constexpr unsigned char texels[] = {0, 64, 128, 255};
static constexpr float tolerance = 1e-5f;
static float expected(unsigned view, unsigned texel, unsigned channel) {
    const float t = float(texels[texel]) / 255.0f;
    return view ? (channel == 3 ? t : 0.0f) :
                  (channel == 0 ? t : (channel == 3 ? 1.0f : 0.0f));
}
static bool matches(float actual, float reference) {
    return std::isfinite(actual) && std::fabs(actual - reference) <= tolerance;
}
static bool reference_self_test() {
    // Independently specified reference values, not produced by expected().
    const float identity[4][4] = {{0,0,0,1}, {64.0f/255,0,0,1},
                                 {128.0f/255,0,0,1}, {1,0,0,1}};
    const float mapped[4][4] = {{0,0,0,0}, {0,0,0,64.0f/255},
                               {0,0,0,128.0f/255}, {0,0,0,1}};
    bool pass = true;
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned c = 0; c < 4; ++c)
            pass &= matches(expected(0,i,c), identity[i][c]) &&
                    matches(expected(1,i,c), mapped[i][c]);
    pass &= !matches(std::numeric_limits<float>::quiet_NaN(), 0) &&
            !matches(std::numeric_limits<float>::infinity(), 0) &&
            !matches(0, 1) && !matches(1.0f/255, 0) &&
            matches(0.5f*tolerance, 0) && !matches(2*tolerance, 0);
    std::printf("CPU_REFERENCE_SELF_TEST=%s tolerance=%.9g finite_required=1\n",
                pass ? "PASS" : "FAIL", tolerance);
    return pass;
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !std::strcmp(argv[1], "--self-test"))
        return reference_self_test() ? 0 : 1;
    if (argc != 3 || std::strcmp(argv[1], "--icd") ||
        !((std::strlen(argv[2]) >= 3 && argv[2][1] == ':' &&
           (argv[2][2] == '\\' || argv[2][2] == '/')) ||
          (argv[2][0] == '\\' && argv[2][1] == '\\'))) {
        std::printf("Usage: vk_r8_swizzle_arm64.exe --self-test | --icd ABSOLUTE_DLL_PATH\nRESULT: FAIL\n");
        return 2;
    }
    if (!reference_self_test()) return 1;
    std::printf("probe=r8_view_swizzle no_window=1 no_attachment=1 no_fallback=1\n"
                "format=R8_UNORM texels=0,64,128,255 nearest=1 lod=0 blend=none srgb=none\n"
                "icd_path=%s\n", argv[2]);
    HMODULE module = LoadLibraryA(argv[2]);
    if (!module) {
        std::printf("LoadLibraryA error=%lu\nRESULT: FAIL\n", GetLastError());
        return 1;
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    std::printf("icd_module=%s\n", path);
    g_icd_gipa = reinterpret_cast<PFN_icdGetInstanceProcAddr_t>(GetProcAddress(module, "vk_icdGetInstanceProcAddr"));
    auto create_instance = proc<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance) { std::printf("missing vkCreateInstance\nRESULT: FAIL\n"); return 1; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "r8_view_swizzle";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!ok("vkCreateInstance(1.0)", create_instance(&ici, nullptr, &instance))) {
        std::printf("RESULT: FAIL\n"); return 1;
    }
#define INSTANCE_FN(name) auto name = proc<PFN_vk##name>(instance, "vk" #name); if (!name) { std::printf("missing vk%s\nRESULT: FAIL\n", #name); return 1; }
    INSTANCE_FN(DestroyInstance);
    INSTANCE_FN(EnumeratePhysicalDevices);
    INSTANCE_FN(GetPhysicalDeviceProperties);
    INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_FN(GetPhysicalDeviceMemoryProperties);
    INSTANCE_FN(GetPhysicalDeviceFormatProperties);
    INSTANCE_FN(GetPhysicalDeviceImageFormatProperties);
    INSTANCE_FN(CreateDevice);
    INSTANCE_FN(GetDeviceProcAddr);
#undef INSTANCE_FN
    uint32_t count = 0;
    if (!ok("vkEnumeratePhysicalDevices(count)", EnumeratePhysicalDevices(instance, &count, nullptr)) || !count) {
        std::printf("no device\nRESULT: FAIL\n"); return 1;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (!ok("vkEnumeratePhysicalDevices(list)", EnumeratePhysicalDevices(instance, &count, devices.data())) || !count) {
        std::printf("RESULT: FAIL\n"); return 1;
    }
    // No adapter retry: record and test the first device exposed by the explicit ICD.
    VkPhysicalDevice pd = devices[0];
    VkPhysicalDeviceProperties props{};
    GetPhysicalDeviceProperties(pd, &props);
    std::printf("device.index=0 device.name=%s device.type=%u vendor=0x%x device=0x%x api=0x%x driver=0x%x\n",
                props.deviceName, props.deviceType, props.vendorID, props.deviceID, props.apiVersion, props.driverVersion);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { std::printf("CPU adapter refused\nRESULT: FAIL\n"); return 1; }
    VkFormatProperties fp{};
    GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8_UNORM, &fp);
    std::printf("R8_UNORM linear=0x%x optimal=0x%x buffer=0x%x\n", fp.linearTilingFeatures, fp.optimalTilingFeatures, fp.bufferFeatures);
    VkImageFormatProperties ifp{};
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!ok("vkGetPhysicalDeviceImageFormatProperties", GetPhysicalDeviceImageFormatProperties(pd,
            VK_FORMAT_R8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &ifp)) ||
        !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        ifp.maxExtent.width < 4 || ifp.maxExtent.height < 1 || !(ifp.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
        std::printf("R8 sampled/transfer format unsupported\nRESULT: FAIL\n"); return 1;
    }
    std::printf("image_support maxExtent=%u,%u,%u mipLevels=%u layers=%u samples=0x%x\n",
                ifp.maxExtent.width, ifp.maxExtent.height, ifp.maxExtent.depth, ifp.maxMipLevels, ifp.maxArrayLayers, ifp.sampleCounts);
    GetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    GetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());
    uint32_t family = 0;
    while (family < count && (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))) ++family;
    if (family == count) { std::printf("no compute queue\nRESULT: FAIL\n"); return 1; }
    std::printf("queue_family=%u flags=0x%x (compute queues support transfer commands)\n", family, families[family].queueFlags);
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    if (!ok("vkCreateDevice", CreateDevice(pd, &dci, nullptr, &dev))) { std::printf("RESULT: FAIL\n"); return 1; }
    DevApi api{};
    if (load_dev_api(api, GetDeviceProcAddr, dev, instance)) { std::printf("RESULT: FAIL\n"); return 1; }
#define DEVICE_FN(name) auto name = dev_proc<PFN_vk##name>(GetDeviceProcAddr, dev, instance, "vk" #name); if (!name) { std::printf("missing vk%s\nRESULT: FAIL\n", #name); return 1; }
    DEVICE_FN(CreateImage); DEVICE_FN(DestroyImage); DEVICE_FN(GetImageMemoryRequirements);
    DEVICE_FN(BindImageMemory); DEVICE_FN(CreateImageView); DEVICE_FN(DestroyImageView);
    DEVICE_FN(CreateSampler); DEVICE_FN(DestroySampler); DEVICE_FN(CmdCopyBufferToImage);
#undef DEVICE_FN
    VkPhysicalDeviceMemoryProperties mp{}; GetPhysicalDeviceMemoryProperties(pd, &mp);
    VkQueue queue = VK_NULL_HANDLE; api.GetDeviceQueue(dev, family, 0, &queue);
    VkBuffer upload = VK_NULL_HANDLE, output = VK_NULL_HANDLE;
    VkDeviceMemory upload_mem = VK_NULL_HANDLE, output_mem = VK_NULL_HANDLE, image_mem = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView views[2]{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    const bool pass = [&]() -> bool {
#define CHECK(step, call) if (!ok(step, call)) return false
        uint32_t upload_type = 0, output_type = 0, image_type = 0;
        if (!create_buffer_mem(api, dev, mp, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &upload, &upload_mem, &upload_type) ||
            !create_buffer_mem(api, dev, mp, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &output, &output_mem, &output_type)) return false;
        std::printf("memory upload_type=%u flags=0x%x output_type=%u flags=0x%x\n", upload_type,
                    mp.memoryTypes[upload_type].propertyFlags, output_type, mp.memoryTypes[output_type].propertyFlags);
        // Map whole allocations so whole-range flush/invalidate is legal, including noncoherent tails.
        for (unsigned i = 0; i < 2; ++i) {
            VkDeviceMemory memory = i ? output_mem : upload_mem;
            void *map = nullptr;
            CHECK("vkMapMemory(init,whole)", api.MapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &map));
            if (i) for (unsigned n = 0; n < 32; ++n) static_cast<float *>(map)[n] = std::numeric_limits<float>::quiet_NaN();
            else std::memcpy(map, texels, 4);
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = memory; range.size = VK_WHOLE_SIZE;
            bool flushed = ok("vkFlushMappedMemoryRanges(init)", api.FlushMappedMemoryRanges(dev, 1, &range));
            api.UnmapMemory(dev, memory);
            if (!flushed) return false;
        }
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D; image_info.format = VK_FORMAT_R8_UNORM;
        image_info.extent = {4,1,1}; image_info.mipLevels = 1; image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT; image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage; image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CHECK("vkCreateImage", CreateImage(dev, &image_info, nullptr, &image));
        VkMemoryRequirements req{}; GetImageMemoryRequirements(dev, image, &req);
        if (!find_memory_type(mp, req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image_type)) return false;
        std::printf("image_memory_type=%u flags=0x%x\n", image_type, mp.memoryTypes[image_type].propertyFlags);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize = req.size; alloc.memoryTypeIndex = image_type;
        CHECK("vkAllocateMemory(image)", api.AllocateMemory(dev, &alloc, nullptr, &image_mem));
        CHECK("vkBindImageMemory", BindImageMemory(dev, image, image_mem, 0));
        for (unsigned i = 0; i < 2; ++i) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = VK_FORMAT_R8_UNORM;
            if (i) view.components = {VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            CHECK(i ? "vkCreateImageView(000R)" : "vkCreateImageView(identity)", CreateImageView(dev, &view, nullptr, &views[i]));
        }
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        CHECK("vkCreateSampler(nearest,lod0)", CreateSampler(dev, &sci, nullptr, &sampler));
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            bindings[i].binding = i; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; lci.bindingCount = 3; lci.pBindings = bindings;
        CHECK("vkCreateDescriptorSetLayout", api.CreateDescriptorSetLayout(dev, &lci, nullptr, &layout));
        VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pci.setLayoutCount = 1; pci.pSetLayouts = &layout;
        CHECK("vkCreatePipelineLayout", api.CreatePipelineLayout(dev, &pci, nullptr, &pipeline_layout));
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = sizes;
        CHECK("vkCreateDescriptorPool", api.CreateDescriptorPool(dev, &dpci, nullptr, &pool));
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        CHECK("vkAllocateDescriptorSets", api.AllocateDescriptorSets(dev, &dsai, &set));
        VkDescriptorImageInfo images[] = {{sampler,views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkDescriptorBufferInfo buffer{output,0,128};
        VkWriteDescriptorSet writes[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set; writes[i].dstBinding = i;
            writes[i].descriptorCount = 1; writes[i].descriptorType = bindings[i].descriptorType;
            if (i < 2) writes[i].pImageInfo = &images[i]; else writes[i].pBufferInfo = &buffer;
        }
        api.UpdateDescriptorSets(dev, 3, writes, 0, nullptr);
        std::vector<uint32_t> spirv(r8_swizzle_spirv_len/4);
        if (!r8_swizzle_spirv_len || r8_swizzle_spirv_len % 4) return false;
        std::memcpy(spirv.data(), r8_swizzle_spirv, r8_swizzle_spirv_len);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smci.codeSize = r8_swizzle_spirv_len; smci.pCode = spirv.data();
        CHECK("vkCreateShaderModule", api.CreateShaderModule(dev, &smci, nullptr, &shader));
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = shader; cpci.stage.pName = "main"; cpci.layout = pipeline_layout; cpci.basePipelineIndex = -1;
        CHECK("vkCreateComputePipelines", api.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex = family;
        CHECK("vkCreateCommandPool", api.CreateCommandPool(dev, &cpi, nullptr, &commands));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = commands; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        CHECK("vkAllocateCommandBuffers", api.AllocateCommandBuffers(dev, &cai, &cmd));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK("vkBeginCommandBuffer", api.BeginCommandBuffer(cmd, &begin));
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; host.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {4,1,1};
        CmdCopyBufferToImage(cmd, upload, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
        api.CmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; readback.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; readback.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &readback, 0, nullptr, 0, nullptr);
        CHECK("vkEndCommandBuffer", api.EndCommandBuffer(cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CHECK("vkCreateFence", api.CreateFence(dev, &fci, nullptr, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        CHECK("vkQueueSubmit", api.QueueSubmit(queue, 1, &submit, fence)); pending = true;
        CHECK("vkWaitForFences(10s)", api.WaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull)); pending = false;
        void *map = nullptr;
        CHECK("vkMapMemory(readback,whole)", api.MapMemory(dev, output_mem, 0, VK_WHOLE_SIZE, 0, &map));
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = output_mem; range.size = VK_WHOLE_SIZE;
        const bool invalidated = ok("vkInvalidateMappedMemoryRanges(readback)", api.InvalidateMappedMemoryRanges(dev, 1, &range));
        bool verified = invalidated;
        if (invalidated) for (unsigned v = 0; v < 2; ++v) for (unsigned i = 0; i < 4; ++i) {
            const float *actual = static_cast<const float *>(map) + (v*4+i)*4;
            std::printf("view=%s texel=%u actual=(%.9g,%.9g,%.9g,%.9g) expected=(%.9g,%.9g,%.9g,%.9g)\n",
                        v ? "000R" : "identity", texels[i], actual[0],actual[1],actual[2],actual[3],
                        expected(v,i,0),expected(v,i,1),expected(v,i,2),expected(v,i,3));
            for (unsigned c = 0; c < 4; ++c) verified &= matches(actual[c], expected(v,i,c));
        }
        api.UnmapMemory(dev, output_mem);
        return verified;
#undef CHECK
    }();
    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    // Do not destroy in-flight resources or wait idle indefinitely after a fence failure.
    // The device owner must also supervise the process with a hard external deadline.
    if (pending) { std::printf("inflight=1 exiting without Vulkan cleanup\n"); ExitProcess(1); }
    if (fence) api.DestroyFence(dev, fence, nullptr);
    if (commands) api.DestroyCommandPool(dev, commands, nullptr);
    if (pipeline) api.DestroyPipeline(dev, pipeline, nullptr);
    if (shader) api.DestroyShaderModule(dev, shader, nullptr);
    if (pool) api.DestroyDescriptorPool(dev, pool, nullptr);
    if (pipeline_layout) api.DestroyPipelineLayout(dev, pipeline_layout, nullptr);
    if (layout) api.DestroyDescriptorSetLayout(dev, layout, nullptr);
    if (sampler) DestroySampler(dev, sampler, nullptr);
    for (auto view : views) if (view) DestroyImageView(dev, view, nullptr);
    if (image) DestroyImage(dev, image, nullptr);
    if (upload) api.DestroyBuffer(dev, upload, nullptr);
    if (output) api.DestroyBuffer(dev, output, nullptr);
    if (image_mem) api.FreeMemory(dev, image_mem, nullptr);
    if (upload_mem) api.FreeMemory(dev, upload_mem, nullptr);
    if (output_mem) api.FreeMemory(dev, output_mem, nullptr);
    api.DestroyDevice(dev, nullptr); DestroyInstance(instance, nullptr);
    return pass ? 0 : 1;
}
