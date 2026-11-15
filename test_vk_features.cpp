// Vulkan feature probe for the Windows ARM64 panvk ICD.
//
// The ICD is loaded directly with LoadLibraryA + vk_icdGetInstanceProcAddr,
// so no Vulkan loader (vulkan-1.dll) has to be installed.  For every physical
// device the probe queries VkPhysicalDeviceFeatures2 chained with
// VkPhysicalDeviceVulkan11/12/13Features and prints the feature bits that
// DXVK 2.7+ treats as hard requirements (see src/dxvk/dxvk_device_info.cpp,
// DxvkDeviceCapabilities::getFeatureList()).  Output contract:
//
//   name=0/1        one line per required feature, in DXVK check order
//   MISSING: a,b,c  required features reported as 0
//
// A full dump of every core/1.1/1.2/1.3 feature and of the DXVK-required
// device extensions follows, plus the first feature DXVK itself would reject.
//
// Build: cl /nologo /EHsc /O2 /std:c++17 /I third_party\vulkan\include ^
//            test_vk_features.cpp /Fe:vk_features_arm64.exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include "third_party/vulkan/include/vulkan/vulkan_core.h"

typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_icdGetInstanceProcAddr_t)(VkInstance, const char *);

static PFN_icdGetInstanceProcAddr_t g_icd_gipa = nullptr;

static PFN_vkVoidFunction gipa(VkInstance instance, const char *name) {
    return g_icd_gipa ? g_icd_gipa(instance, name) : nullptr;
}

template <typename T>
static T proc(VkInstance instance, const char *name) {
    return reinterpret_cast<T>(gipa(instance, name));
}

static void pr(const char *name, VkBool32 value) {
    std::printf("%s=%u\n", name, (unsigned)(value ? 1u : 0u));
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
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VK_ERROR_OTHER";
    }
}

struct Req {
    const char *name;
    VkBool32    value;
};

static bool has_extension(const std::vector<VkExtensionProperties> &list, const char *name) {
    for (const auto &e : list) {
        if (!std::strcmp(e.extensionName, name))
            return true;
    }
    return false;
}

int main(int argc, char **argv) {
    const char *dll = (argc > 1) ? argv[1] : "C:\\panvk\\dev_vulkan_virtio.dll";

    std::printf("probe=vk_features\n");
    std::printf("icd_path=%s\n", dll);

    HMODULE mod = LoadLibraryA(dll);
    if (!mod) {
        std::printf("ERROR: LoadLibraryA failed err=%lu\n", (unsigned long)GetLastError());
        return 1;
    }
    char module_path[MAX_PATH] = {0};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::printf("icd_module=%s\n", module_path);

    g_icd_gipa = reinterpret_cast<PFN_icdGetInstanceProcAddr_t>(
        GetProcAddress(mod, "vk_icdGetInstanceProcAddr"));
    if (!g_icd_gipa) {
        std::printf("ERROR: %s does not export vk_icdGetInstanceProcAddr\n", dll);
        return 1;
    }

    PFN_vkCreateInstance create_instance = proc<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_inst_ext =
        proc<PFN_vkEnumerateInstanceExtensionProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!create_instance) {
        std::printf("ERROR: vkCreateInstance not resolvable through the ICD\n");
        return 1;
    }

    bool have_khr_props2 = false;
    if (enum_inst_ext) {
        uint32_t count = 0;
        VkResult r = enum_inst_ext(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> inst_ext(count);
        if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
            r = enum_inst_ext(nullptr, &count, inst_ext.data());
            if (r == VK_SUCCESS || r == VK_INCOMPLETE)
                inst_ext.resize(count);
        }
        if (r != VK_SUCCESS && r != VK_INCOMPLETE)
            inst_ext.clear();
        std::printf("instance_extensions.count=%u\n", (unsigned)inst_ext.size());
        have_khr_props2 = has_extension(inst_ext, "VK_KHR_get_physical_device_properties2");
        std::printf("VK_KHR_get_physical_device_properties2=%u\n", have_khr_props2 ? 1u : 0u);
    } else {
        std::printf("instance_extensions.count=unavailable\n");
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vk_features";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;

    const char *enabled_exts[1] = {nullptr};
    uint32_t enabled_ext_count = 0;
    if (have_khr_props2)
        enabled_exts[enabled_ext_count++] = "VK_KHR_get_physical_device_properties2";

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = enabled_ext_count;
    ici.ppEnabledExtensionNames = enabled_ext_count ? enabled_exts : nullptr;

    const uint32_t try_apis[] = {
        VK_MAKE_API_VERSION(0, 1, 4, 0),
        VK_MAKE_API_VERSION(0, 1, 3, 0),
        VK_MAKE_API_VERSION(0, 1, 2, 0),
        VK_MAKE_API_VERSION(0, 1, 1, 0),
        VK_MAKE_API_VERSION(0, 1, 0, 0),
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult ir = VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t api : try_apis) {
        app.apiVersion = api;
        ir = create_instance(&ici, nullptr, &instance);
        std::printf("vkCreateInstance(api=%u.%u.%u): result=%d (%s)\n",
                    VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                    VK_API_VERSION_PATCH(api), (int)ir, vkres(ir));
        if (ir == VK_SUCCESS)
            break;
    }
    if (ir != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        std::printf("ERROR: could not create a Vulkan instance\n");
        return 1;
    }
    std::printf("instance.api_version=%u.%u.%u\n",
                VK_API_VERSION_MAJOR(app.apiVersion),
                VK_API_VERSION_MINOR(app.apiVersion),
                VK_API_VERSION_PATCH(app.apiVersion));

    PFN_vkEnumeratePhysicalDevices enum_pd = proc<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_props = proc<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 get_props2 = proc<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qf =
        proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkEnumerateDeviceExtensionProperties enum_dev_ext =
        proc<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");

    PFN_vkGetPhysicalDeviceFeatures2 get_features2 = proc<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
    const char *features2_entry = "vkGetPhysicalDeviceFeatures2";
    if (!get_features2) {
        get_features2 = proc<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2KHR");
        features2_entry = "vkGetPhysicalDeviceFeatures2KHR";
    }
    std::printf("features2_entry_point=%s\n", get_features2 ? features2_entry : "MISSING");
    if (!get_features2) {
        std::printf("ERROR: neither vkGetPhysicalDeviceFeatures2 nor the KHR alias is exported\n");
        return 1;
    }
    if (!enum_pd || !get_props || !get_qf) {
        std::printf("ERROR: required physical-device entry points are missing\n");
        return 1;
    }

    uint32_t pd_count = 0;
    VkResult r = enum_pd(instance, &pd_count, nullptr);
    std::printf("physical_devices.count=%u (result=%d %s)\n", (unsigned)pd_count, (int)r, vkres(r));
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || pd_count == 0) {
        std::printf("ERROR: vkEnumeratePhysicalDevices returned no device\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    r = enum_pd(instance, &pd_count, pds.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        std::printf("ERROR: vkEnumeratePhysicalDevices(list) result=%d (%s)\n", (int)r, vkres(r));
        return 1;
    }
    pds.resize(pd_count);

    bool any_missing = false;

    for (uint32_t di = 0; di < pd_count; di++) {
        VkPhysicalDevice pd = pds[di];
        std::printf("\n=== device[%u] ===\n", (unsigned)di);

        VkPhysicalDeviceProperties props{};
        get_props(pd, &props);
        std::printf("device.name=%s\n", props.deviceName);
        std::printf("device.api_version=%u.%u.%u\n",
                    VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion),
                    VK_API_VERSION_PATCH(props.apiVersion));
        std::printf("device.driver_version=0x%08x\n", (unsigned)props.driverVersion);
        std::printf("device.vendor_id=0x%04x\n", (unsigned)props.vendorID);
        std::printf("device.device_id=0x%04x\n", (unsigned)props.deviceID);
        std::printf("device.device_type=%d\n", (int)props.deviceType);

        if (get_props2) {
            VkPhysicalDeviceDriverProperties drv{};
            drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &drv;
            get_props2(pd, &props2);
            std::printf("device.driver_id=%d\n", (int)drv.driverID);
            std::printf("device.driver_name=%s\n", drv.driverName);
            std::printf("device.driver_info=%s\n", drv.driverInfo);
            std::printf("device.conformance=%u.%u.%u.%u\n",
                        (unsigned)drv.conformanceVersion.major,
                        (unsigned)drv.conformanceVersion.minor,
                        (unsigned)drv.conformanceVersion.subminor,
                        (unsigned)drv.conformanceVersion.patch);
        }

        uint32_t qf_count = 0;
        get_qf(pd, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qf_count);
        if (qf_count)
            get_qf(pd, &qf_count, qf.data());
        for (uint32_t q = 0; q < qf_count && q < qf.size(); q++) {
            std::printf("queue_family[%u].flags=0x%x\n", (unsigned)q, (unsigned)qf[q].queueFlags);
            std::printf("queue_family[%u].count=%u\n", (unsigned)q, (unsigned)qf[q].queueCount);
        }

        std::vector<VkExtensionProperties> dev_ext;
        if (enum_dev_ext) {
            uint32_t count = 0;
            VkResult er = enum_dev_ext(pd, nullptr, &count, nullptr);
            if (er == VK_SUCCESS || er == VK_INCOMPLETE) {
                dev_ext.resize(count);
                er = enum_dev_ext(pd, nullptr, &count, dev_ext.data());
                if (er == VK_SUCCESS || er == VK_INCOMPLETE)
                    dev_ext.resize(count);
                else
                    dev_ext.clear();
            }
        }
        std::printf("device_extensions.count=%u\n", (unsigned)dev_ext.size());

        // Query features with the 1.1/1.2/1.3 structs plus the two extension
        // feature structs that DXVK marks as required.
        VkPhysicalDeviceVulkan11Features f11{};
        VkPhysicalDeviceVulkan12Features f12{};
        VkPhysicalDeviceVulkan13Features f13{};
        VkPhysicalDeviceRobustness2FeaturesEXT rob2{};
        VkPhysicalDeviceDepthClipEnableFeaturesEXT dce{};
        VkPhysicalDeviceFeatures2 f2{};

        f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        rob2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        dce.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT;
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &rob2;
        rob2.pNext = &dce;

        get_features2(pd, &f2);

        bool ext_depth_clip = has_extension(dev_ext, "VK_EXT_depth_clip_enable");
        bool ext_robustness2 = has_extension(dev_ext, "VK_EXT_robustness2");

        std::printf("\n--- required features (DXVK 2.7 order) ---\n");

        // Only the entries DXVK marks required (require == true) in
        // DxvkDeviceCapabilities::getFeatureList(), in the same order.
        const Req required[] = {
            {"depthBiasClamp", f2.features.depthBiasClamp},
            {"depthClamp", f2.features.depthClamp},
            {"dualSrcBlend", f2.features.dualSrcBlend},
            {"fillModeNonSolid", f2.features.fillModeNonSolid},
            {"fragmentStoresAndAtomics", f2.features.fragmentStoresAndAtomics},
            {"fullDrawIndexUint32", f2.features.fullDrawIndexUint32},
            {"geometryShader", f2.features.geometryShader},
            {"imageCubeArray", f2.features.imageCubeArray},
            {"independentBlend", f2.features.independentBlend},
            {"multiDrawIndirect", f2.features.multiDrawIndirect},
            {"multiViewport", f2.features.multiViewport},
            {"occlusionQueryPrecise", f2.features.occlusionQueryPrecise},
            {"robustBufferAccess", f2.features.robustBufferAccess},
            {"sampleRateShading", f2.features.sampleRateShading},
            {"samplerAnisotropy", f2.features.samplerAnisotropy},
            {"shaderClipDistance", f2.features.shaderClipDistance},
            {"shaderCullDistance", f2.features.shaderCullDistance},
            {"shaderImageGatherExtended", f2.features.shaderImageGatherExtended},
            {"shaderInt16", f2.features.shaderInt16},
            {"shaderInt64", f2.features.shaderInt64},
            {"shaderSampledImageArrayDynamicIndexing", f2.features.shaderSampledImageArrayDynamicIndexing},
            {"textureCompressionBC", f2.features.textureCompressionBC},
            {"shaderDrawParameters", f11.shaderDrawParameters},
            {"storageBuffer16BitAccess", f11.storageBuffer16BitAccess},
            {"bufferDeviceAddress", f12.bufferDeviceAddress},
            {"descriptorIndexing", f12.descriptorIndexing},
            {"storageBuffer8BitAccess", f12.storageBuffer8BitAccess},
            {"descriptorBindingSampledImageUpdateAfterBind", f12.descriptorBindingSampledImageUpdateAfterBind},
            {"descriptorBindingUpdateUnusedWhilePending", f12.descriptorBindingUpdateUnusedWhilePending},
            {"descriptorBindingPartiallyBound", f12.descriptorBindingPartiallyBound},
            {"hostQueryReset", f12.hostQueryReset},
            {"runtimeDescriptorArray", f12.runtimeDescriptorArray},
            {"samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge},
            {"scalarBlockLayout", f12.scalarBlockLayout},
            {"shaderInt8", f12.shaderInt8},
            {"timelineSemaphore", f12.timelineSemaphore},
            {"uniformBufferStandardLayout", f12.uniformBufferStandardLayout},
            {"vulkanMemoryModel", f12.vulkanMemoryModel},
            {"inlineUniformBlock", f13.inlineUniformBlock},
            {"computeFullSubgroups", f13.computeFullSubgroups},
            {"dynamicRendering", f13.dynamicRendering},
            {"maintenance4", f13.maintenance4},
            {"shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation},
            {"shaderZeroInitializeWorkgroupMemory", f13.shaderZeroInitializeWorkgroupMemory},
            {"subgroupSizeControl", f13.subgroupSizeControl},
            {"synchronization2", f13.synchronization2},
        };

        const size_t required_count = sizeof(required) / sizeof(required[0]);
        size_t missing_count = 0;
        bool dev_missing = false;
        for (size_t i = 0; i < required_count; i++) {
            pr(required[i].name, required[i].value);
            if (!required[i].value)
                missing_count++;
        }

        std::printf("MISSING:");
        if (!missing_count) {
            std::printf(" none\n");
        } else {
            bool first = true;
            for (size_t i = 0; i < required_count; i++) {
                if (required[i].value)
                    continue;
                std::printf("%s%s", first ? " " : ",", required[i].name);
                first = false;
            }
            std::printf("\n");
        }
        std::printf("MISSING_COUNT: %u\n", (unsigned)missing_count);
        if (missing_count)
            dev_missing = true;

        std::printf("\n--- required extensions ---\n");
        const char *ext_names[] = {
            "VK_EXT_robustness2",
            "VK_EXT_depth_clip_enable",
            "VK_KHR_load_store_op_none",
            "VK_KHR_maintenance5",
            "VK_KHR_maintenance6",
            "VK_EXT_transform_feedback",
            "VK_KHR_swapchain",
        };
        std::vector<const char *> missing_ext;
        for (const char *name : ext_names) {
            const bool present = has_extension(dev_ext, name);
            std::printf("ext.%s=%u\n", name, present ? 1u : 0u);
            if (!present)
                missing_ext.push_back(name);
        }

        // DXVK treats extension features as unsupported when the extension is
        // not advertised, so gate them the same way.
        pr("depthClipEnable", ext_depth_clip ? dce.depthClipEnable : VK_FALSE);
        pr("robustBufferAccess2", ext_robustness2 ? rob2.robustBufferAccess2 : VK_FALSE);
        pr("nullDescriptor", ext_robustness2 ? rob2.nullDescriptor : VK_FALSE);
        std::printf("MISSING_EXT:");
        if (missing_ext.empty()) {
            std::printf(" none\n");
        } else {
            for (size_t i = 0; i < missing_ext.size(); i++)
                std::printf("%s%s", i ? "," : " ", missing_ext[i]);
            std::printf("\n");
        }
        if (!ext_depth_clip || !dce.depthClipEnable || !ext_robustness2 ||
            !rob2.robustBufferAccess2 || !rob2.nullDescriptor)
            dev_missing = true;
        if (dev_missing)
            any_missing = true;

        // Emulate DXVK's checkDeviceCompatibility(): it returns the first
        // required feature that is zero, in getFeatureList() order.
        const char *first_missing = nullptr;
        if (props.apiVersion < VK_API_VERSION_1_3) {
            std::printf("\nDXVK_FIRST_MISSING: <Vulkan 1.3 API version>\n");
        } else if (!dev_missing) {
            std::printf("\nDXVK_FIRST_MISSING: none\n");
        } else {
            for (size_t i = 0; i < required_count && !first_missing; i++) {
                if (!required[i].value)
                    first_missing = required[i].name;
            }
            if (!first_missing) {
                if (!ext_depth_clip || !dce.depthClipEnable)
                    first_missing = "depthClipEnable (VK_EXT_depth_clip_enable)";
                else if (!ext_robustness2 || !rob2.robustBufferAccess2)
                    first_missing = "robustBufferAccess2 (VK_EXT_robustness2)";
                else if (!rob2.nullDescriptor)
                    first_missing = "nullDescriptor (VK_EXT_robustness2)";
            }
            std::printf("\nDXVK_FIRST_MISSING: %s\n", first_missing ? first_missing : "none");
        }

        std::printf("\n--- full core features ---\n");
        pr("robustBufferAccess", f2.features.robustBufferAccess);
        pr("fullDrawIndexUint32", f2.features.fullDrawIndexUint32);
        pr("imageCubeArray", f2.features.imageCubeArray);
        pr("independentBlend", f2.features.independentBlend);
        pr("geometryShader", f2.features.geometryShader);
        pr("tessellationShader", f2.features.tessellationShader);
        pr("sampleRateShading", f2.features.sampleRateShading);
        pr("dualSrcBlend", f2.features.dualSrcBlend);
        pr("logicOp", f2.features.logicOp);
        pr("multiDrawIndirect", f2.features.multiDrawIndirect);
        pr("drawIndirectFirstInstance", f2.features.drawIndirectFirstInstance);
        pr("depthClamp", f2.features.depthClamp);
        pr("depthBiasClamp", f2.features.depthBiasClamp);
        pr("fillModeNonSolid", f2.features.fillModeNonSolid);
        pr("depthBounds", f2.features.depthBounds);
        pr("wideLines", f2.features.wideLines);
        pr("largePoints", f2.features.largePoints);
        pr("alphaToOne", f2.features.alphaToOne);
        pr("multiViewport", f2.features.multiViewport);
        pr("samplerAnisotropy", f2.features.samplerAnisotropy);
        pr("textureCompressionETC2", f2.features.textureCompressionETC2);
        pr("textureCompressionASTC_LDR", f2.features.textureCompressionASTC_LDR);
        pr("textureCompressionBC", f2.features.textureCompressionBC);
        pr("occlusionQueryPrecise", f2.features.occlusionQueryPrecise);
        pr("pipelineStatisticsQuery", f2.features.pipelineStatisticsQuery);
        pr("vertexPipelineStoresAndAtomics", f2.features.vertexPipelineStoresAndAtomics);
        pr("fragmentStoresAndAtomics", f2.features.fragmentStoresAndAtomics);
        pr("shaderTessellationAndGeometryPointSize", f2.features.shaderTessellationAndGeometryPointSize);
        pr("shaderImageGatherExtended", f2.features.shaderImageGatherExtended);
        pr("shaderStorageImageExtendedFormats", f2.features.shaderStorageImageExtendedFormats);
        pr("shaderStorageImageMultisample", f2.features.shaderStorageImageMultisample);
        pr("shaderStorageImageReadWithoutFormat", f2.features.shaderStorageImageReadWithoutFormat);
        pr("shaderStorageImageWriteWithoutFormat", f2.features.shaderStorageImageWriteWithoutFormat);
        pr("shaderUniformBufferArrayDynamicIndexing", f2.features.shaderUniformBufferArrayDynamicIndexing);
        pr("shaderSampledImageArrayDynamicIndexing", f2.features.shaderSampledImageArrayDynamicIndexing);
        pr("shaderStorageBufferArrayDynamicIndexing", f2.features.shaderStorageBufferArrayDynamicIndexing);
        pr("shaderStorageImageArrayDynamicIndexing", f2.features.shaderStorageImageArrayDynamicIndexing);
        pr("shaderClipDistance", f2.features.shaderClipDistance);
        pr("shaderCullDistance", f2.features.shaderCullDistance);
        pr("shaderFloat64", f2.features.shaderFloat64);
        pr("shaderInt64", f2.features.shaderInt64);
        pr("shaderInt16", f2.features.shaderInt16);
        pr("shaderResourceResidency", f2.features.shaderResourceResidency);
        pr("shaderResourceMinLod", f2.features.shaderResourceMinLod);
        pr("sparseBinding", f2.features.sparseBinding);
        pr("sparseResidencyBuffer", f2.features.sparseResidencyBuffer);
        pr("sparseResidencyImage2D", f2.features.sparseResidencyImage2D);
        pr("sparseResidencyImage3D", f2.features.sparseResidencyImage3D);
        pr("sparseResidency2Samples", f2.features.sparseResidency2Samples);
        pr("sparseResidency4Samples", f2.features.sparseResidency4Samples);
        pr("sparseResidency8Samples", f2.features.sparseResidency8Samples);
        pr("sparseResidency16Samples", f2.features.sparseResidency16Samples);
        pr("sparseResidencyAliased", f2.features.sparseResidencyAliased);
        pr("variableMultisampleRate", f2.features.variableMultisampleRate);
        pr("inheritedQueries", f2.features.inheritedQueries);

        std::printf("\n--- full Vulkan 1.1 features ---\n");
        pr("vk11.storageBuffer16BitAccess", f11.storageBuffer16BitAccess);
        pr("vk11.uniformAndStorageBuffer16BitAccess", f11.uniformAndStorageBuffer16BitAccess);
        pr("vk11.storagePushConstant16", f11.storagePushConstant16);
        pr("vk11.storageInputOutput16", f11.storageInputOutput16);
        pr("vk11.multiview", f11.multiview);
        pr("vk11.multiviewGeometryShader", f11.multiviewGeometryShader);
        pr("vk11.multiviewTessellationShader", f11.multiviewTessellationShader);
        pr("vk11.variablePointersStorageBuffer", f11.variablePointersStorageBuffer);
        pr("vk11.variablePointers", f11.variablePointers);
        pr("vk11.protectedMemory", f11.protectedMemory);
        pr("vk11.samplerYcbcrConversion", f11.samplerYcbcrConversion);
        pr("vk11.shaderDrawParameters", f11.shaderDrawParameters);

        std::printf("\n--- full Vulkan 1.2 features ---\n");
        pr("vk12.samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge);
        pr("vk12.drawIndirectCount", f12.drawIndirectCount);
        pr("vk12.storageBuffer8BitAccess", f12.storageBuffer8BitAccess);
        pr("vk12.uniformAndStorageBuffer8BitAccess", f12.uniformAndStorageBuffer8BitAccess);
        pr("vk12.storagePushConstant8", f12.storagePushConstant8);
        pr("vk12.shaderBufferInt64Atomics", f12.shaderBufferInt64Atomics);
        pr("vk12.shaderSharedInt64Atomics", f12.shaderSharedInt64Atomics);
        pr("vk12.shaderFloat16", f12.shaderFloat16);
        pr("vk12.shaderInt8", f12.shaderInt8);
        pr("vk12.descriptorIndexing", f12.descriptorIndexing);
        pr("vk12.shaderInputAttachmentArrayDynamicIndexing", f12.shaderInputAttachmentArrayDynamicIndexing);
        pr("vk12.shaderUniformTexelBufferArrayDynamicIndexing", f12.shaderUniformTexelBufferArrayDynamicIndexing);
        pr("vk12.shaderStorageTexelBufferArrayDynamicIndexing", f12.shaderStorageTexelBufferArrayDynamicIndexing);
        pr("vk12.shaderUniformBufferArrayNonUniformIndexing", f12.shaderUniformBufferArrayNonUniformIndexing);
        pr("vk12.shaderSampledImageArrayNonUniformIndexing", f12.shaderSampledImageArrayNonUniformIndexing);
        pr("vk12.shaderStorageBufferArrayNonUniformIndexing", f12.shaderStorageBufferArrayNonUniformIndexing);
        pr("vk12.shaderStorageImageArrayNonUniformIndexing", f12.shaderStorageImageArrayNonUniformIndexing);
        pr("vk12.shaderInputAttachmentArrayNonUniformIndexing", f12.shaderInputAttachmentArrayNonUniformIndexing);
        pr("vk12.shaderUniformTexelBufferArrayNonUniformIndexing", f12.shaderUniformTexelBufferArrayNonUniformIndexing);
        pr("vk12.shaderStorageTexelBufferArrayNonUniformIndexing", f12.shaderStorageTexelBufferArrayNonUniformIndexing);
        pr("vk12.descriptorBindingUniformBufferUpdateAfterBind", f12.descriptorBindingUniformBufferUpdateAfterBind);
        pr("vk12.descriptorBindingSampledImageUpdateAfterBind", f12.descriptorBindingSampledImageUpdateAfterBind);
        pr("vk12.descriptorBindingStorageImageUpdateAfterBind", f12.descriptorBindingStorageImageUpdateAfterBind);
        pr("vk12.descriptorBindingStorageBufferUpdateAfterBind", f12.descriptorBindingStorageBufferUpdateAfterBind);
        pr("vk12.descriptorBindingUniformTexelBufferUpdateAfterBind", f12.descriptorBindingUniformTexelBufferUpdateAfterBind);
        pr("vk12.descriptorBindingStorageTexelBufferUpdateAfterBind", f12.descriptorBindingStorageTexelBufferUpdateAfterBind);
        pr("vk12.descriptorBindingUpdateUnusedWhilePending", f12.descriptorBindingUpdateUnusedWhilePending);
        pr("vk12.descriptorBindingPartiallyBound", f12.descriptorBindingPartiallyBound);
        pr("vk12.descriptorBindingVariableDescriptorCount", f12.descriptorBindingVariableDescriptorCount);
        pr("vk12.runtimeDescriptorArray", f12.runtimeDescriptorArray);
        pr("vk12.samplerFilterMinmax", f12.samplerFilterMinmax);
        pr("vk12.scalarBlockLayout", f12.scalarBlockLayout);
        pr("vk12.imagelessFramebuffer", f12.imagelessFramebuffer);
        pr("vk12.uniformBufferStandardLayout", f12.uniformBufferStandardLayout);
        pr("vk12.shaderSubgroupExtendedTypes", f12.shaderSubgroupExtendedTypes);
        pr("vk12.separateDepthStencilLayouts", f12.separateDepthStencilLayouts);
        pr("vk12.hostQueryReset", f12.hostQueryReset);
        pr("vk12.timelineSemaphore", f12.timelineSemaphore);
        pr("vk12.bufferDeviceAddress", f12.bufferDeviceAddress);
        pr("vk12.bufferDeviceAddressCaptureReplay", f12.bufferDeviceAddressCaptureReplay);
        pr("vk12.bufferDeviceAddressMultiDevice", f12.bufferDeviceAddressMultiDevice);
        pr("vk12.vulkanMemoryModel", f12.vulkanMemoryModel);
        pr("vk12.vulkanMemoryModelDeviceScope", f12.vulkanMemoryModelDeviceScope);
        pr("vk12.vulkanMemoryModelAvailabilityVisibilityChains", f12.vulkanMemoryModelAvailabilityVisibilityChains);
        pr("vk12.shaderOutputViewportIndex", f12.shaderOutputViewportIndex);
        pr("vk12.shaderOutputLayer", f12.shaderOutputLayer);
        pr("vk12.subgroupBroadcastDynamicId", f12.subgroupBroadcastDynamicId);

        std::printf("\n--- full Vulkan 1.3 features ---\n");
        pr("vk13.robustImageAccess", f13.robustImageAccess);
        pr("vk13.inlineUniformBlock", f13.inlineUniformBlock);
        pr("vk13.descriptorBindingInlineUniformBlockUpdateAfterBind", f13.descriptorBindingInlineUniformBlockUpdateAfterBind);
        pr("vk13.pipelineCreationCacheControl", f13.pipelineCreationCacheControl);
        pr("vk13.privateData", f13.privateData);
        pr("vk13.shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation);
        pr("vk13.shaderTerminateInvocation", f13.shaderTerminateInvocation);
        pr("vk13.subgroupSizeControl", f13.subgroupSizeControl);
        pr("vk13.computeFullSubgroups", f13.computeFullSubgroups);
        pr("vk13.synchronization2", f13.synchronization2);
        pr("vk13.textureCompressionASTC_HDR", f13.textureCompressionASTC_HDR);
        pr("vk13.shaderZeroInitializeWorkgroupMemory", f13.shaderZeroInitializeWorkgroupMemory);
        pr("vk13.dynamicRendering", f13.dynamicRendering);
        pr("vk13.shaderIntegerDotProduct", f13.shaderIntegerDotProduct);
        pr("vk13.maintenance4", f13.maintenance4);
    }

    std::printf("\nRESULT: %s\n", any_missing ? "DXVK_REQUIREMENTS_MISSING" : "DXVK_REQUIREMENTS_MET");
    return any_missing ? 2 : 0;
}
