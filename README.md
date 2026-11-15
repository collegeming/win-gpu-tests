# win-gpu-tests

Minimal D3D11 offscreen smoke test for the DroidVM Windows ARM64 GPU stack.
It creates a hardware D3D11 device, clears a 256x256 RTV and reads the pixel
back through a staging texture.  A PASS means the KMD's D3D11 UMD plus the
Vulkan ICD (PanVK once installed) executed the clear and the copy.

Build-only; no device credentials.
