// Minimal D3D11 offscreen smoke test for DroidVM Windows GPU bring-up.
// Builds with the Windows SDK; runs on ARM64 without a desktop session.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static bool check(HRESULT hr, const char *what) {
    if (FAILED(hr)) {
        std::printf("FAIL %s hr=0x%08lx\n", what, (unsigned long)hr);
        return false;
    }
    return true;
}

int main() {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    std::printf("D3D11CreateDevice(HARDWARE) hr=0x%08lx feature=0x%x\n", (unsigned long)hr, (unsigned)fl);
    if (FAILED(hr)) return 2;

    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        wprintf(L"adapter: %s vendor=0x%04x device=0x%04x\n",
                     desc.Description, desc.VendorId, desc.DeviceId);
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = 256; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = nullptr;
    if (!check(dev->CreateTexture2D(&td, nullptr, &tex), "CreateTexture2D")) return 3;
    ID3D11RenderTargetView *rtv = nullptr;
    if (!check(dev->CreateRenderTargetView(tex, nullptr, &rtv), "CreateRenderTargetView")) return 3;
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);
    ctx->Flush();

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    if (!check(dev->CreateTexture2D(&td, nullptr, &staging), "CreateStaging")) return 4;
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (!check(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map), "Map")) return 5;
    unsigned char *p = (unsigned char *)map.pData;
    std::printf("pixel: %u %u %u %u\n", p[0], p[1], p[2], p[3]);
    bool ok = p[0] > 50 && p[0] < 80 && p[1] > 110 && p[1] < 145 &&
              p[2] > 175 && p[2] < 210 && p[3] > 250;
    ctx->Unmap(staging, 0);
    if (staging) staging->Release();
    if (rtv) rtv->Release();
    if (tex) tex->Release();
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 6;
}
