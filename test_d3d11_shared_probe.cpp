// Cross-process D3D11 shared-texture probe for the DroidVM Windows ARM64 stack.
//
// Producer creates a shared 2D texture, clears it, exports a share handle and
// waits on named events.  Consumer opens the same handle in a second process,
// copies each frame to a staging texture and validates the pixels.  The same
// exe is both roles:
//
//   d3d11_shared_probe_arm64.exe --producer --name demo --loop
//   d3d11_shared_probe_arm64.exe --consumer --name demo
//
// Default is legacy IDXGIResource::GetSharedHandle / OpenSharedResource.
// --nthandle uses IDXGIResource1::CreateSharedHandle, duplicates the NT handle
// into the consumer process, and opens it with OpenSharedResource1.
//
// --loop (or --frames N) clears a different colour for each frame, so a
// consumer that reads the stale first frame is distinguishable from one that
// sees producer updates.  The process-level RESULT line is PASS/FAIL; the
// exit code is 0 on PASS and 2 on FAIL.

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

constexpr uint32_t kMagic = 0x53484431u; // "SHD1"
constexpr int kMaxFrames = 16;
constexpr DWORD kWaitMs = 30000;
constexpr int kPixelTolerance = 8;

struct SharedBlock {
    volatile LONG magic;
    volatile LONG protocol;
    volatile LONG role; // 1 = producer, 2 = consumer
    volatile LONG frames;
    volatile LONG frame_index;
    volatile LONG producer_failed;
    volatile LONG consumer_failed;
    volatile LONG producer_pid;
    volatile LONG consumer_pid;
    volatile LONG nthandle;
    volatile LONG width;
    volatile LONG height;
    volatile LONG format; // DXGI_FORMAT
    volatile LONG consumer_open_hr;
    volatile LONG consumer_read_hr;
    volatile LONG producer_hr;
    volatile LONG frame_mismatch;
    volatile LONG producer_done;
    volatile LONG consumer_done;
    // Bytes in the texture's memory order (BGRA for B8G8R8A8, RGBA for
    // R8G8B8A8), not the logical RGBA order.
    volatile LONG expected[kMaxFrames][4];
    volatile LONG actual[kMaxFrames][4];
    volatile ULONGLONG shared_handle;
    char note[256];
};

struct Options {
    enum class Mode { None, Producer, Consumer } mode = Mode::None;
    std::string name = "default";
    int frames = 0;
    bool loop = false;
    bool nthandle = false;
    int size = 256;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
};

void usage() {
    std::printf(
        "usage:\n"
        "  d3d11_shared_probe_arm64.exe --producer --name NAME [options]\n"
        "  d3d11_shared_probe_arm64.exe --consumer --name NAME [--nthandle]\n"
        "\n"
        "options:\n"
        "  --frames N    number of frames (1..16, default 1)\n"
        "  --loop        shorthand for --frames 3\n"
        "  --size N      texture width/height (16..4096, default 256)\n"
        "  --format F    bgra8 (default) or rgba8\n"
        "  --nthandle    use SHARED_NTHANDLE + OpenSharedResource1\n"
        "  --help        this text\n");
}

bool parse_int(const char *s, int *out) {
    if (!s || !s[0])
        return false;
    char *end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (*end != '\0' || v < 0 || v > 1 << 20)
        return false;
    *out = (int)v;
    return true;
}

bool parse_args(int argc, char **argv, Options *o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--producer") {
            o->mode = Options::Mode::Producer;
        } else if (a == "--consumer") {
            o->mode = Options::Mode::Consumer;
        } else if (a == "--name" && i + 1 < argc) {
            o->name = argv[++i];
        } else if (a == "--frames" && i + 1 < argc) {
            int v = 0;
            if (!parse_int(argv[++i], &v) || v < 1 || v > kMaxFrames) {
                std::printf("bad --frames value\n");
                return false;
            }
            o->frames = v;
        } else if (a == "--loop") {
            o->loop = true;
        } else if (a == "--nthandle") {
            o->nthandle = true;
        } else if (a == "--size" && i + 1 < argc) {
            int v = 0;
            if (!parse_int(argv[++i], &v) || v < 16 || v > 4096) {
                std::printf("bad --size value\n");
                return false;
            }
            o->size = v;
        } else if (a == "--format" && i + 1 < argc) {
            const std::string f = argv[++i];
            if (f == "bgra8") {
                o->format = DXGI_FORMAT_B8G8R8A8_UNORM;
            } else if (f == "rgba8") {
                o->format = DXGI_FORMAT_R8G8B8A8_UNORM;
            } else {
                std::printf("bad --format value: %s\n", f.c_str());
                return false;
            }
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            std::printf("unknown argument: %s\n", a.c_str());
            return false;
        }
    }

    if (o->mode == Options::Mode::None) {
        std::printf("one of --producer/--consumer is required\n");
        return false;
    }
    if (o->frames == 0)
        o->frames = o->loop ? 3 : 1;
    if (o->frames < 1 || o->frames > kMaxFrames) {
        std::printf("frame count out of range\n");
        return false;
    }
    return true;
}

std::string sanitize_name(const std::string &in) {
    std::string out;
    for (char c : in) {
        const unsigned char u = (unsigned char)c;
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
            (u >= '0' && u <= '9') || c == '_' || c == '-')
            out.push_back(c);
    }
    if (out.empty())
        out = "default";
    if (out.size() > 48)
        out.resize(48);
    return out;
}

std::wstring widen_ascii(const std::string &s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s)
        w.push_back((wchar_t)(unsigned char)c);
    return w;
}

std::wstring base_name(const Options &o) {
    return L"Local\\D3D11SharedProbe_" + widen_ascii(o.name);
}

std::wstring suffixed(const std::wstring &base, const wchar_t *suffix) {
    return base + suffix;
}

const char *format_name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    default:
        return "UNKNOWN";
    }
}

void print_hr(const char *what, HRESULT hr) {
    std::printf("%s: hr=0x%08lx\n", what, (unsigned long)hr);
}

bool check_hr(HRESULT hr, const char *what) {
    print_hr(what, hr);
    return SUCCEEDED(hr);
}

HANDLE create_event_checked(const std::wstring &name) {
    HANDLE h = CreateEventW(nullptr, FALSE, FALSE, name.c_str());
    if (h)
        ResetEvent(h);
    return h;
}

HANDLE open_event_retry(const std::wstring &name, DWORD timeout_ms) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        HANDLE h = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                              name.c_str());
        if (h)
            return h;
        if (GetTickCount64() - start >= timeout_ms)
            return nullptr;
        Sleep(50);
    }
}

HANDLE open_mapping_retry(const std::wstring &name, DWORD timeout_ms) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                    name.c_str());
        if (h)
            return h;
        if (GetTickCount64() - start >= timeout_ms)
            return nullptr;
        Sleep(50);
    }
}

bool wait_ev(HANDLE h, DWORD ms, const char *what) {
    if (!h) {
        std::printf("%s: event is NULL\n", what);
        return false;
    }
    const DWORD r = WaitForSingleObject(h, ms);
    if (r == WAIT_OBJECT_0)
        return true;
    if (r == WAIT_TIMEOUT)
        std::printf("%s: timeout after %lu ms\n", what, (unsigned long)ms);
    else
        std::printf("%s: wait failed, gle=%lu\n", what,
                    (unsigned long)GetLastError());
    return false;
}

bool set_ev(HANDLE h, const char *what) {
    if (!h) {
        std::printf("%s: event is NULL\n", what);
        return false;
    }
    if (!SetEvent(h)) {
        std::printf("%s: SetEvent failed, gle=%lu\n", what,
                    (unsigned long)GetLastError());
        return false;
    }
    return true;
}

void adapter_name(ID3D11Device *dev, const char *tag) {
    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        std::printf("%s: adapter=%ls vendor=0x%04x device=0x%04x "
                    "luid=%08lx:%08lx\n",
                    tag, desc.Description, desc.VendorId, desc.DeviceId,
                    (unsigned long)desc.AdapterLuid.HighPart,
                    (unsigned long)desc.AdapterLuid.LowPart);
    }
    if (adapter)
        adapter->Release();
    if (dxgi)
        dxgi->Release();
}

HRESULT create_device(ID3D11Device **dev, ID3D11DeviceContext **ctx,
                      const char *tag) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, dev,
        &fl, ctx);
    std::printf("%s: D3D11CreateDevice hr=0x%08lx feature=0x%x\n", tag,
                (unsigned long)hr, (unsigned)fl);
    return hr;
}

HRESULT wait_gpu(ID3D11DeviceContext *ctx, ID3D11Query *query) {
    if (!query) {
        // Query creation can fail on very old UMDs; this is only a fallback.
        Sleep(100);
        return S_OK;
    }
    ctx->End(query);
    ctx->Flush();
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const HRESULT hr = ctx->GetData(query, nullptr, 0, 0);
        if (hr == S_OK)
            return S_OK;
        if (hr != S_FALSE)
            return hr;
        if (GetTickCount64() - start > 5000)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Sleep(1);
    }
}

void expected_bytes(DXGI_FORMAT fmt, const float color[4], LONG out[4]) {
    auto q = [](float v) -> LONG {
        int i = (int)(v * 255.0f + 0.5f);
        if (i < 0)
            i = 0;
        if (i > 255)
            i = 255;
        return (LONG)i;
    };
    const LONG r = q(color[0]);
    const LONG g = q(color[1]);
    const LONG b = q(color[2]);
    const LONG a = q(color[3]);
    if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
        out[0] = b;
        out[1] = g;
        out[2] = r;
        out[3] = a;
    } else {
        out[0] = r;
        out[1] = g;
        out[2] = b;
        out[3] = a;
    }
}

void frame_color(int index, float out[4]) {
    static const float colors[][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, // red
        {0.0f, 1.0f, 0.0f, 1.0f}, // green
        {0.0f, 0.0f, 1.0f, 1.0f}, // blue
        {1.0f, 1.0f, 0.0f, 1.0f}, // yellow
        {1.0f, 0.0f, 1.0f, 1.0f}, // magenta
        {0.0f, 1.0f, 1.0f, 1.0f}, // cyan
        {1.0f, 1.0f, 1.0f, 1.0f}, // white
        {0.0f, 0.0f, 0.0f, 1.0f}, // black
    };
    constexpr int n = (int)(sizeof(colors) / sizeof(colors[0]));
    std::memcpy(out, colors[index % n], sizeof(float) * 4);
}

bool pixel_matches(const LONG expected[4], const BYTE *p) {
    for (int i = 0; i < 4; ++i) {
        const int e = (int)expected[i];
        const int a = (int)p[i];
        if (std::abs(a - e) > kPixelTolerance)
            return false;
    }
    return true;
}

int run_producer(const Options &o) {
    const std::wstring base = base_name(o);
    const std::wstring map_name = suffixed(base, L"_map");

    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, (DWORD)sizeof(SharedBlock),
                                    map_name.c_str());
    if (!map) {
        std::printf("producer: CreateFileMapping failed, gle=%lu\n",
                    (unsigned long)GetLastError());
        return 2;
    }
    SharedBlock *shm =
        (SharedBlock *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                                     sizeof(SharedBlock));
    if (!shm) {
        std::printf("producer: MapViewOfFile failed, gle=%lu\n",
                    (unsigned long)GetLastError());
        CloseHandle(map);
        return 2;
    }
    std::memset(shm, 0, sizeof(*shm));
    shm->magic = (LONG)kMagic;
    shm->protocol = 1;
    shm->role = 1;
    shm->frames = o.frames;
    shm->width = o.size;
    shm->height = o.size;
    shm->format = (LONG)o.format;
    shm->nthandle = o.nthandle ? 1 : 0;
    shm->producer_pid = (LONG)GetCurrentProcessId();

    HANDLE ev_consumer_ready =
        create_event_checked(suffixed(base, L"_consumer_ready"));
    HANDLE ev_init_ready = create_event_checked(suffixed(base, L"_init_ready"));
    HANDLE ev_consumer_opened =
        create_event_checked(suffixed(base, L"_consumer_opened"));
    HANDLE ev_frame_ready =
        create_event_checked(suffixed(base, L"_frame_ready"));
    HANDLE ev_frame_done = create_event_checked(suffixed(base, L"_frame_done"));
    HANDLE ev_consumer_done =
        create_event_checked(suffixed(base, L"_consumer_done"));
    HANDLE ev_producer_done =
        create_event_checked(suffixed(base, L"_producer_done"));

    auto fail = [&](HRESULT hr, const char *what) -> int {
        std::printf("producer: FAIL %s hr=0x%08lx\n", what,
                    (unsigned long)hr);
        if (shm) {
            shm->producer_failed = 1;
            shm->producer_hr = (LONG)hr;
            MemoryBarrier();
        }
        if (ev_init_ready)
            SetEvent(ev_init_ready);
        if (ev_frame_ready)
            SetEvent(ev_frame_ready);
        if (ev_consumer_done)
            SetEvent(ev_consumer_done);
        return 2;
    };

    if (!ev_consumer_ready || !ev_init_ready || !ev_consumer_opened ||
        !ev_frame_ready || !ev_frame_done || !ev_consumer_done ||
        !ev_producer_done) {
        return fail(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent");
    }

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = create_device(&dev, &ctx, "producer");
    if (FAILED(hr))
        return fail(hr, "D3D11CreateDevice");
    adapter_name(dev, "producer");

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)o.size;
    td.Height = (UINT)o.size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = o.format;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (o.nthandle)
        td.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    ID3D11Texture2D *tex = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &tex);
    print_hr("producer: CreateTexture2D", hr);
    if (FAILED(hr)) {
        std::printf("producer: desc=%ux%u format=%s misc=0x%x bind=0x%x\n",
                    td.Width, td.Height, format_name(td.Format),
                    (unsigned)td.MiscFlags, (unsigned)td.BindFlags);
        return fail(hr, "CreateTexture2D");
    }
    std::printf("producer: texture=%ux%u format=%s misc=0x%x bind=0x%x\n",
                td.Width, td.Height, format_name(td.Format),
                (unsigned)td.MiscFlags, (unsigned)td.BindFlags);

    ID3D11RenderTargetView *rtv = nullptr;
    hr = dev->CreateRenderTargetView(tex, nullptr, &rtv);
    print_hr("producer: CreateRenderTargetView", hr);
    if (FAILED(hr))
        return fail(hr, "CreateRenderTargetView");

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    ID3D11Query *query = nullptr;
    hr = dev->CreateQuery(&qd, &query);
    if (FAILED(hr)) {
        std::printf("producer: CreateQuery failed hr=0x%08lx (sleep fallback)\n",
                    (unsigned long)hr);
        query = nullptr;
    }

    const float initial[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, initial);
    ctx->Flush();
    hr = wait_gpu(ctx, query);
    if (FAILED(hr))
        return fail(hr, "initial clear wait");

    HANDLE shared = nullptr;
    if (o.nthandle) {
        IDXGIResource1 *res1 = nullptr;
        hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1);
        print_hr("producer: QueryInterface(IDXGIResource1)", hr);
        if (FAILED(hr))
            return fail(hr, "QueryInterface(IDXGIResource1)");
        hr = res1->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &shared);
        res1->Release();
        print_hr("producer: CreateSharedHandle", hr);
        if (FAILED(hr))
            return fail(hr, "CreateSharedHandle");
    } else {
        IDXGIResource *res = nullptr;
        hr = tex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
        print_hr("producer: QueryInterface(IDXGIResource)", hr);
        if (FAILED(hr))
            return fail(hr, "QueryInterface(IDXGIResource)");
        hr = res->GetSharedHandle(&shared);
        res->Release();
        print_hr("producer: GetSharedHandle", hr);
        if (FAILED(hr) || !shared)
            return fail(FAILED(hr) ? hr : E_FAIL, "GetSharedHandle");
    }
    std::printf("producer: shared handle=0x%llx nthandle=%d\n",
                (unsigned long long)(uintptr_t)shared, o.nthandle ? 1 : 0);

    if (!wait_ev(ev_consumer_ready, kWaitMs, "producer: consumer_ready"))
        return fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "wait consumer_ready");

    const DWORD consumer_pid = (DWORD)shm->consumer_pid;
    MemoryBarrier();
    std::printf("producer: consumer pid=%lu\n", (unsigned long)consumer_pid);
    if (consumer_pid == 0)
        return fail(E_FAIL, "consumer pid is zero");

    if (o.nthandle) {
        HANDLE consumer_proc =
            OpenProcess(PROCESS_DUP_HANDLE, FALSE, consumer_pid);
        if (!consumer_proc)
            return fail(HRESULT_FROM_WIN32(GetLastError()), "OpenProcess");
        HANDLE remote = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), shared, consumer_proc,
                             &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            const DWORD e = GetLastError();
            CloseHandle(consumer_proc);
            return fail(HRESULT_FROM_WIN32(e), "DuplicateHandle");
        }
        CloseHandle(consumer_proc);
        shm->shared_handle = (ULONGLONG)(uintptr_t)remote;
        std::printf("producer: duplicated handle to consumer=0x%llx\n",
                    (unsigned long long)(uintptr_t)remote);
    } else {
        shm->shared_handle = (ULONGLONG)(uintptr_t)shared;
    }
    shm->producer_hr = S_OK;
    MemoryBarrier();
    if (!set_ev(ev_init_ready, "producer: init_ready"))
        return fail(HRESULT_FROM_WIN32(GetLastError()), "set init_ready");

    if (!wait_ev(ev_consumer_opened, kWaitMs, "producer: consumer_opened"))
        return fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "wait consumer_opened");
    const HRESULT consumer_open_hr = (HRESULT)shm->consumer_open_hr;
    std::printf("producer: consumer OpenSharedResource hr=0x%08lx\n",
                (unsigned long)consumer_open_hr);
    if (FAILED(consumer_open_hr))
        return fail(consumer_open_hr, "consumer OpenSharedResource");

    for (int f = 0; f < o.frames; ++f) {
        float color[4];
        frame_color(f, color);
        LONG expected[4];
        expected_bytes(o.format, color, expected);

        ctx->ClearRenderTargetView(rtv, color);
        ctx->Flush();
        hr = wait_gpu(ctx, query);
        if (FAILED(hr))
            return fail(hr, "frame clear wait");

        for (int c = 0; c < 4; ++c)
            shm->expected[f][c] = expected[c];
        shm->frame_index = f;
        MemoryBarrier();
        if (!set_ev(ev_frame_ready, "producer: frame_ready"))
            return fail(HRESULT_FROM_WIN32(GetLastError()), "set frame_ready");

        if (!wait_ev(ev_frame_done, kWaitMs, "producer: frame_done"))
            return fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "wait frame_done");
        if (shm->consumer_failed) {
            const HRESULT chr = (HRESULT)shm->consumer_read_hr;
            std::printf("producer: consumer failed at frame %d hr=0x%08lx\n", f,
                        (unsigned long)chr);
            return 2;
        }
        std::printf("producer: frame %d color=(%.2f,%.2f,%.2f,%.2f) "
                    "expected=%ld,%ld,%ld,%ld\n",
                    f, color[0], color[1], color[2], color[3], expected[0],
                    expected[1], expected[2], expected[3]);
    }

    if (!wait_ev(ev_consumer_done, kWaitMs, "producer: consumer_done"))
        return fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "wait consumer_done");
    if (shm->consumer_failed)
        return fail((HRESULT)shm->consumer_read_hr, "consumer failed");

    shm->producer_done = 1;
    MemoryBarrier();
    SetEvent(ev_producer_done);
    std::printf("RESULT: PASS\n");

    if (rtv)
        rtv->Release();
    if (query)
        query->Release();
    if (tex)
        tex->Release();
    if (ctx)
        ctx->Release();
    if (dev)
        dev->Release();
    if (o.nthandle && shared)
        CloseHandle(shared);
    if (ev_consumer_ready)
        CloseHandle(ev_consumer_ready);
    if (ev_init_ready)
        CloseHandle(ev_init_ready);
    if (ev_consumer_opened)
        CloseHandle(ev_consumer_opened);
    if (ev_frame_ready)
        CloseHandle(ev_frame_ready);
    if (ev_frame_done)
        CloseHandle(ev_frame_done);
    if (ev_consumer_done)
        CloseHandle(ev_consumer_done);
    if (ev_producer_done)
        CloseHandle(ev_producer_done);
    UnmapViewOfFile(shm);
    CloseHandle(map);
    return 0;
}

int run_consumer(const Options &o) {
    const std::wstring base = base_name(o);
    const std::wstring map_name = suffixed(base, L"_map");

    HANDLE map = open_mapping_retry(map_name, kWaitMs);
    if (!map) {
        std::printf("consumer: OpenFileMapping timed out\n");
        return 2;
    }
    SharedBlock *shm =
        (SharedBlock *)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                     sizeof(SharedBlock));
    if (!shm) {
        std::printf("consumer: MapViewOfFile failed, gle=%lu\n",
                    (unsigned long)GetLastError());
        CloseHandle(map);
        return 2;
    }

    HANDLE ev_consumer_ready =
        open_event_retry(suffixed(base, L"_consumer_ready"), kWaitMs);
    HANDLE ev_init_ready =
        open_event_retry(suffixed(base, L"_init_ready"), kWaitMs);
    HANDLE ev_consumer_opened =
        open_event_retry(suffixed(base, L"_consumer_opened"), kWaitMs);
    HANDLE ev_frame_ready =
        open_event_retry(suffixed(base, L"_frame_ready"), kWaitMs);
    HANDLE ev_frame_done =
        open_event_retry(suffixed(base, L"_frame_done"), kWaitMs);
    HANDLE ev_consumer_done =
        open_event_retry(suffixed(base, L"_consumer_done"), kWaitMs);

    auto fail = [&](HRESULT hr, const char *what) -> int {
        std::printf("consumer: FAIL %s hr=0x%08lx\n", what,
                    (unsigned long)hr);
        if (shm) {
            shm->consumer_failed = 1;
            shm->consumer_read_hr = (LONG)hr;
            shm->consumer_done = 1;
            MemoryBarrier();
        }
        if (ev_consumer_opened)
            SetEvent(ev_consumer_opened);
        if (ev_frame_done)
            SetEvent(ev_frame_done);
        if (ev_consumer_done)
            SetEvent(ev_consumer_done);
        return 2;
    };

    if (!ev_consumer_ready || !ev_init_ready || !ev_consumer_opened ||
        !ev_frame_ready || !ev_frame_done || !ev_consumer_done) {
        return fail(HRESULT_FROM_WIN32(GetLastError()), "OpenEvent");
    }
    if (shm->magic != (LONG)kMagic)
        return fail(E_FAIL, "bad shared-memory magic");

    shm->consumer_pid = (LONG)GetCurrentProcessId();
    MemoryBarrier();
    if (!set_ev(ev_consumer_ready, "consumer: consumer_ready"))
        return fail(HRESULT_FROM_WIN32(GetLastError()), "set consumer_ready");

    if (!wait_ev(ev_init_ready, kWaitMs, "consumer: init_ready"))
        return fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "wait init_ready");
    if (shm->producer_failed) {
        const HRESULT phr = (HRESULT)shm->producer_hr;
        std::printf("consumer: producer failed hr=0x%08lx\n",
                    (unsigned long)phr);
        return fail(phr, "producer failed");
    }

    const ULONGLONG handle64 = shm->shared_handle;
    const int width = (int)shm->width;
    const int height = (int)shm->height;
    const DXGI_FORMAT fmt = (DXGI_FORMAT)shm->format;
    const int frames = (int)shm->frames;
    const bool nthandle = shm->nthandle != 0;
    std::printf("consumer: handle=0x%llx size=%dx%d format=%s nthandle=%d "
                "frames=%d\n",
                (unsigned long long)handle64, width, height, format_name(fmt),
                nthandle ? 1 : 0, frames);
    if (!handle64)
        return fail(E_INVALIDARG, "empty shared handle");
    if (frames < 1 || frames > kMaxFrames || width <= 0 || height <= 0)
        return fail(E_INVALIDARG, "bad shared metadata");

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = create_device(&dev, &ctx, "consumer");
    if (FAILED(hr))
        return fail(hr, "D3D11CreateDevice");
    adapter_name(dev, "consumer");

    ID3D11Texture2D *tex = nullptr;
    if (nthandle) {
        ID3D11Device1 *dev1 = nullptr;
        hr = dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
        print_hr("consumer: QueryInterface(ID3D11Device1)", hr);
        if (FAILED(hr))
            return fail(hr, "QueryInterface(ID3D11Device1)");
        hr = dev1->OpenSharedResource1((HANDLE)(uintptr_t)handle64,
                                       __uuidof(ID3D11Texture2D),
                                       (void **)&tex);
        dev1->Release();
        print_hr("consumer: OpenSharedResource1", hr);
    } else {
        hr = dev->OpenSharedResource((HANDLE)(uintptr_t)handle64,
                                     __uuidof(ID3D11Texture2D),
                                     (void **)&tex);
        print_hr("consumer: OpenSharedResource", hr);
    }
    shm->consumer_open_hr = (LONG)hr;
    MemoryBarrier();
    SetEvent(ev_consumer_opened);
    if (FAILED(hr) || !tex)
        return fail(FAILED(hr) ? hr : E_FAIL, "OpenSharedResource");

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    std::printf("consumer: opened desc=%ux%u format=%s misc=0x%x bind=0x%x "
                "mips=%u array=%u samples=%u\n",
                desc.Width, desc.Height, format_name(desc.Format),
                (unsigned)desc.MiscFlags, (unsigned)desc.BindFlags,
                desc.MipLevels, desc.ArraySize, desc.SampleDesc.Count);
    if ((int)desc.Width != width || (int)desc.Height != height ||
        desc.Format != fmt) {
        return fail(E_INVALIDARG, "opened description mismatch");
    }

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    ID3D11Texture2D *staging = nullptr;
    hr = dev->CreateTexture2D(&sd, nullptr, &staging);
    print_hr("consumer: CreateStagingTexture", hr);
    if (FAILED(hr))
        return fail(hr, "CreateStagingTexture");

    for (int f = 0; f < frames; ++f) {
        if (!wait_ev(ev_frame_ready, kWaitMs, "consumer: frame_ready"))
            return fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "wait frame_ready");
        if (shm->producer_failed)
            return fail((HRESULT)shm->producer_hr, "producer failed");

        LONG expected[4];
        for (int c = 0; c < 4; ++c)
            expected[c] = shm->expected[f][c];

        ctx->CopyResource(staging, tex);
        ctx->Flush();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            SetEvent(ev_frame_done);
            return fail(hr, "Map staging");
        }
        const BYTE *p = (const BYTE *)mapped.pData;
        LONG actual[4] = {p[0], p[1], p[2], p[3]};
        for (int c = 0; c < 4; ++c)
            shm->actual[f][c] = actual[c];

        const bool match = pixel_matches(expected, p);
        std::printf("consumer: frame %d pixel=%ld,%ld,%ld,%ld "
                    "expected=%ld,%ld,%ld,%ld %s\n",
                    f, actual[0], actual[1], actual[2], actual[3],
                    expected[0], expected[1], expected[2], expected[3],
                    match ? "OK" : "MISMATCH");
        ctx->Unmap(staging, 0);
        MemoryBarrier();
        SetEvent(ev_frame_done);
        if (!match) {
            shm->frame_mismatch = f;
            return fail(E_FAIL, "pixel mismatch");
        }
    }

    shm->consumer_read_hr = S_OK;
    shm->consumer_done = 1;
    MemoryBarrier();
    SetEvent(ev_consumer_done);
    std::printf("RESULT: PASS\n");

    if (staging)
        staging->Release();
    if (tex)
        tex->Release();
    if (ctx)
        ctx->Release();
    if (dev)
        dev->Release();
    if (nthandle && handle64)
        CloseHandle((HANDLE)(uintptr_t)handle64);
    if (ev_consumer_ready)
        CloseHandle(ev_consumer_ready);
    if (ev_init_ready)
        CloseHandle(ev_init_ready);
    if (ev_consumer_opened)
        CloseHandle(ev_consumer_opened);
    if (ev_frame_ready)
        CloseHandle(ev_frame_ready);
    if (ev_frame_done)
        CloseHandle(ev_frame_done);
    if (ev_consumer_done)
        CloseHandle(ev_consumer_done);
    UnmapViewOfFile(shm);
    CloseHandle(map);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    Options o;
    if (!parse_args(argc, argv, &o)) {
        usage();
        return 2;
    }
    o.name = sanitize_name(o.name);
    if (o.mode == Options::Mode::Producer)
        return run_producer(o);
    return run_consumer(o);
}
