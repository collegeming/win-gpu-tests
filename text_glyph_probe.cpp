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

// ---------------------------------------------------------------------------
// v4 diagnostic modes. Additive: the v1/v2/v3 modes and their output lines are
// unchanged.
//
// Device state when these were added: a 64x64 D2D FillRectangle and a 1x1
// bitmap brush filled correctly, while a 1px D2D rectangle stroke and
// DrawTextLayout produced a single-colour readback with no ink; GDI text on the
// same device passed, the same exe passed through WARP, and the A8 contract
// (Clear, PS with and without blending, typed UAV store then SRV sample)
// passed 5/5. So A8 store, A8 blend and A8 sample work, and the open questions
// are the upload path, the render-target-to-SRV path, and which D2D primitive
// families need the coverage rasterizer.
//
//   --mode a8-upload    UpdateSubresource of {0,64,128,255} into a 4x4
//                       R8_UNORM (and A8_UNORM when the UMD accepts it)
//                       texture: raw bytes read back, then the same texture
//                       sampled through the default SRV twice, once with the
//                       PS replicating the stored channel into RGB and once
//                       with the PS forcing XYZ to zero and writing the stored
//                       channel into alpha (the (ZERO,ZERO,ZERO,R) shape;
//                       D3D11 has no component mapping on an SRV).
//                       Answers "does an 8-bit upload land" separately from the
//                       typed UAV store that already worked.
//   --mode rt-to-srv    one R8_UNORM texture used as render target and then as
//                       SRV in the same frame: PS 0.5 -> 128 in the R8 target
//                       -> sampled -> 128 in the BGRA target. That is the shape
//                       of a D2D coverage / glyph mask.
//   --mode d2d-aa-modes six D2D primitives, each on its own target and readback:
//                       AA and aliased 1px stroke, AA triangle fill, AA ellipse
//                       fill, and a 1px half-pixel line AA and aliased. A
//                       primitive that needs partial coverage is compared with
//                       the same primitive that does not.
//   --mode text-paths   six glyph paths, each on its own target and readback:
//                       solid brush, bitmap brush, aliased text, a direct
//                       DrawGlyphRun, and a GRAYSCALE draw into an
//                       ALPHA_MODE_IGNORE and an ALPHA_MODE_PREMULTIPLIED
//                       target.
//
// Items keep the v2 rule set: judged by presence of the expected pixels, with an
// expected and an actual number on every line. A mode failure only changes the
// exit code and SUMMARY_MODES.

constexpr int kUploadSize = 4;
constexpr int kRtSize = 8;
constexpr int kTexelTolerance = 2;

const char* fmt_name(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_A8_UNORM: return "A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        default: return "other";
    }
}

int int_diff(int a, int b) { return a > b ? a - b : b - a; }

struct SmallRead {
    bool ok = false;
    HRESULT hr_map = E_FAIL;
    int pitch = 0;
    std::vector<uint8_t> bytes;
};

bool small_texture(const char* path, ID3D11Device* device, DXGI_FORMAT fmt, int size,
                   UINT bind, ComPtr<ID3D11Texture2D>& tex,
                   ComPtr<ID3D11Texture2D>& staging) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(size);
    desc.Height = static_cast<UINT>(size);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
    out("MODE=%s stage=create_texture fmt=%s size=%dx%d bind=0x%x hr=0x%08lx\n", path,
        fmt_name(fmt), size, size, bind, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    out("MODE=%s stage=texture_desc fmt=%s size=%dx%d mip=1 array=1 sample=1 usage=DEFAULT"
        " bind=0x%x\n", path, fmt_name(fmt), size, size, bind);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT hr_staging = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    out("MODE=%s stage=create_staging fmt=%s hr=0x%08lx\n", path, fmt_name(fmt),
        static_cast<unsigned long>(hr_staging));
    return SUCCEEDED(hr_staging);
}

// Copy to staging, Flush, Map(READ) and keep the bytes. Map(READ) is the GPU
// completion point, exactly as in target_map; there is no separate fence.
SmallRead small_read(const char* path, ID3D11DeviceContext* context, ID3D11Texture2D* tex,
                     ID3D11Texture2D* staging, int size, int bytes_per_pixel) {
    SmallRead r;
    context->CopyResource(staging, tex);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    r.hr_map = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    out("MODE=%s stage=map_read size=%dx%d bpp=%d row_pitch=%u hr=0x%08lx\n", path, size,
        size, bytes_per_pixel, mapped.RowPitch, static_cast<unsigned long>(r.hr_map));
    if (FAILED(r.hr_map)) return r;
    r.pitch = static_cast<int>(mapped.RowPitch);
    r.bytes.assign(static_cast<size_t>(r.pitch) * static_cast<size_t>(size), 0);
    memcpy(r.bytes.data(), mapped.pData, r.bytes.size());
    context->Unmap(staging, 0);
    r.ok = true;
    return r;
}

// Draw state for one fullscreen triangle into a square target of `size` pixels.
// Culling off, no depth, blending off: the cases must fail on the value path and
// on nothing else.
bool small_pipeline(const char* path, Gpu& g, int size, ID3D11VertexShader* vs) {
    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    HRESULT hr = g.device->CreateRasterizerState(&raster_desc, &raster);
    out("MODE=%s stage=create_rasterizer_state cull=none hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    const HRESULT hr_blend = g.device->CreateBlendState(&blend_desc, &blend);
    out("MODE=%s stage=create_blend_state name=off hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_blend));
    if (FAILED(hr_blend)) return false;

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<FLOAT>(size);
    viewport.Height = static_cast<FLOAT>(size);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g.context->RSSetViewports(1, &viewport);
    g.context->RSSetState(raster.Get());
    g.context->OMSetBlendState(blend.Get(), blend_factor, 0xffffffff);
    g.context->IASetInputLayout(nullptr);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(vs, nullptr, 0);
    return true;
}

void format_bgra_list(char* buf, size_t cap, const Rgba* v, int count) {
    size_t used = 0;
    buf[0] = 0;
    for (int i = 0; i < count; ++i) {
        if (used + 24 >= cap) break;
        const int n = _snprintf_s(buf + used, cap - used, _TRUNCATE,
                                  i ? "|%d,%d,%d,%d" : "%d,%d,%d,%d", v[i].b, v[i].g, v[i].r,
                                  v[i].a);
        if (n <= 0) break;
        used += static_cast<size_t>(n);
    }
}

// Per-texel compare with the expected and the actual vector on one line, plus
// one TEXEL line per mismatching texel so a single bad texel is locatable.
const char* print_texel_item(const char* mode, const char* item, const char* extra,
                             const Rgba* want, const Rgba* got, int count, int tolerance) {
    int bad = 0;
    for (int i = 0; i < count; ++i) {
        if (int_diff(want[i].b, got[i].b) > tolerance ||
            int_diff(want[i].g, got[i].g) > tolerance ||
            int_diff(want[i].r, got[i].r) > tolerance ||
            int_diff(want[i].a, got[i].a) > tolerance)
            ++bad;
    }
    char want_text[512] = "none";
    char got_text[512] = "none";
    format_bgra_list(want_text, sizeof(want_text), want, count);
    format_bgra_list(got_text, sizeof(got_text), got, count);
    const char* reason = bad ? "value_mismatch" : nullptr;
    out("MODE=%s ITEM=%s%s RESULT=%s texels=%d mismatches=%d tolerance=%d expected_bgra=%s"
        " actual_bgra=%s reason=%s\n", mode, item, extra, reason ? "FAIL" : "PASS", count,
        bad, tolerance, want_text, got_text, reason ? reason : "ok");
    for (int i = 0; i < count; ++i) {
        if (int_diff(want[i].b, got[i].b) <= tolerance &&
            int_diff(want[i].g, got[i].g) <= tolerance &&
            int_diff(want[i].r, got[i].r) <= tolerance &&
            int_diff(want[i].a, got[i].a) <= tolerance)
            continue;
        out("MODE=%s TEXEL item=%s index=%d expected_bgra=%d,%d,%d,%d"
            " actual_bgra=%d,%d,%d,%d\n", mode, item, i, want[i].b, want[i].g, want[i].r,
            want[i].a, got[i].b, got[i].g, got[i].r, got[i].a);
    }
    if (reason) ++g_failures;
    return reason;
}

// Whole-surface colour compare: expected one BGRA value for every texel, actual
// reported as the per-channel min/max so a uniform wrong value is visible.
const char* print_plane_item(const char* mode, const char* item, const char* extra,
                             const SmallRead& r, int size, Rgba want, int tolerance) {
    int bad = 0;
    int lo[4] = {255, 255, 255, 255};
    int hi[4] = {0, 0, 0, 0};
    for (int y = 0; y < size; ++y) {
        const uint8_t* row = r.bytes.data() + static_cast<size_t>(y) * r.pitch;
        for (int x = 0; x < size; ++x) {
            const int c[4] = {row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]};
            for (int k = 0; k < 4; ++k) {
                if (c[k] < lo[k]) lo[k] = c[k];
                if (c[k] > hi[k]) hi[k] = c[k];
            }
            if (int_diff(c[0], want.b) > tolerance || int_diff(c[1], want.g) > tolerance ||
                int_diff(c[2], want.r) > tolerance || int_diff(c[3], want.a) > tolerance)
                ++bad;
        }
    }
    const char* reason = bad ? "value_mismatch" : nullptr;
    out("MODE=%s ITEM=%s%s RESULT=%s texels=%d mismatches=%d tolerance=%d"
        " expected_bgra=%d,%d,%d,%d actual_min_bgra=%d,%d,%d,%d actual_max_bgra=%d,%d,%d,%d"
        " reason=%s\n", mode, item, extra, reason ? "FAIL" : "PASS", size * size, bad,
        tolerance, want.b, want.g, want.r, want.a, lo[0], lo[1], lo[2], lo[3], hi[0], hi[1],
        hi[2], hi[3], reason ? reason : "ok");
    if (reason) ++g_failures;
    return reason;
}

// Single-channel plane compare for an R8/A8 texture read back directly.
const char* print_scalar_item(const char* mode, const char* item, const char* extra,
                              const SmallRead& r, int size, unsigned want, int tolerance) {
    unsigned lo = 255;
    unsigned hi = 0;
    int bad = 0;
    for (int y = 0; y < size; ++y) {
        const uint8_t* row = r.bytes.data() + static_cast<size_t>(y) * r.pitch;
        for (int x = 0; x < size; ++x) {
            const unsigned v = row[x];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            if (int_diff(static_cast<int>(v), static_cast<int>(want)) > tolerance) ++bad;
        }
    }
    const char* reason = bad ? "value_mismatch" : nullptr;
    out("MODE=%s ITEM=%s%s RESULT=%s texels=%d mismatches=%d expected=%u tolerance=%d"
        " actual_min=%u actual_max=%u reason=%s\n", mode, item, extra,
        reason ? "FAIL" : "PASS", size * size, bad, want, tolerance, lo, hi,
        reason ? reason : "ok");
    if (reason) ++g_failures;
    return reason;
}

// The identity view of an R8_UNORM texture hands the shader (v,0,0,1) and the
// identity view of an A8_UNORM texture hands it (0,0,0,v), so the meaningful
// channel differs per format; the shaders are separate per format so the
// expected value stays honest instead of a max(r,a) that would read the
// constant alpha of the R8 view.
//
// D3D11 has no component mapping on a shader resource view: the
// Shader4ComponentMapping field belongs to D3D12's D3D12_SHADER_RESOURCE_VIEW_DESC,
// and D3D11_SHADER_RESOURCE_VIEW_DESC has only Format / ViewDimension / the
// per-dimension union (the compiler rejects the field). So the (ZERO,ZERO,ZERO,R)
// shape is produced in the pixel shader from the identity view: the value the
// view hands the shader is what the readback must then show in the target
// alpha channel, with RGB forced to zero. The two shader families below are
// exactly that pair, and the item names say which shape each one measures.
const char* kUploadPsReplicateR =
    "Texture2D<float4> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    float4 s = src.Load(int3(int2(pos.xy), 0));\n"
    "    float v = s.r;\n"
    "    return float4(v, v, v, 1.0);\n"
    "}\n";

const char* kUploadPsReplicateA =
    "Texture2D<float4> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    float4 s = src.Load(int3(int2(pos.xy), 0));\n"
    "    float v = s.a;\n"
    "    return float4(v, v, v, 1.0);\n"
    "}\n";

// (ZERO,ZERO,ZERO,R) output shape: x/y/z forced to 0, the sampled value in
// alpha only. A readback of (0,0,0,v) proves the value survived the move to the
// alpha channel; anything else shows what the SRV actually handed the shader.
const char* kUploadPsAlphaR =
    "Texture2D<float4> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    float4 s = src.Load(int3(int2(pos.xy), 0));\n"
    "    return float4(0.0, 0.0, 0.0, s.r);\n"
    "}\n";

const char* kUploadPsAlphaA =
    "Texture2D<float4> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    float4 s = src.Load(int3(int2(pos.xy), 0));\n"
    "    return float4(0.0, 0.0, 0.0, s.a);\n"
    "}\n";

// Mode 5: does UpdateSubresource into an 8-bit texture land, and does the value
// reach a shader through the default SRV in both output shapes?
int run_a8_upload(bool warp, bool debug_layer) {
    const char* path = "a8-upload";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob, ps_replicate_r_blob, ps_replicate_a_blob, ps_alpha_r_blob,
        ps_alpha_a_blob;
    const char* used_profile = nullptr;
    HRESULT hr = a8_compile(path, "upload_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "upload_ps_replicate_r", kUploadPsReplicateR, "ps_5_0",
                        "ps_4_0", ps_replicate_r_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "upload_ps_replicate_a", kUploadPsReplicateA, "ps_5_0",
                        "ps_4_0", ps_replicate_a_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "upload_ps_alpha_r", kUploadPsAlphaR, "ps_5_0", "ps_4_0",
                        ps_alpha_r_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "upload_ps_alpha_a", kUploadPsAlphaA, "ps_5_0", "ps_4_0",
                        ps_alpha_a_blob.GetAddressOf(), &used_profile);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_replicate_r;
    ComPtr<ID3D11PixelShader> ps_replicate_a;
    ComPtr<ID3D11PixelShader> ps_alpha_r;
    ComPtr<ID3D11PixelShader> ps_alpha_a;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, &vs);
    out("MODE=%s stage=create_vs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_replicate_r_blob->GetBufferPointer(),
                                         ps_replicate_r_blob->GetBufferSize(), nullptr,
                                         &ps_replicate_r);
        out("MODE=%s stage=create_ps_replicate_r hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_replicate_a_blob->GetBufferPointer(),
                                         ps_replicate_a_blob->GetBufferSize(), nullptr,
                                         &ps_replicate_a);
        out("MODE=%s stage=create_ps_replicate_a hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_alpha_r_blob->GetBufferPointer(),
                                         ps_alpha_r_blob->GetBufferSize(), nullptr,
                                         &ps_alpha_r);
        out("MODE=%s stage=create_ps_alpha_r hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_alpha_a_blob->GetBufferPointer(),
                                         ps_alpha_a_blob->GetBufferSize(), nullptr,
                                         &ps_alpha_a);
        out("MODE=%s stage=create_ps_alpha_a hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    if (!small_pipeline(path, g, kUploadSize, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=state_create_failed\n", path);
        return 2;
    }

    // (ZERO,ZERO,ZERO,R) has to be produced by the pixel shader: D3D11 has no
    // component mapping on a shader resource view (that field is D3D12's). See
    // the note above kUploadPsAlphaR.

    const unsigned values[kUploadSize] = {0, 64, 128, 255};
    struct FmtCase {
        DXGI_FORMAT fmt;
        const char* name;
    };
    const FmtCase formats[] = {
        {DXGI_FORMAT_R8_UNORM, "R8_UNORM"},
        {DXGI_FORMAT_A8_UNORM, "A8_UNORM"},
    };
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int failed = 0;
    int skipped = 0;
    int items = 0;

    for (const FmtCase& f : formats) {
        UINT support = 0;
        const HRESULT hr_support = g.device->CheckFormatSupport(f.fmt, &support);
        out("MODE=%s stage=format_support fmt=%s hr=0x%08lx texture2d=%s shader_sample=%s"
            " shader_load=%s\n", path, f.name, static_cast<unsigned long>(hr_support),
            (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? "yes" : "no",
            (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ? "yes" : "no",
            (support & D3D11_FORMAT_SUPPORT_SHADER_LOAD) ? "yes" : "no");

        ComPtr<ID3D11Texture2D> src, src_staging;
        if (!small_texture(path, g.device.Get(), f.fmt, kUploadSize,
                           D3D11_BIND_SHADER_RESOURCE, src, src_staging)) {
            // An 8-bit format the UMD refuses is recorded as unsupported, not as
            // a crash and not as a value failure: the A8 question is then
            // answered by the R8 case alone.
            out("MODE=%s ITEM=%s_upload_raw RESULT=SKIP reason=format_unsupported\n", path,
                f.name);
            ++skipped;
            continue;
        }

        uint8_t data[kUploadSize * kUploadSize];
        for (int y = 0; y < kUploadSize; ++y)
            for (int x = 0; x < kUploadSize; ++x)
                data[y * kUploadSize + x] = static_cast<uint8_t>(values[x]);
        g.context->UpdateSubresource(src.Get(), 0, nullptr, data,
                                     static_cast<UINT>(kUploadSize),
                                     static_cast<UINT>(kUploadSize * kUploadSize));
        g.context->Flush();
        const HRESULT hr_upload = g.device->GetDeviceRemovedReason();
        out("MODE=%s stage=update_subresource fmt=%s size=%dx%d values=0,64,128,255"
            " src_row_pitch=%d hr_draw=0x%08lx\n", path, f.name, kUploadSize, kUploadSize,
            kUploadSize, static_cast<unsigned long>(hr_upload));

        // 1) the uploaded bytes, read back from the texture itself.
        Rgba want_raw[kUploadSize * kUploadSize];
        Rgba got_raw[kUploadSize * kUploadSize];
        for (int y = 0; y < kUploadSize; ++y)
            for (int x = 0; x < kUploadSize; ++x) {
                const int v = static_cast<int>(values[x]);
                want_raw[y * kUploadSize + x] = Rgba{v, v, v, 255};
                got_raw[y * kUploadSize + x] = Rgba{0, 0, 0, 0};
            }
        const SmallRead raw =
            small_read(path, g.context.Get(), src.Get(), src_staging.Get(), kUploadSize, 1);
        char item[64];
        char extra[192];
        if (!raw.ok) {
            ++failed;
            ++g_failures;
            _snprintf_s(item, sizeof(item), _TRUNCATE, "%s_upload_raw", f.name);
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
        } else {
            for (int y = 0; y < kUploadSize; ++y)
                for (int x = 0; x < kUploadSize; ++x) {
                    const int v =
                        static_cast<int>(raw.bytes[static_cast<size_t>(y) * raw.pitch + x]);
                    got_raw[y * kUploadSize + x] = Rgba{v, v, v, 255};
                }
            _snprintf_s(item, sizeof(item), _TRUNCATE, "%s_upload_raw", f.name);
            _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                        " fmt=%s path=UpdateSubresource_then_copy channel=stored_byte_as_bgr"
                        " hr_draw=0x%08lx", f.name, static_cast<unsigned long>(hr_upload));
            ++items;
            if (print_texel_item(path, item, extra, want_raw, got_raw,
                                 kUploadSize * kUploadSize, 0))
                ++failed;
        }

        ComPtr<ID3D11ShaderResourceView> srv;
        const HRESULT hr_srv = g.device->CreateShaderResourceView(src.Get(), nullptr, &srv);
        out("MODE=%s stage=create_srv fmt=%s identity_hr=0x%08lx"
            " component_mapping=ps_side_d3d11_srv_desc_has_no_mapping_field\n", path, f.name,
            static_cast<unsigned long>(hr_srv));
        if (FAILED(hr_srv)) {
            // An SRV the UMD refuses is "this format cannot be sampled", not a
            // value failure: the R8 case carries the answer.
            out("MODE=%s ITEM=%s_identity_replicate RESULT=SKIP reason=srv_create_failed\n",
                path, f.name);
            out("MODE=%s ITEM=%s_map_zzzr_in_ps RESULT=SKIP reason=srv_create_failed\n", path,
                f.name);
            skipped += 2;
            continue;
        }

        // 2) and 3): the same identity SRV sampled by two shaders, one that
        // replicates the stored channel into RGB, one that forces XYZ to zero and
        // puts it in alpha (the (ZERO,ZERO,ZERO,R) output shape).
        struct SampleCase {
            const char* suffix;
            ID3D11PixelShader* ps;
            bool raw_output;
            int alpha;
            const char* note;
        };
        const bool is_a8 = (f.fmt == DXGI_FORMAT_A8_UNORM);
        const SampleCase cases[] = {
            {"identity_replicate", is_a8 ? ps_replicate_a.Get() : ps_replicate_r.Get(),
             false, 255, "ps=stored_channel_replicated_to_rgb_alpha_255"},
            {"map_zzzr_in_ps", is_a8 ? ps_alpha_a.Get() : ps_alpha_r.Get(), true, 0,
             "ps=xyz_zero_alpha=stored_channel_d3d11_has_no_srv_mapping"},
        };
        for (const SampleCase& c : cases) {
            ComPtr<ID3D11Texture2D> dst, dst_staging;
            if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kUploadSize,
                               D3D11_BIND_RENDER_TARGET, dst, dst_staging)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=%s_%s RESULT=FAIL reason=dst_texture_create_failed\n",
                    path, f.name, c.suffix);
                continue;
            }
            ComPtr<ID3D11RenderTargetView> rtv;
            const HRESULT hr_rtv =
                g.device->CreateRenderTargetView(dst.Get(), nullptr, &rtv);
            out("MODE=%s stage=create_dst_rtv fmt=%s item=%s hr=0x%08lx\n", path, f.name,
                c.suffix, static_cast<unsigned long>(hr_rtv));
            if (FAILED(hr_rtv)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=%s_%s RESULT=FAIL reason=rtv_create_failed\n", path,
                    f.name, c.suffix);
                continue;
            }
            g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            g.context->ClearRenderTargetView(rtv.Get(), clear);
            g.context->PSSetShader(c.ps, nullptr, 0);
            ID3D11ShaderResourceView* src_view = srv.Get();
            g.context->PSSetShaderResources(0, 1, &src_view);
            g.context->Draw(3, 0);
            ID3D11ShaderResourceView* no_srvs[1] = {nullptr};
            g.context->PSSetShaderResources(0, 1, no_srvs);
            g.context->Flush();
            const HRESULT hr_draw = g.device->GetDeviceRemovedReason();
            g.context->OMSetRenderTargets(0, nullptr, nullptr);

            const SmallRead back = small_read(path, g.context.Get(), dst.Get(),
                                             dst_staging.Get(), kUploadSize, 4);
            _snprintf_s(item, sizeof(item), _TRUNCATE, "%s_%s", f.name, c.suffix);
            if (!back.ok) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
                continue;
            }
            Rgba want[kUploadSize * kUploadSize];
            Rgba got[kUploadSize * kUploadSize];
            for (int y = 0; y < kUploadSize; ++y)
                for (int x = 0; x < kUploadSize; ++x) {
                    const int v = static_cast<int>(values[x]);
                    want[y * kUploadSize + x] =
                        c.raw_output ? Rgba{0, 0, 0, v} : Rgba{v, v, v, c.alpha};
                    const uint8_t* p =
                        back.bytes.data() + static_cast<size_t>(y) * back.pitch + x * 4;
                    got[y * kUploadSize + x] =
                        Rgba{static_cast<int>(p[0]), static_cast<int>(p[1]),
                             static_cast<int>(p[2]), static_cast<int>(p[3])};
                }
            _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                        " fmt=%s note=%s hr_draw=0x%08lx", f.name, c.note,
                        static_cast<unsigned long>(hr_draw));
            ++items;
            if (print_texel_item(path, item, extra, want, got, kUploadSize * kUploadSize,
                                 kTexelTolerance))
                ++failed;
        }
    }

    out("MODE=%s SUMMARY items=%d failed=%d skipped=%d formats=R8_UNORM,A8_UNORM"
        " size=%dx%d tolerance=%d\n", path, items, failed, skipped, kUploadSize, kUploadSize,
        kTexelTolerance);
    if (failed) ++g_failures;
    return failed ? 2 : 0;
}

// Mode 6: render target and SRV over the same R8_UNORM resource in one frame.
const char* kRtPsConst =
    "float4 main() : SV_Target {\n"
    "    return float4(0.5, 0.5, 0.5, 0.5);\n"
    "}\n";

const char* kRtPsSample =
    "Texture2D<float4> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    float4 s = src.Load(int3(int2(pos.xy), 0));\n"
    "    return float4(s.r, s.r, s.r, 1.0);\n"
    "}\n";

int run_rt_to_srv(bool warp, bool debug_layer) {
    const char* path = "rt-to-srv";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob, ps_const_blob, ps_sample_blob;
    const char* used_profile = nullptr;
    HRESULT hr = a8_compile(path, "rt_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "rt_ps_const", kRtPsConst, "ps_5_0", "ps_4_0",
                        ps_const_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "rt_ps_sample", kRtPsSample, "ps_5_0", "ps_4_0",
                        ps_sample_blob.GetAddressOf(), &used_profile);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_const;
    ComPtr<ID3D11PixelShader> ps_sample;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, &vs);
    out("MODE=%s stage=create_vs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_const_blob->GetBufferPointer(),
                                         ps_const_blob->GetBufferSize(), nullptr, &ps_const);
        out("MODE=%s stage=create_ps_const hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_sample_blob->GetBufferPointer(),
                                         ps_sample_blob->GetBufferSize(), nullptr,
                                         &ps_sample);
        out("MODE=%s stage=create_ps_sample hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    if (!small_pipeline(path, g, kRtSize, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=state_create_failed\n", path);
        return 2;
    }

    int failed = 0;
    // One resource, both bindings: this is how a D2D coverage mask is produced
    // and consumed.
    ComPtr<ID3D11Texture2D> src, src_staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_R8_UNORM, kRtSize,
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, src,
                       src_staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=r8_texture_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT hr_rtv = g.device->CreateRenderTargetView(src.Get(), nullptr, &rtv);
    out("MODE=%s stage=create_r8_rtv bind=render_target|shader_resource hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_rtv));
    ComPtr<ID3D11ShaderResourceView> srv;
    const HRESULT hr_srv = g.device->CreateShaderResourceView(src.Get(), nullptr, &srv);
    out("MODE=%s stage=create_r8_srv hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_srv));
    if (FAILED(hr_rtv) || FAILED(hr_srv)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=r8_view_create_failed\n", path);
        return 2;
    }

    // 1) PS 0.5 into the R8 render target.
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.context->ClearRenderTargetView(rtv.Get(), clear);
    g.context->PSSetShader(ps_const.Get(), nullptr, 0);
    g.context->Draw(3, 0);
    g.context->Flush();
    const HRESULT hr_rt_draw = g.device->GetDeviceRemovedReason();
    g.context->OMSetRenderTargets(0, nullptr, nullptr);
    out("MODE=%s stage=draw_rt ps=const_0.5 target=R8_UNORM size=%dx%d hr_draw=0x%08lx\n",
        path, kRtSize, kRtSize, static_cast<unsigned long>(hr_rt_draw));

    const SmallRead rt =
        small_read(path, g.context.Get(), src.Get(), src_staging.Get(), kRtSize, 1);
    char extra[192];
    if (!rt.ok) {
        ++failed;
        ++g_failures;
        out("MODE=%s ITEM=rt_r8_fill_0p5 RESULT=FAIL reason=map_read_failed\n", path);
    } else {
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " path=RT_write_then_copy ps=const_0.5 store=rn hr_draw=0x%08lx",
                    static_cast<unsigned long>(hr_rt_draw));
        if (print_scalar_item(path, "rt_r8_fill_0p5", extra, rt, kRtSize, 128,
                              kTexelTolerance))
            ++failed;
    }

    // 2) the same resource as SRV, sampled into a BGRA target.
    ComPtr<ID3D11Texture2D> dst, dst_staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kRtSize,
                       D3D11_BIND_RENDER_TARGET, dst, dst_staging)) {
        ++failed;
        ++g_failures;
        out("MODE=%s ITEM=rt_then_srv RESULT=FAIL reason=dst_texture_create_failed\n", path);
    } else {
        ComPtr<ID3D11RenderTargetView> dst_rtv;
        const HRESULT hr_dst_rtv =
            g.device->CreateRenderTargetView(dst.Get(), nullptr, &dst_rtv);
        out("MODE=%s stage=create_dst_rtv hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr_dst_rtv));
        if (FAILED(hr_dst_rtv)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=rt_then_srv RESULT=FAIL reason=rtv_create_failed\n", path);
        } else {
            g.context->OMSetRenderTargets(1, dst_rtv.GetAddressOf(), nullptr);
            g.context->ClearRenderTargetView(dst_rtv.Get(), clear);
            g.context->PSSetShader(ps_sample.Get(), nullptr, 0);
            ID3D11ShaderResourceView* views[1] = {srv.Get()};
            g.context->PSSetShaderResources(0, 1, views);
            g.context->Draw(3, 0);
            ID3D11ShaderResourceView* no_srvs[1] = {nullptr};
            g.context->PSSetShaderResources(0, 1, no_srvs);
            g.context->Flush();
            const HRESULT hr_draw = g.device->GetDeviceRemovedReason();
            g.context->OMSetRenderTargets(0, nullptr, nullptr);
            const SmallRead back = small_read(path, g.context.Get(), dst.Get(),
                                             dst_staging.Get(), kRtSize, 4);
            if (!back.ok) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=rt_then_srv RESULT=FAIL reason=map_read_failed\n", path);
            } else {
                _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                            " path=RT_then_SRV_same_resource ps=load_r_replicate_alpha_255"
                            " sample_point=pixel_xy hr_draw=0x%08lx",
                            static_cast<unsigned long>(hr_draw));
                if (print_plane_item(path, "rt_then_srv", extra, back, kRtSize,
                                     Rgba{128, 128, 128, 255}, kTexelTolerance))
                    ++failed;
            }
        }
    }

    out("MODE=%s SUMMARY items=2 failed=%d resource=R8_UNORM size=%dx%d tolerance=%d"
        " bind=render_target|shader_resource\n", path, failed, kRtSize, kRtSize,
        kTexelTolerance);
    if (failed) ++g_failures;
    return failed ? 2 : 0;
}

// Mode 7: which D2D primitive families survive. Same 512x128 BGRA target and
// same black solid brush as d2d-primitives, one fresh target per item.
enum AaDrawKind { kAaRectStroke, kAaTriangleFill, kAaEllipseFill, kAaLine };
enum AaJudge { kJudgeInk, kJudgeLine, kJudgeCoverage };

struct AaItem {
    AaDrawKind draw;
    D2D1_ANTIALIAS_MODE aa;
    AaJudge judge;
    RECT region;
    float line_y;
    float line_width;
    const char* name;
    const char* expected;
};

int run_d2d_aa_modes(bool warp, bool debug_layer) {
    const char* path = "d2d-aa-modes";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID2D1PathGeometry> triangle;
    HRESULT hr = g.d2d->CreatePathGeometry(&triangle);
    out("MODE=%s stage=create_path_geometry hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        ComPtr<ID2D1GeometrySink> sink;
        hr = triangle->Open(&sink);
        if (SUCCEEDED(hr)) {
            sink->BeginFigure(D2D1::Point2F(40.0f, 20.0f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(120.0f, 20.0f));
            sink->AddLine(D2D1::Point2F(80.0f, 100.0f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            hr = sink->Close();
        }
        out("MODE=%s stage=triangle_geometry points=40,20_120,20_80,100 hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=path_geometry_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    const AaItem items[] = {
        {kAaRectStroke, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, kJudgeInk, RECT{76, 4, 148, 76},
         0.0f, 0.0f, "rect_stroke_aa",
         "dark_gt_50_bbox_ge_8x4_outline_63x63_1px_aa_on"},
        {kAaRectStroke, D2D1_ANTIALIAS_MODE_ALIASED, kJudgeInk, RECT{76, 4, 148, 76}, 0.0f,
         0.0f, "rect_stroke_aliased",
         "dark_gt_50_bbox_ge_8x4_outline_63x63_1px_aa_off"},
        {kAaTriangleFill, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, kJudgeInk, RECT{36, 16, 124, 104},
         0.0f, 0.0f, "triangle_fill_aa", "dark_gt_50_bbox_ge_8x4_filled_triangle_aa_on"},
        {kAaEllipseFill, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, kJudgeInk, RECT{36, 20, 124, 108},
         0.0f, 0.0f, "ellipse_fill_aa", "dark_gt_50_bbox_ge_8x4_filled_ellipse_aa_on"},
        {kAaLine, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, kJudgeCoverage, RECT{0, 84, 512, 118},
         100.5f, 1.0f, "line_d2d_aa",
         "nonwhite_gt_50_bbox_width_ge_100_half_pixel_aa_on"},
        {kAaLine, D2D1_ANTIALIAS_MODE_ALIASED, kJudgeLine, RECT{0, 84, 512, 118}, 100.5f,
         1.0f, "line_d2d_aliased",
         "dark_gt_50_bbox_width_ge_100_half_pixel_aa_off"},
    };

    int failed = 0;
    unsigned long long dark_by_item[ARRAYSIZE(items)] = {0, 0, 0, 0, 0, 0};
    size_t index = 0;
    for (const AaItem& item : items) {
        const size_t this_index = index++;
        Target t;
        if (!target_create(path, g, warp, t)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed\n", path,
                item.name);
            continue;
        }
        t.rt->SetAntialiasMode(item.aa);
        const D2D1_ANTIALIAS_MODE effective = t.rt->GetAntialiasMode();
        ComPtr<ID2D1SolidColorBrush> black;
        hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &black);
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s stage=d2d_solid_brush hr=0x%08lx RESULT=FAIL\n", path,
                item.name, static_cast<unsigned long>(hr));
            continue;
        }
        t.rt->BeginDraw();
        t.rt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        switch (item.draw) {
            case kAaRectStroke:
                t.rt->DrawRectangle(D2D1::RectF(80.5f, 8.5f, 143.5f, 71.5f), black.Get(),
                                    1.0f);
                break;
            case kAaTriangleFill:
                t.rt->FillGeometry(triangle.Get(), black.Get());
                break;
            case kAaEllipseFill:
                t.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(80.0f, 64.0f), 40.0f, 40.0f),
                                  black.Get());
                break;
            case kAaLine:
                t.rt->DrawLine(D2D1::Point2F(8.0f, item.line_y),
                               D2D1::Point2F(240.0f, item.line_y), black.Get(),
                               item.line_width);
                break;
        }
        hr = t.rt->EndDraw();
        char extra[256];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " region=%d,%d,%d,%d requested_aa=%d effective_aa=%d end_draw_hr=0x%08lx"
                    " expected=%s", static_cast<int>(item.region.left),
                    static_cast<int>(item.region.top), static_cast<int>(item.region.right),
                    static_cast<int>(item.region.bottom), static_cast<int>(item.aa),
                    static_cast<int>(effective), static_cast<unsigned long>(hr),
                    item.expected);
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s%s RESULT=FAIL reason=end_draw_failed\n", path, item.name,
                extra);
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!target_map(path, g, t, mapped)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s%s RESULT=FAIL reason=map_read_failed\n", path, item.name,
                extra);
            continue;
        }
        const PixelStats s = analyze(static_cast<const uint8_t*>(mapped.pData),
                                     static_cast<int>(mapped.RowPitch), kWidth, kHeight,
                                     item.region);
        g.context->Unmap(t.staging.Get(), 0);
        dark_by_item[this_index] = s.dark;
        const char* reason = nullptr;
        switch (item.judge) {
            case kJudgeInk: reason = ink_reason(s); break;
            case kJudgeLine: reason = line_reason(s); break;
            case kJudgeCoverage: reason = coverage_reason(s); break;
        }
        if (print_ink_item_with(path, item.name, extra, s, reason)) ++failed;
    }

    out("MODE=%s SUMMARY items=%d failed=%d targets=%d"
        " dark_by_item=%llu,%llu,%llu,%llu,%llu,%llu\n", path,
        static_cast<int>(ARRAYSIZE(items)), failed, static_cast<int>(ARRAYSIZE(items)),
        dark_by_item[0], dark_by_item[1], dark_by_item[2], dark_by_item[3],
        dark_by_item[4], dark_by_item[5]);
    if (failed) ++g_failures;
    return failed ? 2 : 0;
}

// target_create with an explicit D2D alpha mode. A separate function on purpose:
// the v2/v3 modes keep calling target_create and their output lines are not
// touched by the alpha-mode cases.
bool target_create_alpha(const char* path, const Gpu& g, bool warp, D2D1_ALPHA_MODE alpha,
                         const char* alpha_name, Target& t) {
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
        " alpha_mode=%s hr=0x%08lx\n", path, kWidth, kHeight, alpha_name,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    hr = t.tex.As(&t.surface);
    if (FAILED(hr)) {
        out("MODE=%s stage=query_dxgi_surface alpha_mode=%s hr=0x%08lx\n", path, alpha_name,
            static_cast<unsigned long>(hr));
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        warp ? D2D1_RENDER_TARGET_TYPE_DEFAULT : D2D1_RENDER_TARGET_TYPE_HARDWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alpha), 96.0f, 96.0f);
    hr = g.d2d->CreateDxgiSurfaceRenderTarget(t.surface.Get(), &props, &t.rt);
    out("MODE=%s stage=d2d_surface_rt target_type=%s alpha_mode=%s hr=0x%08lx\n", path,
        warp ? "default" : "hardware", alpha_name, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    t.rt->SetDpi(96.0f, 96.0f);

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g.device->CreateTexture2D(&staging_desc, nullptr, &t.staging);
    if (FAILED(hr)) {
        out("MODE=%s stage=create_staging alpha_mode=%s hr=0x%08lx\n", path, alpha_name,
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// Mode 8: which glyph path survives.
struct GlyphSource {
    ComPtr<IDWriteFontFace> face;
    UINT16 glyphs[kTextLen] = {0, 0, 0, 0, 0, 0, 0, 0};
    FLOAT advances[kTextLen] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    UINT32 design_units = 0;
    FLOAT ascent_px = 0.0f;
    FLOAT advance_sum = 0.0f;
    bool ok = false;
};

GlyphSource glyph_source(const char* path, const Gpu& g) {
    GlyphSource s;
    ComPtr<IDWriteFontCollection> fonts;
    HRESULT hr = g.dwrite->GetSystemFontCollection(&fonts, FALSE);
    out("MODE=%s stage=get_system_fonts hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return s;
    UINT32 index = 0;
    BOOL exists = FALSE;
    hr = fonts->FindFamilyName(kFontFamily, &index, &exists);
    out("MODE=%s stage=find_family family=\"Segoe UI\" hr=0x%08lx index=%u exists=%s\n", path,
        static_cast<unsigned long>(hr), index, exists ? "yes" : "no");
    if (FAILED(hr) || !exists) return s;
    ComPtr<IDWriteFontFamily> family;
    hr = fonts->GetFontFamily(index, &family);
    out("MODE=%s stage=get_font_family hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return s;
    ComPtr<IDWriteFont> font;
    hr = family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, &font);
    out("MODE=%s stage=get_first_matching_font hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return s;
    hr = font->CreateFontFace(&s.face);
    out("MODE=%s stage=create_font_face hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return s;
    DWRITE_FONT_METRICS metrics{};
    s.face->GetMetrics(&metrics);
    s.design_units = metrics.designUnitsPerEm;
    UINT32 codepoints[kTextLen];
    for (UINT32 i = 0; i < kTextLen; ++i) codepoints[i] = static_cast<UINT32>(kText[i]);
    hr = s.face->GetGlyphIndices(codepoints, kTextLen, s.glyphs);
    out("MODE=%s stage=get_glyph_indices count=%u hr=0x%08lx\n", path, kTextLen,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return s;
    DWRITE_GLYPH_METRICS glyph_metrics[kTextLen]{};
    hr = s.face->GetDesignGlyphMetrics(s.glyphs, kTextLen, glyph_metrics, FALSE);
    out("MODE=%s stage=get_design_glyph_metrics count=%u hr=0x%08lx design_units=%u\n", path,
        kTextLen, static_cast<unsigned long>(hr), s.design_units);
    if (FAILED(hr)) return s;
    const FLOAT scale =
        (s.design_units > 0) ? kFontSize / static_cast<FLOAT>(s.design_units) : 0.0f;
    for (UINT32 i = 0; i < kTextLen; ++i) {
        s.advances[i] = static_cast<FLOAT>(glyph_metrics[i].advanceWidth) * scale;
        s.advance_sum += s.advances[i];
    }
    s.ascent_px = static_cast<FLOAT>(metrics.ascent) * scale;
    out("MODE=%s stage=glyph_run_ready glyphs=%u,%u,%u,%u,%u,%u,%u,%u advance_sum=%.2f"
        " ascent_px=%.2f baseline_y=%.2f\n",
        path, s.glyphs[0], s.glyphs[1], s.glyphs[2], s.glyphs[3], s.glyphs[4], s.glyphs[5],
        s.glyphs[6], s.glyphs[7], static_cast<double>(s.advance_sum),
        static_cast<double>(s.ascent_px),
        static_cast<double>(static_cast<FLOAT>(kTextTop) + s.ascent_px));
    s.ok = true;
    return s;
}

int finish_text_item(const char* path, const char* item, const char* extra, Gpu& g,
                     Target& t) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!target_map(path, g, t, mapped)) {
        ++g_failures;
        out("MODE=%s ITEM=%s%s RESULT=FAIL reason=map_read_failed\n", path, item, extra);
        return 1;
    }
    const PixelStats s = analyze(static_cast<const uint8_t*>(mapped.pData),
                                 static_cast<int>(mapped.RowPitch), kWidth, kHeight,
                                 text_rect());
    g.context->Unmap(t.staging.Get(), 0);
    return print_ink_item(path, item, extra, s) ? 1 : 0;
}

int run_text_paths(bool warp, bool debug_layer) {
    const char* path = "text-paths";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    int failed = 0;
    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = g.dwrite->CreateTextLayout(kText, kTextLen, g.format.Get(), 496.0f, 112.0f,
                                            &layout);
    out("MODE=%s stage=dwrite_create_layout hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=text_layout_failed\n", path);
        return 2;
    }
    const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

    // a) DrawTextLayout with a solid black brush: the path the device already
    //    reported as inkless, kept as the control.
    {
        Target t;
        if (!target_create(path, g, warp, t)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=solid_brush RESULT=FAIL reason=target_create_failed\n", path);
        } else {
            ComPtr<ID2D1SolidColorBrush> black;
            hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &black);
            out("MODE=%s stage=d2d_solid_brush item=solid_brush hr=0x%08lx\n", path,
                static_cast<unsigned long>(hr));
            if (FAILED(hr)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=solid_brush RESULT=FAIL reason=solid_brush_failed\n",
                    path);
            } else {
                t.rt->BeginDraw();
                t.rt->Clear(white);
                t.rt->DrawTextLayout(D2D1::Point2F(8.0f, 8.0f), layout.Get(), black.Get(),
                                     D2D1_DRAW_TEXT_OPTIONS_NONE);
                hr = t.rt->EndDraw();
                char extra[256];
                _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                            " region=%d,%d,%d,%d alpha_mode=ignore brush=solid_black"
                            " effective_text_aa=%d end_draw_hr=0x%08lx"
                            " expected=dark_gt_50_bbox_ge_8x4_glyph_ink",
                            kTextLeft, kTextTop, kTextRight, kTextBottom,
                            static_cast<int>(t.rt->GetTextAntialiasMode()),
                            static_cast<unsigned long>(hr));
                if (FAILED(hr)) {
                    ++failed;
                    ++g_failures;
                    out("MODE=%s ITEM=solid_brush%s RESULT=FAIL reason=end_draw_failed\n",
                        path, extra);
                } else {
                    failed += finish_text_item(path, "solid_brush", extra, g, t);
                }
            }
        }
    }

    // b) same layout, 1x1 black bitmap brush (clamp/nearest): separates "this
    //    brush type" from "glyph coverage".
    {
        Target t;
        if (!target_create(path, g, warp, t)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=bitmap_brush RESULT=FAIL reason=target_create_failed\n", path);
        } else {
            const uint8_t black_px[4] = {0, 0, 0, 255};
            const D2D1_BITMAP_PROPERTIES bitmap_props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                         D2D1_ALPHA_MODE_IGNORE),
                                       96.0f, 96.0f);
            ComPtr<ID2D1Bitmap> black_bitmap;
            hr = t.rt->CreateBitmap(D2D1::SizeU(1, 1), black_px, 4, bitmap_props,
                                    &black_bitmap);
            out("MODE=%s stage=create_bitmap item=bitmap_brush size=1x1 color=black"
                " hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
            ComPtr<ID2D1BitmapBrush> brush;
            HRESULT hr_brush = E_FAIL;
            if (SUCCEEDED(hr)) {
                const D2D1_BITMAP_BRUSH_PROPERTIES brush_props = D2D1::BitmapBrushProperties(
                    D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                hr_brush = t.rt->CreateBitmapBrush(black_bitmap.Get(), brush_props, &brush);
                out("MODE=%s stage=create_bitmap_brush item=bitmap_brush extend=clamp"
                    " hr=0x%08lx\n", path, static_cast<unsigned long>(hr_brush));
            }
            if (FAILED(hr) || FAILED(hr_brush)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=bitmap_brush RESULT=FAIL"
                    " reason=bitmap_brush_setup_failed\n", path);
            } else {
                t.rt->BeginDraw();
                t.rt->Clear(white);
                t.rt->DrawTextLayout(D2D1::Point2F(8.0f, 8.0f), layout.Get(), brush.Get(),
                                     D2D1_DRAW_TEXT_OPTIONS_NONE);
                hr = t.rt->EndDraw();
                char extra[256];
                _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                            " region=%d,%d,%d,%d alpha_mode=ignore"
                            " brush=bitmap_1x1_black_clamp effective_text_aa=%d"
                            " end_draw_hr=0x%08lx expected=dark_gt_50_bbox_ge_8x4_glyph_ink",
                            kTextLeft, kTextTop, kTextRight, kTextBottom,
                            static_cast<int>(t.rt->GetTextAntialiasMode()),
                            static_cast<unsigned long>(hr));
                if (FAILED(hr)) {
                    ++failed;
                    ++g_failures;
                    out("MODE=%s ITEM=bitmap_brush%s RESULT=FAIL reason=end_draw_failed\n",
                        path, extra);
                } else {
                    failed += finish_text_item(path, "bitmap_brush", extra, g, t);
                }
            }
        }
    }

    // c) antialiasing off, both the geometry and the text mode set.
    {
        Target t;
        if (!target_create(path, g, warp, t)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=text_aliased RESULT=FAIL reason=target_create_failed\n", path);
        } else {
            t.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            t.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
            const int effective_aa = static_cast<int>(t.rt->GetAntialiasMode());
            const int effective_text_aa = static_cast<int>(t.rt->GetTextAntialiasMode());
            ComPtr<ID2D1SolidColorBrush> black;
            hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &black);
            if (FAILED(hr)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=text_aliased stage=d2d_solid_brush hr=0x%08lx RESULT=FAIL\n",
                    path, static_cast<unsigned long>(hr));
            } else {
                t.rt->BeginDraw();
                t.rt->Clear(white);
                t.rt->DrawTextLayout(D2D1::Point2F(8.0f, 8.0f), layout.Get(), black.Get(),
                                     D2D1_DRAW_TEXT_OPTIONS_NONE);
                hr = t.rt->EndDraw();
                char extra[256];
                _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                            " region=%d,%d,%d,%d alpha_mode=ignore brush=solid_black"
                            " effective_aa=%d effective_text_aa=%d end_draw_hr=0x%08lx"
                            " expected=dark_gt_50_bbox_ge_8x4_glyph_ink_no_coverage",
                            kTextLeft, kTextTop, kTextRight, kTextBottom, effective_aa,
                            effective_text_aa, static_cast<unsigned long>(hr));
                if (FAILED(hr)) {
                    ++failed;
                    ++g_failures;
                    out("MODE=%s ITEM=text_aliased%s RESULT=FAIL reason=end_draw_failed\n",
                        path, extra);
                } else {
                    failed += finish_text_item(path, "text_aliased", extra, g, t);
                }
            }
        }
    }

    // d) DrawGlyphRun directly: no text layout, no glyph-run cache, explicit
    //    glyph indices and advances from the font face.
    {
        const GlyphSource gs = glyph_source(path, g);
        if (!gs.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=glyph_run RESULT=FAIL reason=font_face_or_metrics_failed\n",
                path);
        } else {
            Target t;
            if (!target_create(path, g, warp, t)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=glyph_run RESULT=FAIL reason=target_create_failed\n", path);
            } else {
                ComPtr<ID2D1SolidColorBrush> black;
                hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                                                &black);
                if (FAILED(hr)) {
                    ++failed;
                    ++g_failures;
                    out("MODE=%s ITEM=glyph_run stage=d2d_solid_brush hr=0x%08lx"
                        " RESULT=FAIL\n", path, static_cast<unsigned long>(hr));
                } else {
                    DWRITE_GLYPH_RUN run{};
                    run.fontFace = gs.face.Get();
                    run.fontEmSize = kFontSize;
                    run.glyphCount = kTextLen;
                    run.glyphIndices = gs.glyphs;
                    run.glyphAdvances = gs.advances;
                    run.glyphOffsets = nullptr;
                    run.isSideways = FALSE;
                    run.bidiLevel = 0;
                    t.rt->BeginDraw();
                    t.rt->Clear(white);
                    // DrawGlyphRun takes a BASELINE origin, not a layout top-left,
                    // so shift down by the font ascent or the glyph bodies would
                    // be drawn above the surface.
                    const FLOAT baseline_y =
                        static_cast<FLOAT>(kTextTop) + gs.ascent_px;
                    // DrawGlyphRun returns void; EndDraw is the error signal.
                    t.rt->DrawGlyphRun(D2D1::Point2F(static_cast<FLOAT>(kTextLeft), baseline_y),
                                       &run, black.Get(), DWRITE_MEASURING_MODE_NATURAL);
                    hr = t.rt->EndDraw();
                    char extra[256];
                    _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                                " region=%d,%d,%d,%d alpha_mode=ignore brush=solid_black"
                                " glyphs=%u advance_sum=%.2f baseline_y=%.2f"
                                " effective_text_aa=%d end_draw_hr=0x%08lx"
                                " expected=dark_gt_50_bbox_ge_8x4_glyph_ink_direct_run",
                                kTextLeft, kTextTop, kTextRight, kTextBottom, kTextLen,
                                static_cast<double>(gs.advance_sum),
                                static_cast<double>(baseline_y),
                                static_cast<int>(t.rt->GetTextAntialiasMode()),
                                static_cast<unsigned long>(hr));
                    if (FAILED(hr)) {
                        ++failed;
                        ++g_failures;
                        out("MODE=%s ITEM=glyph_run%s RESULT=FAIL reason=end_draw_failed\n",
                            path, extra);
                    } else {
                        failed += finish_text_item(path, "glyph_run", extra, g, t);
                    }
                }
            }
        }
    }

    // e) the same GRAYSCALE text draw into an ALPHA_MODE_IGNORE and an
    //    ALPHA_MODE_PREMULTIPLIED target: the alpha mode is the only difference.
    struct AlphaCase {
        D2D1_ALPHA_MODE mode;
        const char* name;
        const char* item;
    };
    const AlphaCase alpha_cases[] = {
        {D2D1_ALPHA_MODE_IGNORE, "ignore", "target_alpha_ignore"},
        {D2D1_ALPHA_MODE_PREMULTIPLIED, "premultiplied", "target_alpha_premultiplied"},
    };
    for (const AlphaCase& c : alpha_cases) {
        Target t;
        if (!target_create_alpha(path, g, warp, c.mode, c.name, t)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed alpha_mode=%s\n",
                path, c.item, c.name);
            continue;
        }
        t.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const int effective_text_aa = static_cast<int>(t.rt->GetTextAntialiasMode());
        ComPtr<ID2D1SolidColorBrush> black;
        hr = t.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &black);
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s stage=d2d_solid_brush hr=0x%08lx RESULT=FAIL\n", path,
                c.item, static_cast<unsigned long>(hr));
            continue;
        }
        t.rt->BeginDraw();
        t.rt->Clear(white);
        t.rt->DrawTextLayout(D2D1::Point2F(8.0f, 8.0f), layout.Get(), black.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
        hr = t.rt->EndDraw();
        char extra[256];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " region=%d,%d,%d,%d alpha_mode=%s brush=solid_black"
                    " effective_text_aa=%d end_draw_hr=0x%08lx"
                    " expected=dark_gt_50_bbox_ge_8x4_glyph_ink_grayscale",
                    kTextLeft, kTextTop, kTextRight, kTextBottom, c.name, effective_text_aa,
                    static_cast<unsigned long>(hr));
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s%s RESULT=FAIL reason=end_draw_failed\n", path, c.item,
                extra);
            continue;
        }
        failed += finish_text_item(path, c.item, extra, g, t);
    }

    out("MODE=%s SUMMARY items=6 failed=%d targets=6 separate_readbacks=yes"
        " text=\"TEST 123\" font=\"Segoe UI\" size_px=48\n", path, failed);
    if (failed) ++g_failures;
    return failed ? 2 : 0;
}

// ============================================================================
// v5 modes: clip/discard translation, R32_UINT coverage-mask round trips with
// typed Load + countbits, and dual-source blend factors.
//
// Why these three: a D2D antialiased stroke is not MSAA. D2D rasterizes
// coverage into a uint mask texture, then a second pass turns the per-pixel
// coverage into alpha with countbits (a 16x16 cell packs 256 samples, each bit
// one sample) and the mask PS ends with clip(cov - 1/32) so zero-coverage
// cells are dropped. Text is the same machinery with an A8 glyph atlas plus a
// lookup. So the suspects are (1) clip/discard translation to the UMD,
// (2) R32_UINT typed Load and countbits on a texture that was just written, and
// (3) the dual-source blend factors a coverage shader needs. Each mode below
// isolates one of those, with a control that says what the same code does when
// the feature works.
// ============================================================================
constexpr int kClipSize = 64;
constexpr int kMaskSize = 256;
constexpr int kDualSize = 64;

// A full-cover draw either paints everywhere or paints nothing, so the only
// question is whether ink exists; the bbox requirement is kept so a single
// stray dark pixel cannot pass as "the quad was kept".
const char* expect_ink_reason(const PixelStats& s, bool expect_ink) {
    const bool ink = s.dark > kMinDarkPixels;
    if (expect_ink) {
        if (!ink) return "expected_ink_but_dark_zero";
        if (!s.dark_bbox_valid) return "expected_ink_but_no_dark_bbox";
        if (s.dark_right - s.dark_left < kItemMinBBoxWidth)
            return "expected_ink_but_bbox_too_narrow";
        if (s.dark_bottom - s.dark_top < kItemMinBBoxHeight)
            return "expected_ink_but_bbox_too_short";
        return nullptr;
    }
    if (ink) return "expected_discard_but_ink_present";
    return nullptr;
}

// clip() and the discard keyword are two spellings of the same requirement:
// when the coverage expression is negative the pixel must not be written. The
// two variants are kept separate because a translator can support one and drop
// the other.
const char* kClipPs =
    "cbuffer Args : register(b0) { float x; };\n"
    "float4 main() : SV_Target {\n"
    "    clip(x - 0.5);\n"
    "    return float4(0.0, 0.0, 0.0, 1.0);\n"
    "}\n";

const char* kDiscardPs =
    "cbuffer Args : register(b0) { float x; };\n"
    "float4 main() : SV_Target {\n"
    "    if (x - 0.5 < 0.0) discard;\n"
    "    return float4(0.0, 0.0, 0.0, 1.0);\n"
    "}\n";

int run_clip_contract(bool warp, bool debug_layer) {
    const char* path = "clip-contract";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob, clip50_blob, discard50_blob, clip40_blob;
    const char* used_profile = nullptr;
    HRESULT hr = a8_compile(path, "clip_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "clip_ps_clip_ps50", kClipPs, "ps_5_0", "ps_5_0",
                        clip50_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "clip_ps_discard_ps50", kDiscardPs, "ps_5_0", "ps_5_0",
                        discard50_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "clip_ps_clip_ps40", kClipPs, "ps_4_0", "ps_4_0",
                        clip40_blob.GetAddressOf(), &used_profile);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_clip50, ps_discard50, ps_clip40;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, &vs);
    out("MODE=%s stage=create_vs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(clip50_blob->GetBufferPointer(),
                                         clip50_blob->GetBufferSize(), nullptr, &ps_clip50);
        out("MODE=%s stage=create_ps clip_variant=clip profile=ps_5_0 hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(discard50_blob->GetBufferPointer(),
                                         discard50_blob->GetBufferSize(), nullptr,
                                         &ps_discard50);
        out("MODE=%s stage=create_ps clip_variant=discard profile=ps_5_0 hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(clip40_blob->GetBufferPointer(),
                                         clip40_blob->GetBufferSize(), nullptr, &ps_clip40);
        out("MODE=%s stage=create_ps clip_variant=clip profile=ps_4_0 hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    if (!small_pipeline(path, g, kClipSize, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=state_create_failed\n", path);
        return 2;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = 16;
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> cb;
    const HRESULT hr_cb = g.device->CreateBuffer(&cb_desc, nullptr, &cb);
    out("MODE=%s stage=create_constant_buffer bytes=16 usage=dynamic hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_cb));
    if (FAILED(hr_cb)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=constant_buffer_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr_cb));
        return 2;
    }

    struct ClipCase {
        const char* item;
        ID3D11PixelShader* ps;
        const char* variant;
        const char* profile;
        float x;
        bool expect_ink;
    };
    const ClipCase cases[] = {
        {"clip_keep", ps_clip50.Get(), "clip", "ps_5_0", 0.75f, true},
        {"clip_discard", ps_clip50.Get(), "clip", "ps_5_0", 0.25f, false},
        {"discard_keep", ps_discard50.Get(), "discard", "ps_5_0", 0.75f, true},
        {"discard_discard", ps_discard50.Get(), "discard", "ps_5_0", 0.25f, false},
        {"clip_ps40_keep", ps_clip40.Get(), "clip", "ps_4_0", 0.75f, true},
        {"clip_ps40_discard", ps_clip40.Get(), "clip", "ps_4_0", 0.25f, false},
    };
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    int failed = 0;
    int keep_with_ink = 0;
    int keep_items = 0;
    int discard_without_ink = 0;
    int discard_items = 0;

    for (const ClipCase& c : cases) {
        ComPtr<ID3D11Texture2D> tex, staging;
        if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kClipSize,
                           D3D11_BIND_RENDER_TARGET, tex, staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed\n", path, c.item);
            continue;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        HRESULT hr_rtv = g.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
        if (FAILED(hr_rtv)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=rtv_create_failed hr=0x%08lx\n", path,
                c.item, static_cast<unsigned long>(hr_rtv));
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped_cb{};
        HRESULT hr_map = g.context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_cb);
        if (FAILED(hr_map)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=constant_map_failed hr=0x%08lx\n", path,
                c.item, static_cast<unsigned long>(hr_map));
            continue;
        }
        float args[4] = {c.x, 0.0f, 0.0f, 0.0f};
        memcpy(mapped_cb.pData, args, sizeof(args));
        g.context->Unmap(cb.Get(), 0);

        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->ClearRenderTargetView(rtv.Get(), white);
        ID3D11Buffer* cb_ptr = cb.Get();
        g.context->PSSetConstantBuffers(0, 1, &cb_ptr);
        g.context->PSSetShader(c.ps, nullptr, 0);
        g.context->Draw(3, 0);
        ID3D11Buffer* no_cb = nullptr;
        g.context->PSSetConstantBuffers(0, 1, &no_cb);
        g.context->Flush();
        const HRESULT hr_draw = g.device->GetDeviceRemovedReason();
        g.context->OMSetRenderTargets(0, nullptr, nullptr);

        const SmallRead back =
            small_read(path, g.context.Get(), tex.Get(), staging.Get(), kClipSize, 4);
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, c.item);
            continue;
        }
        const PixelStats s =
            analyze(back.bytes.data(), back.pitch, kClipSize, kClipSize,
                    RECT{0, 0, kClipSize, kClipSize});
        const char* reason = expect_ink_reason(s, c.expect_ink);
        char extra[320];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " region=0,0,%d,%d variant=%s profile=%s x=%.2f expected=%s"
                    " hr_draw=0x%08lx", kClipSize, kClipSize, c.variant, c.profile,
                    static_cast<double>(c.x), c.expect_ink ? "ink" : "no_ink",
                    static_cast<unsigned long>(hr_draw));
        if (c.expect_ink) {
            ++keep_items;
            if (s.dark > kMinDarkPixels) ++keep_with_ink;
        } else {
            ++discard_items;
            if (s.dark <= kMinDarkPixels) ++discard_without_ink;
        }
        if (print_ink_item_with(path, c.item, extra, s, reason)) ++failed;
    }

    const char* verdict = "mixed";
    if (keep_with_ink == 0) {
        verdict = "always_discards";
    } else if (discard_without_ink == 0) {
        verdict = "always_keeps";
    } else if (keep_with_ink == keep_items && discard_without_ink == discard_items) {
        verdict = "clip_and_discard_respected";
    }
    out("MODE=%s SUMMARY items=%d failed=%d keep_with_ink=%d/%d discard_without_ink=%d/%d"
        " verdict=%s\n", path, keep_items + discard_items, failed, keep_with_ink, keep_items,
        discard_without_ink, discard_items, verdict);
    if (failed) ++g_failures;
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// mask-uint: the coverage-mask shape. A 256x256 R32_UINT texture is written by
// a PS (one uint per pixel, bits 0 and 16 set) and by a typed UAV store, then
// read back both as raw uint (must be exactly 0x00010001) and through a uint
// SRV + countbits in a second pass (must be 2/16 = 0.125 -> about 32/255).
// The array variant indexes an 8-element texture array with a cbuffer value,
// which is how a glyph LUT/mask lookup can be spelled. A result of 0 in the
// countbits items is the bug signature: the mask exists but reads as empty.
// ---------------------------------------------------------------------------
const char* kMaskPsWrite =
    "uint4 main() : SV_Target {\n"
    "    return uint4(65537u, 0u, 0u, 0u);\n"
    "}\n";

const char* kMaskCsStore =
    "RWTexture2D<uint> dst : register(u0);\n"
    "[numthreads(16, 16, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    dst[id.xy] = 65537u;\n"
    "}\n";

const char* kMaskPsSample =
    "Texture2D<uint> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    uint v = src.Load(int3(int2(pos.xy), 0));\n"
    "    float c = (float)countbits(v) / 16.0;\n"
    "    return float4(c, c, c, 1.0);\n"
    "}\n";

// Eight separately bound uint SRVs selected from a cbuffer value. The literal
// shape "Texture2D<uint> g[8]; g[idx]" does NOT compile: HLSL reports
// "X3512: sampler array index must be a literal expression", so a compiled
// shader can only reach one of several masks through a chain like this (or
// through an array-slice coordinate). This item therefore tests the part a
// driver actually has to translate: several uint SRVs bound at once plus a
// non-constant choice between them.
const char* kMaskPsArray =
    "cbuffer Args : register(b0) { uint idx; };\n"
    "Texture2D<uint> g0 : register(t0);\n"
    "Texture2D<uint> g1 : register(t1);\n"
    "Texture2D<uint> g2 : register(t2);\n"
    "Texture2D<uint> g3 : register(t3);\n"
    "Texture2D<uint> g4 : register(t4);\n"
    "Texture2D<uint> g5 : register(t5);\n"
    "Texture2D<uint> g6 : register(t6);\n"
    "Texture2D<uint> g7 : register(t7);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    int3 p = int3(int2(pos.xy), 0);\n"
    "    uint v = g0.Load(p);\n"
    "    if (idx == 1u) v = g1.Load(p);\n"
    "    else if (idx == 2u) v = g2.Load(p);\n"
    "    else if (idx == 3u) v = g3.Load(p);\n"
    "    else if (idx == 4u) v = g4.Load(p);\n"
    "    else if (idx == 5u) v = g5.Load(p);\n"
    "    else if (idx == 6u) v = g6.Load(p);\n"
    "    else if (idx == 7u) v = g7.Load(p);\n"
    "    float c = (float)countbits(v) / 16.0;\n"
    "    return float4(c, c, c, 1.0);\n"
    "}\n";

// Raw uint readback: every texel must be exactly the written value, so the
// actual value is reported as min/max plus the mismatch count.
const char* print_uint_item(const char* mode, const char* item, const char* extra,
                            const SmallRead& r, int size, unsigned want) {
    unsigned lo = 0xFFFFFFFFu;
    unsigned hi = 0;
    int bad = 0;
    for (int y = 0; y < size; ++y) {
        const uint8_t* row = r.bytes.data() + static_cast<size_t>(y) * r.pitch;
        for (int x = 0; x < size; ++x) {
            const uint8_t* p = row + x * 4;
            const unsigned v = static_cast<unsigned>(p[0]) |
                               (static_cast<unsigned>(p[1]) << 8) |
                               (static_cast<unsigned>(p[2]) << 16) |
                               (static_cast<unsigned>(p[3]) << 24);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            if (v != want) ++bad;
        }
    }
    const char* reason = bad ? "value_mismatch" : nullptr;
    out("MODE=%s ITEM=%s%s RESULT=%s texels=%d mismatches=%d expected_uint=0x%08x"
        " actual_min_uint=0x%08x actual_max_uint=0x%08x reason=%s\n", mode, item, extra,
        reason ? "FAIL" : "PASS", size * size, bad, want, lo, hi,
        reason ? reason : "ok");
    if (reason) ++g_failures;
    return reason;
}

int run_mask_uint(bool warp, bool debug_layer) {
    const char* path = "mask-uint";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    UINT support = 0;
    const HRESULT hr_support = g.device->CheckFormatSupport(DXGI_FORMAT_R32_UINT, &support);
    out("MODE=%s stage=format_support fmt=R32_UINT hr=0x%08lx texture2d=%s render_target=%s"
        " shader_load=%s typed_uav=%s\n", path, static_cast<unsigned long>(hr_support),
        (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? "yes" : "no",
        (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? "yes" : "no",
        (support & D3D11_FORMAT_SUPPORT_SHADER_LOAD) ? "yes" : "no",
        (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) ? "yes" : "no");

    ComPtr<ID3DBlob> vs_blob, ps_write_blob, ps_sample_blob, ps_array_blob, cs_store_blob;
    const char* used_profile = nullptr;
    HRESULT hr = a8_compile(path, "mask_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "mask_ps_write", kMaskPsWrite, "ps_5_0", "ps_4_0",
                        ps_write_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "mask_ps_sample", kMaskPsSample, "ps_5_0", "ps_4_0",
                        ps_sample_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "mask_cs_store", kMaskCsStore, "cs_5_0", "cs_4_0",
                        cs_store_blob.GetAddressOf(), &used_profile);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    // The eight-SRV variant is optional: if the compiler rejects it the other
    // items still produce values and this one is reported as SKIP, so a probe
    // shader problem never hides the evidence the mode exists to collect.
    const HRESULT hr_array_profile =
        a8_compile(path, "mask_ps_array", kMaskPsArray, "ps_5_0", "ps_5_0",
                   ps_array_blob.GetAddressOf(), &used_profile);
    if (FAILED(hr_array_profile))
        out("MODE=%s stage=optional_shader_unavailable shader=mask_ps_array hr=0x%08lx\n",
            path, static_cast<unsigned long>(hr_array_profile));

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_write, ps_sample, ps_array;
    ComPtr<ID3D11ComputeShader> cs_store;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, &vs);
    out("MODE=%s stage=create_vs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_write_blob->GetBufferPointer(),
                                         ps_write_blob->GetBufferSize(), nullptr, &ps_write);
        out("MODE=%s stage=create_ps name=write_uint hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_sample_blob->GetBufferPointer(),
                                         ps_sample_blob->GetBufferSize(), nullptr,
                                         &ps_sample);
        out("MODE=%s stage=create_ps name=countbits_sample hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr) && ps_array_blob) {
        hr = g.device->CreatePixelShader(ps_array_blob->GetBufferPointer(),
                                         ps_array_blob->GetBufferSize(), nullptr, &ps_array);
        out("MODE=%s stage=create_ps name=countbits_array hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreateComputeShader(cs_store_blob->GetBufferPointer(),
                                           cs_store_blob->GetBufferSize(), nullptr,
                                           &cs_store);
        out("MODE=%s stage=create_cs name=uav_store_uint hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    if (!small_pipeline(path, g, kMaskSize, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=state_create_failed\n", path);
        return 2;
    }

    // One texture, three bindings: rendered to, stored to by a UAV, sampled.
    ComPtr<ID3D11Texture2D> mask, mask_staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_R32_UINT, kMaskSize,
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                           D3D11_BIND_UNORDERED_ACCESS,
                       mask, mask_staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=r32uint_texture_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    const HRESULT hr_rtv = g.device->CreateRenderTargetView(mask.Get(), nullptr, &rtv);
    const HRESULT hr_srv = g.device->CreateShaderResourceView(mask.Get(), nullptr, &srv);
    const HRESULT hr_uav = g.device->CreateUnorderedAccessView(mask.Get(), nullptr, &uav);
    out("MODE=%s stage=create_views rtv_hr=0x%08lx srv_hr=0x%08lx uav_hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_rtv), static_cast<unsigned long>(hr_srv),
        static_cast<unsigned long>(hr_uav));
    const bool have_rtv = SUCCEEDED(hr_rtv);
    const bool have_srv = SUCCEEDED(hr_srv);
    const bool have_uav = SUCCEEDED(hr_uav);

    const unsigned want_uint = 65537u;
    const unsigned want_count = 32u;  // 2/16 * 255 = 31.875
    int failed = 0;
    int items = 0;
    int skipped = 0;
    char extra[320];

    // (a) PS writes one uint per pixel.
    if (!have_rtv) {
        out("MODE=%s ITEM=ps_write_uint RESULT=SKIP reason=rtv_create_failed\n", path);
        ++skipped;
    } else {
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), clear);
        g.context->PSSetShader(ps_write.Get(), nullptr, 0);
        g.context->Draw(3, 0);
        g.context->Flush();
        const HRESULT hr_draw = g.device->GetDeviceRemovedReason();
        g.context->OMSetRenderTargets(0, nullptr, nullptr);
        const SmallRead back =
            small_read(path, g.context.Get(), mask.Get(), mask_staging.Get(), kMaskSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=ps_write_uint RESULT=FAIL reason=map_read_failed\n", path);
        } else {
            _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                        " path=ps_uint4_to_R32_UINT expected=0x00010001"
                        " bits_set=2 hr_draw=0x%08lx", static_cast<unsigned long>(hr_draw));
            if (print_uint_item(path, "ps_write_uint", extra, back, kMaskSize, want_uint))
                ++failed;
        }
    }

    // (b) the same value from a typed UAV store in a compute shader.
    if (!have_uav) {
        out("MODE=%s ITEM=uav_store_uint RESULT=SKIP reason=uav_create_failed\n", path);
        ++skipped;
    } else {
        g.context->CSSetShader(cs_store.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* uav_ptr = uav.Get();
        g.context->CSSetUnorderedAccessViews(0, 1, &uav_ptr, nullptr);
        const UINT groups = static_cast<UINT>(kMaskSize / 16);
        g.context->Dispatch(groups, groups, 1);
        ID3D11UnorderedAccessView* no_uav = nullptr;
        g.context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
        g.context->CSSetShader(nullptr, nullptr, 0);
        g.context->Flush();
        const HRESULT hr_draw = g.device->GetDeviceRemovedReason();
        const SmallRead back =
            small_read(path, g.context.Get(), mask.Get(), mask_staging.Get(), kMaskSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=uav_store_uint RESULT=FAIL reason=map_read_failed\n", path);
        } else {
            _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                        " path=typed_uav_store expected=0x00010001 bits_set=2"
                        " dispatch=%ux%ux1 hr_draw=0x%08lx", groups, groups,
                        static_cast<unsigned long>(hr_draw));
            if (print_uint_item(path, "uav_store_uint", extra, back, kMaskSize, want_uint))
                ++failed;
        }
    }

    // (c) and (d): the mask read through a uint SRV and countbits into BGRA.
    ComPtr<ID3D11Texture2D> out_tex, out_staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kMaskSize,
                       D3D11_BIND_RENDER_TARGET, out_tex, out_staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=bgra_target_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> out_rtv;
    const HRESULT hr_out_rtv = g.device->CreateRenderTargetView(out_tex.Get(), nullptr,
                                                               &out_rtv);
    out("MODE=%s stage=create_bgra_rtv hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_out_rtv));

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = 16;
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> cb;
    const HRESULT hr_cb = g.device->CreateBuffer(&cb_desc, nullptr, &cb);
    out("MODE=%s stage=create_constant_buffer bytes=16 usage=dynamic hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr_cb));

    struct SampleVariant {
        const char* item;
        ID3D11PixelShader* ps;
        bool array_index;
        const char* note;
    };
    const SampleVariant variants[] = {
        {"countbits_sample", ps_sample.Get(), false,
         "ps=Texture2D_uint_Load_then_countbits_div16"},
        {"countbits_array_index", ps_array.Get(), true,
         "ps=eight_uint_SRVs_cbuffer_selected_idx3_then_countbits"},
    };

    for (const SampleVariant& v : variants) {
        if (!have_srv || FAILED(hr_out_rtv)) {
            out("MODE=%s ITEM=%s RESULT=SKIP reason=srv_or_rtv_create_failed\n", path,
                v.item);
            ++skipped;
            continue;
        }
        if (v.array_index && (FAILED(hr_cb) || !ps_array)) {
            out("MODE=%s ITEM=%s RESULT=SKIP reason=array_variant_unavailable hr=0x%08lx\n",
                path, v.item, static_cast<unsigned long>(hr_cb));
            ++skipped;
            continue;
        }
        if (v.array_index) {
            D3D11_MAPPED_SUBRESOURCE mapped_cb{};
            const HRESULT hr_map =
                g.context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_cb);
            if (FAILED(hr_map)) {
                ++failed;
                ++g_failures;
                out("MODE=%s ITEM=%s RESULT=FAIL reason=constant_map_failed hr=0x%08lx\n",
                    path, v.item, static_cast<unsigned long>(hr_map));
                continue;
            }
            unsigned idx_args[4] = {3u, 0u, 0u, 0u};
            memcpy(mapped_cb.pData, idx_args, sizeof(idx_args));
            g.context->Unmap(cb.Get(), 0);
        }

        ID3D11RenderTargetView* out_rtv_ptr = out_rtv.Get();
        g.context->OMSetRenderTargets(1, &out_rtv_ptr, nullptr);
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(out_rtv.Get(), clear);
        if (v.array_index) {
            ID3D11ShaderResourceView* views[8];
            for (int k = 0; k < 8; ++k) views[k] = srv.Get();
            g.context->PSSetShaderResources(0, 8, views);
            ID3D11Buffer* cb_ptr = cb.Get();
            g.context->PSSetConstantBuffers(0, 1, &cb_ptr);
        } else {
            ID3D11ShaderResourceView* view = srv.Get();
            g.context->PSSetShaderResources(0, 1, &view);
        }
        g.context->PSSetShader(v.ps, nullptr, 0);
        g.context->Draw(3, 0);
        ID3D11ShaderResourceView* no_srvs[8] = {nullptr, nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr, nullptr};
        g.context->PSSetShaderResources(0, 8, no_srvs);
        g.context->Flush();
        const HRESULT hr_draw = g.device->GetDeviceRemovedReason();
        g.context->OMSetRenderTargets(0, nullptr, nullptr);

        const SmallRead back = small_read(path, g.context.Get(), out_tex.Get(),
                                          out_staging.Get(), kMaskSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, v.item);
            continue;
        }
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " path=countbits_of_0x00010001 expected=32_of_255_value_range_31_to_33"
                    " note=%s hr_draw=0x%08lx", v.note, static_cast<unsigned long>(hr_draw));
        if (print_plane_item(path, v.item, extra, back, kMaskSize,
                             Rgba{static_cast<int>(want_count), static_cast<int>(want_count),
                                  static_cast<int>(want_count), 255},
                             2))
            ++failed;
    }

    out("MODE=%s SUMMARY items=%d failed=%d skipped=%d formats=R32_UINT,B8G8R8A8_UNORM"
        " size=%dx%d expected_uint=0x00010001 expected_countbits_value=32\n", path, items,
        failed, skipped, kMaskSize, kMaskSize);
    if (failed) ++g_failures;
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// dualsrc-blend: the blend factors a coverage shader uses. The PS writes an
// opaque black to SV_Target0 and alpha 0.5 to SV_Target1, the target is cleared
// white, and only the SrcBlend/DestBlend factors change between items.
//   src1_alpha   SRC1_ALPHA/INV_SRC1_ALPHA  -> 0.5 black over white = 128 grey
//   src1_color   SRC1_COLOR/INV_SRC1_COLOR  -> source1 rgb is (0,0,0), so the
//                                              factor is 0 and white survives;
//                                              a driver that maps SRC1_COLOR to
//                                              SRC1_ALPHA reports 128 here
//   src_alpha    SRC_ALPHA/INV_SRC_ALPHA    -> source0 alpha is 1, fully black
// ---------------------------------------------------------------------------
const char* kDualPs =
    "struct PSOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };\n"
    "PSOut main() {\n"
    "    PSOut o;\n"
    "    o.c0 = float4(0.0, 0.0, 0.0, 1.0);\n"
    "    o.c1 = float4(0.0, 0.0, 0.0, 0.5);\n"
    "    return o;\n"
    "}\n";

int run_dualsrc_blend(bool warp, bool debug_layer) {
    const char* path = "dualsrc-blend";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob, ps_blob;
    const char* used_profile = nullptr;
    HRESULT hr = a8_compile(path, "dual_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used_profile);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "dual_ps", kDualPs, "ps_5_0", "ps_4_0",
                        ps_blob.GetAddressOf(), &used_profile);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, &vs);
    out("MODE=%s stage=create_vs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(), nullptr, &ps);
        out("MODE=%s stage=create_ps targets=2 dual_source=yes hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    if (!small_pipeline(path, g, kDualSize, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=state_create_failed\n", path);
        return 2;
    }

    struct BlendCase {
        const char* item;
        D3D11_BLEND src;
        D3D11_BLEND dst;
        const char* factors;
        Rgba want;
        const char* expected_text;
    };
    const BlendCase cases[] = {
        {"src1_alpha_blend", D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_ALPHA,
         "SRC1_ALPHA/INV_SRC1_ALPHA", Rgba{128, 128, 128, 255},
         "0.5_black_over_white_grey_128"},
        {"src1_color_blend", D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_INV_SRC1_COLOR,
         "SRC1_COLOR/INV_SRC1_COLOR", Rgba{255, 255, 255, 255},
         "source1_rgb_black_factor_zero_white_255"},
        {"src_alpha_blend", D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
         "SRC_ALPHA/INV_SRC_ALPHA", Rgba{0, 0, 0, 255}, "source0_alpha_one_black_0"},
    };
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    int failed = 0;
    int items = 0;
    int skipped = 0;

    for (const BlendCase& c : cases) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = c.src;
        bd.RenderTarget[0].DestBlend = c.dst;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> blend;
        const HRESULT hr_blend = g.device->CreateBlendState(&bd, &blend);
        out("MODE=%s stage=create_blend_state factors=%s hr=0x%08lx\n", path, c.factors,
            static_cast<unsigned long>(hr_blend));
        if (FAILED(hr_blend)) {
            out("MODE=%s ITEM=%s RESULT=SKIP reason=blend_state_create_failed\n", path,
                c.item);
            ++skipped;
            continue;
        }

        ComPtr<ID3D11Texture2D> tex, staging;
        if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kDualSize,
                           D3D11_BIND_RENDER_TARGET, tex, staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed\n", path, c.item);
            continue;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        const HRESULT hr_rtv = g.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
        if (FAILED(hr_rtv)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=rtv_create_failed hr=0x%08lx\n", path,
                c.item, static_cast<unsigned long>(hr_rtv));
            continue;
        }
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->ClearRenderTargetView(rtv.Get(), white);
        const FLOAT blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        g.context->OMSetBlendState(blend.Get(), blend_factor, 0xffffffff);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        g.context->Draw(3, 0);
        g.context->Flush();
        const HRESULT hr_draw = g.device->GetDeviceRemovedReason();
        g.context->OMSetRenderTargets(0, nullptr, nullptr);

        const SmallRead back = small_read(path, g.context.Get(), tex.Get(), staging.Get(),
                                          kDualSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, c.item);
            continue;
        }
        const RECT rect{0, 0, kDualSize, kDualSize};
        const PixelStats s = analyze(back.bytes.data(), back.pitch, kDualSize, kDualSize,
                                     rect);
        const unsigned long long matched =
            count_color_match(back.bytes.data(), back.pitch, rect, c.want, 2);
        const long long area = static_cast<long long>(kDualSize) * kDualSize;
        char extra[320];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE,
                    " region=0,0,%d,%d blend=%s blend_factor=1,1,1,1"
                    " expected=%s alpha_channel_not_compared hr_draw=0x%08lx", kDualSize,
                    kDualSize, c.factors, c.expected_text,
                    static_cast<unsigned long>(hr_draw));
        // count_color_match compares B/G/R only, so the alpha channel of the
        // readback is reported by analyze/plane stats instead of being asserted.
        if (print_color_item(path, c.item, c.want,
                             Rgba{back.bytes[0], back.bytes[1], back.bytes[2], back.bytes[3]},
                             matched, area, s))
            ++failed;
    }

    out("MODE=%s SUMMARY items=%d failed=%d skipped=%d targets=%d"
        " ps_outputs=SV_Target0_opaque_black,SV_Target1_alpha_0.5\n", path, items, failed,
        skipped, items);
    if (failed) ++g_failures;
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// v6: vertex/index buffer paths
// ---------------------------------------------------------------------------
// Every earlier mode drew with SV_VertexID and Draw(3, 0): no input layout, no
// vertex buffer, no index buffer. D2D, however, batches geometry through
// vertex and index buffers, and its internal "Atlas blend vertex buffer/index
// buffer" is the only index-buffer user in the text path. These cases cover
// what was previously invisible: explicit vertex buffers, R16 and R32 index
// buffers, dynamic buffers refilled with Map(WRITE_DISCARD), two indexed
// ranges in one frame, and an instanced indexed draw.

const char* kQuadVsSource =
    "struct VSIn { float2 pos : POSITION; };\n"
    "float4 main(VSIn i) : SV_POSITION { return float4(i.pos, 0.0f, 1.0f); }\n";

const char* kQuadVsInstSource =
    "struct VSIn { float2 pos : POSITION; };\n"
    "float4 main(VSIn i, uint id : SV_InstanceID) : SV_POSITION {\n"
    "    float2 p = i.pos; p.x += float(id);\n"
    "    return float4(p, 0.0f, 1.0f);\n"
    "}\n";

const char* kQuadPsSource =
    "float4 main() : SV_Target { return float4(0.0f, 0.0f, 0.0f, 1.0f); }\n";

constexpr int kVbSize = 256;
constexpr int kVbMinDark = 5000;

// A quad from x=-1..0, y=-1..1 in NDC: six vertices, no index buffer.
const float kQuad6Left[] = {
    -1.0f, -1.0f, -1.0f, 1.0f, 0.0f, -1.0f,
     0.0f, -1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
};

// The same quad as four vertices plus indices.
const float kQuad4Left[] = {
    -1.0f, -1.0f, -1.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f,
};

// Left and right halves: the second range starts at vertex 4.
const float kQuad4TwoRanges[] = {
    -1.0f, -1.0f, -1.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f,
     0.0f, -1.0f,  0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
};

const unsigned short kIdx6[] = {0, 1, 2, 2, 1, 3};
const unsigned short kIdx12[] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
const unsigned int kIdx6_32[] = {0, 1, 2, 2, 1, 3};

bool create_buffer(const char* path, ID3D11Device* device, const void* data, UINT bytes,
                   UINT bind, D3D11_USAGE usage, ComPtr<ID3D11Buffer>& buffer) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = usage;
    desc.BindFlags = bind;
    if (usage == D3D11_USAGE_DYNAMIC) desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    const HRESULT hr = device->CreateBuffer(&desc, data ? &init : nullptr,
                                            buffer.GetAddressOf());
    out("MODE=%s stage=create_buffer bytes=%u bind=0x%x usage=%u hr=0x%08lx\n", path, bytes,
        bind, static_cast<unsigned>(usage), static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

bool fill_dynamic(const char* path, ID3D11DeviceContext* context, ID3D11Buffer* buffer,
                  const void* data, UINT bytes) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    out("MODE=%s stage=map_write_discard bytes=%u hr=0x%08lx\n", path, bytes,
        static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    memcpy(mapped.pData, data, bytes);
    context->Unmap(buffer, 0);
    return true;
}

void report_vb_item(const char* path, const char* item, const SmallRead& back,
                    bool want_left, bool want_right) {
    const PixelStats left = analyze(back.bytes.data(), back.pitch, kVbSize, kVbSize,
                                    RECT{4, 4, 124, 252});
    const PixelStats right = analyze(back.bytes.data(), back.pitch, kVbSize, kVbSize,
                                     RECT{132, 4, 252, 252});
    const bool left_ok = left.dark > kVbMinDark;
    const bool right_ok = right.dark > kVbMinDark;
    const bool ok = (!want_left || left_ok) && (!want_right || right_ok);
    const char* reason = ok ? "ok"
        : ((want_left && !left_ok) ? "left_ink_missing"
        : ((want_right && !right_ok) ? "right_ink_missing" : "unexpected_ink"));
    out("MODE=%s ITEM=%s RESULT=%s left_dark=%llu right_dark=%llu want_left=%d"
        " want_right=%d reason=%s\n", path, item, ok ? "PASS" : "FAIL", left.dark,
        right.dark, want_left ? 1 : 0, want_right ? 1 : 0, reason);
    if (!ok) ++g_failures;
}

int run_vb_paths(bool warp, bool debug_layer) {
    const char* path = "vb-paths";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob, vs_inst_blob, ps_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "vb_vs", kQuadVsSource, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "vb_vs_inst", kQuadVsInstSource, "vs_5_0", "vs_5_0",
                        vs_inst_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "vb_ps", kQuadPsSource, "ps_5_0", "ps_4_0",
                        ps_blob.GetAddressOf(), &used);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11VertexShader> vs, vs_inst;
    ComPtr<ID3D11PixelShader> ps;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, vs.GetAddressOf());
    out("MODE=%s stage=create_vs name=plain hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreateVertexShader(vs_inst_blob->GetBufferPointer(),
                                          vs_inst_blob->GetBufferSize(), nullptr,
                                          vs_inst.GetAddressOf());
        out("MODE=%s stage=create_vs name=instanced hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = g.device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                         nullptr, ps.GetAddressOf());
        out("MODE=%s stage=create_ps name=opaque_black hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed\n", path);
        return 2;
    }

    D3D11_INPUT_ELEMENT_DESC elem{};
    elem.SemanticName = "POSITION";
    elem.SemanticIndex = 0;
    elem.Format = DXGI_FORMAT_R32G32_FLOAT;
    elem.InputSlot = 0;
    elem.AlignedByteOffset = 0;
    elem.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elem.InstanceDataStepRate = 0;
    ComPtr<ID3D11InputLayout> layout, layout_inst;
    hr = g.device->CreateInputLayout(&elem, 1, vs_blob->GetBufferPointer(),
                                     vs_blob->GetBufferSize(), layout.GetAddressOf());
    out("MODE=%s stage=create_input_layout name=plain hr=0x%08lx\n", path,
        static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = g.device->CreateInputLayout(&elem, 1, vs_inst_blob->GetBufferPointer(),
                                         vs_inst_blob->GetBufferSize(),
                                         layout_inst.GetAddressOf());
        out("MODE=%s stage=create_input_layout name=instanced hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=input_layout_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Texture2D> tex, staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kVbSize,
                       D3D11_BIND_RENDER_TARGET, tex, staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=target_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = g.device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=rtv_create_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    if (!small_pipeline(path, g, kVbSize, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=state_create_failed\n", path);
        return 2;
    }

    int items = 0;
    int failed = 0;
    auto read_item = [&](const char* item, bool want_left, bool want_right, auto&& draw) {
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        g.context->ClearRenderTargetView(rtv.Get(), white);
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        draw();
        g.context->Flush();
        const SmallRead back = small_read(path, g.context.Get(), tex.Get(), staging.Get(),
                                          kVbSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
            return;
        }
        const unsigned long long before = g_failures;
        report_vb_item(path, item, back, want_left, want_right);
        if (g_failures != before) ++failed;
    };

    // Case 1: static vertex buffer, Draw(6, 0), no index buffer.
    ComPtr<ID3D11Buffer> vb6;
    if (!create_buffer(path, g.device.Get(), kQuad6Left, sizeof(kQuad6Left),
                       D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb6)) {
        ++g_failures;
    } else {
        g.context->IASetInputLayout(layout.Get());
        g.context->VSSetShader(vs.Get(), nullptr, 0);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* vb = vb6.Get();
        UINT stride = 8, offset = 0;
        g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        read_item("static_vb_nonindexed", true, false,
                  [&]() { g.context->Draw(6, 0); });
    }

    // Case 2: static vertex buffer + R16 index buffer.
    ComPtr<ID3D11Buffer> vb4, ib16;
    if (!create_buffer(path, g.device.Get(), kQuad4Left, sizeof(kQuad4Left),
                       D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb4) ||
        !create_buffer(path, g.device.Get(), kIdx6, sizeof(kIdx6),
                       D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT, ib16)) {
        ++g_failures;
    } else {
        ID3D11Buffer* vb = vb4.Get();
        UINT stride = 8, offset = 0;
        g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g.context->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, 0);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        read_item("static_vb_indexed_r16", true, false,
                  [&]() { g.context->DrawIndexed(6, 0, 0); });
    }

    // Case 3: the same draw with R32 indices.
    ComPtr<ID3D11Buffer> ib32;
    if (!create_buffer(path, g.device.Get(), kIdx6_32, sizeof(kIdx6_32),
                       D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT, ib32)) {
        ++g_failures;
    } else {
        ID3D11Buffer* vb = vb4.Get();
        UINT stride = 8, offset = 0;
        g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g.context->IASetIndexBuffer(ib32.Get(), DXGI_FORMAT_R32_UINT, 0);
        read_item("static_vb_indexed_r32", true, false,
                  [&]() { g.context->DrawIndexed(6, 0, 0); });
    }

    // Case 4: dynamic vertex buffer filled through Map(WRITE_DISCARD).
    ComPtr<ID3D11Buffer> vb_dyn6;
    if (!create_buffer(path, g.device.Get(), nullptr, sizeof(kQuad6Left),
                       D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, vb_dyn6) ||
        !fill_dynamic(path, g.context.Get(), vb_dyn6.Get(), kQuad6Left,
                      sizeof(kQuad6Left))) {
        ++g_failures;
    } else {
        ID3D11Buffer* vb = vb_dyn6.Get();
        UINT stride = 8, offset = 0;
        g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        read_item("dynamic_vb_nonindexed", true, false,
                  [&]() { g.context->Draw(6, 0); });
    }

    // Case 5: dynamic vertex and index buffers, indexed draw.
    ComPtr<ID3D11Buffer> vb_dyn4, ib_dyn16;
    if (!create_buffer(path, g.device.Get(), nullptr, sizeof(kQuad4Left),
                       D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, vb_dyn4) ||
        !create_buffer(path, g.device.Get(), nullptr, sizeof(kIdx6),
                       D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DYNAMIC, ib_dyn16) ||
        !fill_dynamic(path, g.context.Get(), vb_dyn4.Get(), kQuad4Left,
                      sizeof(kQuad4Left)) ||
        !fill_dynamic(path, g.context.Get(), ib_dyn16.Get(), kIdx6, sizeof(kIdx6))) {
        ++g_failures;
    } else {
        ID3D11Buffer* vb = vb_dyn4.Get();
        UINT stride = 8, offset = 0;
        g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g.context->IASetIndexBuffer(ib_dyn16.Get(), DXGI_FORMAT_R16_UINT, 0);
        read_item("dynamic_vb_indexed_r16", true, false,
                  [&]() { g.context->DrawIndexed(6, 0, 0); });
    }

    // Case 6: one buffer pair, two indexed ranges in one frame.
    ComPtr<ID3D11Buffer> vb_dyn8, ib_dyn12;
    if (!create_buffer(path, g.device.Get(), nullptr, sizeof(kQuad4TwoRanges),
                       D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, vb_dyn8) ||
        !create_buffer(path, g.device.Get(), nullptr, sizeof(kIdx12),
                       D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DYNAMIC, ib_dyn12) ||
        !fill_dynamic(path, g.context.Get(), vb_dyn8.Get(), kQuad4TwoRanges,
                      sizeof(kQuad4TwoRanges)) ||
        !fill_dynamic(path, g.context.Get(), ib_dyn12.Get(), kIdx12, sizeof(kIdx12))) {
        ++g_failures;
    } else {
        ID3D11Buffer* vb = vb_dyn8.Get();
        UINT stride = 8, offset = 0;
        g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g.context->IASetIndexBuffer(ib_dyn12.Get(), DXGI_FORMAT_R16_UINT, 0);
        read_item("two_ranges_dynamic", true, true, [&]() {
            g.context->DrawIndexed(6, 0, 0);
            g.context->DrawIndexed(6, 6, 0);
        });
    }

    // Case 7: instanced indexed draw, one instance per half.
    if (vb4 && ib16) {
        g.context->IASetInputLayout(layout_inst.Get());
        g.context->VSSetShader(vs_inst.Get(), nullptr, 0);
        ID3D11Buffer* vb = vb4.Get();
        UINT stride = 8, offset = 0;
        g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g.context->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, 0);
        read_item("instanced_indexed", true, true,
                  [&]() { g.context->DrawIndexedInstanced(6, 2, 0, 0, 0); });
    } else {
        ++failed;
        ++g_failures;
        out("MODE=%s ITEM=instanced_indexed RESULT=FAIL reason=buffer_create_failed\n",
            path);
    }

    out("MODE=%s SUMMARY items=%d failed=%d size=%dx%d\n", path, items, failed, kVbSize,
        kVbSize);
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// v6: R32_UINT mask write vs readback
// ---------------------------------------------------------------------------
// mask-uint showed a PS write of 0x00010001 to an R32_UINT render target
// reading back as zero, while a UAV store of the same value passed. That
// texture carried RENDER_TARGET|SHADER_RESOURCE|UNORDERED_ACCESS at once, so
// three explanations remain: the PS write is lost, the direct map of an
// integer target is lost, or an image bound for storage cannot be a color
// attachment on this driver. This mode separates them: a PS write read back
// through an SRV shader, a ClearRenderTargetView read back both ways, and the
// same pair again on a texture that also carries the UAV bind.

int run_mask_write_srv(bool warp, bool debug_layer) {
    const char* path = "mask-write-srv";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob, ps_write_blob, ps_sample_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "mws_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "mws_ps_write", kMaskPsWrite, "ps_5_0", "ps_4_0",
                        ps_write_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "mws_ps_sample", kMaskPsSample, "ps_5_0", "ps_4_0",
                        ps_sample_blob.GetAddressOf(), &used);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_write, ps_sample;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, vs.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = g.device->CreatePixelShader(ps_write_blob->GetBufferPointer(),
                                         ps_write_blob->GetBufferSize(), nullptr,
                                         ps_write.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = g.device->CreatePixelShader(ps_sample_blob->GetBufferPointer(),
                                         ps_sample_blob->GetBufferSize(), nullptr,
                                         ps_sample.GetAddressOf());
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_object_create_failed\n", path);
        return 2;
    }
    if (!small_pipeline(path, g, kMaskSize, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=state_create_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Texture2D> out_tex, out_staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kMaskSize,
                       D3D11_BIND_RENDER_TARGET, out_tex, out_staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=bgra_target_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> out_rtv;
    hr = g.device->CreateRenderTargetView(out_tex.Get(), nullptr, out_rtv.GetAddressOf());
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=bgra_rtv_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    ComPtr<ID3D11Texture2D> mask_a, stage_a, mask_b, stage_b;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_R32_UINT, kMaskSize,
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, mask_a,
                       stage_a) ||
        !small_texture(path, g.device.Get(), DXGI_FORMAT_R32_UINT, kMaskSize,
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                           D3D11_BIND_UNORDERED_ACCESS,
                       mask_b, stage_b)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=mask_texture_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> rtv_a, rtv_b;
    ComPtr<ID3D11ShaderResourceView> srv_a, srv_b;
    ComPtr<ID3D11UnorderedAccessView> uav_b;
    const HRESULT hr_ra = g.device->CreateRenderTargetView(mask_a.Get(), nullptr,
                                                           rtv_a.GetAddressOf());
    const HRESULT hr_sa = g.device->CreateShaderResourceView(mask_a.Get(), nullptr,
                                                             srv_a.GetAddressOf());
    const HRESULT hr_rb = g.device->CreateRenderTargetView(mask_b.Get(), nullptr,
                                                           rtv_b.GetAddressOf());
    const HRESULT hr_sb = g.device->CreateShaderResourceView(mask_b.Get(), nullptr,
                                                             srv_b.GetAddressOf());
    const HRESULT hr_ub = g.device->CreateUnorderedAccessView(mask_b.Get(), nullptr,
                                                              uav_b.GetAddressOf());
    out("MODE=%s stage=create_views no_uav rtv=0x%08lx srv=0x%08lx"
        " with_uav rtv=0x%08lx srv=0x%08lx uav=0x%08lx\n", path,
        static_cast<unsigned long>(hr_ra), static_cast<unsigned long>(hr_sa),
        static_cast<unsigned long>(hr_rb), static_cast<unsigned long>(hr_sb),
        static_cast<unsigned long>(hr_ub));
    if (FAILED(hr_ra) || FAILED(hr_sa) || FAILED(hr_rb) || FAILED(hr_sb)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=mask_view_create_failed\n", path);
        return 2;
    }

    int items = 0;
    int failed = 0;

    auto clear_bits = [&](ID3D11RenderTargetView* rtv, unsigned bits) {
        float color[4];
        memcpy(&color[0], &bits, sizeof(bits));
        color[1] = 0.0f;
        color[2] = 0.0f;
        color[3] = 0.0f;
        g.context->ClearRenderTargetView(rtv, color);
    };

    auto srv_readback = [&](ID3D11ShaderResourceView* srv, const char* item) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        g.context->ClearRenderTargetView(out_rtv.Get(), black);
        g.context->OMSetRenderTargets(1, out_rtv.GetAddressOf(), nullptr);
        g.context->PSSetShader(ps_sample.Get(), nullptr, 0);
        g.context->PSSetShaderResources(0, 1, &srv);
        g.context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        g.context->PSSetShaderResources(0, 1, &none);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        g.context->Flush();
        const SmallRead back = small_read(path, g.context.Get(), out_tex.Get(),
                                          out_staging.Get(), kMaskSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
            return;
        }
        unsigned lo = 255, hi = 0;
        int bad = 0;
        for (int y = 0; y < kMaskSize; ++y) {
            const uint8_t* row = back.bytes.data() + static_cast<size_t>(y) * back.pitch;
            for (int x = 0; x < kMaskSize; ++x) {
                const unsigned v = row[x * 4];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                if (v < 30 || v > 34) ++bad;
            }
        }
        const bool ok = (bad == 0);
        out("MODE=%s ITEM=%s RESULT=%s expected_gray=32 tolerance=2 mismatches=%d"
            " actual_min=%u actual_max=%u reason=%s\n", path, item, ok ? "PASS" : "FAIL",
            bad, lo, hi, ok ? "ok" : "value_mismatch");
        if (!ok) {
            ++failed;
            ++g_failures;
        }
    };

    auto direct_readback = [&](ID3D11Texture2D* tex, ID3D11Texture2D* staging,
                               const char* item) {
        const SmallRead back = small_read(path, g.context.Get(), tex, staging, kMaskSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
            return;
        }
        const unsigned want = 65537u;
        const unsigned long long before = g_failures;
        print_uint_item(path, item, " path=direct_map_after_clear", back, kMaskSize, want);
        if (g_failures != before) ++failed;
    };

    auto write_mask = [&](ID3D11RenderTargetView* rtv) {
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv, zero);
        g.context->OMSetRenderTargets(1, &rtv, nullptr);
        g.context->PSSetShader(ps_write.Get(), nullptr, 0);
        g.context->Draw(3, 0);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        g.context->Flush();
    };

    write_mask(rtv_a.Get());
    srv_readback(srv_a.Get(), "no_uav_ps_write_srv");

    clear_bits(rtv_a.Get(), 65537u);
    srv_readback(srv_a.Get(), "no_uav_clear_srv");
    direct_readback(mask_a.Get(), stage_a.Get(), "no_uav_clear_direct");

    write_mask(rtv_b.Get());
    srv_readback(srv_b.Get(), "with_uav_ps_write_srv");

    // A fresh texture that is never cleared: if the PS write survives here but
    // not after a ClearRenderTargetView, the clear is being submitted after the
    // draw (deferred-clear ordering) rather than the integer write being lost.
    ComPtr<ID3D11Texture2D> mask_c, stage_c;
    if (small_texture(path, g.device.Get(), DXGI_FORMAT_R32_UINT, kMaskSize,
                      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, mask_c,
                      stage_c)) {
        ComPtr<ID3D11RenderTargetView> rtv_c;
        ComPtr<ID3D11ShaderResourceView> srv_c;
        const HRESULT hr_rc = g.device->CreateRenderTargetView(mask_c.Get(), nullptr,
                                                               rtv_c.GetAddressOf());
        const HRESULT hr_sc = g.device->CreateShaderResourceView(mask_c.Get(), nullptr,
                                                                 srv_c.GetAddressOf());
        out("MODE=%s stage=create_views no_clear rtv=0x%08lx srv=0x%08lx\n", path,
            static_cast<unsigned long>(hr_rc), static_cast<unsigned long>(hr_sc));
        if (SUCCEEDED(hr_rc) && SUCCEEDED(hr_sc)) {
            g.context->OMSetRenderTargets(1, rtv_c.GetAddressOf(), nullptr);
            g.context->PSSetShader(ps_write.Get(), nullptr, 0);
            g.context->Draw(3, 0);
            ID3D11RenderTargetView* no_rtv = nullptr;
            g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
            g.context->Flush();
            srv_readback(srv_c.Get(), "no_clear_ps_write_srv");
        } else {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=no_clear_ps_write_srv RESULT=FAIL"
                " reason=view_create_failed\n", path);
        }
    } else {
        ++failed;
        ++g_failures;
        out("MODE=%s ITEM=no_clear_ps_write_srv RESULT=FAIL"
            " reason=texture_create_failed\n", path);
    }

    clear_bits(rtv_b.Get(), 65537u);
    srv_readback(srv_b.Get(), "with_uav_clear_srv");
    direct_readback(mask_b.Get(), stage_b.Get(), "with_uav_clear_direct");

    out("MODE=%s SUMMARY items=%d failed=%d size=%dx%d expected_uint=0x00010001"
        " expected_gray=32\n", path, items, failed, kMaskSize, kMaskSize);
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// v6: integer render-target matrix
// ---------------------------------------------------------------------------
// The R32_UINT mask write reads back as zero on the device while the same
// shader passes on WARP. This mode asks whether that is specific to R32_UINT,
// to integer targets in general, or to rasterisation into integer formats,
// with float targets as the control. Every case writes one known value with a
// pixel shader and compares the raw map bytes, so a conversion cannot hide a
// missing write.

const char* kUintPsR32 = "uint4 main() : SV_Target { return uint4(65537u, 0u, 0u, 0u); }\n";
const char* kUintPsR16 = "uint4 main() : SV_Target { return uint4(4660u, 0u, 0u, 0u); }\n";
const char* kUintPsR8 = "uint4 main() : SV_Target { return uint4(7u, 0u, 0u, 0u); }\n";
const char* kUintPsRGBA =
    "uint4 main() : SV_Target { return uint4(1u, 2u, 3u, 4u); }\n";
const char* kFloatPsHalf =
    "float4 main() : SV_Target { return float4(0.5f, 0.5f, 0.5f, 1.0f); }\n";
const char* kFloatPsQuarter =
    "float4 main() : SV_Target { return float4(0.25f, 0.25f, 0.25f, 1.0f); }\n";

int run_uint_rt_matrix(bool warp, bool debug_layer) {
    const char* path = "uint-rt-matrix";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "matrix_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    ComPtr<ID3D11VertexShader> vs;
    if (SUCCEEDED(hr))
        hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs.GetAddressOf());
    if (FAILED(hr) || !small_pipeline(path, g, 64, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=setup_failed\n", path);
        return 2;
    }

    struct Case {
        const char* item;
        DXGI_FORMAT fmt;
        int bpp;
        const char* ps;
        const uint8_t* want;
        int want_len;
    };
    static const uint8_t want_r32[] = {0x01, 0x00, 0x01, 0x00};
    static const uint8_t want_r16[] = {0x34, 0x12};
    static const uint8_t want_r8[] = {0x07};
    static const uint8_t want_rgba[] = {0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                        0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
    static const uint8_t want_f32[] = {0x00, 0x00, 0x00, 0x3F};
    static const uint8_t want_bgra[] = {0x40, 0x40, 0x40, 0xFF};
    const Case cases[] = {
        {"r32_uint_65537", DXGI_FORMAT_R32_UINT, 4, kUintPsR32, want_r32, 4},
        {"r16_uint_4660", DXGI_FORMAT_R16_UINT, 2, kUintPsR16, want_r16, 2},
        {"r8_uint_7", DXGI_FORMAT_R8_UINT, 1, kUintPsR8, want_r8, 1},
        {"rgba32_uint_1_2_3_4", DXGI_FORMAT_R32G32B32A32_UINT, 16, kUintPsRGBA, want_rgba,
         16},
        {"r32_float_0p5", DXGI_FORMAT_R32_FLOAT, 4, kFloatPsHalf, want_f32, 4},
        {"bgra8_unorm_0p25", DXGI_FORMAT_B8G8R8A8_UNORM, 4, kFloatPsQuarter, want_bgra, 4},
    };

    int items = 0;
    int failed = 0;
    for (const Case& c : cases) {
        ComPtr<ID3DBlob> ps_blob;
        hr = a8_compile(path, c.item, c.ps, "ps_5_0", "ps_4_0", ps_blob.GetAddressOf(), &used);
        ComPtr<ID3D11PixelShader> ps;
        if (SUCCEEDED(hr))
            hr = g.device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                             ps_blob->GetBufferSize(), nullptr,
                                             ps.GetAddressOf());
        ++items;
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=shader_create_failed\n", path, c.item);
            continue;
        }
        ComPtr<ID3D11Texture2D> tex, staging;
        if (!small_texture(path, g.device.Get(), c.fmt, 64,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, tex,
                           staging)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=texture_create_failed\n", path,
                c.item);
            continue;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = g.device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=rtv_create_failed hr=0x%08lx\n", path,
                c.item, static_cast<unsigned long>(hr));
            continue;
        }
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), zero);
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        g.context->Draw(3, 0);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        g.context->Flush();
        const SmallRead back = small_read(path, g.context.Get(), tex.Get(), staging.Get(),
                                          64, c.bpp);
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, c.item);
            continue;
        }
        int mismatches = 0;
        unsigned first = 0;
        for (int y = 0; y < 64; ++y) {
            const uint8_t* row = back.bytes.data() + static_cast<size_t>(y) * back.pitch;
            for (int x = 0; x < 64; ++x) {
                const uint8_t* p = row + x * c.bpp;
                if (memcmp(p, c.want, static_cast<size_t>(c.want_len)) != 0) {
                    if (!mismatches) {
                        first = 0;
                        for (int i = 0; i < c.want_len && i < 4; ++i)
                            first |= static_cast<unsigned>(p[i]) << (8 * i);
                    }
                    ++mismatches;
                }
            }
        }
        const bool ok = (mismatches == 0);
        out("MODE=%s ITEM=%s RESULT=%s mismatches=%d first_actual_hex=0x%08x"
            " want_len=%d reason=%s\n", path, c.item, ok ? "PASS" : "FAIL", mismatches,
            first, c.want_len, ok ? "ok" : "value_mismatch");
        if (!ok) {
            ++failed;
            ++g_failures;
        }
    }

    out("MODE=%s SUMMARY items=%d failed=%d size=64x64 cases=r32/r16/r8/rgba32/float/bgra\n",
        path, items, failed);
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// v6: integer target ordering
// ---------------------------------------------------------------------------
// If a UAV store survives but a pixel-shader write does not, the write can be
// lost in two very different places: the graphics pipeline may be rejected on
// this integer attachment, or a pending clear may run after the draw. Storing
// with a compute shader first puts the image in a defined state; a PS draw
// after that has no pending clear left to lose against. The value that comes
// back then separates "draw dropped" from "clear ordering".

const char* kOrderCsStore =
    "RWTexture2D<uint> dst : register(u0);\n"
    "[numthreads(16, 16, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) { dst[id.xy] = 0x11111111u; }\n";

int run_uint_order(bool warp, bool debug_layer) {
    const char* path = "uint-order";
    const int size = 64;
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob, ps_blob, cs_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "order_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "order_ps", kMaskPsWrite, "ps_5_0", "ps_4_0",
                        ps_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "order_cs", kOrderCsStore, "cs_5_0", "cs_4_0",
                        cs_blob.GetAddressOf(), &used);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    if (SUCCEEDED(hr))
        hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = g.device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(), nullptr,
                                         ps.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = g.device->CreateComputeShader(cs_blob->GetBufferPointer(),
                                           cs_blob->GetBufferSize(), nullptr,
                                           cs.GetAddressOf());
    if (FAILED(hr) || !small_pipeline(path, g, size, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=setup_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Texture2D> tex, staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_R32_UINT, size,
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                           D3D11_BIND_UNORDERED_ACCESS,
                       tex, staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=texture_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    const HRESULT hr_rtv = g.device->CreateRenderTargetView(tex.Get(), nullptr,
                                                            rtv.GetAddressOf());
    const HRESULT hr_uav = g.device->CreateUnorderedAccessView(tex.Get(), nullptr,
                                                               uav.GetAddressOf());
    out("MODE=%s stage=create_views rtv=0x%08lx uav=0x%08lx\n", path,
        static_cast<unsigned long>(hr_rtv), static_cast<unsigned long>(hr_uav));
    if (FAILED(hr_rtv) || FAILED(hr_uav)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=view_create_failed\n", path);
        return 2;
    }

    auto uav_store = [&]() {
        g.context->CSSetShader(cs.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* u = uav.Get();
        g.context->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        const UINT groups = static_cast<UINT>(size / 16);
        g.context->Dispatch(groups, groups, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        g.context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        g.context->CSSetShader(nullptr, nullptr, 0);
        g.context->Flush();
    };
    auto ps_draw = [&]() {
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        g.context->Draw(3, 0);
        ID3D11RenderTargetView* none = nullptr;
        g.context->OMSetRenderTargets(1, &none, nullptr);
        g.context->Flush();
    };

    int items = 0;
    int failed = 0;
    auto report = [&](const char* item, const SmallRead& back) {
        unsigned lo = 0xFFFFFFFFu, hi = 0;
        for (int y = 0; y < size; ++y) {
            const uint8_t* row = back.bytes.data() + static_cast<size_t>(y) * back.pitch;
            for (int x = 0; x < size; ++x) {
                const uint8_t* p = row + x * 4;
                const unsigned v = static_cast<unsigned>(p[0]) |
                                   (static_cast<unsigned>(p[1]) << 8) |
                                   (static_cast<unsigned>(p[2]) << 16) |
                                   (static_cast<unsigned>(p[3]) << 24);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        out("MODE=%s ITEM=%s actual_min=0x%08x actual_max=0x%08x\n", path, item, lo, hi);
    };

    // A: compute store first, then a pixel-shader draw into the same target.
    uav_store();
    ps_draw();
    ++items;
    {
        const SmallRead back = small_read(path, g.context.Get(), tex.Get(),
                                          staging.Get(), size, 4);
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=uav_then_ps RESULT=FAIL reason=map_read_failed\n", path);
        } else {
            report("uav_then_ps", back);
            out("MODE=%s ITEM=uav_then_ps RESULT=INFO expected_ps=0x00010001"
                " expected_if_dropped=0x11111111\n", path);
        }
    }

    // B: pixel-shader draw first, then a compute store (control: the store must
    // win, proving both paths reach the same image).
    ps_draw();
    uav_store();
    ++items;
    {
        const SmallRead back = small_read(path, g.context.Get(), tex.Get(),
                                          staging.Get(), size, 4);
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=ps_then_uav RESULT=FAIL reason=map_read_failed\n", path);
        } else {
            report("ps_then_uav", back);
            out("MODE=%s ITEM=ps_then_uav RESULT=INFO expected=0x11111111\n", path);
        }
    }

    out("MODE=%s SUMMARY items=%d failed=%d\n", path, items, failed);
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// v7: extreme integer render-target values
// ---------------------------------------------------------------------------
// The matrix mode writes values small enough to survive a float round trip.
// These cases use bit patterns that cannot: 0x7FC00001 is a NaN bit pattern as
// float, 0x80000001 and 0xFFFFFFFF are not representable in a 32-bit float,
// and 0xDEADBEEF is outside the exact integer range. A bit-exact readback
// therefore proves the pixel-shader integer result reached the target without
// a float->uint conversion; an all-zero readback proves the integer write was
// dropped instead. The R32_FLOAT control writes the same 0x7FC00001 bit
// pattern through asfloat from a constant buffer and compares the raw map
// bytes, so the readback path itself is shown not to convert.

const char* kExtremePsR32Nan =
    "uint4 main() : SV_Target { return uint4(0x7FC00001u, 0u, 0u, 0u); }\n";
const char* kExtremePsR32Dead =
    "uint4 main() : SV_Target { return uint4(0xDEADBEEFu, 0u, 0u, 0u); }\n";
const char* kExtremePsRgba =
    "uint4 main() : SV_Target {\n"
    "    return uint4(0x80000001u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0xDEADBEEFu);\n"
    "}\n";
const char* kExtremePsFloatNan =
    "cbuffer C : register(b0) { uint g_bits; };\n"
    "float4 main() : SV_Target { return float4(asfloat(g_bits), 0.0f, 0.0f, 0.0f); }\n";

void format_uints(char* buf, size_t cap, const unsigned* v, int count) {
    size_t used = 0;
    buf[0] = 0;
    for (int i = 0; i < count; ++i) {
        if (used + 16 >= cap) break;
        const int n = _snprintf_s(buf + used, cap - used, _TRUNCATE,
                                  i ? ",0x%08x" : "0x%08x", v[i]);
        if (n <= 0) break;
        used += static_cast<size_t>(n);
    }
}

struct ExtremeScan {
    int mismatches = 0;
    bool all_zero = true;
    bool ok = false;
    unsigned lo[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    unsigned hi[4] = {0, 0, 0, 0};
};

ExtremeScan extreme_scan(const SmallRead& back, int size, int channels,
                         const unsigned* want) {
    ExtremeScan s;
    for (int y = 0; y < size; ++y) {
        const uint8_t* row = back.bytes.data() + static_cast<size_t>(y) * back.pitch;
        for (int x = 0; x < size; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * channels * 4;
            bool zero = true;
            bool match = true;
            for (int c = 0; c < channels; ++c) {
                const unsigned v = static_cast<unsigned>(p[c * 4 + 0]) |
                                   (static_cast<unsigned>(p[c * 4 + 1]) << 8) |
                                   (static_cast<unsigned>(p[c * 4 + 2]) << 16) |
                                   (static_cast<unsigned>(p[c * 4 + 3]) << 24);
                if (v != 0) zero = false;
                if (v < s.lo[c]) s.lo[c] = v;
                if (v > s.hi[c]) s.hi[c] = v;
                if (v != want[c]) match = false;
            }
            if (!zero) s.all_zero = false;
            if (!match) ++s.mismatches;
        }
    }
    s.ok = (s.mismatches == 0);
    return s;
}

int run_uint_rt_extreme(bool warp, bool debug_layer) {
    const char* path = "uint-rt-extreme";
    const int size = 64;
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "extreme_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    ComPtr<ID3D11VertexShader> vs;
    if (SUCCEEDED(hr))
        hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs.GetAddressOf());
    if (FAILED(hr) || !small_pipeline(path, g, size, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=setup_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Buffer> bits_cb;
    const unsigned bits[4] = {0x7FC00001u, 0u, 0u, 0u};
    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = static_cast<UINT>(sizeof(bits));
    cb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cb_init{};
    cb_init.pSysMem = bits;
    const HRESULT hr_cb = g.device->CreateBuffer(&cb_desc, &cb_init,
                                                 bits_cb.GetAddressOf());
    out("MODE=%s stage=create_constant_buffer bytes=%zu hr=0x%08lx\n", path,
        sizeof(bits), static_cast<unsigned long>(hr_cb));
    if (FAILED(hr_cb)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=constant_buffer_failed\n", path);
        return 2;
    }

    struct ExtremeCase {
        const char* item;
        DXGI_FORMAT fmt;
        const char* fmt_text;
        int channels;
        const char* ps;
        bool bind_bits;
        unsigned want[4];
    };
    const ExtremeCase cases[] = {
        {"r32_uint_nan_bits", DXGI_FORMAT_R32_UINT, "R32_UINT", 1, kExtremePsR32Nan,
         false, {0x7FC00001u, 0u, 0u, 0u}},
        {"r32_uint_deadbeef", DXGI_FORMAT_R32_UINT, "R32_UINT", 1, kExtremePsR32Dead,
         false, {0xDEADBEEFu, 0u, 0u, 0u}},
        {"rgba32_uint_extremes", DXGI_FORMAT_R32G32B32A32_UINT, "RGBA32_UINT", 4,
         kExtremePsRgba, false,
         {0x80000001u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0xDEADBEEFu}},
        {"r32_float_nan_bits", DXGI_FORMAT_R32_FLOAT, "R32_FLOAT", 1,
         kExtremePsFloatNan, true, {0x7FC00001u, 0u, 0u, 0u}},
    };

    int items = 0;
    int failed = 0;
    int uint_cases = 0;
    int uint_failed = 0;
    int uint_zero = 0;
    bool control_ok = false;
    for (const ExtremeCase& c : cases) {
        const bool control = (c.fmt == DXGI_FORMAT_R32_FLOAT);
        ComPtr<ID3DBlob> ps_blob;
        hr = a8_compile(path, c.item, c.ps, "ps_5_0", "ps_4_0",
                        ps_blob.GetAddressOf(), &used);
        ComPtr<ID3D11PixelShader> ps;
        if (SUCCEEDED(hr))
            hr = g.device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                             ps_blob->GetBufferSize(), nullptr,
                                             ps.GetAddressOf());
        ++items;
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            if (!control) {
                ++uint_cases;
                ++uint_failed;
            }
            out("MODE=%s ITEM=%s RESULT=FAIL reason=shader_create_failed\n", path,
                c.item);
            continue;
        }
        if (!control) ++uint_cases;

        ComPtr<ID3D11Texture2D> tex, staging;
        if (!small_texture(path, g.device.Get(), c.fmt, size,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, tex,
                           staging)) {
            ++failed;
            ++g_failures;
            if (!control) ++uint_failed;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=texture_create_failed\n", path,
                c.item);
            continue;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = g.device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
        if (FAILED(hr)) {
            ++failed;
            ++g_failures;
            if (!control) ++uint_failed;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=rtv_create_failed hr=0x%08lx\n", path,
                c.item, static_cast<unsigned long>(hr));
            continue;
        }
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), zero);
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        if (c.bind_bits) {
            ID3D11Buffer* cb = bits_cb.Get();
            g.context->PSSetConstantBuffers(0, 1, &cb);
        }
        g.context->Draw(3, 0);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        if (c.bind_bits) {
            ID3D11Buffer* no_cb = nullptr;
            g.context->PSSetConstantBuffers(0, 1, &no_cb);
        }
        g.context->Flush();
        const SmallRead back = small_read(path, g.context.Get(), tex.Get(), staging.Get(),
                                          size, c.channels * 4);
        if (!back.ok) {
            ++failed;
            ++g_failures;
            if (!control) ++uint_failed;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, c.item);
            continue;
        }
        const ExtremeScan scan = extreme_scan(back, size, c.channels, c.want);
        char want_text[160], lo_text[160], hi_text[160];
        format_uints(want_text, sizeof(want_text), c.want, c.channels);
        format_uints(lo_text, sizeof(lo_text), scan.lo, c.channels);
        format_uints(hi_text, sizeof(hi_text), scan.hi, c.channels);
        const char* reason = scan.ok ? "ok" : (scan.all_zero ? "all_zero" : "value_mismatch");
        out("MODE=%s ITEM=%s RESULT=%s fmt=%s texels=%d mismatches=%d expected=%s"
            " actual_min=%s actual_max=%s all_zero=%s reason=%s\n", path, c.item,
            scan.ok ? "PASS" : "FAIL", c.fmt_text, size * size, scan.mismatches,
            want_text, lo_text, hi_text, scan.all_zero ? "yes" : "no", reason);
        if (!scan.ok) {
            ++failed;
            ++g_failures;
            if (!control) {
                ++uint_failed;
                if (scan.all_zero) ++uint_zero;
            }
        } else if (control) {
            control_ok = true;
        }
    }

    const int uint_exact = uint_cases - uint_failed;
    out("MODE=%s SUMMARY items=%d failed=%d driver=%s uint_cases=%d uint_exact=%d"
        " uint_all_zero=%d control_float_nan_exact=%s\n", path, items, failed,
        warp ? "warp" : "hardware", uint_cases, uint_exact, uint_zero,
        control_ok ? "yes" : "no");
    if (uint_cases > 0 && uint_failed == 0) {
        out("MODE=%s VERDICT=integer_output_bit_exact driver=%s items=%d"
            " float_to_uint_conversion=excluded\n", path, warp ? "warp" : "hardware",
            uint_cases);
    } else if (uint_cases > 0 && uint_zero == uint_cases) {
        out("MODE=%s VERDICT=integer_write_dropped driver=%s all_zero_items=%d"
            " confirmation=%s\n", path, warp ? "warp" : "hardware", uint_zero,
            warp ? "warp_should_not_drop_so_mode_or_driver_is_broken"
                 : "umd_drops_ps_integer_write");
    } else {
        out("MODE=%s VERDICT=integer_value_mismatch driver=%s exact=%d/%d all_zero=%d\n",
            path, warp ? "warp" : "hardware", uint_exact, uint_cases, uint_zero);
    }
    return failed ? 2 : 0;
}

// ---- typed-buffer-srv -------------------------------------------------------
//
// The failing D2D shader fetches uvec4 from image<u32> dim=Buffer, i.e. a typed
// texel buffer SRV, while text-paths/d2d-primitives use no integer render
// target at all. This mode isolates that path: one buffer, one SRV, one draw
// and one readback per case, every case storing to R32_FLOAT so the readback is
// a raw byte compare. The 64 words are 0x00010001, 0xDEADBEEF, 0x7FC00001,
// 0x12345678 and 1..64 (the four specials replace 1..4); pixel column x reads
// element x through SV_Position.x. The uint cases bitcast with asfloat, the
// float case moves Buffer<float> unchanged. 0x00010001 and 1..64 become
// denormals through asfloat and 0xDEADBEEF/0x7FC00001 are NaNs, so the item
// lines count denormal/nan columns separately; a denormal flush must not be
// misread as a typed-buffer failure. The Texture2D<uint> case is the
// known-good control and the Buffer<float> case separates a uint path from a
// buffer-view path. WARP must be bit exact.

const char* kTypedPsBufferUint =
    "Buffer<uint> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    uint v = src.Load((uint)pos.x);\n"
    "    return float4(asfloat(v), 0.0f, 0.0f, 0.0f);\n"
    "}\n";
const char* kTypedPsStructuredUint =
    "StructuredBuffer<uint> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    uint v = src.Load((uint)pos.x);\n"
    "    return float4(asfloat(v), 0.0f, 0.0f, 0.0f);\n"
    "}\n";
const char* kTypedPsRawUint =
    "ByteAddressBuffer src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    uint v = src.Load((uint)pos.x * 4u);\n"
    "    return float4(asfloat(v), 0.0f, 0.0f, 0.0f);\n"
    "}\n";
const char* kTypedPsTexture2dUint =
    "Texture2D<uint> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    uint v = src.Load(int3((int)pos.x, 0, 0));\n"
    "    return float4(asfloat(v), 0.0f, 0.0f, 0.0f);\n"
    "}\n";
const char* kTypedPsBufferFloat =
    "Buffer<float> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    return float4(src.Load((uint)pos.x), 0.0f, 0.0f, 0.0f);\n"
    "}\n";
const char* kTypedPsTexture2dFloatLoad =
    "Texture2D<float> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    float v = src.Load(int3((int)pos.x, (int)pos.y, 0));\n"
    "    return float4(v, 0.0f, 0.0f, 0.0f);\n"
    "}\n";
const char* kTypedPsTexture2dBgra8Load =
    "Texture2D<float4> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    return src.Load(int3((int)pos.x, (int)pos.y, 0));\n"
    "}\n";

const char* srv_dim_text(D3D11_SRV_DIMENSION dim) {
    switch (dim) {
        case D3D11_SRV_DIMENSION_BUFFER: return "BUFFER";
        case D3D11_SRV_DIMENSION_BUFFEREX: return "BUFFEREX";
        case D3D11_SRV_DIMENSION_TEXTURE2D: return "TEXTURE2D";
        default: return "other";
    }
}

const char* srv_format_text(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        default: return "other";
    }
}

struct InitDataCheck {
    bool checked = false;
    bool ok = false;
    int mismatches = 0;
    HRESULT hr_create = E_FAIL;
    HRESULT hr_map = E_FAIL;
    unsigned first[4] = {0, 0, 0, 0};
    unsigned last[4] = {0, 0, 0, 0};
};

void copy_first_last(unsigned* first, unsigned* last, const unsigned* v, int count) {
    const int tail = count > 4 ? count - 4 : 0;
    for (int i = 0; i < 4 && i < count; ++i) {
        first[i] = v[i];
        last[i] = v[tail + i];
    }
}

// Copy the resource back through a STAGING buffer and compare the words the
// probe uploaded. The staging buffer keeps the structured/raw flags first and
// falls back to a plain staging buffer if the runtime refuses them.
InitDataCheck check_buffer_initial(ID3D11Device* device, ID3D11DeviceContext* context,
                                   ID3D11Buffer* buffer, UINT misc, UINT stride,
                                   const unsigned* want, int count) {
    InitDataCheck c;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(count) * 4;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = misc;
    desc.StructureByteStride = stride;
    ComPtr<ID3D11Buffer> staging;
    c.hr_create = device->CreateBuffer(&desc, nullptr, staging.GetAddressOf());
    if (FAILED(c.hr_create) && (misc != 0 || stride != 0)) {
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        c.hr_create = device->CreateBuffer(&desc, nullptr, staging.GetAddressOf());
    }
    if (FAILED(c.hr_create)) return c;
    context->CopyResource(staging.Get(), buffer);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    c.hr_map = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(c.hr_map)) return c;
    const unsigned* got = static_cast<const unsigned*>(mapped.pData);
    for (int i = 0; i < count; ++i) {
        if (got[i] != want[i]) ++c.mismatches;
    }
    copy_first_last(c.first, c.last, got, count);
    context->Unmap(staging.Get(), 0);
    c.checked = true;
    c.ok = (c.mismatches == 0);
    return c;
}

InitDataCheck check_texture_initial(ID3D11Device* device, ID3D11DeviceContext* context,
                                    ID3D11Texture2D* tex,
                                    const D3D11_TEXTURE2D_DESC& tex_desc,
                                    const unsigned* want, int count) {
    InitDataCheck c;
    D3D11_TEXTURE2D_DESC desc = tex_desc;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    c.hr_create = device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
    if (FAILED(c.hr_create)) return c;
    context->CopyResource(staging.Get(), tex);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    c.hr_map = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(c.hr_map)) return c;
    const unsigned* got = static_cast<const unsigned*>(mapped.pData);
    for (int i = 0; i < count; ++i) {
        if (got[i] != want[i]) ++c.mismatches;
    }
    copy_first_last(c.first, c.last, got, count);
    context->Unmap(staging.Get(), 0);
    c.checked = true;
    c.ok = (c.mismatches == 0);
    return c;
}

bool typed_bits_denormal(unsigned v) {
    return (v & 0x7F800000u) == 0 && (v & 0x007FFFFFu) != 0;
}

bool typed_bits_nan(unsigned v) {
    return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu) != 0;
}

struct ColumnScan {
    int mismatches = 0;
    int bad_columns = 0;
    bool all_zero = true;
    bool ok = false;
    unsigned lo = 0xFFFFFFFFu;
    unsigned hi = 0;
    unsigned row0[64] = {0};
    bool bad[64] = {false};
    int denormal_expected = 0;
    int denormal_bad = 0;
    int nan_expected = 0;
    int nan_bad = 0;
};

ColumnScan column_scan(const SmallRead& back, int size, const unsigned* want) {
    ColumnScan s;
    for (int y = 0; y < size; ++y) {
        const uint8_t* row = back.bytes.data() + static_cast<size_t>(y) * back.pitch;
        for (int x = 0; x < size; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            const unsigned v = static_cast<unsigned>(p[0]) |
                               (static_cast<unsigned>(p[1]) << 8) |
                               (static_cast<unsigned>(p[2]) << 16) |
                               (static_cast<unsigned>(p[3]) << 24);
            if (y == 0) s.row0[x] = v;
            if (v != 0) s.all_zero = false;
            if (v < s.lo) s.lo = v;
            if (v > s.hi) s.hi = v;
            if (v != want[x]) {
                ++s.mismatches;
                if (!s.bad[x]) {
                    s.bad[x] = true;
                    ++s.bad_columns;
                }
            }
        }
    }
    for (int x = 0; x < size; ++x) {
        if (typed_bits_denormal(want[x])) {
            ++s.denormal_expected;
            if (s.bad[x]) ++s.denormal_bad;
        }
        if (typed_bits_nan(want[x])) {
            ++s.nan_expected;
            if (s.bad[x]) ++s.nan_bad;
        }
    }
    s.ok = (s.mismatches == 0);
    return s;
}

// Per-texel comparison for the 2D texture Load cases: every pixel is one
// 32-bit word (raw float bits, or packed BGRA8 bytes), so the 32x32 result can
// be checked without any float interpretation. The top-left 4x4 tile is kept
// for a compact expected/actual print.
struct TexelScan {
    int mismatches = 0;
    int first_bad = -1;
    unsigned want_bad = 0;
    unsigned got_bad = 0;
    bool all_zero = true;
    bool ok = false;
    unsigned lo = 0xFFFFFFFFu;
    unsigned hi = 0;
    unsigned tile[16] = {0};
};

TexelScan texel_scan(const SmallRead& back, int width, int height, int bpp,
                     const unsigned* want) {
    TexelScan s;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = back.bytes.data() + static_cast<size_t>(y) * back.pitch;
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * bpp;
            const unsigned v = static_cast<unsigned>(p[0]) |
                               (static_cast<unsigned>(p[1]) << 8) |
                               (static_cast<unsigned>(p[2]) << 16) |
                               (static_cast<unsigned>(p[3]) << 24);
            if (x < 4 && y < 4) s.tile[y * 4 + x] = v;
            if (v != 0) s.all_zero = false;
            if (v < s.lo) s.lo = v;
            if (v > s.hi) s.hi = v;
            const int index = y * width + x;
            if (v != want[index]) {
                ++s.mismatches;
                if (s.first_bad < 0) {
                    s.first_bad = index;
                    s.want_bad = want[index];
                    s.got_bad = v;
                }
            }
        }
    }
    s.ok = (s.mismatches == 0);
    return s;
}

int run_typed_buffer_srv(bool warp, bool debug_layer) {
    const char* path = "typed-buffer-srv";
    const int size = 64;
    const int count = 64;
    unsigned data[64];
    for (int i = 0; i < count; ++i) data[i] = static_cast<unsigned>(i + 1);
    data[0] = 0x00010001u;
    data[1] = 0xDEADBEEFu;
    data[2] = 0x7FC00001u;
    data[3] = 0x12345678u;

    out("MODE=%s stage=data words=%d bytes=%zu values=0x00010001,0xdeadbeef,"
        "0x7fc00001,0x12345678,1..64\n", path, count, sizeof(data));

    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "typed_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    ComPtr<ID3D11VertexShader> vs;
    if (SUCCEEDED(hr))
        hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs.GetAddressOf());
    if (FAILED(hr) || !small_pipeline(path, g, size, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=setup_failed\n", path);
        return 2;
    }

    enum SourceKind { kTypedUint, kStructuredUint, kRawUint, kTexture2dUint, kTypedFloat };
    struct TypedCase {
        const char* item;
        const char* ps;
        SourceKind kind;
        DXGI_FORMAT srv_format;
        D3D11_SRV_DIMENSION srv_dim;
        UINT misc;
        UINT stride;
    };
    const TypedCase cases[] = {
        {"typed_buffer_r32_uint", kTypedPsBufferUint, kTypedUint, DXGI_FORMAT_R32_UINT,
         D3D11_SRV_DIMENSION_BUFFER, 0, 0},
        {"structured_buffer_uint", kTypedPsStructuredUint, kStructuredUint,
         DXGI_FORMAT_UNKNOWN, D3D11_SRV_DIMENSION_BUFFER,
         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4},
        {"raw_byteaddress_uint", kTypedPsRawUint, kRawUint, DXGI_FORMAT_R32_TYPELESS,
         D3D11_SRV_DIMENSION_BUFFEREX, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0},
        {"texture2d_uint_control", kTypedPsTexture2dUint, kTexture2dUint,
         DXGI_FORMAT_R32_UINT, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 0},
        {"typed_buffer_r32_float", kTypedPsBufferFloat, kTypedFloat,
         DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_BUFFER, 0, 0},
    };

    int items = 0;
    int failed = 0;
    int uint_buffer_cases = 0;
    int uint_buffer_exact = 0;
    bool typed_uint_ok = false;
    bool structured_ok = false;
    bool raw_ok = false;
    bool texture_ok = false;
    bool float_ok = false;
    bool texture2d_float_ok = false;
    bool texture2d_bgra8_ok = false;
    int denormal_expected_total = 0;
    int denormal_bad_total = 0;
    int nan_expected_total = 0;
    int nan_bad_total = 0;

    auto fail_item = [&](const char* item, const char* reason) {
        ++failed;
        ++g_failures;
        out("MODE=%s ITEM=%s RESULT=FAIL reason=%s\n", path, item, reason);
    };

    for (const TypedCase& c : cases) {
        ++items;
        const bool uint_buffer = (c.kind == kTypedUint ||
                                  c.kind == kStructuredUint ||
                                  c.kind == kRawUint);
        if (uint_buffer) ++uint_buffer_cases;

        ComPtr<ID3DBlob> ps_blob;
        const char* ps_hi = "ps_5_0";
        const char* ps_lo = (c.kind == kStructuredUint || c.kind == kRawUint)
                                ? "ps_5_0"
                                : "ps_4_0";
        hr = a8_compile(path, c.item, c.ps, ps_hi, ps_lo,
                        ps_blob.GetAddressOf(), &used);
        ComPtr<ID3D11PixelShader> ps;
        if (SUCCEEDED(hr))
            hr = g.device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                             ps_blob->GetBufferSize(), nullptr,
                                             ps.GetAddressOf());
        if (FAILED(hr)) {
            fail_item(c.item, "shader_create_failed");
            continue;
        }

        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11Texture2D> source_tex;
        D3D11_TEXTURE2D_DESC source_desc{};
        if (c.kind == kTexture2dUint) {
            source_desc.Width = static_cast<UINT>(count);
            source_desc.Height = 1;
            source_desc.MipLevels = 1;
            source_desc.ArraySize = 1;
            source_desc.Format = DXGI_FORMAT_R32_UINT;
            source_desc.SampleDesc.Count = 1;
            source_desc.Usage = D3D11_USAGE_DEFAULT;
            source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = data;
            init.SysMemPitch = static_cast<UINT>(count) * 4;
            hr = g.device->CreateTexture2D(&source_desc, &init,
                                           source_tex.GetAddressOf());
            out("MODE=%s ITEM=%s stage=create_source_texture fmt=R32_UINT size=%dx1"
                " data=64_words hr=0x%08lx\n", path, c.item, count,
                static_cast<unsigned long>(hr));
            if (FAILED(hr)) {
                fail_item(c.item, "source_texture_create_failed");
                continue;
            }
        } else {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(count) * 4;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = c.misc;
            desc.StructureByteStride = c.stride;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = data;
            hr = g.device->CreateBuffer(&desc, &init, buffer.GetAddressOf());
            out("MODE=%s ITEM=%s stage=create_buffer bytes=%d usage=IMMUTABLE"
                " bind=SHADER_RESOURCE misc=0x%x stride=%u hr=0x%08lx\n", path,
                c.item, count * 4, c.misc, c.stride,
                static_cast<unsigned long>(hr));
            if (FAILED(hr)) {
                fail_item(c.item, "buffer_create_failed");
                continue;
            }
        }

        const InitDataCheck initial =
            (c.kind == kTexture2dUint)
                ? check_texture_initial(g.device.Get(), g.context.Get(),
                                        source_tex.Get(), source_desc, data, count)
                : check_buffer_initial(g.device.Get(), g.context.Get(),
                                       buffer.Get(), c.misc, c.stride, data, count);
        char first_text[160], last_text[160];
        format_uints(first_text, sizeof(first_text), initial.first, 4);
        format_uints(last_text, sizeof(last_text), initial.last, 4);
        out("MODE=%s ITEM=%s stage=%s_initial bytes=%zu mismatches=%d first=%s last=%s"
            " create_hr=0x%08lx map_hr=0x%08lx result=%s\n", path, c.item,
            c.kind == kTexture2dUint ? "texture" : "buffer", sizeof(data),
            initial.mismatches, first_text, last_text,
            static_cast<unsigned long>(initial.hr_create),
            static_cast<unsigned long>(initial.hr_map),
            !initial.checked ? "SKIP" : (initial.ok ? "PASS" : "FAIL"));
        if (initial.checked && !initial.ok) {
            fail_item(c.item, "initial_data_mismatch");
            continue;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = c.srv_format;
        srv_desc.ViewDimension = c.srv_dim;
        switch (c.srv_dim) {
            case D3D11_SRV_DIMENSION_BUFFER:
                srv_desc.Buffer.FirstElement = 0;
                srv_desc.Buffer.NumElements = static_cast<UINT>(count);
                break;
            case D3D11_SRV_DIMENSION_BUFFEREX:
                srv_desc.BufferEx.FirstElement = 0;
                srv_desc.BufferEx.NumElements = static_cast<UINT>(count);
                srv_desc.BufferEx.Flags =
                    (c.kind == kRawUint) ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2D:
                srv_desc.Texture2D.MostDetailedMip = 0;
                srv_desc.Texture2D.MipLevels = 1;
                break;
            default:
                break;
        }
        ID3D11Resource* source = nullptr;
        if (c.kind == kTexture2dUint) source = source_tex.Get();
        else source = buffer.Get();
        ComPtr<ID3D11ShaderResourceView> srv;
        hr = g.device->CreateShaderResourceView(source, &srv_desc, srv.GetAddressOf());
        if (c.srv_dim == D3D11_SRV_DIMENSION_TEXTURE2D) {
            out("MODE=%s ITEM=%s stage=srv_desc format=%s viewdim=%s mip=0 levels=1"
                " hr=0x%08lx\n", path, c.item, srv_format_text(c.srv_format),
                srv_dim_text(c.srv_dim), static_cast<unsigned long>(hr));
        } else if (c.srv_dim == D3D11_SRV_DIMENSION_BUFFEREX) {
            out("MODE=%s ITEM=%s stage=srv_desc format=%s viewdim=%s first=0 last=%d"
                " elements=%d flags=0x%x hr=0x%08lx\n", path, c.item,
                srv_format_text(c.srv_format), srv_dim_text(c.srv_dim), count - 1,
                count, srv_desc.BufferEx.Flags, static_cast<unsigned long>(hr));
        } else {
            out("MODE=%s ITEM=%s stage=srv_desc format=%s viewdim=%s first=0 last=%d"
                " elements=%d hr=0x%08lx\n", path, c.item,
                srv_format_text(c.srv_format), srv_dim_text(c.srv_dim), count - 1,
                count, static_cast<unsigned long>(hr));
        }
        if (FAILED(hr)) {
            fail_item(c.item, "srv_create_failed");
            continue;
        }

        ComPtr<ID3D11Texture2D> target, staging;
        if (!small_texture(path, g.device.Get(), DXGI_FORMAT_R32_FLOAT, size,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                           target, staging)) {
            fail_item(c.item, "target_create_failed");
            continue;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = g.device->CreateRenderTargetView(target.Get(), nullptr, rtv.GetAddressOf());
        if (FAILED(hr)) {
            fail_item(c.item, "rtv_create_failed");
            continue;
        }
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), zero);
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* bound = srv.Get();
        g.context->PSSetShaderResources(0, 1, &bound);
        g.context->Draw(3, 0);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        ID3D11ShaderResourceView* no_srv = nullptr;
        g.context->PSSetShaderResources(0, 1, &no_srv);
        g.context->Flush();
        const SmallRead back = small_read(path, g.context.Get(), target.Get(),
                                          staging.Get(), size, 4);
        if (!back.ok) {
            fail_item(c.item, "map_read_failed");
            continue;
        }

        const ColumnScan scan = column_scan(back, size, data);
        denormal_expected_total += scan.denormal_expected;
        denormal_bad_total += scan.denormal_bad;
        nan_expected_total += scan.nan_expected;
        nan_bad_total += scan.nan_bad;
        char expected_text[1200], row0_text[1200];
        format_uints(expected_text, sizeof(expected_text), data, count);
        format_uints(row0_text, sizeof(row0_text), scan.row0, count);
        out("MODE=%s ITEM=%s RESULT=%s target=R32_FLOAT size=%dx%d texels=%d"
            " mismatches=%d bad_columns=%d expected=%s actual_min=0x%08x"
            " actual_max=0x%08x all_zero=%s denormal_expected=%d denormal_bad=%d"
            " nan_expected=%d nan_bad=%d reason=%s\n", path, c.item,
            scan.ok ? "PASS" : "FAIL", size, size, size * size, scan.mismatches,
            scan.bad_columns, expected_text, scan.lo, scan.hi,
            scan.all_zero ? "yes" : "no", scan.denormal_expected,
            scan.denormal_bad, scan.nan_expected, scan.nan_bad,
            scan.ok ? "ok" : (scan.all_zero ? "all_zero" : "value_mismatch"));
        out("MODE=%s ITEM=%s stage=row0_actual values=%s\n", path, c.item,
            row0_text);

        if (!scan.ok) {
            ++failed;
            ++g_failures;
        } else if (uint_buffer) {
            ++uint_buffer_exact;
        }
        if (c.kind == kTypedUint) typed_uint_ok = scan.ok;
        else if (c.kind == kStructuredUint) structured_ok = scan.ok;
        else if (c.kind == kRawUint) raw_ok = scan.ok;
        else if (c.kind == kTexture2dUint) texture_ok = scan.ok;
        else float_ok = scan.ok;
    }

    // The failing D2D shader also does three 2D texture OpImageFetch calls, so
    // two texture Load cases use the same raw-bits discipline: the PS loads the
    // stored texel and writes it out again, and the readback is compared as
    // words (raw float bits, or the four packed BGRA8 bytes).
    auto run_texture_load = [&](const char* item, const char* ps_source,
                                DXGI_FORMAT fmt, const unsigned* words,
                                const unsigned* tile, int width, int height) -> bool {
        const int texel_bytes = 4;
        ComPtr<ID3DBlob> lps_blob;
        HRESULT lhr = a8_compile(path, item, ps_source, "ps_5_0", "ps_4_0",
                                 lps_blob.GetAddressOf(), &used);
        ComPtr<ID3D11PixelShader> lps;
        if (SUCCEEDED(lhr))
            lhr = g.device->CreatePixelShader(lps_blob->GetBufferPointer(),
                                              lps_blob->GetBufferSize(), nullptr,
                                              lps.GetAddressOf());
        if (FAILED(lhr)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=shader_create_failed\n", path,
                item);
            return false;
        }

        D3D11_TEXTURE2D_DESC src_desc{};
        src_desc.Width = static_cast<UINT>(width);
        src_desc.Height = static_cast<UINT>(height);
        src_desc.MipLevels = 1;
        src_desc.ArraySize = 1;
        src_desc.Format = fmt;
        src_desc.SampleDesc.Count = 1;
        src_desc.Usage = D3D11_USAGE_DEFAULT;
        src_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> src;
        lhr = g.device->CreateTexture2D(&src_desc, nullptr, src.GetAddressOf());
        out("MODE=%s ITEM=%s stage=create_source_texture fmt=%s size=%dx%d"
            " upload=UpdateSubresource hr=0x%08lx\n", path, item,
            srv_format_text(fmt), width, height,
            static_cast<unsigned long>(lhr));
        if (FAILED(lhr)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=source_texture_create_failed\n",
                path, item);
            return false;
        }
        g.context->UpdateSubresource(src.Get(), 0, nullptr, words,
                                     static_cast<UINT>(width) * texel_bytes, 0);

        D3D11_SHADER_RESOURCE_VIEW_DESC src_srv_desc{};
        src_srv_desc.Format = fmt;
        src_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        src_srv_desc.Texture2D.MostDetailedMip = 0;
        src_srv_desc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> src_srv;
        lhr = g.device->CreateShaderResourceView(src.Get(), &src_srv_desc,
                                                 src_srv.GetAddressOf());
        out("MODE=%s ITEM=%s stage=srv_desc format=%s viewdim=%s mip=0 levels=1"
            " hr=0x%08lx\n", path, item, srv_format_text(fmt),
            srv_dim_text(D3D11_SRV_DIMENSION_TEXTURE2D),
            static_cast<unsigned long>(lhr));
        if (FAILED(lhr)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=srv_create_failed\n", path,
                item);
            return false;
        }

        if (!small_pipeline(path, g, width, vs.Get())) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=pipeline_failed\n", path, item);
            return false;
        }

        ComPtr<ID3D11Texture2D> target, staging;
        if (!small_texture(path, g.device.Get(), fmt, width,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                           target, staging)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed\n", path,
                item);
            return false;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        lhr = g.device->CreateRenderTargetView(target.Get(), nullptr,
                                               rtv.GetAddressOf());
        if (FAILED(lhr)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=rtv_create_failed\n", path, item);
            return false;
        }
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), zero);
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->PSSetShader(lps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* bound = src_srv.Get();
        g.context->PSSetShaderResources(0, 1, &bound);
        g.context->Draw(3, 0);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        ID3D11ShaderResourceView* no_srv = nullptr;
        g.context->PSSetShaderResources(0, 1, &no_srv);
        g.context->Flush();
        const SmallRead lback = small_read(path, g.context.Get(), target.Get(),
                                           staging.Get(), width, texel_bytes);
        if (!lback.ok) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
            return false;
        }

        const TexelScan lscan = texel_scan(lback, width, height, texel_bytes, words);
        char tile_expected[220], tile_actual[220];
        format_uints(tile_expected, sizeof(tile_expected), tile, 16);
        format_uints(tile_actual, sizeof(tile_actual), lscan.tile, 16);
        out("MODE=%s ITEM=%s RESULT=%s source=%s target=%s size=%dx%d texels=%d"
            " upload=UpdateSubresource mismatches=%d first_bad=%d want=0x%08x"
            " got=0x%08x actual_min=0x%08x actual_max=0x%08x all_zero=%s"
            " expected_tile=%s reason=%s\n", path, item,
            lscan.ok ? "PASS" : "FAIL", srv_format_text(fmt),
            srv_format_text(fmt), width, height, width * height, lscan.mismatches,
            lscan.first_bad, lscan.want_bad, lscan.got_bad, lscan.lo, lscan.hi,
            lscan.all_zero ? "yes" : "no", tile_expected,
            lscan.ok ? "ok" : (lscan.all_zero ? "all_zero" : "value_mismatch"));
        out("MODE=%s ITEM=%s stage=tile_actual values=%s\n", path, item,
            tile_actual);
        return lscan.ok;
    };

    const int texture_side = 32;
    const unsigned float_tile[16] = {
        0x3F800000u, 0x00010001u, 0xDEADBEEFu, 0x7FC00001u,
        0xBF800000u, 0x3F000000u, 0x12345678u, 0x00000000u,
        0x7FC00001u, 0xDEADBEEFu, 0x3F800000u, 0x00010001u,
        0x00000000u, 0x12345678u, 0x3F000000u, 0xBF800000u,
    };
    unsigned float_words[texture_side * texture_side];
    for (int y = 0; y < texture_side; ++y) {
        for (int x = 0; x < texture_side; ++x) {
            float_words[y * texture_side + x] = float_tile[(y % 4) * 4 + (x % 4)];
        }
    }

    unsigned bgra8_tile[16];
    for (int ty = 0; ty < 4; ++ty) {
        for (int tx = 0; tx < 4; ++tx) {
            const unsigned b = static_cast<unsigned>(tx * 64);
            const unsigned g = static_cast<unsigned>(ty * 64);
            const unsigned r = static_cast<unsigned>((tx + ty) * 32);
            bgra8_tile[ty * 4 + tx] =
                b | (g << 8) | (r << 16) | (255u << 24);
        }
    }
    unsigned bgra8_words[texture_side * texture_side];
    for (int y = 0; y < texture_side; ++y) {
        for (int x = 0; x < texture_side; ++x) {
            bgra8_words[y * texture_side + x] = bgra8_tile[(y % 4) * 4 + (x % 4)];
        }
    }

    ++items;
    if (run_texture_load("texture2d_float_load", kTypedPsTexture2dFloatLoad,
                         DXGI_FORMAT_R32_FLOAT, float_words, float_tile,
                         texture_side, texture_side)) {
        texture2d_float_ok = true;
    } else {
        ++failed;
        ++g_failures;
    }
    ++items;
    if (run_texture_load("texture2d_bgra8_load", kTypedPsTexture2dBgra8Load,
                         DXGI_FORMAT_B8G8R8A8_UNORM, bgra8_words, bgra8_tile,
                         texture_side, texture_side)) {
        texture2d_bgra8_ok = true;
    } else {
        ++failed;
        ++g_failures;
    }

    out("MODE=%s SUMMARY items=%d failed=%d driver=%s uint_buffer_cases=%d"
        " uint_buffer_exact=%d typed_r32_uint=%s structured_uint=%s"
        " raw_byteaddress=%s texture2d_control=%s typed_r32_float=%s"
        " texture2d_float_load=%s texture2d_bgra8_load=%s"
        " denormal_expected=%d denormal_bad=%d nan_expected=%d nan_bad=%d\n", path,
        items, failed, warp ? "warp" : "hardware", uint_buffer_cases,
        uint_buffer_exact, typed_uint_ok ? "yes" : "no",
        structured_ok ? "yes" : "no", raw_ok ? "yes" : "no",
        texture_ok ? "yes" : "no", float_ok ? "yes" : "no",
        texture2d_float_ok ? "yes" : "no", texture2d_bgra8_ok ? "yes" : "no",
        denormal_expected_total, denormal_bad_total, nan_expected_total,
        nan_bad_total);
    if (failed == 0) {
        out("MODE=%s VERDICT=typed_buffer_srv_bit_exact driver=%s items=%d\n", path,
            warp ? "warp" : "hardware", items);
    } else if (uint_buffer_cases > 0 && uint_buffer_exact == 0 && texture_ok &&
               float_ok) {
        out("MODE=%s VERDICT=uint_buffer_srv_broken driver=%s uint_buffer=0/%d"
            " texture2d_control=exact float_buffer_control=exact\n", path,
            warp ? "warp" : "hardware", uint_buffer_cases);
    } else if (uint_buffer_cases > 0 && uint_buffer_exact == 0 && texture_ok) {
        out("MODE=%s VERDICT=buffer_srv_broken driver=%s uint_buffer=0/%d"
            " texture2d_control=exact float_buffer_control=failed\n", path,
            warp ? "warp" : "hardware", uint_buffer_cases);
    } else if (!texture_ok) {
        out("MODE=%s VERDICT=inconclusive driver=%s texture2d_control=not_exact"
            " uint_buffer=%d/%d\n", path, warp ? "warp" : "hardware",
            uint_buffer_exact, uint_buffer_cases);
    } else {
        out("MODE=%s VERDICT=typed_buffer_srv_partial driver=%s uint_buffer=%d/%d"
            " texture2d_control=exact float_buffer_control=%s\n", path,
            warp ? "warp" : "hardware", uint_buffer_exact, uint_buffer_cases,
            float_ok ? "exact" : "failed");
    }
    return failed ? 2 : 0;
}

// ---- upload-paths ----------------------------------------------------------

// The upload path is the other half of the text failure: glyph bitmaps and LUTs
// are first written into a texture or buffer, then fetched. If the upload lands
// in the wrong place, or not at all, everything downstream reads zero. These
// cases write a known 32x32 block through each upload mechanism and compare the
// fetched bytes with the block, including the expected zero outside the box.

const char* kUploadPsR8Load =
    "Texture2D<float> src : register(t0);\n"
    "float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
    "    float v = src.Load(int3((int)pos.x, (int)pos.y, 0));\n"
    "    return float4(v, v, v, 1.0f);\n"
    "}\n";

struct ByteScan {
    int checked = 0;
    int mismatches = 0;
    int inside_mismatches = 0;
    int outside_mismatches = 0;
    int first_bad = -1;
    unsigned want_bad = 0;
    unsigned got_bad = 0;
    bool all_zero = true;
    bool ok = false;
    unsigned lo = 0xFF;
    unsigned hi = 0;
};

// check[] is 0 (do not judge), 1 (outside the written box, expected 0) or
// 2 (inside the box, expected pattern). The split lets a lost write be told
// apart from a shifted write.
ByteScan byte_scan(const SmallRead& back, int width, int height,
                   const uint8_t* want, const uint8_t* check) {
    ByteScan s;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = back.bytes.data() + static_cast<size_t>(y) * back.pitch;
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            if (check[index] == 0) continue;
            ++s.checked;
            const unsigned v = row[x];
            if (v != 0) s.all_zero = false;
            if (v < s.lo) s.lo = v;
            if (v > s.hi) s.hi = v;
            if (v != want[index]) {
                ++s.mismatches;
                if (check[index] == 2) ++s.inside_mismatches;
                else ++s.outside_mismatches;
                if (s.first_bad < 0) {
                    s.first_bad = index;
                    s.want_bad = want[index];
                    s.got_bad = v;
                }
            }
        }
    }
    s.ok = (s.mismatches == 0);
    return s;
}

int run_upload_paths(bool warp, bool debug_layer) {
    const char* path = "upload-paths";
    const int size = 64;
    const int box_x = 4;
    const int box_y = 4;
    const int box_w = 32;
    const int box_h = 32;

    uint8_t pattern[32 * 32];
    for (int y = 0; y < box_h; ++y) {
        for (int x = 0; x < box_w; ++x) {
            pattern[y * box_w + x] =
                static_cast<uint8_t>((x * 8 + y * 4) & 0xFF);
        }
    }

    uint8_t want_all[64 * 64];
    uint8_t check_all[64 * 64];
    uint8_t want_box[64 * 64];
    uint8_t check_box[64 * 64];
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int index = y * size + x;
            const bool inside = x >= box_x && x < box_x + box_w &&
                                y >= box_y && y < box_y + box_h;
            const uint8_t v =
                inside ? pattern[(y - box_y) * box_w + (x - box_x)] : 0;
            want_all[index] = v;
            check_all[index] = inside ? 2 : 1;
            want_box[index] = v;
            check_box[index] = inside ? 2 : 0;
        }
    }

    out("MODE=%s stage=data box=%d,%d..%d,%d block=32x32 pattern=gradient"
        " outside=clear0\n", path, box_x, box_y, box_x + box_w, box_y + box_h);

    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "upload_vs", kA8Vs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    ComPtr<ID3D11VertexShader> vs;
    if (SUCCEEDED(hr))
        hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs.GetAddressOf());
    if (FAILED(hr) || !small_pipeline(path, g, size, vs.Get())) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=setup_failed\n", path);
        return 2;
    }

    ComPtr<ID3DBlob> r8_blob;
    hr = a8_compile(path, "upload_ps_r8_load", kUploadPsR8Load, "ps_5_0",
                    "ps_4_0", r8_blob.GetAddressOf(), &used);
    ComPtr<ID3D11PixelShader> r8_ps;
    if (SUCCEEDED(hr))
        hr = g.device->CreatePixelShader(r8_blob->GetBufferPointer(),
                                         r8_blob->GetBufferSize(), nullptr,
                                         r8_ps.GetAddressOf());
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=r8_ps_create_failed\n", path);
        return 2;
    }

    ComPtr<ID3DBlob> buf_blob;
    hr = a8_compile(path, "upload_ps_buffer_uint", kTypedPsBufferUint, "ps_5_0",
                    "ps_4_0", buf_blob.GetAddressOf(), &used);
    ComPtr<ID3D11PixelShader> buffer_ps;
    if (SUCCEEDED(hr))
        hr = g.device->CreatePixelShader(buf_blob->GetBufferPointer(),
                                         buf_blob->GetBufferSize(), nullptr,
                                         buffer_ps.GetAddressOf());
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=buffer_ps_create_failed\n", path);
        return 2;
    }

    auto clear_tex_zero = [&](const char* item, ID3D11Texture2D* tex) -> bool {
        ComPtr<ID3D11RenderTargetView> rtv;
        HRESULT lhr =
            g.device->CreateRenderTargetView(tex, nullptr, rtv.GetAddressOf());
        out("MODE=%s ITEM=%s stage=clear_zero_create_rtv hr=0x%08lx\n", path, item,
            static_cast<unsigned long>(lhr));
        if (FAILED(lhr)) return false;
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), zero);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        g.context->Flush();
        out("MODE=%s ITEM=%s stage=clear_zero color=0,0,0,0 hr=0x%08lx\n", path,
            item, 0L);
        return true;
    };

    auto make_tex_srv = [&](const char* item, ID3D11Texture2D* tex,
                            DXGI_FORMAT fmt,
                            ComPtr<ID3D11ShaderResourceView>& srv) -> HRESULT {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels = 1;
        const HRESULT lhr =
            g.device->CreateShaderResourceView(tex, &sd, srv.GetAddressOf());
        out("MODE=%s ITEM=%s stage=srv_desc format=%s viewdim=TEXTURE2D mip=0"
            " levels=1 hr=0x%08lx\n", path, item, srv_format_text(fmt),
            static_cast<unsigned long>(lhr));
        return lhr;
    };

    auto render_to_readback = [&](const char* item, ID3D11ShaderResourceView* srv,
                                  ID3D11PixelShader* ps, DXGI_FORMAT fmt,
                                  int bpp, SmallRead* back) -> bool {
        ComPtr<ID3D11Texture2D> target, staging;
        if (!small_texture(path, g.device.Get(), fmt, size,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                           target, staging)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=target_create_failed\n", path,
                item);
            return false;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        HRESULT lhr = g.device->CreateRenderTargetView(target.Get(), nullptr,
                                                       rtv.GetAddressOf());
        if (FAILED(lhr)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=rtv_create_failed"
                " hr=0x%08lx\n", path, item, static_cast<unsigned long>(lhr));
            return false;
        }
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), zero);
        g.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        g.context->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* bound = srv;
        g.context->PSSetShaderResources(0, 1, &bound);
        g.context->Draw(3, 0);
        ID3D11RenderTargetView* no_rtv = nullptr;
        g.context->OMSetRenderTargets(1, &no_rtv, nullptr);
        ID3D11ShaderResourceView* no_srv = nullptr;
        g.context->PSSetShaderResources(0, 1, &no_srv);
        g.context->Flush();
        *back = small_read(path, g.context.Get(), target.Get(), staging.Get(),
                           size, bpp);
        if (!back->ok) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
            return false;
        }
        return true;
    };

    auto print_byte_item = [&](const char* item, const ByteScan& b) {
        out("MODE=%s ITEM=%s RESULT=%s target=R8_UNORM size=%dx%d texels=%d"
            " checked=%d mismatches=%d inside_mismatches=%d"
            " outside_mismatches=%d expected=gradient_box actual_min=%u"
            " actual_max=%u all_zero=%s first_bad=%d want=%u got=%u reason=%s\n",
            path, item, b.ok ? "PASS" : "FAIL", size, size, size * size,
            b.checked, b.mismatches, b.inside_mismatches, b.outside_mismatches,
            b.lo, b.hi, b.all_zero ? "yes" : "no", b.first_bad, b.want_bad,
            b.got_bad, b.ok ? "ok" : (b.all_zero ? "all_zero" : "value_mismatch"));
    };

    int items = 0;
    int failed = 0;
    bool update_ok = false;
    bool copy_ok = false;
    bool buffer_ok = false;
    bool dyn_tex_ok = false;

    // 1. UpdateSubresource with a destination box into a cleared DEFAULT texture.
    ++items;
    {
        const char* item = "texture_update_box";
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(size);
        desc.Height = static_cast<UINT>(size);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        HRESULT lhr = g.device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        out("MODE=%s ITEM=%s stage=create usage=DEFAULT bind=RT|SRV fmt=R8_UNORM"
            " size=%dx%d hr=0x%08lx\n", path, item, size, size,
            static_cast<unsigned long>(lhr));
        bool ok = false;
        if (SUCCEEDED(lhr) && clear_tex_zero(item, tex.Get())) {
            D3D11_BOX dst_box{};
            dst_box.left = static_cast<UINT>(box_x);
            dst_box.top = static_cast<UINT>(box_y);
            dst_box.front = 0;
            dst_box.right = static_cast<UINT>(box_x + box_w);
            dst_box.bottom = static_cast<UINT>(box_y + box_h);
            dst_box.back = 1;
            g.context->UpdateSubresource(tex.Get(), 0, &dst_box, pattern, box_w, 0);
            out("MODE=%s ITEM=%s stage=update_box dst_box=%d,%d..%d,%d"
                " src_pitch=%d bytes=%d hr=0x%08lx\n", path, item, box_x, box_y,
                box_x + box_w, box_y + box_h, box_w, box_w * box_h, 0L);
            ComPtr<ID3D11ShaderResourceView> srv;
            lhr = make_tex_srv(item, tex.Get(), DXGI_FORMAT_R8_UNORM, srv);
            SmallRead back;
            if (SUCCEEDED(lhr) && render_to_readback(item, srv.Get(), r8_ps.Get(),
                                                     DXGI_FORMAT_R8_UNORM, 1,
                                                     &back)) {
                const ByteScan b = byte_scan(back, size, size, want_all, check_all);
                print_byte_item(item, b);
                ok = b.ok;
            } else {
                out("MODE=%s ITEM=%s RESULT=FAIL reason=update_readback_failed\n",
                    path, item);
            }
        } else {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=update_setup_failed\n", path,
                item);
        }
        if (ok) update_ok = true;
        else {
            ++failed;
            ++g_failures;
        }
    }

    // 2. CopySubresourceRegion from a known staging texture into a cleared
    //    DEFAULT texture at (4,4).
    ++items;
    {
        const char* item = "texture_copy_box";
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(size);
        desc.Height = static_cast<UINT>(size);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> dst;
        HRESULT lhr = g.device->CreateTexture2D(&desc, nullptr, dst.GetAddressOf());
        out("MODE=%s ITEM=%s stage=create usage=DEFAULT bind=RT|SRV fmt=R8_UNORM"
            " size=%dx%d hr=0x%08lx\n", path, item, size, size,
            static_cast<unsigned long>(lhr));
        bool ok = false;
        if (SUCCEEDED(lhr) && clear_tex_zero(item, dst.Get())) {
            D3D11_TEXTURE2D_DESC stage_desc = desc;
            stage_desc.Width = static_cast<UINT>(box_w);
            stage_desc.Height = static_cast<UINT>(box_h);
            stage_desc.Usage = D3D11_USAGE_STAGING;
            stage_desc.BindFlags = 0;
            stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            stage_desc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> src;
            lhr = g.device->CreateTexture2D(&stage_desc, nullptr,
                                            src.GetAddressOf());
            out("MODE=%s ITEM=%s stage=create_staging_source fmt=R8_UNORM"
                " size=%dx%d usage=STAGING cpu=WRITE hr=0x%08lx\n", path, item,
                box_w, box_h, static_cast<unsigned long>(lhr));
            if (SUCCEEDED(lhr)) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                lhr = g.context->Map(src.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
                out("MODE=%s ITEM=%s stage=map_write row_pitch=%u hr=0x%08lx\n",
                    path, item, mapped.RowPitch,
                    static_cast<unsigned long>(lhr));
                if (SUCCEEDED(lhr)) {
                    for (int y = 0; y < box_h; ++y) {
                        memcpy(static_cast<uint8_t*>(mapped.pData) +
                                   static_cast<size_t>(y) * mapped.RowPitch,
                               pattern + y * box_w, box_w);
                    }
                    g.context->Unmap(src.Get(), 0);
                    D3D11_BOX src_box{};
                    src_box.left = 0;
                    src_box.top = 0;
                    src_box.front = 0;
                    src_box.right = static_cast<UINT>(box_w);
                    src_box.bottom = static_cast<UINT>(box_h);
                    src_box.back = 1;
                    g.context->CopySubresourceRegion(dst.Get(), 0, box_x, box_y,
                                                     0, src.Get(), 0, &src_box);
                    out("MODE=%s ITEM=%s stage=copy_region dst=%d,%d,0"
                        " src_box=0,0..%d,%d hr=0x%08lx\n", path, item, box_x,
                        box_y, box_w, box_h, 0L);
                    ComPtr<ID3D11ShaderResourceView> srv;
                    lhr = make_tex_srv(item, dst.Get(), DXGI_FORMAT_R8_UNORM, srv);
                    SmallRead back;
                    if (SUCCEEDED(lhr) &&
                        render_to_readback(item, srv.Get(), r8_ps.Get(),
                                           DXGI_FORMAT_R8_UNORM, 1, &back)) {
                        const ByteScan b =
                            byte_scan(back, size, size, want_all, check_all);
                        print_byte_item(item, b);
                        ok = b.ok;
                    } else {
                        out("MODE=%s ITEM=%s RESULT=FAIL"
                            " reason=copy_readback_failed\n", path, item);
                    }
                } else {
                    out("MODE=%s ITEM=%s RESULT=FAIL reason=staging_map_failed\n",
                        path, item);
                }
            } else {
                out("MODE=%s ITEM=%s RESULT=FAIL reason=staging_create_failed\n",
                    path, item);
            }
        } else {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=copy_setup_failed\n", path,
                item);
        }
        if (ok) copy_ok = true;
        else {
            ++failed;
            ++g_failures;
        }
    }

    // 3. DYNAMIC buffer: Map(WRITE_DISCARD), write 64 uint words, fetch through
    //    a typed SRV and store the raw bits into R32_FLOAT.
    ++items;
    {
        const char* item = "dynamic_buffer_map";
        unsigned data[64];
        for (int i = 0; i < 64; ++i) data[i] = static_cast<unsigned>(i + 1);
        data[0] = 0x00010001u;
        data[1] = 0xDEADBEEFu;
        data[2] = 0x7FC00001u;
        data[3] = 0x12345678u;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 64 * 4;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        HRESULT lhr = g.device->CreateBuffer(&desc, nullptr,
                                             buffer.GetAddressOf());
        out("MODE=%s ITEM=%s stage=create usage=DYNAMIC bytes=%d cpu=WRITE"
            " hr=0x%08lx\n", path, item, 64 * 4,
            static_cast<unsigned long>(lhr));
        bool ok = false;
        if (SUCCEEDED(lhr)) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            lhr = g.context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped);
            out("MODE=%s ITEM=%s stage=map_write_discard row_pitch=%u"
                " hr=0x%08lx\n", path, item, mapped.RowPitch,
                static_cast<unsigned long>(lhr));
            if (SUCCEEDED(lhr)) {
                memcpy(mapped.pData, data, sizeof(data));
                g.context->Unmap(buffer.Get(), 0);
                out("MODE=%s ITEM=%s stage=unmap words=%d bytes=%zu\n", path, item,
                    static_cast<int>(sizeof(data) / sizeof(data[0])),
                    sizeof(data));
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R32_UINT;
                sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                sd.Buffer.FirstElement = 0;
                sd.Buffer.NumElements = 64;
                ComPtr<ID3D11ShaderResourceView> srv;
                lhr = g.device->CreateShaderResourceView(buffer.Get(), &sd,
                                                         srv.GetAddressOf());
                out("MODE=%s ITEM=%s stage=srv_desc format=R32_UINT viewdim=BUFFER"
                    " first=0 elements=64 hr=0x%08lx\n", path, item,
                    static_cast<unsigned long>(lhr));
                SmallRead back;
                if (SUCCEEDED(lhr) &&
                    render_to_readback(item, srv.Get(), buffer_ps.Get(),
                                       DXGI_FORMAT_R32_FLOAT, 4, &back)) {
                    const ColumnScan scan = column_scan(back, size, data);
                    char expected_text[1200], row0_text[1200];
                    format_uints(expected_text, sizeof(expected_text), data, 64);
                    format_uints(row0_text, sizeof(row0_text), scan.row0, 64);
                    out("MODE=%s ITEM=%s RESULT=%s target=R32_FLOAT size=%dx%d"
                        " texels=%d mismatches=%d bad_columns=%d expected=%s"
                        " actual_min=0x%08x actual_max=0x%08x all_zero=%s"
                        " denormal_expected=%d denormal_bad=%d nan_expected=%d"
                        " nan_bad=%d reason=%s\n", path, item,
                        scan.ok ? "PASS" : "FAIL", size, size, size * size,
                        scan.mismatches, scan.bad_columns, expected_text, scan.lo,
                        scan.hi, scan.all_zero ? "yes" : "no",
                        scan.denormal_expected, scan.denormal_bad,
                        scan.nan_expected, scan.nan_bad,
                        scan.ok ? "ok"
                                : (scan.all_zero ? "all_zero"
                                                 : "value_mismatch"));
                    out("MODE=%s ITEM=%s stage=row0_actual values=%s\n", path,
                        item, row0_text);
                    ok = scan.ok;
                } else {
                    out("MODE=%s ITEM=%s RESULT=FAIL"
                        " reason=buffer_readback_failed\n", path, item);
                }
            } else {
                out("MODE=%s ITEM=%s RESULT=FAIL reason=buffer_map_failed\n",
                    path, item);
            }
        } else {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=buffer_create_failed\n", path,
                item);
        }
        if (ok) buffer_ok = true;
        else {
            ++failed;
            ++g_failures;
        }
    }

    // 4. DYNAMIC texture: Map(WRITE_DISCARD) and write only the 32x32 block at
    //    (4,4). The rest of a discarded dynamic texture is undefined, so only
    //    the written block is judged.
    ++items;
    {
        const char* item = "dynamic_texture_map";
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(size);
        desc.Height = static_cast<UINT>(size);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Texture2D> tex;
        HRESULT lhr = g.device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        out("MODE=%s ITEM=%s stage=create usage=DYNAMIC bind=SRV fmt=R8_UNORM"
            " size=%dx%d cpu=WRITE hr=0x%08lx\n", path, item, size, size,
            static_cast<unsigned long>(lhr));
        bool ok = false;
        if (SUCCEEDED(lhr)) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            lhr = g.context->Map(tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped);
            out("MODE=%s ITEM=%s stage=map_write_discard row_pitch=%u"
                " hr=0x%08lx\n", path, item, mapped.RowPitch,
                static_cast<unsigned long>(lhr));
            if (SUCCEEDED(lhr)) {
                for (int y = 0; y < box_h; ++y) {
                    memcpy(static_cast<uint8_t*>(mapped.pData) +
                               static_cast<size_t>(box_y + y) * mapped.RowPitch +
                               box_x,
                           pattern + y * box_w, box_w);
                }
                g.context->Unmap(tex.Get(), 0);
                out("MODE=%s ITEM=%s stage=unmap block=%d,%d..%d,%d judged=block\n",
                    path, item, box_x, box_y, box_x + box_w, box_y + box_h);
                ComPtr<ID3D11ShaderResourceView> srv;
                lhr = make_tex_srv(item, tex.Get(), DXGI_FORMAT_R8_UNORM, srv);
                SmallRead back;
                if (SUCCEEDED(lhr) &&
                    render_to_readback(item, srv.Get(), r8_ps.Get(),
                                       DXGI_FORMAT_R8_UNORM, 1, &back)) {
                    const ByteScan b =
                        byte_scan(back, size, size, want_box, check_box);
                    print_byte_item(item, b);
                    ok = b.ok;
                } else {
                    out("MODE=%s ITEM=%s RESULT=FAIL"
                        " reason=dynamic_texture_readback_failed\n", path, item);
                }
            } else {
                out("MODE=%s ITEM=%s RESULT=FAIL reason=dynamic_texture_map_failed\n",
                    path, item);
            }
        } else {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=dynamic_texture_create_failed\n",
                path, item);
        }
        if (ok) dyn_tex_ok = true;
        else {
            ++failed;
            ++g_failures;
        }
    }

    out("MODE=%s SUMMARY items=%d failed=%d driver=%s texture_update_box=%s"
        " texture_copy_box=%s dynamic_buffer_map=%s dynamic_texture_map=%s\n",
        path, items, failed, warp ? "warp" : "hardware",
        update_ok ? "yes" : "no", copy_ok ? "yes" : "no",
        buffer_ok ? "yes" : "no", dyn_tex_ok ? "yes" : "no");
    if (failed == 0) {
        out("MODE=%s VERDICT=upload_paths_bit_exact driver=%s items=%d\n", path,
            warp ? "warp" : "hardware", items);
    } else if (!update_ok && !copy_ok) {
        out("MODE=%s VERDICT=update_and_copy_broken driver=%s"
            " dynamic_buffer=%s dynamic_texture=%s\n", path,
            warp ? "warp" : "hardware", buffer_ok ? "exact" : "failed",
            dyn_tex_ok ? "exact" : "failed");
    } else if (!buffer_ok) {
        out("MODE=%s VERDICT=dynamic_buffer_broken driver=%s\n", path,
            warp ? "warp" : "hardware");
    } else if (!dyn_tex_ok) {
        out("MODE=%s VERDICT=dynamic_texture_broken driver=%s\n", path,
            warp ? "warp" : "hardware");
    } else {
        out("MODE=%s VERDICT=upload_paths_partial driver=%s\n", path,
            warp ? "warp" : "hardware");
    }
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// mixed-fetch: D2D's text shader (persisted as fs.8ed1f4d2...) reads a
// Buffer<uint> and a Texture2D<float4> in the same draw: five OpImageFetch on
// image<u32, Buffer> plus three on image<f32, 2D>. Every earlier mode bound a
// single resource kind, so a descriptor/binding bug that only shows when both
// are alive would not have been visible. Each case draws one fullscreen quad
// into its own cleared 64x64 BGRA8 target and checks the centre pixel.
// ---------------------------------------------------------------------------

const char* kMixedVs =
    "struct VSOut { float4 pos : SV_POSITION; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "    float2 p = float2(float((id << 1) & 2u), float(id & 2u));\n"
    "    VSOut o;\n"
    "    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

// Buffer only, but through the same bitcast-then-signed-convert shape the text
// shader uses: asint(sum) then (float) -> ConvertSToF.
const char* kMixedPsBufferOnly =
    "Buffer<uint> gBits : register(t0);\n"
    "float4 main() : SV_Target {\n"
    "    uint s = gBits.Load(0u) + gBits.Load(1u) + gBits.Load(2u) + gBits.Load(3u)\n"
    "           + gBits.Load(4u);\n"
    "    float v = (float)asint(s) / 255.0;\n"
    "    return float4(v, v, v, 1.0);\n"
    "}\n";

const char* kMixedPsTextureOnly =
    "Texture2D<float4> gLut : register(t0);\n"
    "float4 main() : SV_Target {\n"
    "    return gLut.Load(int3(33, 0, 0));\n"
    "}\n";

const char* kMixedPsBoth =
    "Buffer<uint> gBits : register(t0);\n"
    "Texture2D<float4> gLut : register(t1);\n"
    "float4 main() : SV_Target {\n"
    "    uint s = gBits.Load(0u) + gBits.Load(1u) + gBits.Load(2u) + gBits.Load(3u)\n"
    "           + gBits.Load(4u);\n"
    "    float cov = (float)asint(s) / 255.0;\n"
    "    float4 lut = gLut.Load(int3((int)(s + 217u), 0, 0));\n"
    "    return float4(lut.rgb * cov, 1.0);\n"
    "}\n";

// The LUT texture is 512x1 BGRA8; texel x holds (x & 255) in every channel.
constexpr int kMixedLutWidth = 512;
constexpr int kMixedTarget = 64;

bool mixed_lut_texture(const char* path, Gpu& g, ComPtr<ID3D11Texture2D>& tex,
                       ComPtr<ID3D11ShaderResourceView>& srv) {
    std::vector<uint8_t> px(static_cast<size_t>(kMixedLutWidth) * 4, 0);
    for (int x = 0; x < kMixedLutWidth; ++x) {
        const uint8_t v = static_cast<uint8_t>(x & 255);
        px[static_cast<size_t>(x) * 4 + 0] = v;
        px[static_cast<size_t>(x) * 4 + 1] = v;
        px[static_cast<size_t>(x) * 4 + 2] = v;
        px[static_cast<size_t>(x) * 4 + 3] = 255;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kMixedLutWidth;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = px.data();
    init.SysMemPitch = kMixedLutWidth * 4;
    HRESULT hr = g.device->CreateTexture2D(&td, &init, tex.GetAddressOf());
    out("MODE=%s stage=create_lut_texture width=%d height=1 fmt=BGRA8 hr=0x%08lx\n", path,
        kMixedLutWidth, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = g.device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf());
    out("MODE=%s stage=create_lut_srv hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

bool mixed_uint_buffer(const char* path, Gpu& g, ComPtr<ID3D11Buffer>& buf,
                       ComPtr<ID3D11ShaderResourceView>& srv) {
    const uint32_t values[5] = {51u, 51u, 51u, 51u, 51u};  // sum 255
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(values);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = values;
    HRESULT hr = g.device->CreateBuffer(&bd, &init, buf.GetAddressOf());
    out("MODE=%s stage=create_uint_buffer bytes=%u hr=0x%08lx\n", path,
        static_cast<unsigned>(sizeof(values)), static_cast<unsigned long>(hr));
    if (FAILED(hr)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_UINT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = 5;
    hr = g.device->CreateShaderResourceView(buf.Get(), &sd, srv.GetAddressOf());
    out("MODE=%s stage=create_uint_srv hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

int run_mixed_fetch(bool warp, bool debug_layer) {
    const char* path = "mixed-fetch";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "mixed_vs", kMixedVs, "vs_5_0", "vs_4_0",
                            vs_blob.GetAddressOf(), &used);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=vs_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }
    ComPtr<ID3D11VertexShader> vs;
    hr = g.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                      nullptr, vs.GetAddressOf());
    out("MODE=%s stage=create_vs hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=vs_create_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Texture2D> tex, staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kMixedTarget,
                       D3D11_BIND_RENDER_TARGET, tex, staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=target_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = g.device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
    out("MODE=%s stage=create_rtv hr=0x%08lx\n", path, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=rtv_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Buffer> bits;
    ComPtr<ID3D11ShaderResourceView> bits_srv;
    if (!mixed_uint_buffer(path, g, bits, bits_srv)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=uint_buffer_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11Texture2D> lut;
    ComPtr<ID3D11ShaderResourceView> lut_srv;
    if (!mixed_lut_texture(path, g, lut, lut_srv)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=lut_texture_failed\n", path);
        return 2;
    }

    // name, PS source, whether the buffer is bound at t0, whether the LUT is
    // bound at t1 (or t0 when alone), expected centre BGRA.
    struct Case {
        const char* name;
        const char* ps_src;
        bool bind_buffer;
        bool bind_lut;
        int expected[4];
    };
    const Case cases[] = {
        {"buffer_only", kMixedPsBufferOnly, true, false, {255, 255, 255, 255}},
        {"texture_only", kMixedPsTextureOnly, false, true, {33, 33, 33, 255}},
        // sum 255 -> coverage 1.0, LUT index 255 + 217 = 472 -> texel 216.
        {"buffer_and_texture", kMixedPsBoth, true, true, {216, 216, 216, 255}},
    };

    int failed = 0;
    for (const Case& c : cases) {
        ComPtr<ID3DBlob> ps_blob;
        const char* ps_used = nullptr;
        HRESULT ps_hr = a8_compile(path, c.name, c.ps_src, "ps_5_0", "ps_4_0",
                                   ps_blob.GetAddressOf(), &ps_used);
        if (FAILED(ps_hr)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=ps_compile_failed hr=0x%08lx\n", path,
                c.name, static_cast<unsigned long>(ps_hr));
            ++failed;
            continue;
        }
        ComPtr<ID3D11PixelShader> ps;
        ps_hr = g.device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                            ps_blob->GetBufferSize(), nullptr,
                                            ps.GetAddressOf());
        if (FAILED(ps_hr)) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=ps_create_failed hr=0x%08lx\n", path,
                c.name, static_cast<unsigned long>(ps_hr));
            ++failed;
            continue;
        }
        if (!small_pipeline(path, g, kMixedTarget, vs.Get())) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=pipeline_failed\n", path, c.name);
            ++failed;
            continue;
        }
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.context->ClearRenderTargetView(rtv.Get(), clear);
        ID3D11RenderTargetView* rtv_array[1] = {rtv.Get()};
        g.context->OMSetRenderTargets(1, rtv_array, nullptr);
        g.context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {nullptr, nullptr};
        if (c.bind_lut && !c.bind_buffer) srvs[0] = lut_srv.Get();
        if (c.bind_buffer) srvs[0] = bits_srv.Get();
        if (c.bind_buffer && c.bind_lut) srvs[1] = lut_srv.Get();
        g.context->PSSetShaderResources(0, 2, srvs);
        g.context->Draw(6, 0);
        const SmallRead back =
            small_read(path, g.context.Get(), tex.Get(), staging.Get(), kMixedTarget, 4);
        if (!back.ok) {
            out("MODE=%s ITEM=%s RESULT=FAIL reason=readback_failed\n", path, c.name);
            ++failed;
            continue;
        }
        const size_t off = static_cast<size_t>(kMixedTarget / 2) * back.pitch +
                           static_cast<size_t>(kMixedTarget / 2) * 4;
        const int got[4] = {back.bytes[off + 0], back.bytes[off + 1], back.bytes[off + 2],
                            back.bytes[off + 3]};
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            if (abs(got[i] - c.expected[i]) > 2) ok = false;
        }
        out("MODE=%s ITEM=%s RESULT=%s got=%d,%d,%d,%d expected=%d,%d,%d,%d\n", path,
            c.name, ok ? "PASS" : "FAIL", got[0], got[1], got[2], got[3], c.expected[0],
            c.expected[1], c.expected[2], c.expected[3]);
        if (!ok) ++failed;
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        g.context->PSSetShaderResources(0, 2, none);
    }

    if (failed) ++g_failures;
    out("MODE=%s SUMMARY cases=%d failed=%d\n", path,
        static_cast<int>(sizeof(cases) / sizeof(cases[0])), failed);
    return failed ? 2 : 0;
}

// ---------------------------------------------------------------------------
// int-attribs: the D2D text vertex shader takes five attributes and forwards
// four of them to the pixel shader as flat integers (the glyph index and run
// data); the failing PS receives them as uint4 at one location with one flat
// component each. Every earlier geometry mode used float attributes only, so a
// broken integer vertex attribute fetch would have been invisible. Each case
// draws one fullscreen quad with a known bit pattern and reads back the centre.
// ---------------------------------------------------------------------------

const char* kIntVsPos =
    "struct VSIn { float2 pos : POSITION; };\n"
    "float4 main(VSIn i) : SV_POSITION { return float4(i.pos, 0.0, 1.0); }\n";

const char* kIntVsFloat4 =
    "struct VSIn { float2 pos : POSITION; float4 g : GLYPH; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 g : GLYPH; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.g = i.g; return o; }\n";

const char* kIntVsUint4 =
    "struct VSIn { float2 pos : POSITION; uint4 g : GLYPH; };\n"
    "struct VSOut { float4 pos : SV_POSITION; nointerpolation uint4 g : GLYPH; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.g = i.g; return o; }\n";

const char* kIntVsUint1 =
    "struct VSIn { float2 pos : POSITION; uint g : G16; };\n"
    "struct VSOut { float4 pos : SV_POSITION; nointerpolation uint g : G16; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.g = i.g; return o; }\n";

// 0x2A63 -> low byte 99, high byte 42; g.y -> 7. Written as x,y,z so the BGRA
// readback is 7,42,99,255.
const char* kIntPsUint4 =
    "struct PSIn { nointerpolation uint4 g : GLYPH; };\n"
    "float4 main(PSIn i) : SV_Target {\n"
    "    return float4(float(i.g.x & 255u), float((i.g.x >> 8) & 255u),\n"
    "                  float(i.g.y & 255u), 255.0) / 255.0;\n"
    "}\n";

const char* kIntPsFloat4 =
    "struct PSIn { float4 g : GLYPH; };\n"
    "float4 main(PSIn i) : SV_Target { return float4(i.g.rgb, 1.0); }\n";

const char* kIntPsUint1 =
    "struct PSIn { nointerpolation uint g : G16; };\n"
    "float4 main(PSIn i) : SV_Target {\n"
    "    return float4(float(i.g & 255u), float((i.g >> 8) & 255u), 0.0, 255.0) / 255.0;\n"
    "}\n";

const char* kIntPsConst = "float4 main() : SV_Target { return float4(0.0, 0.0, 0.0, 1.0); }\n";

const char* kIntVsConstVarying =
    "struct VSIn { float2 pos : POSITION; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 g : GLYPH; };\n"
    "VSOut main(VSIn i) {\n"
    "    VSOut o; o.pos = float4(i.pos, 0.0, 1.0);\n"
    "    o.g = float4(0.5, 0.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

const char* kIntVsTexcoordVarying =
    "struct VSIn { float2 pos : POSITION; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 g : TEXCOORD0; };\n"
    "VSOut main(VSIn i) {\n"
    "    VSOut o; o.pos = float4(i.pos, 0.0, 1.0);\n"
    "    o.g = float4(0.5, 0.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

const char* kIntPsTexcoordVarying =
    "struct PSIn { float4 g : TEXCOORD0; };\n"
    "float4 main(PSIn i) : SV_Target { return float4(i.g.rgb, 1.0); }\n";

const char* kIntVsColorVarying =
    "struct VSIn { float2 pos : POSITION; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 g : COLOR0; };\n"
    "VSOut main(VSIn i) {\n"
    "    VSOut o; o.pos = float4(i.pos, 0.0, 1.0);\n"
    "    o.g = float4(0.5, 0.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

const char* kIntPsColorVarying =
    "struct PSIn { float4 g : COLOR0; };\n"
    "float4 main(PSIn i) : SV_Target { return float4(i.g.rgb, 1.0); }\n";

const char* kIntPsRed = "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

const char* kIntVsDerived =
    "struct VSIn { float2 pos : POSITION; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 g : GLYPH; };\n"
    "VSOut main(VSIn i) {\n"
    "    VSOut o; o.pos = float4(i.pos, 0.0, 1.0);\n"
    "    o.g = float4(i.pos.x * 0.5 + 0.5, 0.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

constexpr int kIntSize = 256;

void write_bmp(const char* item, const SmallRead& r, int size) {
    char file[260];
    _snprintf_s(file, sizeof(file), _TRUNCATE, "C:\\Users\\USER\\ia_%s.bmp", item);
    FILE* f = nullptr;
    if (fopen_s(&f, file, "wb") != 0 || !f) return;
    const int row = size * 4;
    const unsigned int data_size = static_cast<unsigned int>(row * size);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    fh.bfSize = fh.bfOffBits + data_size;
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = size;
    ih.biHeight = size;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = data_size;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = size - 1; y >= 0; --y)
        fwrite(r.bytes.data() + static_cast<size_t>(y) * r.pitch,
               static_cast<size_t>(row), 1, f);
    fclose(f);
    out("MODE=%s stage=write_bmp item=%s file=%s\n", "int-attribs", item, file);
}

void reflect_signatures(const char* path, const char* name, ID3DBlob* vs_blob,
                        ID3DBlob* ps_blob) {    ID3DBlob* blobs[2] = {vs_blob, ps_blob};
    const char* which[2] = {"vs", "ps"};
    for (int b = 0; b < 2; ++b) {
        ComPtr<ID3D11ShaderReflection> refl;
        HRESULT hr = D3DReflect(blobs[b]->GetBufferPointer(), blobs[b]->GetBufferSize(),
                                __uuidof(ID3D11ShaderReflection),
                                reinterpret_cast<void**>(refl.GetAddressOf()));
        if (FAILED(hr)) {
            out("MODE=%s stage=reflect name=%s who=%s hr=0x%08lx\n", path, name, which[b],
                static_cast<unsigned long>(hr));
            continue;
        }
        D3D11_SHADER_DESC d{};
        refl->GetDesc(&d);
        out("MODE=%s stage=reflect name=%s who=%s inputs=%u outputs=%u\n", path, name,
            which[b], d.InputParameters, d.OutputParameters);
        for (UINT i = 0; i < d.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            if (SUCCEEDED(refl->GetInputParameterDesc(i, &p)))
                out("MODE=%s stage=reflect name=%s who=%s in[%u] sem=%s%u reg=%u comp=%u"
                    " mask=0x%x\n", path, name, which[b], i, p.SemanticName,
                    p.SemanticIndex, p.Register, p.ComponentType, p.Mask);
        }
        for (UINT i = 0; i < d.OutputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            if (SUCCEEDED(refl->GetOutputParameterDesc(i, &p)))
                out("MODE=%s stage=reflect name=%s who=%s out[%u] sem=%s%u reg=%u comp=%u"
                    " mask=0x%x\n", path, name, which[b], i, p.SemanticName,
                    p.SemanticIndex, p.Register, p.ComponentType, p.Mask);
        }
    }
}

// Fullscreen quad, two triangles, y flipped in NDC is irrelevant here.
const float kIntPos6[] = {
    -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
};

int run_int_attribs(bool warp, bool debug_layer) {
    const char* path = "int-attribs";
    Gpu g;
    if (!gpu_create(path, warp, debug_layer, g)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=gpu_create_failed\n", path);
        return 2;
    }
    gpu_adapter(path, g.device.Get());

    ComPtr<ID3DBlob> vs_pos_blob, vs_float_blob, vs_uint4_blob, vs_uint1_blob;
    ComPtr<ID3DBlob> ps_float_blob, ps_uint4_blob, ps_uint1_blob;
    const char* used = nullptr;
    HRESULT hr = a8_compile(path, "ia_vs_pos", kIntVsPos, "vs_5_0", "vs_5_0",
                            vs_pos_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "ia_vs_float4", kIntVsFloat4, "vs_5_0", "vs_5_0",
                        vs_float_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "ia_vs_uint4", kIntVsUint4, "vs_5_0", "vs_5_0",
                        vs_uint4_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "ia_vs_uint1", kIntVsUint1, "vs_5_0", "vs_5_0",
                        vs_uint1_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "ia_ps_float4", kIntPsFloat4, "ps_5_0", "ps_5_0",
                        ps_float_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "ia_ps_uint4", kIntPsUint4, "ps_5_0", "ps_5_0",
                        ps_uint4_blob.GetAddressOf(), &used);
    if (SUCCEEDED(hr))
        hr = a8_compile(path, "ia_ps_uint1", kIntPsUint1, "ps_5_0", "ps_5_0",
                        ps_uint1_blob.GetAddressOf(), &used);
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_compile_failed hr=0x%08lx\n", path,
            static_cast<unsigned long>(hr));
        return 2;
    }

    struct Pair {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3DBlob> vs_blob;
    };
    Pair float4_pair, uint4_pair, uint1_pair;
    auto make_pair = [&](const char* name, ID3DBlob* vs_blob, ID3DBlob* ps_blob,
                         Pair& pair) {
        pair.vs_blob = vs_blob;
        HRESULT h = g.device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                                 vs_blob->GetBufferSize(), nullptr,
                                                 pair.vs.GetAddressOf());
        if (SUCCEEDED(h))
            h = g.device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                            ps_blob->GetBufferSize(), nullptr,
                                            pair.ps.GetAddressOf());
        out("MODE=%s stage=create_shader_pair name=%s hr=0x%08lx\n", path, name,
            static_cast<unsigned long>(h));
        return SUCCEEDED(h);
    };
    if (!make_pair("float4", vs_float_blob.Get(), ps_float_blob.Get(), float4_pair) ||
        !make_pair("uint4", vs_uint4_blob.Get(), ps_uint4_blob.Get(), uint4_pair) ||
        !make_pair("uint1", vs_uint1_blob.Get(), ps_uint1_blob.Get(), uint1_pair)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=shader_create_failed\n", path);
        return 2;
    }

    ComPtr<ID3D11Texture2D> tex, staging;
    if (!small_texture(path, g.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, kIntSize,
                       D3D11_BIND_RENDER_TARGET, tex, staging)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=target_create_failed\n", path);
        return 2;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = g.device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) {
        ++g_failures;
        out("MODE=%s RESULT=FAIL reason=rtv_failed\n", path);
        return 2;
    }

    int items = 0;
    int failed = 0;
    auto check_center = [&](const char* item, Pair& pair, const int expected[4]) {
        g.context->OMSetRenderTargets(0, nullptr, nullptr);
        g.context->Flush();
        const SmallRead back = small_read(path, g.context.Get(), tex.Get(), staging.Get(),
                                          kIntSize, 4);
        ++items;
        if (!back.ok) {
            ++failed;
            ++g_failures;
            out("MODE=%s ITEM=%s RESULT=FAIL reason=map_read_failed\n", path, item);
            return;
        }
        const size_t off = static_cast<size_t>(kIntSize / 2) * back.pitch +
                           static_cast<size_t>(kIntSize / 2) * 4;
        const int got[4] = {back.bytes[off + 0], back.bytes[off + 1], back.bytes[off + 2],
                            back.bytes[off + 3]};
        auto sample = [&](int x, int* dst) {
            const size_t o = static_cast<size_t>(kIntSize / 2) * back.pitch +
                             static_cast<size_t>(x) * 4;
            dst[0] = back.bytes[o + 0];
            dst[1] = back.bytes[o + 1];
            dst[2] = back.bytes[o + 2];
            dst[3] = back.bytes[o + 3];
        };
        int s0[4], s64[4], s192[4], s255[4];
        sample(0, s0); sample(64, s64); sample(192, s192); sample(255, s255);
        out("MODE=%s ITEM=%s SAMPLES x0=%d,%d,%d,%d x64=%d,%d,%d,%d x128=%d,%d,%d,%d"
            " x192=%d,%d,%d,%d x255=%d,%d,%d,%d\n", path, item, s0[0], s0[1], s0[2], s0[3],
            s64[0], s64[1], s64[2], s64[3], got[0], got[1], got[2], got[3], s192[0],
            s192[1], s192[2], s192[3], s255[0], s255[1], s255[2], s255[3]);
        write_bmp(item, back, kIntSize);
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            if (abs(got[i] - expected[i]) > 1) ok = false;
        }
        out("MODE=%s ITEM=%s RESULT=%s got=%d,%d,%d,%d expected=%d,%d,%d,%d\n", path,
            item, ok ? "PASS" : "FAIL", got[0], got[1], got[2], got[3], expected[0],
            expected[1], expected[2], expected[3]);
        if (!ok) {
            ++failed;
            ++g_failures;
        }
        (void)pair;
    };
    auto dump_grid = [&](const char* item) {
        const SmallRead back = small_read(path, g.context.Get(), tex.Get(), staging.Get(),
                                          kIntSize, 4);
        if (!back.ok) return;
        out("MODE=%s ITEM=%s GRID_BEGIN\n", path, item);
        for (int gy = 0; gy < 16; ++gy) {
            char line[17];
            const int y = 8 + gy * 16;
            for (int gx = 0; gx < 16; ++gx) {
                const int x = 8 + gx * 16;
                const size_t o = static_cast<size_t>(y) * back.pitch +
                                 static_cast<size_t>(x) * 4;
                line[gx] = "0123456789abcdef"[static_cast<int>(back.bytes[o + 2]) >> 4];
            }
            line[16] = 0;
            out("MODE=%s ITEM=%s GRID y=%d %s\n", path, item, y, line);
        }
        out("MODE=%s ITEM=%s GRID_END\n", path, item);
    };

    auto draw_quad = [&](Pair& pair, ComPtr<ID3D11InputLayout>& layout,
                         ID3D11Buffer* vb0, UINT stride0, ID3D11Buffer* vb1, UINT stride1,
                         bool instanced) {
        if (!small_pipeline(path, g, kIntSize, pair.vs.Get())) return false;
        const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        g.context->ClearRenderTargetView(rtv.Get(), magenta);
        ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
        g.context->OMSetRenderTargets(1, rtvs, nullptr);
        g.context->IASetInputLayout(layout.Get());
        g.context->VSSetShader(pair.vs.Get(), nullptr, 0);
        g.context->PSSetShader(pair.ps.Get(), nullptr, 0);
        ID3D11Buffer* vbs[2] = {vb0, vb1};
        UINT strides[2] = {stride0, stride1};
        UINT offsets[2] = {0, 0};
        g.context->IASetVertexBuffers(0, vb1 ? 2 : 1, vbs, strides, offsets);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (instanced)
            g.context->DrawInstanced(6, 1, 0, 0);
        else
            g.context->Draw(6, 0);
        return true;
    };

    auto build_pair = [&](const char* name, const char* vs_src, const char* ps_src,
                          Pair& p) -> bool {
        ComPtr<ID3DBlob> vb, pb;
        const char* u = nullptr;
        HRESULT h = a8_compile(path, name, vs_src, "vs_5_0", "vs_5_0", vb.GetAddressOf(), &u);
        if (SUCCEEDED(h))
            h = a8_compile(path, name, ps_src, "ps_5_0", "ps_5_0", pb.GetAddressOf(), &u);
        if (SUCCEEDED(h)) {
            p.vs_blob = vb;
            h = g.device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(),
                                             nullptr, p.vs.GetAddressOf());
        }
        if (SUCCEEDED(h))
            h = g.device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(),
                                            nullptr, p.ps.GetAddressOf());
        out("MODE=%s stage=create_shader_pair name=%s hr=0x%08lx\n", path, name,
            static_cast<unsigned long>(h));
        if (SUCCEEDED(h)) reflect_signatures(path, name, vb.Get(), pb.Get());
        return SUCCEEDED(h);
    };

    // Case 0: clear only, no draw. If this is not magenta the clear itself is
    // not reaching the target and every later sample is suspect.
    {
        Pair p;
        if (!build_pair("clear_only", kIntVsPos, kIntPsConst, p)) {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=clear_only RESULT=FAIL reason=shader_failed\n", path);
        } else {
            small_pipeline(path, g, kIntSize, p.vs.Get());
            const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            g.context->ClearRenderTargetView(rtv.Get(), magenta);
            const int expected[4] = {255, 0, 255, 255};
            check_center("clear_only", p, expected);
            dump_grid("clear_only");
        }
    }

    // Case A: constant black on a magenta clear; if the centre stays magenta the
    // quad never reached it, if it is black the draw and write mask work.
    {
        Pair p;
        if (!build_pair("pos_only", kIntVsPos, kIntPsConst, p)) {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=pos_only_black RESULT=FAIL reason=shader_failed\n", path);
        } else {
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "POSITION"; e.Format = DXGI_FORMAT_R32G32_FLOAT;
            e.InputSlot = 0; e.AlignedByteOffset = 0;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(&e, 1, p.vs_blob->GetBufferPointer(),
                                                    p.vs_blob->GetBufferSize(),
                                                    layout.GetAddressOf());
            ComPtr<ID3D11Buffer> vb;
            if (SUCCEEDED(h) &&
                create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                              D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
                draw_quad(p, layout, vb.Get(), 8, nullptr, 0, false);
                const int expected[4] = {0, 0, 0, 255};
                check_center("pos_only_black", p, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=pos_only_black RESULT=FAIL reason=setup_failed\n", path);
            }
        }
    }

    // Case B: colour derived from POSITION only; proves the VS->PS varying path
    // with a single vertex element.
    {
        Pair p;
        if (!build_pair("pos_derived", kIntVsDerived, kIntPsFloat4, p)) {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=pos_derived RESULT=FAIL reason=shader_failed\n", path);
        } else {
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "POSITION"; e.Format = DXGI_FORMAT_R32G32_FLOAT;
            e.InputSlot = 0; e.AlignedByteOffset = 0;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(&e, 1, p.vs_blob->GetBufferPointer(),
                                                    p.vs_blob->GetBufferSize(),
                                                    layout.GetAddressOf());
            ComPtr<ID3D11Buffer> vb;
            if (SUCCEEDED(h) &&
                create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                              D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
                draw_quad(p, layout, vb.Get(), 8, nullptr, 0, false);
                const int expected[4] = {0, 0, 128, 255};
                check_center("pos_derived", p, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=pos_derived RESULT=FAIL reason=setup_failed\n", path);
            }
        }
    }

    // Case C: constant varying 0.5 -> 128; isolates the VS->PS link from any
    // attribute fetch beyond position.
    {
        Pair p;
        if (!build_pair("pos_const_varying", kIntVsConstVarying, kIntPsFloat4, p)) {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=pos_const_varying RESULT=FAIL reason=shader_failed\n", path);
        } else {
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "POSITION"; e.Format = DXGI_FORMAT_R32G32_FLOAT;
            e.InputSlot = 0; e.AlignedByteOffset = 0;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(&e, 1, p.vs_blob->GetBufferPointer(),
                                                    p.vs_blob->GetBufferSize(),
                                                    layout.GetAddressOf());
            ComPtr<ID3D11Buffer> vb;
            if (SUCCEEDED(h) &&
                create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                              D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
                draw_quad(p, layout, vb.Get(), 8, nullptr, 0, false);
                const int expected[4] = {0, 0, 128, 255};
                check_center("pos_const_varying", p, expected);
                dump_grid("pos_const_varying");
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=pos_const_varying RESULT=FAIL reason=setup_failed\n", path);
            }
        }
    }

    // Case D: standard TEXCOORD semantic, constant varying.
    {
        Pair p;
        if (!build_pair("texcoord_varying", kIntVsTexcoordVarying, kIntPsTexcoordVarying, p)) {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=texcoord_varying RESULT=FAIL reason=shader_failed\n", path);
        } else {
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "POSITION"; e.Format = DXGI_FORMAT_R32G32_FLOAT;
            e.InputSlot = 0; e.AlignedByteOffset = 0;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(&e, 1, p.vs_blob->GetBufferPointer(),
                                                    p.vs_blob->GetBufferSize(),
                                                    layout.GetAddressOf());
            ComPtr<ID3D11Buffer> vb;
            if (SUCCEEDED(h) &&
                create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                              D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
                draw_quad(p, layout, vb.Get(), 8, nullptr, 0, false);
                const int expected[4] = {0, 0, 128, 255};
                check_center("texcoord_varying", p, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=texcoord_varying RESULT=FAIL reason=setup_failed\n", path);
            }
        }
    }

    // Case E: standard COLOR semantic, constant varying.
    {
        Pair p;
        if (!build_pair("color_varying", kIntVsColorVarying, kIntPsColorVarying, p)) {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=color_varying RESULT=FAIL reason=shader_failed\n", path);
        } else {
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "POSITION"; e.Format = DXGI_FORMAT_R32G32_FLOAT;
            e.InputSlot = 0; e.AlignedByteOffset = 0;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(&e, 1, p.vs_blob->GetBufferPointer(),
                                                    p.vs_blob->GetBufferSize(),
                                                    layout.GetAddressOf());
            ComPtr<ID3D11Buffer> vb;
            if (SUCCEEDED(h) &&
                create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                              D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
                draw_quad(p, layout, vb.Get(), 8, nullptr, 0, false);
                const int expected[4] = {0, 0, 128, 255};
                check_center("color_varying", p, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=color_varying RESULT=FAIL reason=setup_failed\n", path);
            }
        }
    }

    // Case F: no varying at all, constant red; proves the write path.
    {
        Pair p;
        if (!build_pair("red_control", kIntVsPos, kIntPsRed, p)) {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=red_control RESULT=FAIL reason=shader_failed\n", path);
        } else {
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "POSITION"; e.Format = DXGI_FORMAT_R32G32_FLOAT;
            e.InputSlot = 0; e.AlignedByteOffset = 0;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(&e, 1, p.vs_blob->GetBufferPointer(),
                                                    p.vs_blob->GetBufferSize(),
                                                    layout.GetAddressOf());
            ComPtr<ID3D11Buffer> vb;
            if (SUCCEEDED(h) &&
                create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                              D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
                draw_quad(p, layout, vb.Get(), 8, nullptr, 0, false);
                const int expected[4] = {0, 0, 255, 255};
                check_center("red_control", p, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=red_control RESULT=FAIL reason=setup_failed\n", path);
            }
        }
    }

    // Case 1: float4 attribute control, both attributes in slot 0.
    {
        struct V { float pos[2]; float g[4]; };
        const float v = 99.0f / 255.0f, w = 42.0f / 255.0f, z = 7.0f / 255.0f;
        V data[6];
        for (int i = 0; i < 6; ++i) {
            data[i].pos[0] = kIntPos6[i * 2];
            data[i].pos[1] = kIntPos6[i * 2 + 1];
            data[i].g[0] = v; data[i].g[1] = w; data[i].g[2] = z; data[i].g[3] = 1.0f;
        }
        D3D11_INPUT_ELEMENT_DESC elems[2] = {};
        elems[0].SemanticName = "POSITION"; elems[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        elems[0].InputSlot = 0; elems[0].AlignedByteOffset = 0;
        elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems[1].SemanticName = "GLYPH"; elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        elems[1].InputSlot = 0; elems[1].AlignedByteOffset = 8;
        elems[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        ComPtr<ID3D11InputLayout> layout;
        HRESULT h = g.device->CreateInputLayout(elems, 2, float4_pair.vs_blob->GetBufferPointer(),
                                                float4_pair.vs_blob->GetBufferSize(),
                                                layout.GetAddressOf());
        out("MODE=%s stage=create_layout name=float4 hr=0x%08lx\n", path,
            static_cast<unsigned long>(h));
        ComPtr<ID3D11Buffer> vb;
        if (SUCCEEDED(h) && create_buffer(path, g.device.Get(), data, sizeof(data),
                                          D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
            draw_quad(float4_pair, layout, vb.Get(), sizeof(V), nullptr, 0, false);
            const int expected[4] = {7, 42, 99, 255};
            check_center("float4_control", float4_pair, expected);
            dump_grid("float4_control");
        } else {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=float4_control RESULT=FAIL reason=setup_failed\n", path);
        }
    }

    // Cases 2-4: uint4 attribute, one slot or two, per vertex or per instance.
    {
        struct VU { float pos[2]; unsigned int g[4]; };
        VU data[6];
        for (int i = 0; i < 6; ++i) {
            data[i].pos[0] = kIntPos6[i * 2];
            data[i].pos[1] = kIntPos6[i * 2 + 1];
            data[i].g[0] = 0x2A63u;  // low 99, high 42
            data[i].g[1] = 7u;
            data[i].g[2] = 3u;
            data[i].g[3] = 4u;
        }
        const int expected[4] = {7, 42, 99, 255};

        // 2: both attributes in slot 0, stride 24.
        {
            D3D11_INPUT_ELEMENT_DESC elems[2] = {};
            elems[0].SemanticName = "POSITION"; elems[0].Format = DXGI_FORMAT_R32G32_FLOAT;
            elems[0].InputSlot = 0; elems[0].AlignedByteOffset = 0;
            elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            elems[1].SemanticName = "GLYPH";
            elems[1].Format = DXGI_FORMAT_R32G32B32A32_UINT;
            elems[1].InputSlot = 0; elems[1].AlignedByteOffset = 8;
            elems[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(
                elems, 2, uint4_pair.vs_blob->GetBufferPointer(),
                uint4_pair.vs_blob->GetBufferSize(), layout.GetAddressOf());
            out("MODE=%s stage=create_layout name=uint4_one_slot hr=0x%08lx\n", path,
                static_cast<unsigned long>(h));
            ComPtr<ID3D11Buffer> vb;
            if (SUCCEEDED(h) && create_buffer(path, g.device.Get(), data, sizeof(data),
                                              D3D11_BIND_VERTEX_BUFFER,
                                              D3D11_USAGE_DEFAULT, vb)) {
                draw_quad(uint4_pair, layout, vb.Get(), sizeof(VU), nullptr, 0, false);
                check_center("uint4_per_vertex", uint4_pair, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=uint4_per_vertex RESULT=FAIL reason=setup_failed\n", path);
            }
        }

        // 3: two slots, both per vertex.
        {
            D3D11_INPUT_ELEMENT_DESC elems[2] = {};
            elems[0].SemanticName = "POSITION"; elems[0].Format = DXGI_FORMAT_R32G32_FLOAT;
            elems[0].InputSlot = 0; elems[0].AlignedByteOffset = 0;
            elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            elems[1].SemanticName = "GLYPH";
            elems[1].Format = DXGI_FORMAT_R32G32B32A32_UINT;
            elems[1].InputSlot = 1; elems[1].AlignedByteOffset = 0;
            elems[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(
                elems, 2, uint4_pair.vs_blob->GetBufferPointer(),
                uint4_pair.vs_blob->GetBufferSize(), layout.GetAddressOf());
            out("MODE=%s stage=create_layout name=uint4_two_slots hr=0x%08lx\n", path,
                static_cast<unsigned long>(h));
            ComPtr<ID3D11Buffer> pos, glyph;
            bool ok = SUCCEEDED(h) &&
                      create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                                    D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, pos);
            unsigned int glyphs[6 * 4];
            for (int i = 0; i < 6; ++i) {
                glyphs[i * 4 + 0] = 0x2A63u;
                glyphs[i * 4 + 1] = 7u;
                glyphs[i * 4 + 2] = 3u;
                glyphs[i * 4 + 3] = 4u;
            }
            ok = ok && create_buffer(path, g.device.Get(), glyphs, sizeof(glyphs),
                                     D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, glyph);
            if (ok) {
                draw_quad(uint4_pair, layout, pos.Get(), 8, glyph.Get(), 16, false);
                check_center("uint4_two_slots", uint4_pair, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=uint4_two_slots RESULT=FAIL reason=setup_failed\n", path);
            }
        }

        // 4: glyph data per instance, one instance.
        {
            D3D11_INPUT_ELEMENT_DESC elems[2] = {};
            elems[0].SemanticName = "POSITION"; elems[0].Format = DXGI_FORMAT_R32G32_FLOAT;
            elems[0].InputSlot = 0; elems[0].AlignedByteOffset = 0;
            elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            elems[1].SemanticName = "GLYPH";
            elems[1].Format = DXGI_FORMAT_R32G32B32A32_UINT;
            elems[1].InputSlot = 1; elems[1].AlignedByteOffset = 0;
            elems[1].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
            elems[1].InstanceDataStepRate = 1;
            ComPtr<ID3D11InputLayout> layout;
            HRESULT h = g.device->CreateInputLayout(
                elems, 2, uint4_pair.vs_blob->GetBufferPointer(),
                uint4_pair.vs_blob->GetBufferSize(), layout.GetAddressOf());
            out("MODE=%s stage=create_layout name=uint4_per_instance hr=0x%08lx\n", path,
                static_cast<unsigned long>(h));
            ComPtr<ID3D11Buffer> pos, glyph;
            const unsigned int gdata[4] = {0x2A63u, 7u, 3u, 4u};
            bool ok = SUCCEEDED(h) &&
                      create_buffer(path, g.device.Get(), kIntPos6, sizeof(kIntPos6),
                                    D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, pos);
            ok = ok && create_buffer(path, g.device.Get(), gdata, sizeof(gdata),
                                     D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, glyph);
            if (ok) {
                draw_quad(uint4_pair, layout, pos.Get(), 8, glyph.Get(), 16, true);
                check_center("uint4_per_instance", uint4_pair, expected);
            } else {
                ++failed; ++g_failures;
                out("MODE=%s ITEM=uint4_per_instance RESULT=FAIL reason=setup_failed\n", path);
            }
        }
    }

    // Case 5: R32_UINT attribute, 0x2A63 -> 99/42.
    {
        struct V16 { float pos[2]; unsigned int g; };
        V16 data[6];
        for (int i = 0; i < 6; ++i) {
            data[i].pos[0] = kIntPos6[i * 2];
            data[i].pos[1] = kIntPos6[i * 2 + 1];
            data[i].g = 0x2A63u;
            // no padding field
        }
        D3D11_INPUT_ELEMENT_DESC elems[2] = {};
        elems[0].SemanticName = "POSITION"; elems[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        elems[0].InputSlot = 0; elems[0].AlignedByteOffset = 0;
        elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems[1].SemanticName = "G16"; elems[1].Format = DXGI_FORMAT_R32_UINT;
        elems[1].InputSlot = 0; elems[1].AlignedByteOffset = 8;
        elems[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        ComPtr<ID3D11InputLayout> layout;
        HRESULT h = g.device->CreateInputLayout(
            elems, 2, uint1_pair.vs_blob->GetBufferPointer(),
            uint1_pair.vs_blob->GetBufferSize(), layout.GetAddressOf());
        out("MODE=%s stage=create_layout name=r32_uint hr=0x%08lx\n", path,
            static_cast<unsigned long>(h));
        ComPtr<ID3D11Buffer> vb;
        if (SUCCEEDED(h) && create_buffer(path, g.device.Get(), data, sizeof(data),
                                          D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vb)) {
            draw_quad(uint1_pair, layout, vb.Get(), sizeof(V16), nullptr, 0, false);
            const int expected[4] = {0, 42, 99, 255};
            check_center("r32_uint", uint1_pair, expected);
        } else {
            ++failed; ++g_failures;
            out("MODE=%s ITEM=r32_uint RESULT=FAIL reason=setup_failed\n", path);
        }
    }

    out("MODE=%s SUMMARY items=%d failed=%d\n", path, items, failed);
    return failed ? 2 : 0;
}

int run_mode(const std::string& mode, bool warp, bool debug_layer) {
    if (mode == "a8-upload") return run_a8_upload(warp, debug_layer);
    if (mode == "rt-to-srv") return run_rt_to_srv(warp, debug_layer);
    if (mode == "d2d-aa-modes") return run_d2d_aa_modes(warp, debug_layer);
    if (mode == "text-paths") return run_text_paths(warp, debug_layer);
    if (mode == "clip-contract") return run_clip_contract(warp, debug_layer);
    if (mode == "mask-uint") return run_mask_uint(warp, debug_layer);
    if (mode == "dualsrc-blend") return run_dualsrc_blend(warp, debug_layer);
    if (mode == "vb-paths") return run_vb_paths(warp, debug_layer);
    if (mode == "mask-write-srv") return run_mask_write_srv(warp, debug_layer);
    if (mode == "uint-rt-matrix") return run_uint_rt_matrix(warp, debug_layer);
    if (mode == "uint-order") return run_uint_order(warp, debug_layer);
    if (mode == "uint-rt-extreme") return run_uint_rt_extreme(warp, debug_layer);
    if (mode == "typed-buffer-srv") return run_typed_buffer_srv(warp, debug_layer);
    if (mode == "upload-paths") return run_upload_paths(warp, debug_layer);
    if (mode == "mixed-fetch") return run_mixed_fetch(warp, debug_layer);
    if (mode == "int-attribs") return run_int_attribs(warp, debug_layer);
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
        "  --mode a8-upload  UpdateSubresource {0,64,128,255} into 4x4 R8_UNORM and\n"
        "                    A8_UNORM, read the bytes back, then sample the same\n"
        "                    texture twice through the default SRV: PS replicates\n"
        "                    the channel into RGB, then PS forces XYZ to 0 and puts\n"
        "                    it in alpha (the (ZERO,ZERO,ZERO,R) shape; D3D11 SRVs\n"
        "                    carry no component mapping); unsupported format is SKIP\n"
        "  --mode rt-to-srv  one R8_UNORM as render target and SRV in one frame:\n"
        "                    PS 0.5 -> 128 in R8 -> sampled -> 128,128,128 in BGRA\n"
        "  --mode d2d-aa-modes  six D2D primitives, each on its own target: AA and\n"
        "                    aliased 1px stroke, AA triangle, AA ellipse, AA and\n"
        "                    aliased 1px half-pixel line\n"
        "  --mode text-paths  six glyph paths, each on its own target: solid and\n"
        "                    bitmap brush, aliased text, direct DrawGlyphRun, and a\n"
        "                    GRAYSCALE draw into ALPHA_MODE_IGNORE and PREMULTIPLIED\n"
        "  --mode clip-contract  clip(x-0.5) and discard with x=0.75/0.25 from a\n"
        "                    cbuffer, plus a ps_4_0 clip pair: the keep items must\n"
        "                    paint and the drop items must leave the target empty\n"
        "  --mode mask-uint  R32_UINT 256x256 mask: PS write and typed UAV store of\n"
        "                    0x00010001 read back exactly, then countbits/16 through\n"
        "                    a Texture2D<uint> Load and through eight uint SRVs\n"
        "                    selected from a cbuffer (expected about 32/255)\n"
        "  --mode dualsrc-blend  PS writes SV_Target0 opaque black and SV_Target1\n"
        "                    alpha 0.5; SRC1_ALPHA/INV_SRC1_ALPHA must give grey 128,\n"
        "                    SRC1_COLOR/INV_SRC1_COLOR white, SRC_ALPHA black\n"
        "  --mode vb-paths   explicit vertex/index buffers, which no earlier mode\n"
        "                    used: static and dynamic vertex buffers, R16 and R32\n"
        "                    index buffers, two indexed ranges in one frame, and an\n"
        "                    instanced indexed draw; each case needs ink in the\n"
        "                    expected half of a 256x256 target\n"
        "  --mode mask-write-srv  R32_UINT mask PS write and Clear read back through\n"
        "                    an SRV countbits shader and through a direct map, on a\n"
        "                    texture with and without the UAV bind (RT|SRV vs\n"
        "                    RT|SRV|UAV); separates a lost PS write from a lost\n"
        "                    integer readback and from a dual-usage effect\n"
        "  --mode uint-order  R32_UINT: UAV store then PS draw, then PS draw then\n"
        "                    UAV store; the winning value separates a dropped draw\n"
        "                    from a clear that runs after the draw\n"
        "  --mode uint-rt-matrix  PS writes one known value into R32_UINT, R16_UINT,\n"
        "                    R8_UINT, RGBA32_UINT, R32_FLOAT and BGRA8 targets and\n"
        "                    the raw map bytes are compared, to separate an integer\n"
        "                    target failure from a rasterisation or format failure\n"
        "  --mode uint-rt-extreme  R32_UINT writes of 0x7FC00001 and 0xDEADBEEF and\n"
        "                    an RGBA32_UINT write of 0x80000001/0xFFFFFFFF/\n"
        "                    0x7FFFFFFF/0xDEADBEEF: none of those survive a float\n"
        "                    round trip, so a bit-exact readback proves the output\n"
        "                    really is integer; all zero confirms the write is\n"
        "                    dropped. An R32_FLOAT control writes the 0x7FC00001\n"
        "                    NaN bit pattern and reads the raw bytes back; WARP\n"
        "                    must be bit exact\n"
        "  --mode typed-buffer-srv  typed texel-buffer SRV fetch, the path the failing\n"
        "                    D2D shader uses: Buffer<uint>, StructuredBuffer<uint>,\n"
        "                    ByteAddressBuffer.Load and a Texture2D<uint>.Load\n"
        "                    control, all reading 0x00010001/0xDEADBEEF/\n"
        "                    0x7FC00001/0x12345678 plus 1..64 and storing asfloat\n"
        "                    into R32_FLOAT; a Buffer<float> control separates a\n"
        "                    uint problem from a buffer-view problem. Each case has\n"
        "                    its own buffer, SRV, draw and readback; the SRV desc\n"
        "                    and the staged initial-data check are printed, and\n"
        "                    WARP must be bit exact\n"
        "  --mode upload-paths  the upload half of the text path: four independent\n"
        "                    writes of a known 32x32 block into 64x64 resources,\n"
        "                    each followed by a fetch and an exact readback.\n"
        "                    texture_update_box clears a DEFAULT R8_UNORM target\n"
        "                    and UpdateSubresource a box at (4,4);\n"
        "                    texture_copy_box clears it and CopySubresourceRegion\n"
        "                    a 32x32 staging texture to (4,4); dynamic_buffer_map\n"
        "                    Map(WRITE_DISCARD)s 64 R32_UINT words and fetches them\n"
        "                    through a typed SRV into R32_FLOAT; dynamic_texture_map\n"
        "                    Map(WRITE_DISCARD)s the block into a DYNAMIC R8_UNORM\n"
        "                    texture and judges only the written block. Every item\n"
        "                    prints the stage/hr trail and expected/actual_min/\n"
        "                    actual_max/mismatches/all_zero; the texture cases also\n"
        "                    require the outside of the box to stay clear-zero\n"
        "  --mode mixed-fetch  one draw binds a Buffer<uint> at t0 and a\n"
        "                    Texture2D<float4> at t1, as the D2D text shader does\n"
        "                    (five buffer fetches, then a LUT fetch); buffer_only\n"
        "                    and texture_only are the controls, every case is\n"
        "                    judged by an exact centre-pixel readback\n"
        "  --mode int-attribs  integer vertex attributes forwarded as flat inputs,\n"
        "                    as the D2D text shader does: float4 control, uint4 in\n"
        "                    one slot, uint4 across two slots, uint4 per instance,\n"
        "                    and R16_UINT, each judged by the exact centre pixel\n"
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
            } else if (value == L"a8-upload") {
                modes.push_back("a8-upload");
            } else if (value == L"rt-to-srv") {
                modes.push_back("rt-to-srv");
            } else if (value == L"d2d-aa-modes") {
                modes.push_back("d2d-aa-modes");
            } else if (value == L"text-paths") {
                modes.push_back("text-paths");
            } else if (value == L"clip-contract") {
                modes.push_back("clip-contract");
            } else if (value == L"mask-uint") {
                modes.push_back("mask-uint");
            } else if (value == L"dualsrc-blend") {
                modes.push_back("dualsrc-blend");
            } else if (value == L"vb-paths") {
                modes.push_back("vb-paths");
            } else if (value == L"mask-write-srv") {
                modes.push_back("mask-write-srv");
            } else if (value == L"uint-rt-matrix") {
                modes.push_back("uint-rt-matrix");
            } else if (value == L"uint-order") {
                modes.push_back("uint-order");
            } else if (value == L"uint-rt-extreme") {
                modes.push_back("uint-rt-extreme");
            } else if (value == L"typed-buffer-srv") {
                modes.push_back("typed-buffer-srv");
            } else if (value == L"upload-paths") {
                modes.push_back("upload-paths");
            } else if (value == L"mixed-fetch") {
                modes.push_back("mixed-fetch");
            } else if (value == L"int-attribs") {
                modes.push_back("int-attribs");
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
