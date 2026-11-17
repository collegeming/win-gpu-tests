// Read-only capture. No windows, display changes, menu activation or UIA actions.
// Build/run instructions and source identity are packaged by the existing CI.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
FILE* log_file = nullptr;
constexpr DWORD kUiaTimeoutMs = 3000;
constexpr size_t kMaxElements = 32;

void log(const char* fmt, ...) {
    if (!log_file) return;
    va_list args;
    va_start(args, fmt);
    const int n = vfprintf(log_file, fmt, args);
    va_end(args);
    if (n < 0 || fflush(log_file) != 0)
        throw std::runtime_error("write log failed");
}
void wincheck(bool ok, const char* api, DWORD error = 0xffffffffu) {
    if (!ok) {
        if (error == 0xffffffffu) error = GetLastError();
        log("FAIL %s win32=%lu (0 means API supplied no extended error)\n", api, error);
        throw std::runtime_error(api);
    }
}
void hrcheck(HRESULT hr, const char* api) {
    if (FAILED(hr)) {
        log("FAIL %s hr=0x%08lx\n", api, static_cast<unsigned long>(hr));
        throw std::runtime_error(api);
    }
}
// Cleanup never throws, including when the diagnostic volume is full.
void cleanup(bool ok, const char* api) noexcept {
    if (!ok && log_file) {
        fprintf(log_file, "FAIL cleanup %s win32=%lu\n", api, GetLastError());
        fflush(log_file);
    }
}
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                               static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    wincheck(n > 0, "WideCharToMultiByte(size)");
    std::string out(n, '\0');
    wincheck(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
             static_cast<int>(s.size()), out.data(), n, nullptr, nullptr) == n,
             "WideCharToMultiByte(data)");
    for (char& c : out) if (static_cast<unsigned char>(c) < 32) c = ' ';
    return out;
}
void open_log(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    wincheck(h != INVALID_HANDLE_VALUE, "CreateFile(log,CREATE_NEW)");
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY | _O_WRONLY);
    if (fd < 0) { cleanup(CloseHandle(h), "CloseHandle(log)"); throw std::runtime_error("_open_osfhandle"); }
    log_file = _fdopen(fd, "wb");
    if (!log_file) { _close(fd); throw std::runtime_error("_fdopen"); }
}
struct Options {
    std::wstring mode, out;
    bool uia = false;
};
bool parse(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--capture" || a == L"--self-test" || a == L"--uia-worker" ||
            a == L"--test-hang-worker") {
            if (!o.mode.empty()) return false;
            o.mode = a;
        } else if (a == L"--out" && i + 1 < argc && o.out.empty()) {
            o.out = argv[++i];
        } else if (a == L"--uia" && !o.uia) o.uia = true;
        else return false;
    }
    return !o.mode.empty() && !o.out.empty() &&
           (!o.uia || o.mode == L"--capture");
}
struct Com {
    Com() { hrcheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~Com() { CoUninitialize(); }
};
ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> f;
    hrcheck(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&f)), "CoCreateInstance(WIC)");
    return f;
}
void save_png(const std::wstring& path, UINT w, UINT h, BYTE* pixels) {
    auto f = factory();
    ComPtr<IWICStream> stream;
    hrcheck(f->CreateStream(&stream), "WIC CreateStream");
    hrcheck(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "WIC InitializeFromFilename");
    ComPtr<IWICBitmapEncoder> encoder;
    hrcheck(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "WIC CreateEncoder");
    hrcheck(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "WIC Encoder Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    hrcheck(encoder->CreateNewFrame(&frame, nullptr), "WIC CreateNewFrame");
    hrcheck(frame->Initialize(nullptr), "WIC Frame Initialize");
    hrcheck(frame->SetSize(w, h), "WIC SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    hrcheck(frame->SetPixelFormat(&fmt), "WIC SetPixelFormat");
    wincheck(IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA), "WIC changed BGRA format", ERROR_INVALID_DATA);
    hrcheck(frame->WritePixels(h, w * 4, w * h * 4, pixels), "WIC WritePixels");
    hrcheck(frame->Commit(), "WIC Frame Commit");
    hrcheck(encoder->Commit(), "WIC Encoder Commit");
    log("PNG saved width=%u height=%u memory_order=BGRA alpha=255\n", w, h);
}
void verify_png(const std::wstring& path, UINT w, UINT h, const BYTE* expected = nullptr) {
    auto f = factory();
    ComPtr<IWICBitmapDecoder> decoder;
    hrcheck(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder), "WIC CreateDecoderFromFilename");
    ComPtr<IWICBitmapFrameDecode> frame;
    hrcheck(decoder->GetFrame(0, &frame), "WIC GetFrame");
    UINT actual_w = 0, actual_h = 0;
    hrcheck(frame->GetSize(&actual_w, &actual_h), "WIC GetSize");
    log("PNG decoded dimensions=%ux%u expected=%ux%u\n", actual_w, actual_h, w, h);
    wincheck(actual_w == w && actual_h == h, "PNG dimensions", ERROR_INVALID_DATA);
    if (expected) {
        ComPtr<IWICFormatConverter> converter;
        hrcheck(f->CreateFormatConverter(&converter), "WIC CreateFormatConverter");
        hrcheck(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "WIC Converter Initialize");
        std::vector<BYTE> decoded(static_cast<size_t>(w) * h * 4);
        hrcheck(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(decoded.size()),
                decoded.data()), "WIC CopyPixels");
        wincheck(memcmp(decoded.data(), expected, decoded.size()) == 0,
                 "PNG reference pixels", ERROR_INVALID_DATA);
        log("PNG reference_pixels=PASS all=%zu bytes (BGRA red/green/blue, row order)\n", decoded.size());
    }
}
struct GdiCapture {
    HDC screen = nullptr, memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    ~GdiCapture() {
        if (old) cleanup(SelectObject(memory, old) != nullptr, "SelectObject(restore)");
        if (bitmap) cleanup(DeleteObject(bitmap) != 0, "DeleteObject(bitmap)");
        if (memory) cleanup(DeleteDC(memory) != 0, "DeleteDC");
        if (screen) cleanup(ReleaseDC(nullptr, screen) != 0, "ReleaseDC");
    }
};
bool interactive_session(DWORD session) { return session != 0; }
void capture(const std::wstring& out) {
    DWORD session = 0;
    wincheck(ProcessIdToSessionId(GetCurrentProcessId(), &session) != 0, "ProcessIdToSessionId");
    log("pid=%lu session=%lu dpi=PER_MONITOR_AWARE_V2\n", GetCurrentProcessId(), session);
    wincheck(interactive_session(session), "capture requires interactive session, not session 0", ERROR_NOT_SUPPORTED);
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wincheck(input != nullptr, "OpenInputDesktop");
    wchar_t input_name[256]{}, thread_name[256]{};
    DWORD needed = 0;
    BOOL input_ok = GetUserObjectInformationW(input, UOI_NAME, input_name, sizeof(input_name), &needed);
    DWORD input_error = GetLastError();
    cleanup(CloseDesktop(input) != 0, "CloseDesktop(input)");
    wincheck(input_ok != 0, "GetUserObjectInformation(input desktop name)", input_error);
    HDESK thread = GetThreadDesktop(GetCurrentThreadId());
    wincheck(thread != nullptr, "GetThreadDesktop");
    wincheck(GetUserObjectInformationW(thread, UOI_NAME, thread_name, sizeof(thread_name), &needed) != 0,
             "GetUserObjectInformation(thread desktop name)");
    log("input_desktop=%s thread_desktop=%s\n", utf8(input_name).c_str(), utf8(thread_name).c_str());
    wincheck(_wcsicmp(input_name, thread_name) == 0, "thread desktop must be current input desktop", ERROR_ACCESS_DENIED);
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    log("physical_virtualscreen x=%d y=%d width=%d height=%d monitors=%d\n",
        x, y, w, h, GetSystemMetrics(SM_CMONITORS));
    wincheck(w > 0 && h > 0 && static_cast<unsigned long long>(w) * h * 4 <= 512ull * 1024 * 1024,
             "virtualscreen dimensions (max 512 MiB)", ERROR_INVALID_DATA);
    GdiCapture g;
    SetLastError(0);
    g.screen = GetDC(nullptr);
    wincheck(g.screen != nullptr, "GetDC(screen)");
    const int caps[] = {HORZRES, VERTRES, DESKTOPHORZRES, DESKTOPVERTRES, LOGPIXELSX,
                        LOGPIXELSY, BITSPIXEL, PLANES, RASTERCAPS};
    const char* names[] = {"HORZRES", "VERTRES", "DESKTOPHORZRES", "DESKTOPVERTRES",
                           "LOGPIXELSX", "LOGPIXELSY", "BITSPIXEL", "PLANES", "RASTERCAPS"};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
        const int v = GetDeviceCaps(g.screen, caps[i]);
        log("dc_caps %s=%d\n", names[i], v);
        wincheck(v > 0, "GetDeviceCaps", ERROR_INVALID_DATA);
    }
    SetLastError(0);
    g.memory = CreateCompatibleDC(g.screen);
    wincheck(g.memory != nullptr, "CreateCompatibleDC");
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down, physical pixels, no scaling
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    SetLastError(0);
    g.bitmap = CreateDIBSection(g.screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    wincheck(g.bitmap != nullptr && bits != nullptr, "CreateDIBSection");
    SetLastError(0);
    HGDIOBJ old = SelectObject(g.memory, g.bitmap);
    wincheck(old != nullptr && old != HGDI_ERROR, "SelectObject(bitmap)");
    g.old = old;
    const ULONGLONG start = GetTickCount64();
    SetLastError(0);
    wincheck(BitBlt(g.memory, 0, 0, w, h, g.screen, x, y, SRCCOPY | CAPTUREBLT) != 0, "BitBlt(screen)");
    SetLastError(0);
    wincheck(GdiFlush() != 0, "GdiFlush");
    log("BitBlt elapsed_ms=%llu rop=SRCCOPY|CAPTUREBLT cursor=not_explicitly_composited\n", GetTickCount64() - start);
    BYTE* p = static_cast<BYTE*>(bits);
    for (size_t i = 3, n = static_cast<size_t>(w) * h * 4; i < n; i += 4) p[i] = 255;
    // Screenshot is committed before any UIA process is started.
    const ULONGLONG png_start = GetTickCount64();
    Com com;
    save_png(out + L"\\screen.png", w, h, p);
    verify_png(out + L"\\screen.png", w, h);
    log("PNG encode_verify_elapsed_ms=%llu\n", GetTickCount64() - png_start);
    log("CAPTURE=OK (API/encoding success, not proof of correct display content; synchronous readback may itself be faulty)\n");
}
struct Child {
    HANDLE job = nullptr;
    PROCESS_INFORMATION pi{};
    ~Child() {
        if (job) cleanup(CloseHandle(job) != 0, "CloseHandle(job,kill-on-close)");
        if (pi.hThread) cleanup(CloseHandle(pi.hThread) != 0, "CloseHandle(thread)");
        if (pi.hProcess) cleanup(CloseHandle(pi.hProcess) != 0, "CloseHandle(process)");
    }
};
// Only this owned child enters COM UIA. Killing its job bounds even a stuck COM
// call or COM teardown; no futures/threads whose destructor can wait forever.
enum class WorkerResult { Success, ChildFailure, TimedOut };
WorkerResult run_worker(const std::wstring& out, bool simulate_hang = false) {
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    wincheck(n > 0 && n < sizeof(exe) / sizeof(exe[0]), "GetModuleFileNameW");
    // Absolute output path has no trailing backslash, so ordinary quoting is safe.
    std::wstring command = L"\"" + std::wstring(exe, n) + L"\" " +
        (simulate_hang ? L"--test-hang-worker" : L"--uia-worker") + L" --out \"" + out + L"\"";
    Child c;
    c.job = CreateJobObjectW(nullptr, nullptr);
    wincheck(c.job != nullptr, "CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    wincheck(SetInformationJobObject(c.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0,
             "SetInformationJobObject");
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    wincheck(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE,
             CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &c.pi) != 0, "CreateProcess(UIA worker)");
    if (!AssignProcessToJobObject(c.job, c.pi.hProcess)) {
        const DWORD error = GetLastError();
        cleanup(TerminateProcess(c.pi.hProcess, 2) != 0, "TerminateProcess(unassigned child)");
        wincheck(false, "AssignProcessToJobObject", error);
    }
    wincheck(ResumeThread(c.pi.hThread) != static_cast<DWORD>(-1), "ResumeThread");
    const ULONGLONG start = GetTickCount64();
    DWORD wait = WaitForSingleObject(c.pi.hProcess, kUiaTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        log("UIA=FAIL timeout_ms=%lu child_pid=%lu (terminating owned job)\n", kUiaTimeoutMs, c.pi.dwProcessId);
        wincheck(TerminateJobObject(c.job, ERROR_TIMEOUT) != 0, "TerminateJobObject");
        DWORD stopped = WaitForSingleObject(c.pi.hProcess, 1000);
        wincheck(stopped == WAIT_OBJECT_0, "WaitForSingleObject(terminated child)",
                 stopped == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        log("UIA worker terminated elapsed_ms=%llu\n", GetTickCount64() - start);
        return WorkerResult::TimedOut;
    }
    wincheck(wait == WAIT_OBJECT_0, "WaitForSingleObject(UIA)");
    DWORD code = 0;
    wincheck(GetExitCodeProcess(c.pi.hProcess, &code) != 0, "GetExitCodeProcess");
    log("UIA child_pid=%lu exit=%lu elapsed_ms=%llu\n", c.pi.dwProcessId, code, GetTickCount64() - start);
    return code == 0 ? WorkerResult::Success : WorkerResult::ChildFailure;
}
struct Targets { std::vector<HWND> windows; bool enumeration_incomplete = false; };
BOOL CALLBACK find_target(HWND hwnd, LPARAM value) {
    auto& targets = *reinterpret_cast<Targets*>(value);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(hwnd, &pid)) {
        targets.enumeration_incomplete = true;
        fprintf(log_file, "FAIL GetWindowThreadProcessId win32=%lu\n", GetLastError());
        return TRUE;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        targets.enumeration_incomplete = true;
        fprintf(log_file, "UNAVAILABLE OpenProcess pid=%lu win32=%lu\n", pid, GetLastError());
        return TRUE;
    }
    wchar_t path[32768];
    DWORD size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        if (_wcsicmp(base, L"StartMenuExperienceHost.exe") == 0 || _wcsicmp(base, L"SystemSettings.exe") == 0) {
            fprintf(log_file, "target pid=%lu hwnd=%p image=%ls\n", pid, hwnd, base);
            targets.windows.push_back(hwnd);
        }
    } else {
        targets.enumeration_incomplete = true;
        fprintf(log_file, "UNAVAILABLE QueryFullProcessImageName pid=%lu win32=%lu\n", pid, GetLastError());
    }
    cleanup(CloseHandle(process) != 0, "CloseHandle(target process)");
    return targets.windows.size() < 8; // bounded target count
}
int uia_worker() {
    Com com;
    Targets targets;
    SetLastError(0);
    BOOL enumerated = EnumWindows(find_target, reinterpret_cast<LPARAM>(&targets));
    wincheck(enumerated != 0 || targets.windows.size() == 8, "EnumWindows");
    if (targets.windows.empty()) {
        log("UIA=UNAVAILABLE no visible existing target window in this desktop/session\n");
        return 2;
    }
    ComPtr<IUIAutomation> uia;
    hrcheck(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&uia)), "CoCreateInstance(UIA)");
    ComPtr<IUIAutomationTreeWalker> walker;
    hrcheck(uia->get_ControlViewWalker(&walker), "UIA ControlViewWalker");
    size_t emitted = 0;
    for (HWND hwnd : targets.windows) {
        if (emitted == kMaxElements) break;
        ComPtr<IUIAutomationElement> root;
        hrcheck(uia->ElementFromHandle(hwnd, &root), "UIA ElementFromHandle");
        std::vector<ComPtr<IUIAutomationElement>> nodes;
        nodes.push_back(root);
        for (size_t i = 0; i < nodes.size() && emitted < kMaxElements; ++i) {
            BOOL password = FALSE;
            hrcheck(nodes[i]->get_CurrentIsPassword(&password), "UIA CurrentIsPassword");
            CONTROLTYPEID type = 0;
            hrcheck(nodes[i]->get_CurrentControlType(&type), "UIA CurrentControlType");
            BSTR name = nullptr;
            // Never request password/edit contents or Value/Text patterns.
            if (!password && type != UIA_EditControlTypeId)
                hrcheck(nodes[i]->get_CurrentName(&name), "UIA CurrentName");
            std::wstring text = password || type == UIA_EditControlTypeId ? L"[redacted]" :
                std::wstring(name ? name : L"", name ? SysStringLen(name) : 0);
            SysFreeString(name);
            if (text.size() > 160) {
                text.resize(160);
                if (text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
            }
            RECT bounds{};
            hrcheck(nodes[i]->get_CurrentBoundingRectangle(&bounds), "UIA CurrentBoundingRectangle");
            log("element=%zu Name=\"%s\" ControlType=%d Bounds=(%ld,%ld,%ld,%ld)\n",
                emitted++, utf8(text).c_str(), type, bounds.left, bounds.top, bounds.right, bounds.bottom);
            if (nodes.size() >= kMaxElements || emitted >= kMaxElements) continue;
            ComPtr<IUIAutomationElement> child;
            hrcheck(walker->GetFirstChildElement(nodes[i].Get(), &child), "UIA GetFirstChild");
            while (child && nodes.size() < kMaxElements) {
                nodes.push_back(child);
                if (nodes.size() == kMaxElements) break;
                ComPtr<IUIAutomationElement> next;
                hrcheck(walker->GetNextSiblingElement(child.Get(), &next), "UIA GetNextSibling");
                child = next;
            }
        }
    }
    log("UIA=%s emitted=%zu max=%zu Name_max_utf16=160 target_max=8 scope=visible_existing_windows enumeration_incomplete=%d target_reads=OK\n",
        targets.enumeration_incomplete ? "PARTIAL" : "OK", emitted, kMaxElements, targets.enumeration_incomplete ? 1 : 0);
    return targets.enumeration_incomplete ? 2 : 0;
}
void self_test(const std::wstring& out) {
    wincheck(!interactive_session(0) && interactive_session(1) && interactive_session(42),
             "self-test session-zero guard", ERROR_INVALID_DATA);
    Options o;
    wchar_t app[] = L"probe", capture_arg[] = L"--capture", out_arg[] = L"--out", dir[] = L"test";
    wchar_t bad[] = L"--unknown", uia_arg[] = L"--uia";
    wchar_t* valid[] = {app, capture_arg, out_arg, dir};
    wchar_t* invalid[] = {app, capture_arg, bad};
    wchar_t* missing[] = {app, capture_arg, out_arg};
    wchar_t* duplicate[] = {app, capture_arg, capture_arg, out_arg, dir};
    wchar_t* uia_only[] = {app, uia_arg, out_arg, dir};
    wincheck(parse(4, valid, o), "self-test valid args", ERROR_INVALID_DATA);
    o = {}; wincheck(!parse(3, invalid, o), "self-test unknown args", ERROR_INVALID_DATA);
    o = {}; wincheck(!parse(3, missing, o), "self-test missing out", ERROR_INVALID_DATA);
    o = {}; wincheck(!parse(5, duplicate, o), "self-test duplicate mode", ERROR_INVALID_DATA);
    o = {}; wincheck(!parse(4, uia_only, o), "self-test UIA without capture", ERROR_INVALID_DATA);
    // Same memory-order discipline as shared probe expected_bytes(bgra8):
    // red=(0,0,255,255), green=(0,255,0,255), blue=(255,0,0,255).
    BYTE pixels[] = {0,0,255,255, 0,255,0,255, 255,0,0,255,
                     255,0,0,255, 0,255,0,255, 0,0,255,255};
    {
        Com com;
        save_png(out + L"\\reference.png", 3, 2, pixels);
        verify_png(out + L"\\reference.png", 3, 2, pixels);
    }
    wincheck(run_worker(out + L"\\missing-output") == WorkerResult::ChildFailure,
             "self-test startup failure must not be a timeout", ERROR_INVALID_DATA);
    const ULONGLONG start = GetTickCount64();
    wincheck(run_worker(out, true) == WorkerResult::TimedOut,
             "self-test hung child must time out, not exit/fail", ERROR_INVALID_DATA);
    const ULONGLONG elapsed = GetTickCount64() - start;
    wincheck(elapsed >= kUiaTimeoutMs && elapsed < 6000, "self-test timeout bound", ERROR_TIMEOUT);
    log("SELF_TEST=PASS args+PNG_dimensions+reference_pixels+owned_child_hard_timeout; hardware NOT tested\n");
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // First action, before any HWND, screen metrics, DC or COM work.
    const BOOL dpi_ok = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD dpi_error = dpi_ok ? ERROR_SUCCESS : GetLastError();
    int result = 2;
    try {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        wincheck(argv != nullptr, "CommandLineToArgvW");
        Options o;
        bool valid = parse(argc, argv, o);
        cleanup(LocalFree(argv) == nullptr, "LocalFree(argv)");
        if (!valid) return 2;
        wchar_t full[32768];
        DWORD n = GetFullPathNameW(o.out.c_str(), static_cast<DWORD>(sizeof(full) / sizeof(full[0])), full, nullptr);
        wincheck(n > 0 && n < sizeof(full) / sizeof(full[0]), "GetFullPathNameW");
        o.out.assign(full, n);
        while (o.out.size() > 3 && (o.out.back() == L'\\' || o.out.back() == L'/')) o.out.pop_back();
        wincheck(o.out.find(L'\"') == std::wstring::npos, "output path quote", ERROR_INVALID_NAME);
        const bool worker = o.mode == L"--uia-worker" || o.mode == L"--test-hang-worker";
        if (!worker) wincheck(CreateDirectoryW(o.out.c_str(), nullptr) != 0, "CreateDirectory(new output only)");
        open_log(o.out + (worker ? L"\\uia.txt" : L"\\capture.txt"));
        log("display_state_probe mode=%s output=%s\n", utf8(o.mode).c_str(), utf8(o.out).c_str());
        wincheck(dpi_ok != 0, "SetProcessDpiAwarenessContext(PMv2)", dpi_error);
        wincheck(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0,
                 "verify PMv2 context", ERROR_INVALID_STATE);
        if (o.mode == L"--test-hang-worker") { Sleep(INFINITE); return 2; }
        if (o.mode == L"--uia-worker") result = uia_worker();
        else if (o.mode == L"--self-test") { self_test(o.out); result = 0; }
        else {
            capture(o.out);
            result = 0;
            if (o.uia && run_worker(o.out) != WorkerResult::Success) result = 2;
        }
        log("RESULT=%s exit=%d\n", result == 0 ? "OK" : "FAIL", result);
    } catch (const std::exception& e) {
        if (log_file) { fprintf(log_file, "RESULT=FAIL exception=%s exit=2\n", e.what()); fflush(log_file); }
        result = 2;
    }
    if (log_file && fclose(log_file) != 0) result = 2;
    log_file = nullptr;
    return result;
}
