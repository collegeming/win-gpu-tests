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

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char *dll = "C:\\panvk\\dev_vulkan_virtio.dll";
    bool nop_mode = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--nop") || !std::strcmp(argv[i], "--shader=nop"))
            nop_mode = true;
        else if (argv[i][0] != '-' && argv[i][0] != '\0')
            dll = argv[i];
    }
    // --nop runs an empty kernel with no descriptors, to separate "compute
    // pipelines cannot be created at all" from "this storage-buffer shader
    // cannot be compiled".
    const unsigned char *spirv_blob = nop_mode ? compute_nop_spirv : compute_spirv;
    const unsigned int spirv_bytes = nop_mode ? compute_nop_spirv_len : compute_spirv_len;

    std::printf("probe=vk_compute\n");
    std::printf("mode=%s\n", nop_mode ? "nop" : "ssbo");
    std::printf("icd_path=%s\n", dll);
    {
        unsigned magic = 0;
        if (spirv_bytes >= 4) {
            magic = (unsigned)spirv_blob[0] | ((unsigned)spirv_blob[1] << 8) |
                    ((unsigned)spirv_blob[2] << 16) | ((unsigned)spirv_blob[3] << 24);
        }
        std::printf("spirv_bytes=%u spirv_magic=0x%08x\n", spirv_bytes, magic);
    }
    if (spirv_bytes == 0 || (spirv_bytes % 4) != 0) {
        std::printf("ERROR: embedded SPIR-V blob is empty or not word aligned\n");
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    HMODULE mod = LoadLibraryA(dll);
    if (!mod) {
        std::printf("ERROR: LoadLibraryA failed err=%lu\n", (unsigned long)GetLastError());
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    char module_path[MAX_PATH] = {0};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::printf("icd_module=%s\n", module_path);

    g_icd_gipa = reinterpret_cast<PFN_icdGetInstanceProcAddr_t>(
        GetProcAddress(mod, "vk_icdGetInstanceProcAddr"));
    if (!g_icd_gipa) {
        std::printf("ERROR: %s does not export vk_icdGetInstanceProcAddr\n", dll);
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    PFN_vkCreateInstance create_instance = proc<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_inst_ext =
        proc<PFN_vkEnumerateInstanceExtensionProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!create_instance) {
        std::printf("ERROR: vkCreateInstance not resolvable\n");
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    bool have_props2 = false;
    bool have_debug_utils = false;
    if (enum_inst_ext) {
        uint32_t count = 0;
        if (enum_inst_ext(nullptr, &count, nullptr) == VK_SUCCESS && count) {
            std::vector<VkExtensionProperties> list(count);
            VkResult er = enum_inst_ext(nullptr, &count, list.data());
            if (er == VK_SUCCESS || er == VK_INCOMPLETE) {
                list.resize(count);
                for (const auto &e : list) {
                    if (!std::strcmp(e.extensionName, "VK_KHR_get_physical_device_properties2"))
                        have_props2 = true;
                    if (!std::strcmp(e.extensionName, "VK_EXT_debug_utils"))
                        have_debug_utils = true;
                }
            }
        }
    }
    std::printf("instance_ext.VK_KHR_get_physical_device_properties2=%u\n", have_props2 ? 1u : 0u);
    std::printf("instance_ext.VK_EXT_debug_utils=%u\n", have_debug_utils ? 1u : 0u);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vk_compute";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;

    const char *enabled_exts[2] = {nullptr, nullptr};
    uint32_t enabled_ext_count = 0;
    if (have_props2)
        enabled_exts[enabled_ext_count++] = "VK_KHR_get_physical_device_properties2";
    if (have_debug_utils)
        enabled_exts[enabled_ext_count++] = "VK_EXT_debug_utils";

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = enabled_ext_count;
    ici.ppEnabledExtensionNames = enabled_ext_count ? enabled_exts : nullptr;

    const uint32_t try_apis[] = {
        VK_MAKE_API_VERSION(0, 1, 4, 0),
        VK_MAKE_API_VERSION(0, 1, 3, 0),
        VK_MAKE_API_VERSION(0, 1, 1, 0),
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t api : try_apis) {
        app.apiVersion = api;
        r = create_instance(&ici, nullptr, &instance);
        std::printf("step=%-42s api=%u.%u result=%d (%s)\n", "vkCreateInstance",
                    VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), (int)r, vkres(r));
        if (r == VK_SUCCESS)
            break;
    }
    if (r != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        std::printf("FAILED_STEP: vkCreateInstance result=%d (%s)\n", (int)r, vkres(r));
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    if (have_debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            proc<PFN_vkCreateDebugUtilsMessengerEXT>(instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_messenger) {
            VkDebugUtilsMessengerCreateInfoEXT dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debug_callback;
            VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
            VkResult mr = create_messenger(instance, &dci, nullptr, &messenger);
            std::printf("step=%-42s result=%d (%s)\n", "vkCreateDebugUtilsMessengerEXT",
                        (int)mr, vkres(mr));
        }
    }

    PFN_vkEnumeratePhysicalDevices enum_pd =
        proc<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_props =
        proc<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props =
        proc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qf =
        proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create_device = proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");

    if (!enum_pd || !get_props || !get_mem_props || !get_qf || !create_device) {
        std::printf("ERROR: physical-device entry points missing\n");
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    uint32_t pd_count = 0;
    r = enum_pd(instance, &pd_count, nullptr);
    std::printf("physical_devices.count=%u result=%d (%s)\n", (unsigned)pd_count, (int)r, vkres(r));
    if (r != VK_SUCCESS || pd_count == 0) {
        std::printf("FAILED_STEP: vkEnumeratePhysicalDevices result=%d (%s)\n", (int)r, vkres(r));
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    r = enum_pd(instance, &pd_count, pds.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        std::printf("FAILED_STEP: vkEnumeratePhysicalDevices(list) result=%d (%s)\n", (int)r, vkres(r));
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    pds.resize(pd_count);
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props{};
    get_props(pd, &props);
    std::printf("device.name=%s\n", props.deviceName);
    std::printf("device.api_version=%u.%u.%u driver_version=0x%08x vendor=0x%04x device=0x%04x\n",
                VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                VK_API_VERSION_PATCH(props.apiVersion), (unsigned)props.driverVersion,
                (unsigned)props.vendorID, (unsigned)props.deviceID);
    std::printf("device.maxComputeWorkGroupInvocations=%u maxComputeSharedMemorySize=%u\n",
                (unsigned)props.limits.maxComputeWorkGroupInvocations,
                (unsigned)props.limits.maxComputeSharedMemorySize);

    VkPhysicalDeviceMemoryProperties mp{};
    get_mem_props(pd, &mp);

    uint32_t host_visible = 0, host_coherent = 0, device_local = 0;
    std::printf("\n--- memory types ---\n");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        char flags[160];
        mem_flags(flags, sizeof(flags), mp.memoryTypes[i].propertyFlags);
        std::printf("memory_type[%u] flags=%s heap=%u\n", (unsigned)i, flags,
                    (unsigned)mp.memoryTypes[i].heapIndex);
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            host_visible++;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            host_coherent++;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            device_local++;
    }
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
        std::printf("memory_heap[%u] size=%llu%s\n", (unsigned)i,
                    (unsigned long long)mp.memoryHeaps[i].size,
                    (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "");
    }
    std::printf("memory_summary: types=%u host_visible=%u host_coherent=%u device_local=%u\n",
                (unsigned)mp.memoryTypeCount, (unsigned)host_visible,
                (unsigned)host_coherent, (unsigned)device_local);

    if (host_visible == 0) {
        std::printf("errors: no HOST_VISIBLE memory type, readback impossible\n");
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    // Choose the buffer strategy: direct host-visible storage buffer when a
    // coherent type exists, otherwise device-local storage + staging copy.
    uint32_t direct_type = 0;
    const bool direct_path =
        find_memory_type(mp, 0xffffffffu,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &direct_type);
    uint32_t storage_type = direct_type, staging_type = direct_type;
    if (!direct_path) {
        if (!find_memory_type(mp, 0xffffffffu, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                              &staging_type)) {
            std::printf("errors: no HOST_VISIBLE memory type for staging\n");
            std::printf("RESULT: FAIL\n");
            return 1;
        }
        if (!find_memory_type(mp, 0xffffffffu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &storage_type))
            storage_type = staging_type;
    }
    std::printf("buffer_path=%s storage_type=%u staging_type=%u\n",
                direct_path ? "host_visible" : "staging", (unsigned)storage_type,
                direct_path ? (unsigned)storage_type : (unsigned)staging_type);

    uint32_t qf_count = 0;
    get_qf(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count);
    if (qf_count)
        get_qf(pd, &qf_count, qf.data());
    if (qf_count == 0 || qf[0].queueCount == 0) {
        std::printf("errors: queue family 0 unavailable\n");
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("queue_family[0] count=%u flags=0x%x\n", (unsigned)qf[0].queueCount,
                (unsigned)qf[0].queueFlags);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice dev = VK_NULL_HANDLE;
    if (!ok("vkCreateDevice", create_device(pd, &dci, nullptr, &dev))) {
        std::printf("FAILED_STEP: %s result=%d (%s)\n", g_last_step, (int)g_last_result,
                    vkres(g_last_result));
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    DevApi api;
    if (load_dev_api(api, gdpa, dev, instance) != 0) {
        std::printf("errors: device entry points missing\n");
        std::printf("RESULT: FAIL\n");
        return 1;
    }

    VkQueue queue = VK_NULL_HANDLE;
    api.GetDeviceQueue(dev, 0, 0, &queue);
    std::printf("queue=%p\n", (void *)queue);

    const uint32_t kElements = 256;
    const VkDeviceSize kBytes = kElements * sizeof(uint32_t);
    const VkBufferUsageFlags kStorageUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkBufferUsageFlags kStagingUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBuffer storage_buf = VK_NULL_HANDLE;
    VkDeviceMemory storage_mem = VK_NULL_HANDLE;
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    uint32_t used_type = 0;
    bool pass = false;
    const char *fail_reason = nullptr;
    uint32_t mismatches = 0, first_bad = 0;
    uint32_t first_expected = 0, first_actual = 0;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    do {
        if (!create_buffer_mem(api, dev, mp, kBytes, kStorageUsage,
                               direct_path ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                           : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               0, &storage_buf, &storage_mem, &used_type)) {
            fail_reason = "storage buffer";
            break;
        }
        std::printf("storage_buffer type=%u size=%llu\n", (unsigned)used_type,
                    (unsigned long long)kBytes);

        if (!direct_path) {
            if (!create_buffer_mem(api, dev, mp, kBytes, kStagingUsage,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_buf,
                                   &staging_mem, &used_type)) {
                fail_reason = "staging buffer";
                break;
            }
            std::printf("staging_buffer type=%u size=%llu\n", (unsigned)used_type,
                        (unsigned long long)kBytes);
        }

        VkDeviceMemory upload_mem = direct_path ? storage_mem : staging_mem;
        void *map = nullptr;
        if (!ok("vkMapMemory(upload)", api.MapMemory(dev, upload_mem, 0, kBytes, 0, &map))) {
            fail_reason = "vkMapMemory(upload)";
            break;
        }
        uint32_t *init = static_cast<uint32_t *>(map);
        for (uint32_t i = 0; i < kElements; i++)
            init[i] = i;
        if (!direct_path) {
            const VkMemoryPropertyFlags f = mp.memoryTypes[used_type].propertyFlags;
            if (!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                VkMappedMemoryRange range{};
                range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = upload_mem;
                range.offset = 0;
                range.size = VK_WHOLE_SIZE;
                if (!ok("vkFlushMappedMemoryRanges(upload)",
                        api.FlushMappedMemoryRanges(dev, 1, &range))) {
                    fail_reason = "vkFlushMappedMemoryRanges(upload)";
                    break;
                }
            }
        }
        api.UnmapMemory(dev, upload_mem);

        if (!nop_mode) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo dslci{};
            dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dslci.bindingCount = 1;
            dslci.pBindings = &binding;
            if (!ok("vkCreateDescriptorSetLayout",
                    api.CreateDescriptorSetLayout(dev, &dslci, nullptr, &set_layout))) {
                fail_reason = "vkCreateDescriptorSetLayout";
                break;
            }
        }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = nop_mode ? 0u : 1u;
        plci.pSetLayouts = nop_mode ? nullptr : &set_layout;
        if (!ok("vkCreatePipelineLayout",
                api.CreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout))) {
            fail_reason = "vkCreatePipelineLayout";
            break;
        }

        if (!nop_mode) {
            VkDescriptorPoolSize pool_size{};
            pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            pool_size.descriptorCount = 1;

            VkDescriptorPoolCreateInfo dpci{};
            dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dpci.maxSets = 1;
            dpci.poolSizeCount = 1;
            dpci.pPoolSizes = &pool_size;
            if (!ok("vkCreateDescriptorPool",
                    api.CreateDescriptorPool(dev, &dpci, nullptr, &pool))) {
                fail_reason = "vkCreateDescriptorPool";
                break;
            }

            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &set_layout;
            if (!ok("vkAllocateDescriptorSets", api.AllocateDescriptorSets(dev, &dsai, &set))) {
                fail_reason = "vkAllocateDescriptorSets";
                break;
            }

            VkDescriptorBufferInfo dbi{};
            dbi.buffer = storage_buf;
            dbi.offset = 0;
            dbi.range = VK_WHOLE_SIZE;
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &dbi;
            api.UpdateDescriptorSets(dev, 1, &write, 0, nullptr);
            std::printf("descriptor_set updated buffer=%p\n", (void *)storage_buf);
        } else {
            std::printf("descriptor_set skipped (nop shader has no bindings)\n");
        }

        // SPIR-V must be 4-byte aligned: copy the byte array into a uint32_t vector.
        std::vector<uint32_t> spirv(spirv_bytes / 4);
        std::memcpy(spirv.data(), spirv_blob, spirv_bytes);

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spirv_bytes;
        smci.pCode = spirv.data();
        if (!ok("vkCreateShaderModule", api.CreateShaderModule(dev, &smci, nullptr, &shader))) {
            fail_reason = "vkCreateShaderModule";
            break;
        }

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = shader;
        cpci.stage.pName = "main";
        cpci.layout = pipeline_layout;
        cpci.basePipelineHandle = VK_NULL_HANDLE;
        cpci.basePipelineIndex = -1;
        if (!ok("vkCreateComputePipelines",
                api.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline))) {
            fail_reason = "vkCreateComputePipelines";
            break;
        }

        VkCommandPoolCreateInfo cpci2{};
        cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci2.queueFamilyIndex = 0;
        if (!ok("vkCreateCommandPool", api.CreateCommandPool(dev, &cpci2, nullptr, &cmd_pool))) {
            fail_reason = "vkCreateCommandPool";
            break;
        }

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (!ok("vkAllocateCommandBuffers", api.AllocateCommandBuffers(dev, &cbai, &cmd))) {
            fail_reason = "vkAllocateCommandBuffers";
            break;
        }

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!ok("vkCreateFence", api.CreateFence(dev, &fci, nullptr, &fence))) {
            fail_reason = "vkCreateFence";
            break;
        }

        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!ok("vkBeginCommandBuffer", api.BeginCommandBuffer(cmd, &cbbi))) {
            fail_reason = "vkBeginCommandBuffer";
            break;
        }

        const bool staging_copy = !direct_path;
        if (staging_copy) {
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 1, &mb, 0, nullptr, 0, nullptr);

            VkBufferCopy copy{};
            copy.srcOffset = 0;
            copy.dstOffset = 0;
            copy.size = kBytes;
            api.CmdCopyBuffer(cmd, staging_buf, storage_buf, 1, &copy);

            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                   nullptr);
        } else {
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                   nullptr);
        }

        api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (!nop_mode)
            api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                                      &set, 0, nullptr);

        const uint32_t workgroups = (kElements + 63) / 64;
        std::printf("dispatch workgroups=(%u,1,1) local_size=64 invocations=%u elements=%u\n",
                    (unsigned)workgroups, (unsigned)(workgroups * 64), (unsigned)kElements);
        api.CmdDispatch(cmd, workgroups, 1, 1);

        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        if (staging_copy) {
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);

            VkBufferCopy copy{};
            copy.srcOffset = 0;
            copy.dstOffset = 0;
            copy.size = kBytes;
            api.CmdCopyBuffer(cmd, storage_buf, staging_buf, 1, &copy);

            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                   0, 1, &mb, 0, nullptr, 0, nullptr);
        } else {
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                   0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        if (!ok("vkEndCommandBuffer", api.EndCommandBuffer(cmd))) {
            fail_reason = "vkEndCommandBuffer";
            break;
        }

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (!ok("vkQueueSubmit", api.QueueSubmit(queue, 1, &si, fence))) {
            fail_reason = "vkQueueSubmit";
            break;
        }

        const uint64_t timeout_ns = 10000000000ull; // 10 s
        VkResult wr = api.WaitForFences(dev, 1, &fence, VK_TRUE, timeout_ns);
        if (!ok("vkWaitForFences", wr)) {
            VkResult fs = api.GetFenceStatus(dev, fence);
            std::printf("fence_status=%d (%s)\n", (int)fs, vkres(fs));
            fail_reason = "vkWaitForFences";
            break;
        }

        VkDeviceMemory readback_mem = direct_path ? storage_mem : staging_mem;
        void *rmap = nullptr;
        if (!ok("vkMapMemory(readback)", api.MapMemory(dev, readback_mem, 0, kBytes, 0, &rmap))) {
            fail_reason = "vkMapMemory(readback)";
            break;
        }
        {
            const uint32_t readback_type = direct_path ? storage_type : staging_type;
            const VkMemoryPropertyFlags f = mp.memoryTypes[readback_type].propertyFlags;
            if (!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                VkMappedMemoryRange range{};
                range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = readback_mem;
                range.offset = 0;
                range.size = VK_WHOLE_SIZE;
                if (!ok("vkInvalidateMappedMemoryRanges(readback)",
                        api.InvalidateMappedMemoryRanges(dev, 1, &range))) {
                    fail_reason = "vkInvalidateMappedMemoryRanges(readback)";
                    break;
                }
            }
        }
        const uint32_t *out = static_cast<const uint32_t *>(rmap);
        const uint32_t delta = nop_mode ? 0u : 1u;
        for (uint32_t i = 0; i < kElements; i++) {
            const uint32_t expected = i + delta;
            if (out[i] != expected) {
                if (mismatches == 0) {
                    first_bad = i;
                    first_expected = expected;
                    first_actual = out[i];
                }
                mismatches++;
            }
        }
        std::printf("readback[0..7]=%u,%u,%u,%u,%u,%u,%u,%u\n", out[0], out[1], out[2], out[3],
                    out[4], out[5], out[6], out[7]);
        api.UnmapMemory(dev, readback_mem);

        if (mismatches) {
            std::printf("mismatches=%u first_bad_index=%u expected=%u actual=%u\n", (unsigned)mismatches,
                        (unsigned)first_bad, (unsigned)first_expected, (unsigned)first_actual);
            fail_reason = "verify";
            break;
        }

        pass = true;
    } while (0);

    if (api.DeviceWaitIdle) {
        VkResult idle = api.DeviceWaitIdle(dev);
        std::printf("step=%-42s result=%d (%s)\n", "vkDeviceWaitIdle", (int)idle, vkres(idle));
    }

    if (api.DestroyFence && fence)
        api.DestroyFence(dev, fence, nullptr);
    if (api.DestroyCommandPool && cmd_pool)
        api.DestroyCommandPool(dev, cmd_pool, nullptr);
    if (api.DestroyPipeline && pipeline)
        api.DestroyPipeline(dev, pipeline, nullptr);
    if (api.DestroyPipelineLayout && pipeline_layout)
        api.DestroyPipelineLayout(dev, pipeline_layout, nullptr);
    if (api.DestroyDescriptorPool && pool)
        api.DestroyDescriptorPool(dev, pool, nullptr);
    if (api.DestroyDescriptorSetLayout && set_layout)
        api.DestroyDescriptorSetLayout(dev, set_layout, nullptr);
    if (api.DestroyShaderModule && shader)
        api.DestroyShaderModule(dev, shader, nullptr);
    if (api.DestroyBuffer) {
        if (staging_buf)
            api.DestroyBuffer(dev, staging_buf, nullptr);
        if (storage_buf)
            api.DestroyBuffer(dev, storage_buf, nullptr);
    }
    if (api.FreeMemory) {
        if (staging_mem)
            api.FreeMemory(dev, staging_mem, nullptr);
        if (storage_mem)
            api.FreeMemory(dev, storage_mem, nullptr);
    }
    if (api.DestroyDevice)
        api.DestroyDevice(dev, nullptr);

    if (!pass) {
        std::printf("FAILED_STEP: %s result=%d (%s)\n", g_last_step, (int)g_last_result,
                    vkres(g_last_result));
        if (fail_reason)
            std::printf("FAILED_STAGE: %s\n", fail_reason);
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("verified %u elements in %s mode, expected value per element = i%s\n",
                (unsigned)kElements, nop_mode ? "nop" : "ssbo", nop_mode ? "" : "+1");
    std::printf("RESULT: PASS\n");
    return 0;
}
