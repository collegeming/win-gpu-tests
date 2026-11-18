// Offscreen text-glyph rendering probe.
//
// Answers one question only: after an OFFSCREEN text draw with no window, no
// focus and no compositor, does the readback buffer actually contain ink?
//
// Path A  d3d11_d2d : D3D11 texture (BGRA8, 512x128, RENDER_TARGET) -> D2D
//                     DXGI-surface render target -> DirectWrite DrawTextLayout
//                     black on white -> GPU completion via Map(READ) on a
//                     STAGING copy -> CPU analysis.
// Path B  gdi       : CreateDIBSection (BGRA8 32bpp, top-down, 512x128) ->
//                     white FillRect -> TextOutW black -> GdiFlush -> CPU
//                     analysis of the section bits.
//
// Console subsystem. No window is created, no activation/focus/desktop API is
// touched, nothing is captured from the screen. Output is byte-deterministic
// ASCII plus UTF-8 adapter names.
//
// Exit codes: 0 = every executed path PASS (or --self-test PASS), 2 = setup or
// render or readback failure, or any path FAIL. Nothing is fabricated: on
// failure the exact stage, HRESULT and Win32 code are printed and the path is
// reported FAIL.
//
// A PASS means "dark pixels of a plausible size were found in the readback
// surface for this exact offscreen path". It does not prove on-screen
// presentation, DWM composition, scanout, or that a specific font face was
// used (fallback is not traced).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <d3dcompiler.h>
#include <winver.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWidth = 512;
constexpr int kHeight = 128;
// Text rectangle in surface pixels (half-open: right/bottom exclusive).
constexpr int kTextLeft = 8;
constexpr int kTextTop = 8;
constexpr int kTextRight = 504;
constexpr int kTextBottom = 120;
constexpr wchar_t kText[] = L"TEST 123";
constexpr UINT32 kTextLen = 8;
constexpr wchar_t kFontFamily[] = L"Segoe UI";
constexpr float kFontSize = 48.0f;
constexpr int kDarkLuma = 128;   // ITU-R BT.601 integer luma < 128 is "dark"
constexpr unsigned long long kMinDarkPixels = 50;
constexpr int kMinBBoxWidth = 40;
constexpr int kMinBBoxHeight = 8;

RECT text_rect() {
    RECT r{kTextLeft, kTextTop, kTextRight, kTextBottom};
    return r;
}

FILE* g_log = nullptr;

void out(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    const size_t len = (n < 0) ? strlen(buf) : static_cast<size_t>(n);
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    if (g_log) {
        fwrite(buf, 1, len, g_log);
        fflush(g_log);
    }
}

std::string utf8(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    // n counts the terminating NUL, so size the buffer for n and then drop it.
    std::string result(static_cast<size_t>(n), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, s, -1, result.data(), n, nullptr,
                                           nullptr);
    if (written <= 0) return {};
    result.resize(static_cast<size_t>(written) - 1);
    for (char& c : result)
        if (static_cast<unsigned char>(c) < 32) c = ' ';
    return result;
}

const char* fl_name(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        case D3D_FEATURE_LEVEL_9_3:  return "9_3";
        case D3D_FEATURE_LEVEL_9_2:  return "9_2";
        case D3D_FEATURE_LEVEL_9_1:  return "9_1";
        default:                     return "unknown";
    }
}

void print_module_version(const wchar_t* module, HMODULE loaded) {
    wchar_t path[MAX_PATH]{};
    if (loaded) {
        if (GetModuleFileNameW(loaded, path, MAX_PATH) == 0) path[0] = 0;
    } else {
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n + 16 >= MAX_PATH) {
            out("module_version name=%s status=path_unavailable win32=%lu\n",
                utf8(module).c_str(), GetLastError());
            return;
        }
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, module);
    }
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) {
        out("module_version name=%s status=no_version_resource win32=%lu\n",
            utf8(module).c_str(), GetLastError());
        return;
    }
    std::string data(size, '\0');
    if (!GetFileVersionInfoW(path, 0, size, data.data())) {
        out("module_version name=%s status=query_failed win32=%lu\n",
            utf8(module).c_str(), GetLastError());
        return;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        !info || info_size < sizeof(VS_FIXEDFILEINFO)) {
        out("module_version name=%s status=no_fixed_info\n", utf8(module).c_str());
        return;
    }
    out("module_version name=%s version=%lu.%lu.%lu.%lu path=%s\n",
        utf8(module).c_str(),
        static_cast<unsigned long>(HIWORD(info->dwFileVersionMS)),
        static_cast<unsigned long>(LOWORD(info->dwFileVersionMS)),
        static_cast<unsigned long>(HIWORD(info->dwFileVersionLS)),
        static_cast<unsigned long>(LOWORD(info->dwFileVersionLS)),
        utf8(path).c_str());
}

void print_environment() {
    DWORD session = 0;
    const BOOL have_session = ProcessIdToSessionId(GetCurrentProcessId(), &session);
    USHORT process_machine = 0;
    USHORT native_machine = 0;
    const BOOL have_machines = IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine);
    // IsWow64Process2 reports IMAGE_FILE_MACHINE_UNKNOWN for the process side
    // when the process is native, so wow64=no means "not emulated".
    const bool wow64 = have_machines && process_machine != IMAGE_FILE_MACHINE_UNKNOWN;
    out("probe=text_glyph_probe v=1 pid=%lu session=%lu session_query=%s wow64=%s"
        " process_machine=0x%04x native_machine=0x%04x machine_query=%s\n",
        GetCurrentProcessId(), have_session ? session : 0xfffffffful,
        have_session ? "ok" : "failed", wow64 ? "yes" : "no",
        static_cast<unsigned>(process_machine), static_cast<unsigned>(native_machine),
        have_machines ? "ok" : "failed");
    if (!have_session) out("win32=ProcessIdToSessionId code=%lu\n", GetLastError());
    out("surface=%dx%d format=BGRA8 text=\"%s\" font=\"%s\" size_px=%d"
        " text_rect=%d,%d,%d,%d luma_threshold=%d bbox_coords=half_open\n",
        kWidth, kHeight, "TEST 123", "Segoe UI", static_cast<int>(kFontSize),
        kTextLeft, kTextTop, kTextRight, kTextBottom, kDarkLuma);

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    out("dxgi_factory hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        for (UINT i = 0; i < 8; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT next = factory->EnumAdapters1(i, &adapter);
            if (next == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(next)) {
                out("dxgi_adapter[%u] status=enum_failed hr=0x%08lx\n", i,
                    static_cast<unsigned long>(next));
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            const HRESULT got = adapter->GetDesc1(&desc);
            if (FAILED(got)) {
                out("dxgi_adapter[%u] status=desc_failed hr=0x%08lx\n", i,
                    static_cast<unsigned long>(got));
                continue;
            }
            out("dxgi_adapter[%u] name=\"%s\" vendor=0x%04x device=0x%04x subsystem=0x%08x"
                " vram_mb=%llu flags=0x%x software=%s\n",
                i, utf8(desc.Description).c_str(), desc.VendorId, desc.DeviceId,
                desc.SubSysId,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024ull * 1024ull)),
                desc.Flags, (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "yes" : "no");
        }
    }
    // The version of the loaded D3D11 runtime is what actually executes the
    // draw, so record it rather than assuming an inbox build.
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) {
        out("module_version name=d3d11.dll status=load_failed win32=%lu\n", GetLastError());
    } else {
        print_module_version(L"d3d11.dll", d3d11);
        FreeLibrary(d3d11);
    }
}

struct PixelStats {
    unsigned long long dark = 0;
    unsigned long long nonwhite = 0;
    size_t unique_colors = 0;
    bool dark_bbox_valid = false;
    bool nonwhite_bbox_valid = false;
    int dark_left = 0, dark_top = 0, dark_right = 0, dark_bottom = 0;
    int nonwhite_left = 0, nonwhite_top = 0, nonwhite_right = 0, nonwhite_bottom = 0;
};

inline int luma(uint8_t b, uint8_t g, uint8_t r) {
    return (77 * static_cast<int>(r) + 150 * static_cast<int>(g) +
            29 * static_cast<int>(b)) >> 8;
}

void grow_dark(PixelStats& s, int x, int y) {
    if (!s.dark_bbox_valid) {
        s.dark_bbox_valid = true;
        s.dark_left = x; s.dark_top = y; s.dark_right = x + 1; s.dark_bottom = y + 1;
        return;
    }
    if (x < s.dark_left) s.dark_left = x;
    if (y < s.dark_top) s.dark_top = y;
    if (x + 1 > s.dark_right) s.dark_right = x + 1;
    if (y + 1 > s.dark_bottom) s.dark_bottom = y + 1;
}

void grow_nonwhite(PixelStats& s, int x, int y) {
    if (!s.nonwhite_bbox_valid) {
        s.nonwhite_bbox_valid = true;
        s.nonwhite_left = x; s.nonwhite_top = y;
        s.nonwhite_right = x + 1; s.nonwhite_bottom = y + 1;
        return;
    }
    if (x < s.nonwhite_left) s.nonwhite_left = x;
    if (y < s.nonwhite_top) s.nonwhite_top = y;
    if (x + 1 > s.nonwhite_right) s.nonwhite_right = x + 1;
    if (y + 1 > s.nonwhite_bottom) s.nonwhite_bottom = y + 1;
}

// Pure function over a BGRA8 top-down buffer; used by both paths and by
// --self-test with synthetic pixels so the analysis logic is verified without
// any GPU or font involvement.
PixelStats analyze(const uint8_t* data, int pitch, int width, int height, RECT rect) {
    PixelStats s;
    if (rect.left < 0) rect.left = 0;
    if (rect.top < 0) rect.top = 0;
    if (rect.right > width) rect.right = width;
    if (rect.bottom > height) rect.bottom = height;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return s;
    std::unordered_set<uint32_t> colors;
    colors.reserve(static_cast<size_t>((rect.right - rect.left) * (rect.bottom - rect.top)));
    for (int y = rect.top; y < rect.bottom; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * static_cast<size_t>(pitch);
        for (int x = rect.left; x < rect.right; ++x) {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            colors.insert((static_cast<uint32_t>(r) << 16) |
                          (static_cast<uint32_t>(g) << 8) | b);
            if (luma(b, g, r) < kDarkLuma) {
                ++s.dark;
                grow_dark(s, x, y);
            }
            if (b != 255 || g != 255 || r != 255) {
                ++s.nonwhite;
                grow_nonwhite(s, x, y);
            }
        }
    }
    s.unique_colors = colors.size();
    return s;
}

const char* verdict_reason(const PixelStats& s) {
    const long long area = static_cast<long long>(kTextRight - kTextLeft) *
                           (kTextBottom - kTextTop);
    if (s.dark <= kMinDarkPixels) return "dark_pixels_too_few";
    if (!s.dark_bbox_valid) return "dark_bbox_missing";
    const int w = s.dark_right - s.dark_left;
    const int h = s.dark_bottom - s.dark_top;
    if (w < kMinBBoxWidth || h < kMinBBoxHeight) return "dark_bbox_too_small";
    if (static_cast<long long>(s.dark) * 10 >= area * 9)
        return "dark_fill_dominates_clear_likely_failed";
    if (s.unique_colors < 2) return "single_color_no_antialiasing_or_no_text";
    return nullptr;
}

void print_result(const char* path, const PixelStats& s) {
    const char* reason = verdict_reason(s);
    char bbox[64] = "none";
    if (s.nonwhite_bbox_valid) {
        _snprintf_s(bbox, sizeof(bbox), _TRUNCATE, "%d,%d,%d,%d", s.nonwhite_left,
                    s.nonwhite_top, s.nonwhite_right, s.nonwhite_bottom);
    }
    char dark_bbox[64] = "none";
    if (s.dark_bbox_valid) {
        _snprintf_s(dark_bbox, sizeof(dark_bbox), _TRUNCATE, "%d,%d,%d,%d", s.dark_left,
                    s.dark_top, s.dark_right, s.dark_bottom);
    }
    out("PATH=%s RESULT=%s dark_pixel_count=%llu nonwhite_pixel_count=%llu"
        " unique_color_count=%zu bbox=%s dark_bbox=%s dark_bbox_size=%dx%d reason=%s\n",
        path, reason ? "FAIL" : "PASS", s.dark, s.nonwhite, s.unique_colors, bbox,
        dark_bbox, s.dark_bbox_valid ? s.dark_right - s.dark_left : 0,
        s.dark_bbox_valid ? s.dark_bottom - s.dark_top : 0,
        reason ? reason : "ok");
}

int g_failures = 0;

int run_d3d11_d2d(bool warp, bool debug_layer) {
    const char* path = "d3d11_d2d";
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required for D2D interop
    if (debug_layer) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_DRIVER_TYPE driver = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    const char* driver_name = warp ? "warp" : "hardware";
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_1_0_CORE;
    HRESULT hr = D3D11CreateDevice(nullptr, driver, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, &got,
                                   &context);
    if (hr == E_INVALIDARG) {
        // Pre-11.1 runtime rejects the 11_1 entry; retry without it.
        hr = D3D11CreateDevice(nullptr, driver, nullptr, flags, levels + 1,
                               ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &device, &got,
                               &context);
        out("PATH=%s note=retried_without_11_1\n", path);
    }
    out("PATH=%s stage=create_device driver=%s flags=0x%x hr=0x%08lx feature_level=%s\n",
        path, driver_name, flags, static_cast<unsigned long>(hr), fl_name(got));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL dark_pixel_count=0 nonwhite_pixel_count=0"
            " unique_color_count=0 bbox=none dark_bbox=none dark_bbox_size=0x0"
            " reason=create_device_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Debug> debug;
    ComPtr<ID3D11InfoQueue> queue;
    const HRESULT debug_hr = device.As(&debug);
    const HRESULT queue_hr = device.As(&queue);
    out("d3d11_runtime debug_layer_requested=%d debug_layer=%s info_queue=%s"
        " debug_hr=0x%08lx info_queue_hr=0x%08lx\n",
        debug_layer ? 1 : 0, SUCCEEDED(debug_hr) ? "present" : "absent",
        SUCCEEDED(queue_hr) ? "present" : "absent",
        static_cast<unsigned long>(debug_hr), static_cast<unsigned long>(queue_hr));

    ComPtr<IDXGIDevice> dxgi_device;
    hr = device.As(&dxgi_device);
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIAdapter> adapter;
        hr = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) {
            DXGI_ADAPTER_DESC desc{};
            hr = adapter->GetDesc(&desc);
            if (SUCCEEDED(hr)) {
                out("d3d11_device_adapter name=\"%s\" vendor=0x%04x device=0x%04x"
                    " subsystem=0x%08x dedicated_vram_mb=%llu\n",
                    utf8(desc.Description).c_str(), desc.VendorId, desc.DeviceId,
                    desc.SubSysId,
                    static_cast<unsigned long long>(desc.DedicatedVideoMemory /
                                                    (1024ull * 1024ull)));
            } else {
                out("d3d11_device_adapter status=desc_failed hr=0x%08lx\n",
                    static_cast<unsigned long>(hr));
            }
        } else {
            out("d3d11_device_adapter status=get_adapter_failed hr=0x%08lx\n",
                static_cast<unsigned long>(hr));
        }
    } else {
        out("d3d11_device_adapter status=dxgi_device_failed hr=0x%08lx\n",
            static_cast<unsigned long>(hr));
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    ComPtr<ID3D11Texture2D> target;
    hr = device->CreateTexture2D(&desc, nullptr, &target);
    out("PATH=%s stage=create_texture size=%dx%d format=BGRA8 usage=DEFAULT"
        " bind=RENDER_TARGET cpu_access=0 hr=0x%08lx\n", path, kWidth, kHeight,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL dark_pixel_count=0 nonwhite_pixel_count=0"
            " unique_color_count=0 bbox=none dark_bbox=none dark_bbox_size=0x0"
            " reason=create_texture_failed\n", path);
        return 2;
    }

    ComPtr<IDXGISurface> surface;
    hr = target.As(&surface);
    out("PATH=%s stage=query_dxgi_surface hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=dxgi_surface_failed\n", path);
        return 2;
    }

    ComPtr<ID2D1Factory> d2d_factory;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory.GetAddressOf());
    out("PATH=%s stage=d2d_create_factory hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=d2d_factory_failed\n", path);
        return 2;
    }

    const D2D1_RENDER_TARGET_TYPE rt_type =
        warp ? D2D1_RENDER_TARGET_TYPE_DEFAULT : D2D1_RENDER_TARGET_TYPE_HARDWARE;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        rt_type, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> rt;
    hr = d2d_factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &rt);
    out("PATH=%s stage=d2d_surface_rt target_type=%s dpi=96 alpha_ignore hr=0x%08lx\n",
        path, warp ? "default" : "hardware", static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=d2d_surface_rt_failed\n", path);
        return 2;
    }
    rt->SetDpi(96.0f, 96.0f);  // 1 DIP == 1 physical surface pixel

    IDWriteFactory* raw_dw = nullptr;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&raw_dw));
    out("PATH=%s stage=dwrite_create_factory hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    ComPtr<IDWriteFactory> dwrite;
    dwrite.Attach(raw_dw);
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=dwrite_factory_failed\n", path);
        return 2;
    }

    ComPtr<IDWriteTextFormat> format;
    hr = dwrite->CreateTextFormat(kFontFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  kFontSize, L"en-US", &format);
    out("PATH=%s stage=dwrite_text_format family=\"Segoe UI\" size_px=48 hr=0x%08lx\n",
        path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=text_format_failed\n", path);
        return 2;
    }
    hr = format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(hr)) {
        out("PATH=%s stage=dwrite_no_wrap hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }

    ComPtr<IDWriteTextLayout> layout;
    hr = dwrite->CreateTextLayout(kText, kTextLen, format.Get(),
                                  static_cast<float>(kTextRight - kTextLeft),
                                  static_cast<float>(kTextBottom - kTextTop), &layout);
    out("PATH=%s stage=dwrite_create_layout hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=text_layout_failed\n", path);
        return 2;
    }
    DWRITE_TEXT_METRICS metrics{};
    hr = layout->GetMetrics(&metrics);
    out("PATH=%s stage=dwrite_layout_metrics width=%.2f height=%.2f lines=%u"
        " hr=0x%08lx glyph_runs_traced=no\n", path, metrics.width, metrics.height,
        metrics.lineCount, static_cast<unsigned long>(hr));

    ComPtr<ID2D1SolidColorBrush> brush;
    hr = rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brush);
    out("PATH=%s stage=d2d_solid_brush hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=solid_brush_failed\n", path);
        return 2;
    }

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));  // opaque white background
    rt->DrawTextLayout(D2D1::Point2F(static_cast<float>(kTextLeft),
                                     static_cast<float>(kTextTop)),
                       layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    hr = rt->EndDraw();
    out("PATH=%s stage=end_draw clear=white text=black hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=end_draw_failed\n", path);
        return 2;
    }

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    out("PATH=%s stage=create_staging usage=STAGING cpu_access=READ"
        " format=BGRA8 hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=create_staging_failed\n", path);
        return 2;
    }
    context->CopyResource(staging.Get(), target.Get());
    context->Flush();
    out("PATH=%s stage=copy_resource hr=0x%08lx (CopyResource returns void;"
        " errors surface on Map)\n", path, static_cast<unsigned long>(S_OK));

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    out("PATH=%s stage=map_read row_pitch=%u hr=0x%08lx (Map READ blocks until the"
        " copy/GPU work completes; this is the synchronization point)\n",
        path, mapped.RowPitch, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=map_read_failed\n", path);
        return 2;
    }
    const PixelStats stats = analyze(static_cast<const uint8_t*>(mapped.pData),
                                     static_cast<int>(mapped.RowPitch), kWidth,
                                     kHeight, text_rect());
    context->Unmap(staging.Get(), 0);
    print_result(path, stats);
    if (!verdict_reason(stats)) return 0;
    ++g_failures;
    return 2;
}

int run_gdi() {
    const char* path = "gdi";
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kWidth;
    bmi.bmiHeader.biHeight = -kHeight;  // top-down, matches the D3D11 path
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    SetLastError(0);
    HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    const DWORD dib_error = GetLastError();
    out("PATH=%s stage=create_dibsection size=%dx%d bpp=32 top_down=yes"
        " win32=%lu hbitmap=%p bits=%p\n", path, kWidth, kHeight, dib_error,
        reinterpret_cast<void*>(dib), bits);
    if (!dib || !bits) {
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=create_dibsection_failed win32=%lu\n", path,
            dib_error);
        return 2;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    const DWORD dc_error = GetLastError();
    out("PATH=%s stage=create_compatible_dc win32=%lu hdc=%p\n", path, dc_error,
        reinterpret_cast<void*>(dc));
    if (!dc) {
        DeleteObject(dib);
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=create_dc_failed win32=%lu\n", path, dc_error);
        return 2;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, dib);
    if (!old_bitmap || old_bitmap == HGDI_ERROR) {
        DeleteDC(dc);
        DeleteObject(dib);
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=select_dib_failed win32=%lu\n", path,
            GetLastError());
        return 2;
    }

    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    RECT full{0, 0, kWidth, kHeight};
    SetLastError(0);
    const int filled = FillRect(dc, &full, white);
    const DWORD fill_error = GetLastError();
    out("PATH=%s stage=fill_rect color=white win32=%lu result=%d\n", path, fill_error,
        filled);

    HFONT font = CreateFontW(-static_cast<int>(kFontSize), 0, 0, 0, FW_NORMAL, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, kFontFamily);
    const DWORD font_error = GetLastError();
    out("PATH=%s stage=create_font family=\"Segoe UI\" height_px=%d win32=%lu hfont=%p\n",
        path, static_cast<int>(kFontSize), font_error, reinterpret_cast<void*>(font));
    HGDIOBJ old_font = nullptr;
    if (font) {
        old_font = SelectObject(dc, font);
        if (!old_font || old_font == HGDI_ERROR) {
            out("PATH=%s stage=select_font win32=%lu result=FAIL\n", path,
                GetLastError());
            old_font = nullptr;
        }
    }

    wchar_t face[128]{};
    const int face_len = (old_font && old_font != HGDI_ERROR)
                             ? GetTextFaceW(dc, ARRAYSIZE(face), face)
                             : 0;
    out("PATH=%s stage=get_text_face actual_face=\"%s\" face_len=%d win32=%lu"
        " requested_face=%s\n", path, utf8(face).c_str(), face_len, GetLastError(),
        "Segoe UI");

    const int bk = SetBkMode(dc, TRANSPARENT);
    const COLORREF previous = SetTextColor(dc, RGB(0, 0, 0));
    SetLastError(0);
    const BOOL drew = TextOutW(dc, kTextLeft, kTextTop, kText, static_cast<int>(kTextLen));
    const DWORD text_error = drew ? 0 : GetLastError();
    out("PATH=%s stage=text_out text=\"TEST 123\" origin=%d,%d color=black"
        " bk_mode=transparent bk_result=%d prev_color=0x%08lx result=%d win32=%lu\n",
        path, kTextLeft, kTextTop, bk, static_cast<unsigned long>(previous), drew,
        text_error);

    SetLastError(0);
    const BOOL flushed = GdiFlush();
    const DWORD flush_error = GetLastError();
    out("PATH=%s stage=gdi_flush result=%d win32=%lu\n", path, flushed, flush_error);

    // Detach everything before touching the section bits so the CPU view is
    // final and no GDI handle is left selected.
    if (old_font && old_font != HGDI_ERROR) SelectObject(dc, old_font);
    if (font) DeleteObject(font);
    SelectObject(dc, old_bitmap);
    if (white) DeleteObject(white);
    DeleteDC(dc);

    if (!drew || !flushed) {
        DeleteObject(dib);
        ++g_failures;
        out("PATH=%s RESULT=FAIL reason=text_out_or_flush_failed win32=%lu\n", path,
            drew ? flush_error : text_error);
        return 2;
    }

    const PixelStats stats =
        analyze(static_cast<const uint8_t*>(bits), kWidth * 4, kWidth, kHeight,
                text_rect());
    DeleteObject(dib);
    print_result(path, stats);
    if (!verdict_reason(stats)) return 0;
    ++g_failures;
    return 2;
}

// Synthetic pixels only: no device, no font, no GDI. Proves the analyzer and
// the PASS/FAIL gate behave as documented before any hardware run.
int run_self_test() {
    const int pitch = kWidth * 4;
    std::vector<uint8_t> buffer(static_cast<size_t>(pitch) * kHeight);
    auto fill = [&](uint8_t b, uint8_t g, uint8_t r) {
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x) {
                uint8_t* p = buffer.data() + static_cast<size_t>(y) * pitch + x * 4;
                p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
            }
    };
    auto put = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
        uint8_t* p = buffer.data() + static_cast<size_t>(y) * pitch + x * 4;
        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
    };
    int failures = 0;
    int cases = 0;
    auto expect = [&](const char* name, bool ok) {
        ++cases;
        out("SELF_TEST_CASE name=%s result=%s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    };

    fill(255, 255, 255);
    for (int y = 30; y < 60; ++y)
        for (int x = 20; x < 120; ++x) put(x, y, 0, 0, 0);
    PixelStats s = analyze(buffer.data(), pitch, kWidth, kHeight, text_rect());
    expect("white_with_black_block", verdict_reason(s) == nullptr && s.dark == 3000 &&
                                         s.nonwhite == 3000 && s.unique_colors == 2 &&
                                         s.dark_left == 20 && s.dark_top == 30 &&
                                         s.dark_right == 120 && s.dark_bottom == 60);

    fill(255, 255, 255);
    s = analyze(buffer.data(), pitch, kWidth, kHeight, text_rect());
    expect("blank_white_must_fail",
           verdict_reason(s) != nullptr && s.dark == 0 && !s.dark_bbox_valid);

    fill(0, 0, 0);
    s = analyze(buffer.data(), pitch, kWidth, kHeight, text_rect());
    expect("all_black_must_fail", verdict_reason(s) != nullptr && s.dark > 50000);

    // Many shades prove unique_color_count tracks distinct RGB values, and a
    // small dark block inside a non-white field must still pass on dark bbox.
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const uint8_t v = static_cast<uint8_t>(200 + ((x + y) % 40));
            put(x, y, v, v, v);
        }
    for (int y = 40; y < 52; ++y)
        for (int x = 60; x < 120; ++x) put(x, y, 0, 0, 0);
    s = analyze(buffer.data(), pitch, kWidth, kHeight, text_rect());
    expect("shaded_field_dark_block",
           verdict_reason(s) == nullptr && s.dark == 720 && s.unique_colors >= 40 &&
               s.dark_left == 60 && s.dark_top == 40 && s.dark_right == 120 &&
               s.dark_bottom == 52);

    // Threshold boundary: luma 127 is dark, luma 128 is not.
    fill(255, 255, 255);
    for (int y = 20; y < 40; ++y)
        for (int x = 20; x < 100; ++x) put(x, y, 128, 128, 128);
    s = analyze(buffer.data(), pitch, kWidth, kHeight, text_rect());
    expect("luma_128_not_dark", s.dark == 0 && s.nonwhite == 1600);
    for (int y = 20; y < 40; ++y)
        for (int x = 20; x < 100; ++x) put(x, y, 127, 127, 127);
    s = analyze(buffer.data(), pitch, kWidth, kHeight, text_rect());
    expect("luma_127_is_dark", s.dark == 1600);

    out("SELF_TEST=%s cases=%d failures=%d\n", failures ? "FAIL" : "PASS", cases,
        failures);
    return failures ? 2 : 0;
}

// ---------------------------------------------------------------------------
// v2 diagnostic modes. Additive: the full-mode paths above and their verdicts
// are unchanged.
//
// Motivation from the device run: on the target GPU the whole d3d11_d2d chain
// returned hr=0 (device, texture, D2D surface RT, DWrite layout, brush,
// EndDraw, CopyResource, Map all succeeded) but the readback held a single
// colour with zero ink, while GDI on the same machine passed and the same exe
// passed through WARP. That rules out setup failures and points at the draw
// itself. These modes split "the draw" into separately measurable items:
//
//   --mode d2d-primitives   solid geometry fill, 1px geometry stroke and glyphs
//                           in ONE target and ONE readback, so "every D2D draw
//                           is lost" can be told apart from "only glyphs are
//                           lost".
//   --mode dwrite-offscreen one glyph draw per text antialias mode, each into
//                           its own texture and readback (nothing is
//                           overwritten before it is measured).
//   --mode bitmap-brush     a 1x1 known-colour bitmap brush and a 16x16
//                           DrawBitmap, to separate texture sampling from
//                           geometry.
//   --mode a8-contract      the A8_UNORM value contract through Clear, a pixel
//                           shader with and without blending, and a typed UAV
//                           store followed by an SRV sample. Text masks are A8,
//                           so a wrong channel or a truncated fraction looks
//                           exactly like "no glyphs".
//   --mode drawline         one long 2px line, antialiased and aliased, each on
//                           its own target: a solid fill measured correct while
//                           a 1px stroke measured white points at the coverage
//                           rasterizer rather than at the D2D surface path.
//
// Items are judged by presence of the expected pixels, not by the full-mode
// text rule: a solid fill is expected to be 100% dark and single-coloured, so
// "dark must not dominate" and "unique_color_count >= 2" cannot apply.

struct Rgba {
    int b = 0, g = 0, r = 0, a = 0;
};

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> format;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_1_0_CORE;
};

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1RenderTarget> rt;
};

// A 1px stroke over a 64x64 outline, or a 64x64 fill, is far smaller than the
// text rect, so item bboxes use their own lower bound.
constexpr int kItemMinBBoxWidth = 8;
constexpr int kItemMinBBoxHeight = 4;

void gpu_adapter(const char* path, ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()));
    if (FAILED(hr)) {
        out("MODE=%s adapter status=dxgi_device_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return;
    }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi->GetAdapter(&adapter);
    if (FAILED(hr)) {
        out("MODE=%s adapter status=get_adapter_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return;
    }
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) {
        out("MODE=%s adapter status=desc_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return;
    }
    out("MODE=%s adapter name=\"%s\" vendor=0x%04x device=0x%04x subsystem=0x%08x"
        " dedicated_vram_mb=%llu\n",
        path, utf8(desc.Description).c_str(), desc.VendorId, desc.DeviceId, desc.SubSysId,
        static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024ull * 1024ull)));
}

bool gpu_create(const char* path, bool warp, bool debug_layer, Gpu& g) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (debug_layer) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_DRIVER_TYPE driver = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    HRESULT hr = D3D11CreateDevice(nullptr, driver, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &g.device, &g.fl,
                                   &g.context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, driver, nullptr, flags, levels + 1,
                               ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &g.device, &g.fl,
                               &g.context);
    }
    out("MODE=%s stage=create_device driver=%s feature_level=%s hr=0x%08lx\n", path,
        warp ? "warp" : "hardware", fl_name(g.fl), static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g.d2d.GetAddressOf());
    out("MODE=%s stage=d2d_create_factory hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(g.dwrite.GetAddressOf()));
    out("MODE=%s stage=dwrite_create_factory hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    hr = g.dwrite->CreateTextFormat(kFontFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    kFontSize, L"en-US", &g.format);
    out("MODE=%s stage=dwrite_text_format family=\"Segoe UI\" size_px=48"
        " hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

bool target_create(const char* path, const Gpu& g, bool warp, Target& t) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = g.device->CreateTexture2D(&desc, nullptr, &t.tex);
    out("MODE=%s stage=create_texture size=%dx%d format=BGRA8 bind=RENDER_TARGET"
        " hr=0x%08lx\n", path, kWidth, kHeight, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    hr = t.tex.As(&t.surface);
    if (FAILED(hr)) {
        out("MODE=%s stage=query_dxgi_surface hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        warp ? D2D1_RENDER_TARGET_TYPE_DEFAULT : D2D1_RENDER_TARGET_TYPE_HARDWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f,
        96.0f);
    hr = g.d2d->CreateDxgiSurfaceRenderTarget(t.surface.Get(), &props, &t.rt);
    out("MODE=%s stage=d2d_surface_rt target_type=%s hr=0x%08lx\n", path,
        warp ? "default" : "hardware", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    t.rt->SetDpi(96.0f, 96.0f);

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g.device->CreateTexture2D(&staging_desc, nullptr, &t.staging);
    if (FAILED(hr)) {
        out("MODE=%s stage=create_staging hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return false;
    }
    const D2D1_SIZE_U px = t.rt->GetPixelSize();
    FLOAT dpi_x = 0.0f;
    FLOAT dpi_y = 0.0f;
    t.rt->GetDpi(&dpi_x, &dpi_y);
    out("MODE=%s stage=target_ready d2d_pixel_size=%ux%u d2d_dpi=%.1fx%.1f\n", path,
        px.width, px.height, dpi_x, dpi_y);
    return true;
}

bool target_map(const char* path, Gpu& g, Target& t, D3D11_MAPPED_SUBRESOURCE& mapped) {
    g.context->CopyResource(t.staging.Get(), t.tex.Get());
    g.context->Flush();
    const HRESULT hr = g.context->Map(t.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    out("MODE=%s stage=map_read row_pitch=%u hr=0x%08lx\n", path, mapped.RowPitch,
        static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

RECT clipped(RECT r, int width, int height) {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > width) r.right = width;
    if (r.bottom > height) r.bottom = height;
    return r;
}

Rgba pixel_at(const uint8_t* data, int pitch, int x, int y) {
    const uint8_t* p = data + static_cast<size_t>(y) * static_cast<size_t>(pitch) + x * 4;
    Rgba c;
    c.b = p[0];
    c.g = p[1];
    c.r = p[2];
    c.a = p[3];
    return c;
}

unsigned long long count_color_match(const uint8_t* data, int pitch, RECT rect, Rgba want,
                                     int tolerance) {
    rect = clipped(rect, kWidth, kHeight);
    unsigned long long n = 0;
    for (int y = rect.top; y < rect.bottom; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * static_cast<size_t>(pitch);
        for (int x = rect.left; x < rect.right; ++x) {
            const int db = static_cast<int>(row[x * 4 + 0]) - want.b;
            const int dg = static_cast<int>(row[x * 4 + 1]) - want.g;
            const int dr = static_cast<int>(row[x * 4 + 2]) - want.r;
            if (db >= -tolerance && db <= tolerance && dg >= -tolerance &&
                dg <= tolerance && dr >= -tolerance && dr <= tolerance)
                ++n;
        }
    }
    return n;
}

const char* ink_reason(const PixelStats& s) {
    if (s.dark <= kMinDarkPixels) return "dark_pixels_too_few";
    if (!s.dark_bbox_valid) return "dark_bbox_missing";
    const int w = s.dark_right - s.dark_left;
    const int h = s.dark_bottom - s.dark_top;
    if (w < kItemMinBBoxWidth || h < kItemMinBBoxHeight) return "dark_bbox_too_small";
    return nullptr;
}

const char* color_reason(unsigned long long matched, long long area) {
    if (area <= 0) return "empty_region";
    if (matched * 10 < static_cast<unsigned long long>(area) * 9)
        return "expected_color_not_dominant";
    return nullptr;
}

const char* line_reason(const PixelStats& s) {
    if (s.dark <= kMinDarkPixels) return "dark_pixels_too_few";
    if (!s.dark_bbox_valid) return "dark_bbox_missing";
    const int w = s.dark_right - s.dark_left;
    const int h = s.dark_bottom - s.dark_top;
    // A 2px stroke is legitimately flat, so only width and a lower bound of
    // one row are required here; the text rule's 40x8 would reject it.
    if (w < 100) return "dark_bbox_too_narrow";
    if (h < 1) return "dark_bbox_too_flat";
    return nullptr;
}

// A half-pixel line can only produce ink through the coverage path, and a
// 50%-covered black-on-white pixel is mid-grey (luma about 127), which the
// luma < 128 rule classifies unpredictably. So a sub-pixel line is judged on
// non-white pixels: any coverage at all, partial or full, proves the AA
// rasterizer produced something. Absence of non-white pixels is the finding.
const char* coverage_reason(const PixelStats& s) {
    if (s.nonwhite <= kMinDarkPixels) return "no_ink_at_all";
    if (!s.nonwhite_bbox_valid) return "nonwhite_bbox_missing";
    const int w = s.nonwhite_right - s.nonwhite_left;
    if (w < 100) return "nonwhite_bbox_too_narrow";
    return nullptr;
}

const char* print_ink_item_with(const char* mode, const char* item, const char* extra,
                                const PixelStats& s, const char* reason) {
    char bbox[64] = "none";
    if (s.nonwhite_bbox_valid) {
        _snprintf_s(bbox, sizeof(bbox), _TRUNCATE, "%d,%d,%d,%d", s.nonwhite_left,
                    s.nonwhite_top, s.nonwhite_right, s.nonwhite_bottom);
    }
    out("MODE=%s ITEM=%s%s RESULT=%s dark_pixel_count=%llu nonwhite_pixel_count=%llu"
        " unique_color_count=%zu bbox=%s dark_bbox_size=%dx%d reason=%s\n",
        mode, item, extra, reason ? "FAIL" : "PASS", s.dark, s.nonwhite, s.unique_colors,
        bbox, s.dark_bbox_valid ? s.dark_right - s.dark_left : 0,
        s.dark_bbox_valid ? s.dark_bottom - s.dark_top : 0, reason ? reason : "ok");
    if (reason) ++g_failures;
    return reason;
}

const char* print_ink_item(const char* mode, const char* item, const char* extra,
                           const PixelStats& s) {
    return print_ink_item_with(mode, item, extra, s, ink_reason(s));
}

const char* print_line_item(const char* mode, const char* item, const char* extra,
                            const PixelStats& s) {
    return print_ink_item_with(mode, item, extra, s, line_reason(s));
}

const char* print_color_item(const char* mode, const char* item, Rgba want, Rgba got,
                             unsigned long long matched, long long area,
                             const PixelStats& s) {
    const char* reason = color_reason(matched, area);
    out("MODE=%s ITEM=%s expected_bgra=%d,%d,%d,%d sampled_bgra=%d,%d,%d,%d"
        " matched=%llu region_area=%lld RESULT=%s dark_pixel_count=%llu"
        " nonwhite_pixel_count=%llu unique_color_count=%zu reason=%s\n",
        mode, item, want.b, want.g, want.r, want.a, got.b, got.g, got.r, got.a, matched,
        area, reason ? "FAIL" : "PASS", s.dark, s.nonwhite, s.unique_colors,
        reason ? reason : "ok");
    if (reason) ++g_failures;
    return reason;
}

// Item 1: one target, one readback, three independently judged items.
int run_d2d_primitives(bool warp, bool debug_layer) {
    const char* path = "d2d-primitives";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());
    Target t;
    if (!target_create(path, g, warp, t)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=target_create_failed\n", path);
        return 2;
    }

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = g.dwrite->CreateTextLayout(kText, kTextLen, g.format.Get(),
                                            512.0f - 160.0f, 112.0f, &layout);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s stage=dwrite_create_layout hr=0x%08lx RESULT=FAIL"
            " reason=text_layout_failed\n", path, static_cast<unsigned long>(hr));
        return 2;
    }
    ComPtr<ID2D1SolidColorBrush> black;
    hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &black);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s stage=d2d_solid_brush hr=0x%08lx RESULT=FAIL"
            " reason=solid_brush_failed\n", path, static_cast<unsigned long>(hr));
        return 2;
    }

    t.rt->BeginDraw();
    t.rt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    t.rt->FillRectangle(D2D1::RectF(8.0f, 8.0f, 72.0f, 72.0f), black.Get());
    // Integer-aligned 1px stroke: path at half pixels covers exactly 1px.
    t.rt->DrawRectangle(D2D1::RectF(80.5f, 8.5f, 143.5f, 71.5f), black.Get(), 1.0f);
    t.rt->DrawTextLayout(D2D1::Point2F(160.0f, 8.0f), layout.Get(), black.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_NONE);
    hr = t.rt->EndDraw();
    out("MODE=%s stage=end_draw items=fill(8,8,64x64) stroke(80.5,8.5,63x63,1px)"
        " text(160,8) hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=end_draw_failed\n", path);
        return 2;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!target_map(path, g, t, mapped)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=map_read_failed\n", path);
        return 2;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped.pData);
    const int pitch = static_cast<int>(mapped.RowPitch);
    const PixelStats fill = analyze(base, pitch, kWidth, kHeight, RECT{4, 4, 76, 76});
    const PixelStats stroke = analyze(base, pitch, kWidth, kHeight, RECT{76, 4, 148, 76});
    const PixelStats text = analyze(base, pitch, kWidth, kHeight, RECT{152, 4, 508, 124});
    g.context->Unmap(t.staging.Get(), 0);

    print_ink_item(path, "rect_fill", " region=4,4,76,76", fill);
    print_ink_item(path, "rect_stroke", " region=76,4,148,76", stroke);
    print_ink_item(path, "text", " region=152,4,508,124", text);
    const bool geometry_ink = fill.dark > kMinDarkPixels || stroke.dark > kMinDarkPixels;
    const bool glyph_ink = text.dark > kMinDarkPixels;
    const int failed = (ink_reason(fill) ? 1 : 0) + (ink_reason(stroke) ? 1 : 0) +
                       (ink_reason(text) ? 1 : 0);
    out("MODE=%s SUMMARY items=3 failed=%d geometry_ink=%s glyph_ink=%s"
        " all_draws_lost=%s\n", path, failed, geometry_ink ? "yes" : "no",
        glyph_ink ? "yes" : "no", (!geometry_ink && !glyph_ink) ? "yes" : "no");
    return failed ? 2 : 0;
}

// Item 2: one glyph draw per text antialias mode, each on its own texture.
int run_dwrite_offscreen(bool warp, bool debug_layer) {
    const char* path = "dwrite-offscreen";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = g.dwrite->CreateTextLayout(kText, kTextLen, g.format.Get(),
                                            496.0f, 112.0f, &layout);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s stage=dwrite_create_layout hr=0x%08lx RESULT=FAIL"
            " reason=text_layout_failed\n", path, static_cast<unsigned long>(hr));
        return 2;
    }

    struct AaCase {
        D2D1_TEXT_ANTIALIAS_MODE mode;
        const char* name;
    };
    const AaCase cases[] = {
        {D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE, "aa_cleartype"},
        {D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, "aa_grayscale"},
        {D2D1_TEXT_ANTIALIAS_MODE_ALIASED, "aa_aliased"},
    };

    int failed = 0;
    for (const AaCase& c : cases) {
        // A fresh texture per case: no case can be hidden by a later draw
        // overwriting the same surface before it is read back.
        Target t;
        if (!target_create(path, g, warp, t)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed\n", path,
                c.name);
            continue;
        }
        t.rt->SetTextAntialiasMode(c.mode);
        const D2D1_TEXT_ANTIALIAS_MODE effective = t.rt->GetTextAntialiasMode();
        ComPtr<ID2D1SolidColorBrush> black;
        hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &black);
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s stage=d2d_solid_brush hr=0x%08lx RESULT=FAIL\n", path,
                c.name, static_cast<unsigned long>(hr));
            continue;
        }
        t.rt->BeginDraw();
        t.rt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        t.rt->DrawTextLayout(D2D1::Point2F(8.0f, 8.0f), layout.Get(), black.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
        hr = t.rt->EndDraw();
        char extra[96];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " requested_aa=%d effective_aa=%d end_draw_hr=0x%08lx",
                    static_cast<int>(c.mode), static_cast<int>(effective),
                    static_cast<unsigned long>(hr));
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s%s RESULT=FAIL reason=end_draw_failed\n", path, c.name,
                extra);
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!target_map(path, g, t, mapped)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s%s RESULT=FAIL reason=map_read_failed\n", path, c.name,
                extra);
            continue;
        }
        const PixelStats s = analyze(static_cast<const uint8_t*>(mapped.pData),
                                     static_cast<int>(mapped.RowPitch), kWidth, kHeight,
                                     text_rect());
        g.context->Unmap(t.staging.Get(), 0);
        if (print_ink_item(path, c.name, extra, s)) ++failed;
    }
    out("MODE=%s SUMMARY items=3 failed=%d textures=3 separate_readbacks=yes\n", path,
        failed);
    return failed ? 2 : 0;
}

// Item 3: known-colour bitmaps, to separate texture sampling from geometry.
int run_bitmap_brush(bool warp, bool debug_layer) {
    const char* path = "bitmap-brush";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());
    Target t;
    if (!target_create(path, g, warp, t)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=target_create_failed\n", path);
        return 2;
    }

    const D2D1_BITMAP_PROPERTIES bitmap_props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                 D2D1_ALPHA_MODE_IGNORE),
                               96.0f, 96.0f);
    const uint8_t red_px[4] = {0, 0, 255, 255};    // BGRA: pure red
    const uint8_t green_px[4] = {0, 255, 0, 255};  // BGRA: pure green
    ComPtr<ID2D1Bitmap> red_bitmap;
    HRESULT hr = t.rt->CreateBitmap(D2D1::SizeU(1, 1), red_px, 4, bitmap_props,
                                    &red_bitmap);
    out("MODE=%s stage=create_bitmap size=1x1 color=red hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=create_bitmap_failed\n", path);
        return 2;
    }
    ComPtr<ID2D1Bitmap> green_bitmap;
    hr = t.rt->CreateBitmap(D2D1::SizeU(1, 1), green_px, 4, bitmap_props, &green_bitmap);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=create_green_bitmap_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    ComPtr<ID2D1BitmapBrush> brush;
    const D2D1_BITMAP_BRUSH_PROPERTIES brush_props = D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    hr = t.rt->CreateBitmapBrush(red_bitmap.Get(), brush_props, &brush);
    out("MODE=%s stage=create_bitmap_brush extend=clamp interpolation=nearest"
        " hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=create_bitmap_brush_failed\n", path);
        return 2;
    }

    t.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // keep edges exact
    t.rt->BeginDraw();
    t.rt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    t.rt->FillRectangle(D2D1::RectF(8.0f, 8.0f, 72.0f, 72.0f), brush.Get());
    t.rt->DrawBitmap(green_bitmap.Get(),
                     D2D1::RectF(200.0f, 8.0f, 264.0f, 72.0f), 1.0f,
                     D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    hr = t.rt->EndDraw();
    out("MODE=%s stage=end_draw items=brush(8,8,64x64) draw_bitmap(200,8,64x64)"
        " hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=end_draw_failed\n", path);
        return 2;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!target_map(path, g, t, mapped)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=map_read_failed\n", path);
        return 2;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped.pData);
    const int pitch = static_cast<int>(mapped.RowPitch);
    // Regions are exactly the drawn rects: the colour gate is a ratio of the
    // region, so padding would count as mismatched pixels.
    const RECT red_rect{8, 8, 72, 72};
    const RECT green_rect{200, 8, 264, 72};
    Rgba want_red{0, 0, 255, 255};
    Rgba want_green{0, 255, 0, 255};
    const Rgba got_red = pixel_at(base, pitch, 40, 40);
    const Rgba got_green = pixel_at(base, pitch, 232, 40);
    const unsigned long long red_matched = count_color_match(base, pitch, red_rect,
                                                             want_red, 2);
    const unsigned long long green_matched = count_color_match(base, pitch, green_rect,
                                                               want_green, 2);
    const PixelStats red_stats = analyze(base, pitch, kWidth, kHeight, red_rect);
    const PixelStats green_stats = analyze(base, pitch, kWidth, kHeight, green_rect);
    g.context->Unmap(t.staging.Get(), 0);

    const long long red_area = static_cast<long long>(red_rect.right - red_rect.left) *
                               (red_rect.bottom - red_rect.top);
    const long long green_area =
        static_cast<long long>(green_rect.right - green_rect.left) *
        (green_rect.bottom - green_rect.top);
    const char* red_reason =
        print_color_item(path, "bitmap_brush_fill", want_red, got_red, red_matched,
                         red_area, red_stats);
    const char* green_reason =
        print_color_item(path, "draw_bitmap", want_green, got_green, green_matched,
                         green_area, green_stats);
    out("MODE=%s SUMMARY items=2 failed=%d sampling_vs_geometry=%s\n", path,
        (red_reason || green_reason) ? 1 : 0,
        red_reason ? "bitmap_sampling_lost" : "bitmap_sampling_ok");
    return (red_reason || green_reason) ? 2 : 0;
}

// ---------------------------------------------------------------------------
// v3: A8_UNORM value contract, and AA-dependent line drawing.
//
// The A8 contract mode answers: for an A8_UNORM render target, does a value
// written by Clear, by a pixel shader, by a blended pixel shader, or by a typed
// UAV store land in the single channel that a readback sees as R?
//
// Every case gets its own texture and its own clear, so no case can be polluted
// by a previous write. Five probe points are read and every one of them must be
// within kA8Tolerance of the expected value.
// ---------------------------------------------------------------------------

constexpr int kA8Size = 256;
constexpr UINT kA8Tolerance = 2;

const char* kA8Vs =
    "struct VSOut { float4 pos : SV_POSITION; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "    float2 p = float2(float((id << 1) & 2u), float(id & 2u));\n"
    "    VSOut o;\n"
    "    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

const char* kA8PsHalf =
    "float4 main() : SV_Target {\n"
    "    return float4(1.0, 1.0, 1.0, 0.5);\n"
    "}\n";

// The shader replicates the sampled value into all four channels: for A8_UNORM
// the readback channel and the shader component that carries the value are not
// guaranteed to agree, and this case must measure the sampled value, not the
// component mapping.
const char* kA8PsSample =
    "Texture2D<float4> src : register(t0);\n"
    "float4 main() : SV_Target {\n"
    "    float4 s = src.Load(int3(128, 128, 0));\n"
    "    float v = max(s.r, s.a);\n"
    "    return float4(v, v, v, v);\n"
    "}\n";

// The value is 0.5 in all four channels on purpose: A8_UNORM does not promise
// which shader component it stores, so a value written only to .w would be
// ambiguous here. Storing the same value everywhere makes the expected readback
// (128) independent of the channel question. The channel question is measured
// once, separately, by ps_half_no_blend, which writes .x=1.0 and .w=0.5.
const char* kA8CsStore =
    "RWTexture2D<float4> dst : register(u0);\n"
    "[numthreads(16, 16, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x == 128u && id.y == 128u) dst[id.xy] = float4(0.5, 0.5, 0.5, 0.5);\n"
    "}\n";

HRESULT a8_compile(const char* path, const char* label, const char* source, const char* hi,
                   const char* lo, ID3DBlob** blob, const char** used) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), label, nullptr, nullptr, "main", hi, 0, 0,
                            blob, &errors);
    if (SUCCEEDED(hr)) {
        *used = hi;
        out("MODE=%s stage=compile_shader shader=%s profile=%s hr=0x%08lx\n", path, label, hi,
            static_cast<unsigned long>(hr));
        return hr;
    }
    if (errors) {
        const char* text = static_cast<const char*>(errors->GetBufferPointer());
        char first[200];
        size_t n = 0;
        while (n < sizeof(first) - 1 && text[n] != 0 && text[n] != '\n' && text[n] != '\r') {
            first[n] = text[n];
            ++n;
        }
        first[n] = 0;
        out("MODE=%s stage=compile_shader shader=%s profile=%s hr=0x%08lx error=\"%s\"\n", path,
            label, hi, static_cast<unsigned long>(hr), first);
        errors->Release();
    }
    ID3DBlob* retry = nullptr;
    hr = D3DCompile(source, strlen(source), label, nullptr, nullptr, "main", lo, 0, 0, &retry,
                    nullptr);
    out("MODE=%s stage=compile_shader shader=%s profile=%s retry_hr=0x%08lx\n", path, label, lo,
        static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        *blob = retry;
        *used = lo;
    }
    return hr;
}

void a8_support(const char* path, ID3D11Device* device) {
    UINT flags = 0;
    const HRESULT hr = device->CheckFormatSupport(DXGI_FORMAT_A8_UNORM, &flags);
    const char* yes_no[] = {"no", "yes"};
    out("MODE=%s stage=format_support fmt=A8_UNORM hr=0x%08lx flags=0x%08x texture2d=%s"
        " render_target=%s blendable=%s shader_sample=%s typed_uav=%s\n", path,
        static_cast<unsigned long>(hr), flags,
        yes_no[(flags & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? 1 : 0],
        yes_no[(flags & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? 1 : 0],
        yes_no[(flags & D3D11_FORMAT_SUPPORT_BLENDABLE) ? 1 : 0],
        yes_no[(flags & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? 1 : 0],
        yes_no[(flags & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) ? 1 : 0]);
}

bool a8_target(const char* path, ID3D11Device* device, UINT bind_flags,
               ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11Texture2D>& staging) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kA8Size;
    desc.Height = kA8Size;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
    out("MODE=%s stage=create_a8_texture size=%dx%d bind=0x%x hr=0x%08lx\n", path, kA8Size,
        kA8Size, bind_flags, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    out("MODE=%s stage=create_a8_staging hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

struct A8Probe {
    unsigned values[5] = {0, 0, 0, 0, 0};
    unsigned center = 0;
    bool ok = false;
    HRESULT hr_map = E_FAIL;
};

A8Probe a8_read(const char* path, ID3D11DeviceContext* context, ID3D11Texture2D* tex,
                ID3D11Texture2D* staging) {
    A8Probe probe;
    context->CopyResource(staging, tex);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    probe.hr_map = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(probe.hr_map)) {
        out("MODE=%s stage=map_a8_read hr=0x%08lx\n", path,
            static_cast<unsigned long>(probe.hr_map));
        return probe;
    }
    const int xs[5] = {128, 16, 240, 64, 192};
    const int ys[5] = {128, 16, 240, 192, 64};
    for (int i = 0; i < 5; ++i) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) +
                             static_cast<size_t>(ys[i]) * mapped.RowPitch;
        probe.values[i] = row[xs[i]];
    }
    probe.center = probe.values[0];
    probe.ok = true;
    context->Unmap(staging, 0);
    return probe;
}

unsigned a8_absdiff(unsigned a, unsigned b) {
    return a > b ? a - b : b - a;
}

// hr_ps is the pixel shader creation/compile result for this case (S_OK for the
// clear-only cases, which use no shader at all). hr_draw is
// ID3D11Device::GetDeviceRemovedReason() taken after the draw plus Flush, since
// ID3D11DeviceContext::Draw returns void; a device removal or hang shows up here
// and nowhere else. hr_map is reported separately on the A8DETAIL line.
//
// Several expected values are allowed where the D3D11 format rules make more than
// one outcome legal for A8_UNORM, e.g. a shader output float4(1,1,1,0.5) may be
// stored from .x (255) or from .w (128) because A8 has a single channel and does
// not fix which shader component feeds it. Every allowed value is listed on the
// A8CASE line, so a legal-but-unexpected mapping is visible instead of being
// reported as a device defect.
const char* print_a8_case_values(const char* mode, const char* name, HRESULT hr_ps,
                                 HRESULT hr_draw, const A8Probe& probe,
                                 const unsigned* expected, int expected_count,
                                 const char* detail) {
    const char* reason = nullptr;
    if (!probe.ok) {
        reason = "map_read_failed";
    } else {
        for (int i = 0; i < 5; ++i) {
            bool matched = false;
            for (int e = 0; e < expected_count; ++e) {
                if (a8_absdiff(probe.values[i], expected[e]) <= kA8Tolerance) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                reason = "value_mismatch";
                break;
            }
        }
    }
    char expected_text[48] = "0";
    size_t used = 0;
    for (int e = 0; e < expected_count && used < sizeof(expected_text) - 8; ++e) {
        const int written = _snprintf_s(expected_text + used, sizeof(expected_text) - used,
                                        _TRUNCATE, e ? "|%u" : "%u", expected[e]);
        if (written <= 0) break;
        used += static_cast<size_t>(written);
    }
    out("A8CASE %s hr_ps=0x%08lx hr_draw=0x%08lx value=%u expected=%s RESULT=%s\n", name,
        static_cast<unsigned long>(hr_ps), static_cast<unsigned long>(hr_draw), probe.center,
        expected_text, reason ? "FAIL" : "PASS");
    out("A8DETAIL %s points=%u,%u,%u,%u,%u tolerance=%u hr_map=0x%08lx detail=%s reason=%s\n",
        name, probe.values[0], probe.values[1], probe.values[2], probe.values[3],
        probe.values[4], kA8Tolerance, static_cast<unsigned long>(probe.hr_map),
        detail ? detail : "-", reason ? reason : "ok");
    out("MODE=%s ITEM=%s RESULT=%s reason=%s\n", mode, name, reason ? "FAIL" : "PASS",
        reason ? reason : "ok");
    if (reason) ++g_failures;
    return reason;
}

const char* print_a8_case(const char* mode, const char* name, HRESULT hr_ps, HRESULT hr_draw,
                          const A8Probe& probe, unsigned expected, const char* detail) {
    return print_a8_case_values(mode, name, hr_ps, hr_draw, probe, &expected, 1, detail);
}

int run_a8_contract(bool warp, bool debug_layer) {
    const char* path = "a8-contract";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());
    a8_support(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_half_blob;
    ComPtr<ID3DBlob> ps_sample_blob;
    ComPtr<ID3DBlob> cs_blob;
    const char* used_profile = nullptr;
    HRESULT hr = a8_compile(path, "a8_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "a8_ps_half", kA8PsHalf, "ps_5_0", "ps_4_0",
                        ps_half_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "a8_ps_sample", kA8PsSample, "ps_5_0", "ps_4_0",
                        ps_sample_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "a8_cs_store", kA8CsStore, "cs_5_0", "cs_4_0",
                        cs_blob.GetAddressOf(), &used_profile);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_half;
    ComPtr<ID3D11PixelShader> ps_sample;
    ComPtr<ID3D11ComputeShader> cs;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, &vs);
    out("MODE=%s stage=create_vs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_half_blob->GetBufferPointer(),
                                         ps_half_blob->GetBufferSize(), nullptr, &ps_half);
        out("MODE=%s stage=create_ps_half hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_sample_blob->GetBufferPointer(),
                                         ps_sample_blob->GetBufferSize(), nullptr, &ps_sample);
        out("MODE=%s stage=create_ps_sample hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreateComputeShader(cs_blob->GetBufferPointer(),
                                          cs_blob->GetBufferSize(), nullptr, &cs);
        out("MODE=%s stage=create_cs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    D3D11_BLEND_DESC blend_off{};
    blend_off.RenderTarget[0].BlendEnable = FALSE;
    blend_off.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> bs_off;
    HRESULT hr_blend_off = g.device->CreateBlendState(&blend_off, &bs_off);
    out("MODE=%s stage=create_blend_state name=off hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_blend_off));

    D3D11_BLEND_DESC blend_src_alpha{};
    blend_src_alpha.RenderTarget[0].BlendEnable = TRUE;
    blend_src_alpha.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_src_alpha.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_src_alpha.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_src_alpha.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_src_alpha.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_src_alpha.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_src_alpha.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> bs_src_alpha;
    HRESULT hr_blend_src = g.device->CreateBlendState(&blend_src_alpha, &bs_src_alpha);
    out("MODE=%s stage=create_blend_state name=src_alpha_inv_src_alpha alpha=one_zero"
        " hr=0x%08lx\n", path, static_cast<unsigned long>(hr_blend_src));
    if (FAILED(hr_blend_off) || FAILED(hr_blend_src)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=blend_state_create_failed\n", path);
        return 2;
    }

    // Culling off and depth off: these cases must fail on A8 value handling and
    // on nothing else, so no raster state default may silently discard the
    // fullscreen triangle or the covered pixels.
    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    HRESULT hr_raster = g.device->CreateRasterizerState(&raster_desc, &raster);
    out("MODE=%s stage=create_rasterizer_state cull=none depth_test=default hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_raster));
    if (FAILED(hr_raster)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=rasterizer_state_create_failed\n", path);
        return 2;
    }
    g.context->RSSetState(raster.Get());

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<FLOAT>(kA8Size);
    viewport.Height = static_cast<FLOAT>(kA8Size);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    g.context->RSSetViewports(1, &viewport);
    g.context->IASetInputLayout(nullptr);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(vs.Get(), nullptr, 0);

    const float zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float one4[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int failed = 0;

    // 1) Clear(0,0,0,0) -> 0
    {
        ComPtr<ID3D11Texture2D> tex, staging;
        if (!a8_target(path, g.device.Get(), D3D11_BIND_RENDER_TARGET, tex, staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=clear_zero RESULT=FAIL reason=a8_texture_create_failed\n", path);
        } else {
            ComPtr<ID3D11RenderTargetView> rtv;
            HRESULT hr_rtv = g.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
            out("MODE=%s stage=create_a8_rtv item=clear_zero hr=0x%08lx\n", path,
                static_cast<unsigned long>(hr_rtv));
            if (FAILED(hr_rtv)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=clear_zero RESULT=FAIL reason=rtv_create_failed\n", path);
            } else {
                g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                g.context->ClearRenderTargetView(rtv.Get(), zero4);
                g.context->OMSetRenderTargets(0, nullptr, nullptr);
                g.context->Flush();
                const HRESULT hr_reason = g.device->GetDeviceRemovedReason();
                const A8Probe probe =
                    a8_read(path, g.context.Get(), tex.Get(), staging.Get());
                if (print_a8_case(path, "clear_zero", S_OK, hr_reason, probe, 0,
                                  "clear=0,0,0,0 no_shader"))
                    ++failed;
            }
        }
    }

    // 2) PS float4(1,1,1,0.5), no blend -> 128
    {
        ComPtr<ID3D11Texture2D> tex, staging;
        if (!a8_target(path, g.device.Get(), D3D11_BIND_RENDER_TARGET, tex, staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=ps_half_no_blend RESULT=FAIL reason=a8_texture_create_failed\n",
                path);
        } else {
            ComPtr<ID3D11RenderTargetView> rtv;
            HRESULT hr_rtv = g.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
            out("MODE=%s stage=create_a8_rtv item=ps_half_no_blend hr=0x%08lx\n", path,
                static_cast<unsigned long>(hr_rtv));
            if (FAILED(hr_rtv)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=ps_half_no_blend RESULT=FAIL reason=rtv_create_failed\n",
                    path);
            } else {
                g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                g.context->OMSetBlendState(bs_off.Get(), blend_factor, 0xffffffff);
                g.context->PSSetShader(ps_half.Get(), nullptr, 0);
                g.context->ClearRenderTargetView(rtv.Get(), zero4);
                g.context->Draw(3, 0);
                g.context->Flush();
                const HRESULT hr_reason = g.device->GetDeviceRemovedReason();
                g.context->OMSetRenderTargets(0, nullptr, nullptr);
                const A8Probe probe =
                    a8_read(path, g.context.Get(), tex.Get(), staging.Get());
                const unsigned expected[2] = {128, 255};
                if (print_a8_case_values(path, "ps_half_no_blend", S_OK, hr_reason, probe,
                                         expected, 2,
                                         "ps=float4(1,1,1,0.5) blend=off; 128=stored_from_w"
                                         " 255=stored_from_x"))
                    ++failed;
            }
        }
    }

    // 3) Same PS with SRC_ALPHA/INV_SRC_ALPHA blending -> 128
    {
        ComPtr<ID3D11Texture2D> tex, staging;
        if (!a8_target(path, g.device.Get(), D3D11_BIND_RENDER_TARGET, tex, staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=ps_half_src_alpha_blend RESULT=FAIL"
                " reason=a8_texture_create_failed\n", path);
        } else {
            ComPtr<ID3D11RenderTargetView> rtv;
            HRESULT hr_rtv = g.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
            out("MODE=%s stage=create_a8_rtv item=ps_half_src_alpha_blend hr=0x%08lx\n", path,
                static_cast<unsigned long>(hr_rtv));
            if (FAILED(hr_rtv)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=ps_half_src_alpha_blend RESULT=FAIL"
                    " reason=rtv_create_failed\n", path);
            } else {
                g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                g.context->OMSetBlendState(bs_src_alpha.Get(), blend_factor, 0xffffffff);
                g.context->PSSetShader(ps_half.Get(), nullptr, 0);
                g.context->ClearRenderTargetView(rtv.Get(), zero4);
                g.context->Draw(3, 0);
                g.context->Flush();
                const HRESULT hr_reason = g.device->GetDeviceRemovedReason();
                g.context->OMSetRenderTargets(0, nullptr, nullptr);
                const A8Probe probe =
                    a8_read(path, g.context.Get(), tex.Get(), staging.Get());
                const unsigned expected[3] = {64, 128, 255};
                if (print_a8_case_values(path, "ps_half_src_alpha_blend", S_OK, hr_reason,
                                         probe, expected, 3,
                                         "blend=src_alpha/inv_src_alpha alpha=one/zero"
                                         " cleared=0; 64_or_128_or_255_are_legal_blends"
                                         " 0_means_blend_dropped_the_source"))
                    ++failed;
            }
        }
    }

    // 4) Clear(0,0,0,1) -> 255
    {
        ComPtr<ID3D11Texture2D> tex, staging;
        if (!a8_target(path, g.device.Get(), D3D11_BIND_RENDER_TARGET, tex, staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=clear_one RESULT=FAIL reason=a8_texture_create_failed\n", path);
        } else {
            ComPtr<ID3D11RenderTargetView> rtv;
            HRESULT hr_rtv = g.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
            out("MODE=%s stage=create_a8_rtv item=clear_one hr=0x%08lx\n", path,
                static_cast<unsigned long>(hr_rtv));
            if (FAILED(hr_rtv)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=clear_one RESULT=FAIL reason=rtv_create_failed\n", path);
            } else {
                g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                g.context->ClearRenderTargetView(rtv.Get(), one4);
                g.context->OMSetRenderTargets(0, nullptr, nullptr);
                g.context->Flush();
                const HRESULT hr_reason = g.device->GetDeviceRemovedReason();
                const A8Probe probe =
                    a8_read(path, g.context.Get(), tex.Get(), staging.Get());
                if (print_a8_case(path, "clear_one", S_OK, hr_reason, probe, 255,
                                  "clear=0,0,0,1 no_shader"))
                    ++failed;
            }
        }
    }

    // 5) typed UAV store float4(0,0,0,0.5), then SRV sample -> 128
    {
        ComPtr<ID3D11Texture2D> src_tex, src_staging;
        const UINT src_bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (!a8_target(path, g.device.Get(), src_bind, src_tex, src_staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=uav_store_then_srv RESULT=FAIL"
                " reason=a8_uav_texture_create_failed\n", path);
        } else {
            ComPtr<ID3D11UnorderedAccessView> uav;
            HRESULT hr_uav = g.device->CreateUnorderedAccessView(src_tex.Get(), nullptr, &uav);
            out("MODE=%s stage=create_a8_uav hr=0x%08lx\n", path,
                static_cast<unsigned long>(hr_uav));
            ComPtr<ID3D11ShaderResourceView> srv;
            HRESULT hr_srv = g.device->CreateShaderResourceView(src_tex.Get(), nullptr, &srv);
            out("MODE=%s stage=create_a8_srv hr=0x%08lx\n", path,
                static_cast<unsigned long>(hr_srv));
            if (FAILED(hr_uav) || FAILED(hr_srv)) {
                ++failed;
                ++g_failures;
                out("A8CASE uav_store_then_srv hr_ps=0x%08lx hr_draw=0x%08lx value=0"
                    " expected=128 RESULT=FAIL\n", static_cast<unsigned long>(hr_uav),
                    static_cast<unsigned long>(hr_srv));
                out("A8DETAIL uav_store_then_srv points=0,0,0,0,0 tolerance=%u hr_map=0x%08lx"
                    " detail=uav_or_srv_create_failed reason=\n", kA8Tolerance, 0ul);
                out("MODE=%s ITEM=uav_store_then_srv RESULT=FAIL reason=uav_or_srv_create_failed\n",
                    path);
            } else {
                g.context->CSSetShader(cs.Get(), nullptr, 0);
                ID3D11UnorderedAccessView* uavs[1] = {uav.Get()};
                g.context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
                g.context->Dispatch(kA8Size / 16, kA8Size / 16, 1);
                ID3D11UnorderedAccessView* no_uavs[1] = {nullptr};
                g.context->CSSetUnorderedAccessViews(0, 1, no_uavs, nullptr);
                g.context->CSSetShader(nullptr, nullptr, 0);
                g.context->Flush();
                const HRESULT hr_dispatch = g.device->GetDeviceRemovedReason();
                const A8Probe uav_stage =
                    a8_read(path, g.context.Get(), src_tex.Get(), src_staging.Get());

                ComPtr<ID3D11Texture2D> dst_tex, dst_staging;
                if (!a8_target(path, g.device.Get(), D3D11_BIND_RENDER_TARGET, dst_tex,
                               dst_staging)) {
                    ++failed;
                    ++g_failures;
                    out("MODE=%s ITEM=uav_store_then_srv RESULT=FAIL"
                        " reason=dst_texture_create_failed\n", path);
                } else {
                    ComPtr<ID3D11RenderTargetView> rtv;
                    HRESULT hr_rtv =
                        g.device->CreateRenderTargetView(dst_tex.Get(), nullptr, &rtv);
                    out("MODE=%s stage=create_a8_rtv item=uav_store_then_srv hr=0x%08lx\n",
                        path, static_cast<unsigned long>(hr_rtv));
                    if (FAILED(hr_rtv)) {
                        ++failed;
                        ++g_failures;
                        out("MODE=%s ITEM=uav_store_then_srv RESULT=FAIL"
                            " reason=rtv_create_failed\n", path);
                    } else {
                        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                        g.context->OMSetBlendState(bs_off.Get(), blend_factor, 0xffffffff);
                        g.context->PSSetShader(ps_sample.Get(), nullptr, 0);
                        g.context->PSSetShaderResources(0, 1, srv.GetAddressOf());
                        g.context->ClearRenderTargetView(rtv.Get(), zero4);
                        g.context->Draw(3, 0);
                        ID3D11ShaderResourceView* no_srvs[1] = {nullptr};
                        g.context->PSSetShaderResources(0, 1, no_srvs);
                        g.context->Flush();
                        const HRESULT hr_reason = g.device->GetDeviceRemovedReason();
                        g.context->OMSetRenderTargets(0, nullptr, nullptr);
                        const A8Probe probe =
                            a8_read(path, g.context.Get(), dst_tex.Get(), dst_staging.Get());
                        char detail[160];
                        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                                    "uav_stage_value=%u dispatch_hr=0x%08lx uav_store_point=128,128"
                                    " srv_load_point=128,128", uav_stage.center,
                                    static_cast<unsigned long>(hr_dispatch));
                        const unsigned expected[1] = {128};
                        if (print_a8_case_values(path, "uav_store_then_srv", S_OK, hr_reason,
                                                 probe, expected, 1, detail))
                            ++failed;
                    }
                }
            }
        }
    }

    out("MODE=%s SUMMARY cases=5 failed=%d format=A8_UNORM size=%dx%d probe_points=5"
        " tolerance=%u\n", path, failed, kA8Size, kA8Size, kA8Tolerance);
    return failed ? 2 : 0;
}

// Optional companion mode: is the lost 1px stroke an antialiasing-path problem?
int run_drawline(bool warp, bool debug_layer) {
    const char* path = "drawline";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    // Four items, each on its own target. The 2px and 1px integer lines are
    // both fully covered, so they must be dark in the same way the v2 solid fill
    // was; they are the control. The y=100.5 pair is the measurement: a
    // half-pixel line can only paint through the coverage path, and its aliased
    // twin shows what the same geometry looks like with AA switched off.
    struct LineCase {
        D2D1_ANTIALIAS_MODE mode;
        float y;
        float width;
        bool coverage_judged;
        const char* name;
    };
    const LineCase cases[] = {
        {D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, 100.0f, 2.0f, false, "line_2px_integer_aa"},
        {D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, 100.0f, 1.0f, false, "line_1px_integer_aa"},
        {D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, 100.5f, 1.0f, true, "line_1px_half_aa"},
        {D2D1_ANTIALIAS_MODE_ALIASED, 100.5f, 1.0f, false, "line_1px_half_aliased"},
    };

    int failed = 0;
    unsigned long long nonwhite_by_item[ARRAYSIZE(cases)] = {0, 0, 0, 0};
    size_t item_index = 0;
    for (const LineCase& c : cases) {
        const size_t this_index = item_index++;
        Target t;
        if (!target_create(path, g, warp, t)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed\n", path, c.name);
            continue;
        }
        t.rt->SetAntialiasMode(c.mode);
        const D2D1_ANTIALIAS_MODE effective = t.rt->GetAntialiasMode();
        ComPtr<ID2D1SolidColorBrush> black;
        HRESULT hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &black);
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s stage=d2d_solid_brush hr=0x%08lx RESULT=FAIL\n", path, c.name,
                static_cast<unsigned long>(hr));
            continue;
        }
        t.rt->BeginDraw();
        t.rt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        t.rt->DrawLine(D2D1::Point2F(8.0f, c.y), D2D1::Point2F(240.0f, c.y), black.Get(),
                       c.width);
        hr = t.rt->EndDraw();
        char extra[144];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " y=%.1f width=%.1f requested_aa=%d effective_aa=%d end_draw_hr=0x%08lx",
                    static_cast<double>(c.y), static_cast<double>(c.width),
                    static_cast<int>(c.mode), static_cast<int>(effective),
                    static_cast<unsigned long>(hr));
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s%s RESULT=FAIL reason=end_draw_failed\n", path, c.name, extra);
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!target_map(path, g, t, mapped)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s%s RESULT=FAIL reason=map_read_failed\n", path, c.name, extra);
            continue;
        }
        const PixelStats s =
            analyze(static_cast<const uint8_t*>(mapped.pData),
                    static_cast<int>(mapped.RowPitch), kWidth, kHeight, RECT{0, 84, 512, 118});
        g.context->Unmap(t.staging.Get(), 0);
        nonwhite_by_item[this_index] = s.nonwhite;
        if (c.coverage_judged
                ? print_ink_item_with(path, c.name, extra, s, coverage_reason(s))
                : print_line_item(path, c.name, extra, s))
            ++failed;
    }
    out("MODE=%s COVERAGE integer_2px_nonwhite=%llu integer_1px_nonwhite=%llu"
        " half_pixel_aa_nonwhite=%llu half_pixel_aliased_nonwhite=%llu"
        " aa_path=%s\n", path, nonwhite_by_item[0], nonwhite_by_item[1],
        nonwhite_by_item[2], nonwhite_by_item[3],
        nonwhite_by_item[2] > kMinDarkPixels ? "paints_ink" : "paints_nothing");
    out("MODE=%s SUMMARY items=%d failed=%d x=8..240 rows=y100(int) y100.5(half)"
        " judge=dark_run_or_partial_coverage aa=per_primitive_vs_aliased\n", path,
        static_cast<int>(ARRAYSIZE(cases)), failed);
    return failed ? 2 : 0;
}

int run_mode(const std::string& mode, bool warp, bool debug_layer) {
    if (mode == "d2d-primitives") return run_d2d_primitives(warp, debug_layer);
    if (mode == "dwrite-offscreen") return run_dwrite_offscreen(warp, debug_layer);
    if (mode == "bitmap-brush") return run_bitmap_brush(warp, debug_layer);
    if (mode == "a8-contract") return run_a8_contract(warp, debug_layer);
    if (mode == "drawline") return run_drawline(warp, debug_layer);
    out("MODE=%s RESULT=FAIL reason=unknown_mode\n", mode.c_str());
    return 2;
}

void usage() {
    out("usage: text_glyph_probe.exe [--driver hardware|warp] [--debug-layer]"
        " [--log PATH] [--self-test] [--mode NAME]...\n"
        "  no arguments      run d3d11_d2d (hardware) then gdi, print stats\n"
        "  --mode full       same as no arguments (default)\n"
        "  --mode d2d-primitives  geometry fill + 1px stroke + glyphs in one\n"
        "                    target and one readback, judged per item\n"
        "  --mode dwrite-offscreen  glyphs under cleartype/grayscale/aliased,\n"
        "                    each on its own texture and readback\n"
        "  --mode bitmap-brush  1x1 known-colour bitmap brush + DrawBitmap,\n"
        "                    separates texture sampling from geometry\n"
        "  --mode a8-contract  A8_UNORM value contract: clear 0/255, PS 0.5 with and\n"
        "                    without blending, typed UAV store then SRV sample\n"
        "  --mode drawline   four long lines: 2px and 1px fully covered, 1px on a\n"
        "                    half-pixel row antialiased vs aliased, each on its own\n"
        "                    target; the half-pixel AA item is judged on non-white\n"
        "                    pixels, not on dark pixels\n"
        "  --driver warp     control run through WARP; does not prove GPU support\n"
        "  --debug-layer     request D3D11 debug device (fails if SDK layer absent)\n"
        "  --log PATH        also append the same lines to PATH (truncating)\n"
        "  --self-test       CPU-only analyzer/pass-gate test; no device, no font\n"
        "exit 0 = all executed paths/items PASS; 2 = failure."
        " GUI/focus/screen untouched.\n"
        "mode items are judged by presence of the expected ink/colour, not by the\n"
        "full-mode text rule; a solid fill is expected to be all dark and one colour.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool warp = false;
    bool debug_layer = false;
    bool self_test = false;
    bool full = false;
    bool saw_mode = false;
    std::wstring log_path;
    std::vector<std::string> modes;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--self-test") {
            self_test = true;
        } else if (arg == L"--debug-layer") {
            debug_layer = true;
        } else if (arg == L"--mode" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            saw_mode = true;
            if (value == L"full") {
                full = true;
            } else if (value == L"d2d-primitives") {
                modes.push_back("d2d-primitives");
            } else if (value == L"dwrite-offscreen") {
                modes.push_back("dwrite-offscreen");
            } else if (value == L"bitmap-brush") {
                modes.push_back("bitmap-brush");
            } else if (value == L"a8-contract") {
                modes.push_back("a8-contract");
            } else if (value == L"drawline") {
                modes.push_back("drawline");
            } else {
                out("invalid --mode=%s\n", utf8(value.c_str()).c_str());
                usage();
                return 2;
            }
        } else if (arg == L"--driver" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"hardware") {
                warp = false;
            } else if (value == L"warp") {
                warp = true;
            } else {
                out("invalid --driver=%s\n", utf8(value.c_str()).c_str());
                usage();
                return 2;
            }
        } else if (arg == L"--log" && i + 1 < argc) {
            log_path = argv[++i];
        } else if (arg == L"--help" || arg == L"-h") {
            usage();
            return 0;
        } else {
            out("invalid argument=%s\n", utf8(arg.c_str()).c_str());
            usage();
            return 2;
        }
    }

    if (!log_path.empty()) {
        g_log = _wfopen(log_path.c_str(), L"wb");
        if (!g_log) {
            out("FAIL log_open path=\"%s\" win32=%lu\n", utf8(log_path.c_str()).c_str(),
                GetLastError());
            return 2;
        }
    }

    if (self_test) {
        const int rc = run_self_test();
        if (g_log) fclose(g_log);
        return rc;
    }

    print_environment();
    // No --mode at all keeps the v1 behaviour byte for byte: full paths only.
    if (!saw_mode) full = true;
    int failed = 0;
    if (full) {
        if (run_d3d11_d2d(warp, debug_layer) != 0) ++failed;
        if (run_gdi() != 0) ++failed;
        out("SUMMARY paths=2 passed=%d failed=%d driver=%s\n", 2 - failed, failed,
            warp ? "warp" : "hardware");
    }
    int mode_failed = 0;
    for (const std::string& mode : modes) {
        if (run_mode(mode, warp, debug_layer) != 0) ++mode_failed;
    }
    if (!modes.empty()) {
        out("SUMMARY_MODES modes=%zu passed=%zu failed=%d driver=%s\n", modes.size(),
            modes.size() - static_cast<size_t>(mode_failed), mode_failed,
            warp ? "warp" : "hardware");
    }
    if (g_log) fclose(g_log);
    return (failed + mode_failed) ? 2 : 0;
}
