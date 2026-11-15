# win-gpu-tests

Minimal D3D11 offscreen smoke test for the DroidVM Windows ARM64 GPU stack.
It creates a hardware D3D11 device, clears a 256x256 RTV and reads the pixel
back through a staging texture.  A PASS means the KMD's D3D11 UMD plus the
Vulkan ICD (PanVK once installed) executed the clear and the copy.

`test_vk_features.cpp` is a loader-free Vulkan probe.  It loads the ICD with
`LoadLibraryA` + `vk_icdGetInstanceProcAddr`, chains
`VkPhysicalDeviceVulkan11/12/13Features` onto `VkPhysicalDeviceFeatures2` and
prints `name=0/1` for every feature DXVK 2.7 marks as required, followed by a
`MISSING:` line.  Default ICD path is `C:\panvk\dev_vulkan_virtio.dll`; pass a
different path as argv[1].  Exit code is 0 when the DXVK requirement set is
met, 2 when something is missing, 1 on a probe error.

Build-only; no device credentials.
