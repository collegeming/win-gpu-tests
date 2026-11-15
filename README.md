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

`test_vk_compute.cpp` is a loader-free Vulkan compute round trip.  It creates a
device on queue family 0, allocates a 256-uint storage buffer, runs the kernel
in `compute.comp` (`v[i] += 1`) over four workgroups of 64 invocations, submits
with a fence, waits, maps the memory back and verifies every element.  It uses
a `HOST_VISIBLE|HOST_COHERENT` buffer when available and falls back to a
device-local buffer plus a host-visible staging buffer with `vkCmdCopyBuffer`.
`--nop` selects the empty kernel in `compute_nop.comp` (no descriptors) and
verifies the buffer comes back unchanged, which separates "compute pipelines
cannot be created" from "this storage-buffer shader cannot be compiled";
`--layout` keeps the empty kernel but still creates the storage-buffer
descriptor set layout, so the descriptor path and the shader compilation can be
told apart.
`compute_spirv.h` and `compute_nop_spirv.h` are generated in CI from the
`.comp` sources by the `spirv` job (`glslangValidator -V --target-env
vulkan1.0` then `xxd -i`); regenerate them locally with those commands, they
are not checked in.

Build-only; no device credentials.
