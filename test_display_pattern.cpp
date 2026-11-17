// Independent GDI / hardware D2D+DirectWrite visual patterns. No capture/readback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <io.h>
#include <fcntl.h>
#include <cwchar>
#include <string>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
constexpr int W = 960, H = 560;
FILE* output = nullptr;
void log(const char* fmt, ...) {
    if (!output) return;
    va_list a; va_start(a, fmt); int n = vfprintf(output, fmt, a); va_end(a);
    if (n < 0 || fflush(output)) throw std::runtime_error("log write");
}
void check(bool ok, const char* api, DWORD error = 0xffffffffu) {
    if (!ok) {
        if (error == 0xffffffffu) error = GetLastError();
        log("FAIL %s win32=%lu (0=no_extended_error)\n", api, error);
        throw std::runtime_error(api);
    }
}
void hr(HRESULT value, const char* api) {
    log("%s hr=0x%08lx\n", api, static_cast<unsigned long>(value));
    if (FAILED(value)) throw std::runtime_error(api);
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    check(n > 0, "UTF8 size");
    std::string s(n, 0);
    check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), s.data(), n, nullptr, nullptr) == n, "UTF8 data");
    return s;
}
struct Options { std::wstring mode, out, capture; UINT duration = 20; bool foreground = false; };
bool absolute_argument(const std::wstring& s) {
    return s.size() > 3 && s[1] == L':' && s[2] == L'\\' &&
           s.back() != L'\\' && s.find_first_of(L"\"\r\n") == std::wstring::npos;
}
bool parse(int count, wchar_t** args, Options& o) {
    bool duration_seen = false;
    for (int i = 1; i < count; ++i) {
        std::wstring a = args[i];
        if (a == L"--mode" && i + 1 < count && o.mode.empty()) o.mode = args[++i];
        else if (a == L"--self-test" && o.mode.empty()) o.mode = L"self-test";
        else if (a == L"--out" && i + 1 < count && o.out.empty()) o.out = args[++i];
        else if (a == L"--foreground" && !o.foreground) o.foreground = true;
        else if (a == L"--capture-exe" && i + 1 < count && o.capture.empty()) o.capture = args[++i];
        else if (a == L"--duration" && i + 1 < count && !duration_seen) {
            duration_seen = true;
            std::wstring s = args[++i];
            if (s.empty() || s.size() > 2) return false;
            UINT n = 0;
            for (wchar_t c : s) { if (c < L'0' || c > L'9') return false; n = n * 10 + c - L'0'; }
            if (n < 1 || n > 30) return false;
            o.duration = n;
        } else return false;
    }
    return !o.out.empty() && (o.mode == L"gdi" || o.mode == L"dwrite" || o.mode == L"self-test") &&
        (o.mode != L"self-test" || (!o.foreground && o.capture.empty())) &&
        (o.foreground == !o.capture.empty()) &&
        (o.capture.empty() || (o.duration == 20 && absolute_argument(o.capture) && absolute_argument(o.out)));
}
bool observation_gate(bool identity, bool painted, bool foreground, bool visible, bool iconic,
                      bool uncloaked, bool unobstructed, bool same_rect, ULONGLONG now,
                      ULONGLONG deadline, ULONGLONG ready) {
    return identity && painted && foreground && visible && !iconic && uncloaked &&
        unobstructed && same_rect && now < deadline && (!ready || (now >= ready && now - ready < 5000));
}
bool observation_can_start(bool activated, bool painted, bool started) { return activated && painted && !started; }
bool completed(bool foreground, bool painted, bool started, bool captured, bool failed) {
    return !failed && painted && (!foreground || (started && captured));
}
ULONGLONG launch_tick(ULONGLONG created, ULONGLONG utc, ULONGLONG tick) {
    ULONGLONG elapsed = utc >= created ? (utc - created + 9999) / 10000 : 30000;
    return tick >= elapsed ? tick - elapsed : 0;
}
ULONGLONG process_created(HANDLE process = GetCurrentProcess()) {
    FILETIME created{}, exited{}, kernel{}, user{};
    check(GetProcessTimes(process, &created, &exited, &kernel, &user) != 0, "GetProcessTimes(owned process)");
    return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
ULONGLONG utc_filetime() {
    FILETIME t{}; GetSystemTimeAsFileTime(&t);
    return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
struct Patch { const char* name; BYTE r, g, b; };
constexpr Patch patches[] = {
    {"red",255,0,0}, {"green",0,255,0}, {"blue",0,0,255}, {"cyan",0,255,255},
    {"magenta",255,0,255}, {"yellow",255,255,0}, {"black",0,0,0}, {"gray64",64,64,64},
    {"gray128",128,128,128}, {"gray192",192,192,192}, {"white",255,255,255}, {"orange",255,128,0}
};
RECT patch_rect(size_t i) {
    const LONG col = static_cast<LONG>(i % 6), row = static_cast<LONG>(i / 6);
    return {24 + col * 152, 24 + row * 96, 160 + col * 152, 104 + row * 96};
}
constexpr RECT markers[] = {{0,0,12,12}, {948,0,960,12}, {0,548,12,560}, {948,548,960,560}};
constexpr wchar_t english[] = L"ABC xyz 0123456789 !? RGB";
constexpr wchar_t chinese[] = L"\u4e2d\u6587\u6d4b\u8bd5 \u663e\u793a\u989c\u8272 0123456789 ABC";
struct App {
    Options o;
    HWND window = nullptr;
    int result = 0;
    bool painted = false;
    ULONGLONG start = GetTickCount64(), created = 0, ready = 0, capture_created = 0;
    HWND created_window = nullptr;
    RECT initial_rect{};
    HWINEVENTHOOK foreground_hook = nullptr;
    bool foreground_changed = false, capture_done = false, activation_checked = false, observation_started = false;
    PROCESS_INFORMATION capture_process{};
    UINT dpi = 96;
    ~App() {
        if (foreground_hook) UnhookWinEvent(foreground_hook);
        if (capture_process.hThread) CloseHandle(capture_process.hThread);
        if (capture_process.hProcess) CloseHandle(capture_process.hProcess);
    }
    std::wstring en_family, zh_family;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> en_format, zh_format;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
};
bool snapshot(App& a, const char* phase, bool verbose) {
    DWORD pid = 0, foreground_pid = 0, cloak = 0;
    HWND fg = GetForegroundWindow();
    bool identity = IsWindow(a.window) && a.window == a.created_window &&
        GetWindowThreadProcessId(a.window, &pid) != 0 && pid == GetCurrentProcessId() && process_created() == a.created;
    GetWindowThreadProcessId(fg, &foreground_pid);
    RECT rect{}, client{}; POINT origin{};
    bool geometry = GetWindowRect(a.window, &rect) && GetClientRect(a.window, &client) && ClientToScreen(a.window, &origin);
    bool same_rect = geometry && EqualRect(&rect, &a.initial_rect) && client.right == W && client.bottom == H &&
        origin.x == rect.left && origin.y == rect.top;
    HRESULT cloak_hr = DwmGetWindowAttribute(a.window, DWMWA_CLOAKED, &cloak, sizeof(cloak));
    bool clear = true; unsigned scanned = 0, intersections = 0;
    HWND above = GetWindow(a.window, GW_HWNDPREV);
    // ponytail: conservative rectangle test, not an alpha/region occlusion solver; unknown => unavailable.
    for (; above && scanned < 64 && intersections < 16; above = GetWindow(above, GW_HWNDPREV)) {
        ++scanned;
        if (!IsWindowVisible(above) || IsIconic(above)) continue;
        RECT r{}, overlap{}; DWORD c = 0, p = 0;
        HRESULT h = DwmGetWindowAttribute(above, DWMWA_CLOAKED, &c, sizeof(c));
        if (FAILED(h) || !GetWindowRect(above, &r)) { clear = false; break; }
        if (c || !IntersectRect(&overlap, &rect, &r)) continue;
        GetWindowThreadProcessId(above, &p); ++intersections; clear = false;
        if (verbose) log("above hwnd=%p pid=%lu z_scan=%u rect=%ld,%ld,%ld,%ld cloak=%lu\n",
            static_cast<void*>(above), p, scanned, r.left, r.top, r.right, r.bottom, c);
    }
    if (above) clear = false; // bounded scan was incomplete
    bool visible = IsWindowVisible(a.window) != 0, iconic = IsIconic(a.window) != 0;
    bool foreground = fg == a.window && GetForegroundWindow() == a.window && !a.foreground_changed;
    const ULONGLONG now = GetTickCount64(); // end-of-snapshot clock, never reuse entry time for TTL
    bool ok = observation_gate(identity, a.painted, foreground,
        visible, iconic, SUCCEEDED(cloak_hr) && !cloak, clear, same_rect,
        now, a.start + a.o.duration * 1000, a.ready);
    if (verbose || !ok) log("state phase=%s utc_filetime=%llu tick_ms=%llu elapsed_ms=%llu self_pid=%lu created_filetime=%llu created_hwnd=%p hwnd=%p foreground_hwnd=%p foreground_pid=%lu visible=%d iconic=%d cloak_hr=0x%08lx cloaked=%lu rect=%ld,%ld,%ld,%ld same_rect=%d scanned=%u intersections=%u scan_complete=%d foreground_changed=%d gate=%s pixels=NOT_VERIFIED\n",
        phase, utc_filetime(), now, now-a.start, GetCurrentProcessId(), a.created,
        static_cast<void*>(a.created_window), static_cast<void*>(a.window), static_cast<void*>(fg), foreground_pid,
        visible, iconic, static_cast<unsigned long>(cloak_hr), cloak, rect.left, rect.top, rect.right, rect.bottom,
        same_rect, scanned, intersections, above == nullptr, a.foreground_changed, ok ? "eligible" : "observation_unavailable");
    const ULONGLONG end = GetTickCount64(); // include logging cost in final freshness gate
    return ok && observation_gate(true,true,true,true,false,true,true,true,
        end, a.start + a.o.duration * 1000, a.ready);
}
void begin_observation(App& a) {
    if (!observation_can_start(a.activation_checked, a.painted, a.observation_started)) return;
    a.observation_started = true; // latch before any call that might dispatch/reenter
    check(snapshot(a, "after_first_paint", true), "observation_unavailable: initial state", ERROR_INVALID_STATE);
    a.ready = GetTickCount64();
    log("READY=metadata_eligible utc_filetime=%llu tick_ms=%llu expires_tick_ms=%llu soft_deadline_tick_ms=%llu pixels=NOT_VERIFIED\n",
        utc_filetime(), a.ready, a.ready+5000, a.start+a.o.duration*1000);
    check(snapshot(a, "pre_capture", true), "observation_unavailable: ready expired/state changed", ERROR_INVALID_STATE);
    std::wstring command = L"\"" + a.o.capture + L"\" --capture --out \"" + a.o.out + L"\\capture\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    check(CreateProcessW(a.o.capture.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &a.capture_process) != 0, "CreateProcess(external capture)");
    a.capture_created = process_created(a.capture_process.hProcess);
    log("owned_capture pid=%lu created_filetime=%llu utc_filetime=%llu tick_ms=%llu\n",a.capture_process.dwProcessId,a.capture_created,utc_filetime(),GetTickCount64());
    a.capture_created = process_created(a.capture_process.hProcess);
    log("external_capture_owned pid=%lu created_filetime=%llu utc_filetime=%llu tick_ms=%llu\n", a.capture_process.dwProcessId, a.capture_created, utc_filetime(), GetTickCount64());
    check(snapshot(a, "capture_resume", false), "observation_unavailable: pre-resume state", ERROR_INVALID_STATE);
    check(ResumeThread(a.capture_process.hThread) != static_cast<DWORD>(-1), "ResumeThread(capture)");
}
std::wstring choose_font(IDWriteFactory* f, bool cjk) {
    ComPtr<IDWriteFontCollection> collection;
    hr(f->GetSystemFontCollection(&collection), "DWrite GetSystemFontCollection");
    const wchar_t* en[] = {L"Segoe UI", L"Arial"};
    const wchar_t* zh[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Noto Sans CJK SC", L"SimSun"};
    const wchar_t** names = cjk ? zh : en;
    const size_t count = cjk ? 4 : 2;
    for (size_t i = 0; i < count; ++i) {
        UINT32 index = 0; BOOL exists = FALSE;
        hr(collection->FindFamilyName(names[i], &index, &exists), "DWrite FindFamilyName");
        if (!exists) continue;
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFace> face;
        hr(collection->GetFontFamily(index, &family), "DWrite GetFontFamily");
        hr(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, &font), "DWrite GetFirstMatchingFont");
        hr(font->CreateFontFace(&face), "DWrite CreateFontFace");
        const UINT32 codepoints[] = {cjk ? 0x4e2du : 0x41u, cjk ? 0x6587u : 0x30u};
        UINT16 glyphs[2]{};
        hr(face->GetGlyphIndices(codepoints, 2, glyphs), "DWrite GetGlyphIndices");
        log("font_candidate family=%s face_index=%u simulations=%u glyph_probe=%u,%u\n",
            utf8(names[i]).c_str(), face->GetIndex(), face->GetSimulations(), glyphs[0], glyphs[1]);
        if (glyphs[0] && glyphs[1]) return names[i];
    }
    throw std::runtime_error(cjk ? "no installed CJK candidate with Chinese glyphs" : "no installed English candidate");
}
void fonts(App& a) {
    hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                          reinterpret_cast<IUnknown**>(a.dw.GetAddressOf())), "DWriteCreateFactory(font selection)");
    a.en_family = choose_font(a.dw.Get(), false);
    a.zh_family = choose_font(a.dw.Get(), true);
    log("font_selection english=%s cjk=%s size_physical_px=32 weight=normal; base_face_only,fallback_not_traced; missing_text_root_cause_not_established\n",
        utf8(a.en_family).c_str(), utf8(a.zh_family).c_str());
}
void metadata(const App& a) {
    log("mode=%s API=%s expected_client_physical=%dx%d duration_seconds=%u\n", utf8(a.o.mode).c_str(),
        a.o.mode == L"gdi" ? "GDI FillRect+TextOutW" : "D2D HARDWARE HwndRenderTarget+DirectWrite DrawText", W, H, a.o.duration);
    log("CPU_reference color_space=unorm8_RGB alpha=255 background=(32,32,32); rects=[left,top,right,bottom)\n");
    for (size_t i = 0; i < 12; ++i) {
        RECT r = patch_rect(i); const Patch& p = patches[i];
        log("patch=%s rect=%ld,%ld,%ld,%ld expected_RGB=%u,%u,%u expected_BGRA_bytes=%u,%u,%u,255\n",
            p.name,r.left,r.top,r.right,r.bottom,p.r,p.g,p.b,p.b,p.g,p.r);
    }
    for (const RECT& r : markers)
        log("position_mark rect=%ld,%ld,%ld,%ld expected_RGB=255,255,255\n",r.left,r.top,r.right,r.bottom);
    log("text English rect=24,244,936,302 white_RGB=255,255,255 expected_UTF8=%s\n",utf8(english).c_str());
    log("text Chinese rect=24,324,936,382 yellow_RGB=255,255,0 expected_UTF8=%s\n",utf8(chinese).c_str());
    log("text BlackOnWhite rect=24,424,936,496 expected_background_RGB=255,255,255 expected_text_RGB=0,0,0 expected_UTF8=%s\n",utf8(chinese).c_str());
    log("window_title_metadata=Display pattern - %s - physical 960x560; WS_POPUP borderless, no visible title; reference layout unscaled\n",utf8(a.o.mode).c_str());
    log("No in-process capture/readback or software fallback. External capture child only when explicitly requested.\n");
    if (!a.o.capture.empty()) log("capture_exe=%s required_SHA256=85577ab4c5006f11b8a5fa4417bf8741d98047fc539806fd4693cdd4d6106c52 (runner must verify before launch)\n",utf8(a.o.capture).c_str());
    log("duration_is_SOFT_WM_TIMER; acceptance_runner_requires_30s_HARD_cutoff_of_recorded_owned_PID_for_default20; hard_timeout=FAIL\n");
}
void fill(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color); check(b != nullptr,"CreateSolidBrush");
    int ok = FillRect(dc,&r,b); DWORD error = GetLastError();
    BOOL deleted = DeleteObject(b); check(deleted != 0,"DeleteObject(brush)");
    check(ok != 0,"FillRect",error);
}
void gdi_text(App& a, HDC dc, bool cjk, int y, COLORREF color, const wchar_t* text) {
    const std::wstring& family = cjk ? a.zh_family : a.en_family;
    HFONT font = CreateFontW(-32,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,family.c_str());
    check(font != nullptr,"CreateFontW");
    HGDIOBJ old = SelectObject(dc,font);
    if (!old || old == HGDI_ERROR) { DeleteObject(font); check(false,"SelectObject(font)"); }
    try {
        wchar_t actual[128]{};
        check(GetTextFaceW(dc,128,actual)>0,"GetTextFaceW");
        const int length = static_cast<int>(wcslen(text));
        std::vector<WORD> glyphs(length);
        check(GetGlyphIndicesW(dc,text,length,glyphs.data(),GGI_MARK_NONEXISTING_GLYPHS)!=GDI_ERROR,"GDI GetGlyphIndicesW");
        size_t missing = 0; for (WORD g : glyphs) if (g == 0xffff) ++missing;
        if (!a.painted) log("GDI actual_mapped_base_face=%s requested=%s glyph_missing_in_base=%zu text_y=%d (not complete fallback trace)\n",utf8(actual).c_str(),utf8(family).c_str(),missing,y);
        check(SetBkMode(dc,TRANSPARENT)!=0,"SetBkMode");
        check(SetTextColor(dc,color)!=CLR_INVALID,"SetTextColor");
        check(TextOutW(dc,24,y,text,length)!=0,"TextOutW");
    } catch (...) { SelectObject(dc,old); DeleteObject(font); throw; }
    check(SelectObject(dc,old)!=nullptr,"SelectObject(restore font)");
    check(DeleteObject(font)!=0,"DeleteObject(font)");
}
void paint_gdi(App& a,HDC dc) {
    fill(dc,{0,0,W,H},RGB(32,32,32));
    for (size_t i=0;i<12;++i) fill(dc,patch_rect(i),RGB(patches[i].r,patches[i].g,patches[i].b));
    for (const RECT& r : markers) fill(dc,r,RGB(255,255,255));
    fill(dc,{24,424,936,496},RGB(255,255,255));
    gdi_text(a,dc,false,244,RGB(255,255,255),english);
    gdi_text(a,dc,true,324,RGB(255,255,0),chinese);
    gdi_text(a,dc,true,432,RGB(0,0,0),chinese);
    check(GdiFlush()!=0,"GdiFlush(pattern only,no readback)");
}
D2D1_RECT_F rectf(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left),static_cast<float>(r.top),static_cast<float>(r.right),static_cast<float>(r.bottom));
}
void setup_d2d(App& a) {
    // HARDWARE is mandatory. No DEFAULT target, WARP, reference device or retry.
    hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.d2d.GetAddressOf()),"D2D1CreateFactory");
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
    hr(a.d2d->CreateHwndRenderTarget(props,D2D1::HwndRenderTargetProperties(a.window,D2D1::SizeU(W,H)),&a.target),"D2D CreateHwndRenderTarget(HARDWARE)");
    a.target->SetDpi(96,96); // one DIP = one physical client pixel, despite window DPI
    log("D2D requested_target_type=HARDWARE creation=OK target_dpi=96 DIP=physical_px BGRA8 alpha_ignore; actual_adapter_not_exposed_by_HwndRenderTarget\n");
    ComPtr<IDXGIFactory1> dxgi;
    hr(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)),"CreateDXGIFactory1(adapter inventory only)");
    for (UINT i=0;i<8;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT value=dxgi->EnumAdapters1(i,&adapter);
        if (value==DXGI_ERROR_NOT_FOUND) break;
        hr(value,"EnumAdapters1");
        DXGI_ADAPTER_DESC1 desc{}; hr(adapter->GetDesc1(&desc),"DXGI GetDesc1");
        log("adapter_inventory index=%u name=%s vendor=0x%x device=0x%x flags=0x%x software=%d (inventory,not proven D2D binding)\n",i,utf8(desc.Description).c_str(),desc.VendorId,desc.DeviceId,desc.Flags,(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)?1:0);
    }
    hr(a.target->CreateSolidColorBrush(D2D1::ColorF(1,1,1),&a.brush),"D2D CreateSolidColorBrush");
    hr(a.dw->CreateTextFormat(a.en_family.c_str(),nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
       DWRITE_FONT_STRETCH_NORMAL,32,L"en-US",&a.en_format),"DWrite CreateTextFormat(English)");
    hr(a.dw->CreateTextFormat(a.zh_family.c_str(),nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
       DWRITE_FONT_STRETCH_NORMAL,32,L"zh-CN",&a.zh_format),"DWrite CreateTextFormat(CJK)");
    hr(a.en_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP),"DWrite no wrap English");
    hr(a.zh_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP),"DWrite no wrap CJK");
}
void paint_d2d(App& a) {
    a.target->BeginDraw();
    a.target->Clear(D2D1::ColorF(32/255.f,32/255.f,32/255.f));
    for (size_t i=0;i<12;++i) {
        const auto& p=patches[i]; a.brush->SetColor(D2D1::ColorF(p.r/255.f,p.g/255.f,p.b/255.f));
        a.target->FillRectangle(rectf(patch_rect(i)),a.brush.Get());
    }
    a.brush->SetColor(D2D1::ColorF(1,1,1));
    for (const RECT& r:markers) a.target->FillRectangle(rectf(r),a.brush.Get());
    a.target->FillRectangle(D2D1::RectF(24,424,936,496),a.brush.Get());
    a.target->DrawText(english,static_cast<UINT32>(wcslen(english)),a.en_format.Get(),D2D1::RectF(24,244,936,302),a.brush.Get());
    a.brush->SetColor(D2D1::ColorF(1,1,0));
    a.target->DrawText(chinese,static_cast<UINT32>(wcslen(chinese)),a.zh_format.Get(),D2D1::RectF(24,324,936,382),a.brush.Get());
    a.brush->SetColor(D2D1::ColorF(0,0,0));
    a.target->DrawText(chinese,static_cast<UINT32>(wcslen(chinese)),a.zh_format.Get(),D2D1::RectF(24,432,936,496),a.brush.Get());
    hr(a.target->EndDraw(),"D2D EndDraw (includes deferred DrawText failures; no retry/fallback)");
}
LRESULT CALLBACK window_proc(HWND window,UINT msg,WPARAM w,LPARAM l) {
    App* a=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        a=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetLastError(0); LONG_PTR previous=SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));
        if(!previous && GetLastError()) return FALSE;
    }
    if(!a) return DefWindowProcW(window,msg,w,l);
    try {
        if(msg==WM_PAINT) {
            PAINTSTRUCT ps{}; HDC dc=BeginPaint(window,&ps); check(dc!=nullptr,"BeginPaint");
            try { if(a->o.mode==L"gdi") paint_gdi(*a,dc); else paint_d2d(*a); }
            catch(...) { EndPaint(window,&ps); throw; }
            check(EndPaint(window,&ps)!=0,"EndPaint");
            if(!a->painted) log("FIRST_PAINT=API_OK elapsed_ms=%llu (not proof of correct pixels/text)\n",GetTickCount64()-a->start);
            bool first = !a->painted; a->painted=true;
            if (first && a->o.foreground) begin_observation(*a);
            return 0;
        }
        if(msg==WM_TIMER && w==2 && a->ready) {
            if (!a->capture_done) {
                check(snapshot(*a, "monitor", false), "observation_unavailable: state/ready changed", ERROR_INVALID_STATE);
                if (a->capture_process.hProcess && WaitForSingleObject(a->capture_process.hProcess, 0) == WAIT_OBJECT_0) {
                    DWORD code = 2;
                    check(GetExitCodeProcess(a->capture_process.hProcess, &code) != 0, "GetExitCodeProcess(capture)");
                    check(snapshot(*a, "post_capture", true) && code == 0,
                        "observation_unavailable: capture failed/state changed", ERROR_INVALID_STATE);
                    a->capture_done = true;
                    log("OBSERVATION=CAPTURE_COMPLETED capture_exit=%lu utc_filetime=%llu tick_ms=%llu pixels=NOT_VERIFIED\n", code, utc_filetime(), GetTickCount64());
                }
            }
            return 0;
        }
        if(msg==WM_TIMER && w==1) {
            log("duration_timer elapsed_ms=%llu\n",GetTickCount64()-a->start);
            check(!a->o.foreground || (a->ready && (a->o.capture.empty() || a->capture_done)),
                "observation_unavailable: no completed observation", ERROR_TIMEOUT);
            check(DestroyWindow(window)!=0,"DestroyWindow(owned)"); return 0;
        }
        if(msg==WM_CLOSE) { check(DestroyWindow(window)!=0,"DestroyWindow(owned close)"); return 0; }
        if(msg==WM_DESTROY) { a->window=nullptr; PostQuitMessage(a->result); return 0; }
    } catch(const std::exception& e) {
        a->result=2;
        if(output) { fprintf(output,"FAIL window exception=%s observation_unavailable=%d\n",e.what(),a->o.foreground?1:0); fflush(output); }
        if(!DestroyWindow(window)) PostQuitMessage(2);
        return 0;
    }
    return DefWindowProcW(window,msg,w,l);
}
App* observed = nullptr; // one owned window, no cross-process IPC
void CALLBACK foreground_event(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD event_ms) {
    if (observed && observed->ready && hwnd != observed->created_window &&
        static_cast<LONG>(event_ms - static_cast<DWORD>(observed->ready)) >= 0)
        observed->foreground_changed = true;
}
void run(App& a,HINSTANCE instance) {
    a.created = process_created();
    a.start = launch_tick(a.created, utc_filetime(), GetTickCount64());
    log("process_start tick_ms=%llu created_filetime=%llu utc_filetime=%llu\n", a.start, a.created, utc_filetime());
    DWORD session=0; check(ProcessIdToSessionId(GetCurrentProcessId(),&session)!=0,"ProcessIdToSessionId");
    log("pid=%lu session=%lu\n",GetCurrentProcessId(),session);
    check(session!=0,"interactive session required",ERROR_NOT_SUPPORTED);
    HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS); check(input!=nullptr,"OpenInputDesktop");
    wchar_t input_name[256]{},current_name[256]{}; DWORD needed=0;
    BOOL ok=GetUserObjectInformationW(input,UOI_NAME,input_name,sizeof(input_name),&needed); DWORD error=GetLastError();
    BOOL closed=CloseDesktop(input); check(closed!=0,"CloseDesktop"); check(ok!=0,"input desktop name",error);
    HDESK current=GetThreadDesktop(GetCurrentThreadId()); check(current!=nullptr,"GetThreadDesktop");
    check(GetUserObjectInformationW(current,UOI_NAME,current_name,sizeof(current_name),&needed)!=0,"thread desktop name");
    log("input_desktop=%s thread_desktop=%s\n",utf8(input_name).c_str(),utf8(current_name).c_str());
    check(_wcsicmp(input_name,current_name)==0,"input desktop mismatch",ERROR_ACCESS_DENIED);
    fonts(a); metadata(a);
    HMONITOR monitor=MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY); check(monitor!=nullptr,"MonitorFromPoint");
    MONITORINFO info{}; info.cbSize=sizeof(info); check(GetMonitorInfoW(monitor,&info)!=0,"GetMonitorInfoW");
    a.dpi=GetDpiForSystem(); check(a.dpi!=0,"GetDpiForSystem");
    WNDCLASSW wc{}; wc.lpfnWndProc=window_proc; wc.hInstance=instance; wc.lpszClassName=L"IndependentDisplayPattern";
    check(RegisterClassW(&wc)!=0,"RegisterClassW");
    const DWORD style=WS_POPUP; // no DPI-scaled caption/frame; fixed physical pattern stays unchanged
    log("rcWork_physical=%ld,%ld,%ld,%ld window_style=WS_POPUP non_topmost noactivate; own_rect_occludes_underlying_content\n",
        info.rcWork.left,info.rcWork.top,info.rcWork.right,info.rcWork.bottom);
    RECT outer{0,0,W,H}; check(AdjustWindowRectExForDpi(&outer,style,FALSE,0,a.dpi)!=0,"AdjustWindowRectExForDpi");
    const int width=outer.right-outer.left,height=outer.bottom-outer.top;
    check(width<=info.rcWork.right-info.rcWork.left && height<=info.rcWork.bottom-info.rcWork.top,"960x560 client must fit work area",ERROR_NOT_SUPPORTED);
    std::wstring title=L"Display pattern - "+a.o.mode+L" - physical 960x560";
    a.window=CreateWindowExW(0,wc.lpszClassName,title.c_str(),style,
        info.rcWork.left+((info.rcWork.right-info.rcWork.left)-width)/2,
        info.rcWork.top+((info.rcWork.bottom-info.rcWork.top)-height)/2,width,height,nullptr,nullptr,instance,&a);
    check(a.window!=nullptr,"CreateWindowExW");
    a.created_window = a.window;
    log("created_hwnd=%p self_pid=%lu created_filetime=%llu utc_filetime=%llu tick_ms=%llu\n",
        static_cast<void*>(a.created_window), GetCurrentProcessId(), a.created, utc_filetime(), GetTickCount64());
    UINT actual_dpi=GetDpiForWindow(a.window); check(actual_dpi!=0,"GetDpiForWindow");
    // System DPI can differ from this monitor. Refit before showing, never scale client content.
    RECT actual_outer{0,0,W,H};
    check(AdjustWindowRectExForDpi(&actual_outer,style,FALSE,0,actual_dpi)!=0,"AdjustWindowRectExForDpi(actual window)");
    int actual_w=actual_outer.right-actual_outer.left,actual_h=actual_outer.bottom-actual_outer.top;
    check(actual_w<=info.rcWork.right-info.rcWork.left&&actual_h<=info.rcWork.bottom-info.rcWork.top,"physical client fit at actual DPI",ERROR_NOT_SUPPORTED);
    check(SetWindowPos(a.window,nullptr,info.rcWork.left+((info.rcWork.right-info.rcWork.left)-actual_w)/2,
        info.rcWork.top+((info.rcWork.bottom-info.rcWork.top)-actual_h)/2,actual_w,actual_h,
        SWP_NOZORDER|SWP_NOACTIVATE)!=0,"SetWindowPos(owned physical client fit)");
    RECT client{}; check(GetClientRect(a.window,&client)!=0,"GetClientRect");
    POINT origin{0,0}; check(ClientToScreen(a.window,&origin)!=0,"ClientToScreen");
    log("DPI window=%u system=%u PMv2 verified client_physical=%ldx%ld origin_screen=%ld,%ld\n",actual_dpi,a.dpi,client.right,client.bottom,origin.x,origin.y);
    check(client.right==W&&client.bottom==H,"physical client dimensions",ERROR_INVALID_DATA);
    RECT window_rect{}; check(GetWindowRect(a.window,&window_rect)!=0,"GetWindowRect(owned)");
    log("window_physical=%ld,%ld,%ld,%ld client_physical_screen=%ld,%ld,%ld,%ld unscaled=1\n",
        window_rect.left,window_rect.top,window_rect.right,window_rect.bottom,origin.x,origin.y,origin.x+W,origin.y+H);
    check(window_rect.left>=info.rcWork.left&&window_rect.top>=info.rcWork.top&&
          window_rect.right<=info.rcWork.right&&window_rect.bottom<=info.rcWork.bottom,
          "owned window must stay inside physical rcWork",ERROR_INVALID_DATA);
    check(window_rect.right-window_rect.left==W&&window_rect.bottom-window_rect.top==H&&
          origin.x==window_rect.left&&origin.y==window_rect.top,"borderless window/client equality",ERROR_INVALID_DATA);
    if(a.o.mode==L"dwrite") setup_d2d(a);
    else log("adapter_type=GDI screen DC; no D3D adapter created/claimed\n");
    a.initial_rect = window_rect;
    ULONGLONG now = GetTickCount64(), deadline = a.start + a.o.duration * 1000;
    check(now < deadline, "observation_unavailable: setup exhausted soft budget", ERROR_TIMEOUT);
    check(SetTimer(a.window,1,static_cast<UINT>(deadline-now),nullptr)!=0,"SetTimer(remaining soft budget)");
    ShowWindow(a.window,SW_SHOWNOACTIVATE);
    if (a.o.foreground) {
        observed = &a;
        a.foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            foreground_event, 0, 0, WINEVENT_OUTOFCONTEXT);
        check(a.foreground_hook != nullptr, "SetWinEventHook(foreground)");
        BOOL requested = SetForegroundWindow(a.window); // once, no focus-protection bypass
        log("foreground_request_once=%d utc_filetime=%llu tick_ms=%llu\n", requested, utc_filetime(), GetTickCount64());
        snapshot(a, "activation", true); // pre-paint snapshot is diagnostic, not READY
        check(requested && GetForegroundWindow() == a.window,
            "observation_unavailable: foreground denied", ERROR_ACCESS_DENIED);
        a.activation_checked = true;
        check(SetTimer(a.window,2,50,nullptr)!=0,"SetTimer(observation monitor)");
        if (a.painted) begin_observation(a); // synchronous/reentrant paint may precede activation check
    } else {
        check(SetWindowPos(a.window,HWND_TOP,0,0,0,0,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE)!=0,"SetWindowPos(owned HWND_TOP,noactivate)");
        log("owned_window_raised=HWND_TOP once noactivate non_TOPMOST; visibility_not_established\n");
    }
    check(InvalidateRect(a.window,nullptr,FALSE)!=0,"InvalidateRect");
    MSG msg{};
    for(;;) { int n=GetMessageW(&msg,nullptr,0,0); check(n!=-1,"GetMessageW"); if(!n) break; DispatchMessageW(&msg); }
    check(completed(a.o.foreground, a.painted, a.observation_started, a.capture_done, a.result != 0),
        "observation_unavailable: incomplete/early close/failed run",ERROR_INVALID_DATA);
    log("window_exit=%d painted=%d elapsed_ms=%llu\n",a.result,a.painted?1:0,GetTickCount64()-a.start);
}
bool finish_capture(App& a) {
    if (!a.capture_process.hProcess) return true;
    DWORD state = WaitForSingleObject(a.capture_process.hProcess, 0);
    if (state == WAIT_OBJECT_0) return true;
    ULONGLONG now = GetTickCount64(), hard = a.start + 30000;
    BOOL stopped = TerminateProcess(a.capture_process.hProcess, 2);
    DWORD error = stopped ? 0 : GetLastError();
    DWORD wait = WaitForSingleObject(a.capture_process.hProcess,
        static_cast<DWORD>(now < hard ? (hard-now < 1000 ? hard-now : 1000) : 0));
    log("observation_unavailable: owned_capture_cleanup pid=%lu created_filetime=%llu terminated=%d error=%lu wait=%lu stopped_confirmed=%d utc_filetime=%llu tick_ms=%llu\n",
        a.capture_process.dwProcessId, a.capture_created, stopped, error, wait, wait==WAIT_OBJECT_0, utc_filetime(), GetTickCount64());
    return false; // even confirmed forced termination is a failed observation
}
void self_test() {
    RECT popup{0,0,W,H};
    check(AdjustWindowRectExForDpi(&popup,WS_POPUP,FALSE,0,192)!=0,"self-test popup at DPI192");
    check(popup.left==0&&popup.top==0&&popup.right==W&&popup.bottom==H,
          "self-test popup has zero nonclient inflation at DPI192",ERROR_INVALID_DATA);
    log("SELF_TEST popup DPI192 outer=960x560 client=960x560 (geometry only,no visible window)\n");
    wchar_t app[]=L"probe",mode[]=L"--mode",gdi[]=L"gdi",out[]=L"--out",dir[]=L"x",duration[]=L"--duration",bad[]=L"31",good[]=L"30";
    wchar_t* valid[]={app,mode,gdi,out,dir,duration,good}; Options o;
    check(parse(7,valid,o)&&o.duration==30,"self-test parse valid",ERROR_INVALID_DATA);
    valid[6]=bad;o={};check(!parse(7,valid,o),"self-test rejects duration31",ERROR_INVALID_DATA);
    valid[6]=good;valid[2]=bad;o={};check(!parse(7,valid,o),"self-test rejects mode",ERROR_INVALID_DATA);
    wchar_t fg[]=L"--foreground", cap[]=L"--capture-exe", exe[]=L"C:\\probe.exe", absout[]=L"C:\\new", self[]=L"--self-test";
    wchar_t* observe[]={app,mode,gdi,out,absout,fg,cap,exe}; o={};
    check(parse(8,observe,o)&&o.foreground&&!o.capture.empty(),"self-test observation args",ERROR_INVALID_DATA);
    o={}; check(!parse(7,observe,o),"self-test missing capture path",ERROR_INVALID_DATA);
    observe[5]=cap; o={}; check(!parse(8,observe,o),"self-test duplicate/bad capture",ERROR_INVALID_DATA);
    wchar_t* forbidden[]={app,self,out,dir,fg}; o={};
    check(!parse(5,forbidden,o),"self-test no activation in self-test",ERROR_INVALID_DATA);
    check(!absolute_argument(L"relative.exe")&&!absolute_argument(L"C:\\bad\"arg"),"self-test safe paths",ERROR_INVALID_DATA);
    check(observation_gate(true,true,true,true,false,true,true,true,100,1000,90),"self-test eligible metadata",ERROR_INVALID_DATA);
    for (int i=0;i<8;++i) {
        bool b[]={true,true,true,true,false,true,true,true}; b[i]=!b[i];
        check(!observation_gate(b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],100,1000,90),"self-test state gates fail closed",ERROR_INVALID_DATA);
    }
    check(!observation_gate(true,true,true,true,false,true,true,true,5090,10000,90) &&
          !observation_gate(true,true,true,true,false,true,true,true,1000,1000,90),"self-test expired ready/deadline",ERROR_INVALID_DATA);
    wchar_t* onlyfg[]={app,mode,gdi,out,absout,fg}; o={};
    check(!parse(6,onlyfg,o),"self-test foreground requires capture",ERROR_INVALID_DATA);
    check(!observation_can_start(false,true,false)&&!observation_can_start(true,false,false)&&
          observation_can_start(true,true,false)&&!observation_can_start(true,true,true),"self-test reentrancy latch",ERROR_INVALID_DATA);
    check(!completed(true,true,true,false,false)&&!completed(true,true,false,true,false)&&
          !completed(true,true,true,true,true)&&completed(true,true,true,true,false),"self-test early close/timeout/completion",ERROR_INVALID_DATA);
    check(launch_tick(1000000,1500000,1000)==950 && launch_tick(2000000,1000000,1000)==0,
          "self-test launch budget subtracts setup/fails closed on clock reversal",ERROR_INVALID_DATA);
    log("OBSERVATION_SELF_TEST=PASS args+state_gates+ready_expiry+deadline+early_close+reentrancy+launch_budget; synthetic only, no activation/capture/hardware\n");
    BYTE red_bgra[]={patches[0].b,patches[0].g,patches[0].r,255};
    BYTE blue_bgra[]={patches[2].b,patches[2].g,patches[2].r,255};
    check(red_bgra[0]==0&&red_bgra[1]==0&&red_bgra[2]==255&&red_bgra[3]==255&&
          blue_bgra[0]==255&&blue_bgra[1]==0&&blue_bgra[2]==0&&blue_bgra[3]==255,"CPU BGRA memory order",ERROR_INVALID_DATA);
    check(patches[0].r==255&&patches[0].g==0&&patches[0].b==0&&patches[2].b==255,"CPU RGB/BGRA reference",ERROR_INVALID_DATA);
    for(size_t i=0;i<12;++i) { RECT r=patch_rect(i);check(r.left>=0&&r.top>=0&&r.right<=W&&r.bottom<=H&&r.right>r.left&&r.bottom>r.top,"CPU region bounds",ERROR_INVALID_DATA); }
    log("SELF_TEST=PASS parameter_bounds+CPU_RGB_BGRA_reference+region_bounds; no window/GDI_draw/D2D/font/GPU tested\n");
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    BOOL dpi=SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DWORD dpi_error=dpi?0:GetLastError();
    int result=2; App a;
    try {
        int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc); check(argv!=nullptr,"CommandLineToArgvW");
        bool valid=parse(argc,argv,a.o); LocalFree(argv); if(!valid) return 2;
        check(CreateDirectoryW(a.o.out.c_str(),nullptr)!=0,"CreateDirectory(new only)");
        std::wstring path=a.o.out+L"\\pattern.txt";
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(file!=INVALID_HANDLE_VALUE,"CreateFile(pattern log,CREATE_NEW)");
        int fd=_open_osfhandle(reinterpret_cast<intptr_t>(file),_O_BINARY|_O_WRONLY);
        if(fd<0) { CloseHandle(file); throw std::runtime_error("_open_osfhandle"); }
        output=_fdopen(fd,"wb");
        if(!output) { _close(fd); throw std::runtime_error("_fdopen"); }
        check(dpi!=0,"SetProcessDpiAwarenessContext(PMv2)",dpi_error);
        check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)!=0,"verify PMv2",ERROR_INVALID_STATE);
        if(a.o.mode==L"self-test") { self_test(); result=0; }
        else { run(a,instance); result=a.result; }
    } catch(const std::exception& e) {
        if(output) {fprintf(output,"RESULT=FAIL exception=%s exit=2\n",e.what());fflush(output);}
        result=2;
    }
    if(a.window&&IsWindow(a.window)) {if(!DestroyWindow(a.window)) result=2;}
    try {
        if (!finish_capture(a)) result=2;
        log("RESULT=%s exit=%d (API/state only; external pixel acceptance required)\n",result?"FAIL":"OK",result);
    } catch (...) { result=2; }
    if(output&&fclose(output)) result=2;
    output=nullptr;
    return result;
}
