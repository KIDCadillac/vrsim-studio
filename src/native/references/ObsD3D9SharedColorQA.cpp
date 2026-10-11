// Gate1 OBS shtex shared-color QA: own hidden window, own ordinary 9HAL(0x44) +
// ExHAL(0x46) devices, this-process only-read offset rediscovery, one scoped
// is_d3d9ex flag TRUE around a single CreateTexture opening the legacy shared
// handle of our own D3D11 X8R8G8B8->B8G8R8X8 64x64 texture, two GPU-stamped
// quadrant patterns verified via D3D11 staging readback. NOT SIM, NOT XR,
// NOT FPS. Compile: MSVC C++17 console, x64, Unicode. Never run from here.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>

// Reuse frozen Gate0 pure-CPU/read-only helpers via the include contract.
// Gate0's wmain is renamed to an unused entry; Gate1 defines its own below.
#define wmain ObsLayoutQA_UnusedGate0Entry
#include "C:/Users/KID/Documents/ChatGPT/项目经理/reviews/vrsim-highres-h2/source4096-output3072-runtime-qa-candidate-run1/qa/builds/native-sampling4096-output3072-run1/frozen-src/references/ObsD3D9LayoutQA.cpp"
#undef wmain

static_assert(kMaxFuncScanBytes + kMaxCmpSize - 1 == 221, "Gate0 scan span contract mismatch");
static_assert(kMaxCmpSize == 22, "Gate0 compare-size contract mismatch");

namespace g1 {

constexpr UINT W = 64, H = 64;
constexpr D3DFORMAT FMT22 = (D3DFORMAT)22;             // D3DFMT_X8R8G8B8 (actual SIM source)
constexpr DXGI_FORMAT FMT88 = (DXGI_FORMAT)88;         // DXGI_FORMAT_B8G8R8X8_UNORM (OBS map)
constexpr DWORD BEH_EX = 0x46, BEH_ORD = 0x44;         // Gate0-frozen behavior flags
constexpr uint32_t OFF_RANGE_MAX = 0xFFFF;             // Gate0-frozen displacement cap
constexpr int MAX_POLLS = 128;
constexpr DWORD DEADLINE_MS = 1000;

typedef HRESULT(WINAPI* PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);

struct Ctx {
    std::vector<std::string> errors;
    HMODULE d3d9 = nullptr, d3d11 = nullptr, dxgi = nullptr;
    PFN_D3D11_CREATE_DEVICE pfnD3D11CreateDevice = nullptr;
    PFN_CREATE_DXGI_FACTORY1 pfnCreateDXGIFactory1 = nullptr;
    HWND hwnd = nullptr;
    IDirect3D9* d9 = nullptr;
    IDirect3D9Ex* d9ex = nullptr;
    IDirect3DDevice9* ord = nullptr;      // ordinary HAL, behavior 0x44
    IDirect3DDevice9Ex* dex = nullptr;    // own Ex HAL, behavior 0x46
    bool ownOrdIsExQI = false;            // ordinary must NOT QI as Ex (canonical identity)
    bool ownExIsExQI = false;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    bool haveCp = false;
    LUID luid{};
    bool haveLuid = false;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter* adapter = nullptr;      // matched same-LUID adapter, held
    bool sameAdapter = false;
    DXGI_ADAPTER_DESC ad{};
    D3DADAPTER_IDENTIFIER9 ai{};
    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx11 = nullptr;
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    ID3D11Texture2D* shared11 = nullptr;
    D3D11_TEXTURE2D_DESC sharedActual{};
    bool sharedDescOk = false;
    HANDLE legacyHandle = nullptr;
    uint32_t clsoff = 0, isoff = 0;
    bool layoutProved = false;
    long long sigMatches = -1;
    std::string candStr;
    bool ordVtInD3d9 = false;
    uintptr_t ordParent = 0, exParent = 0;
    BOOL flagOriginal = TRUE;
    bool flagOriginalKnown = false, flagWasFalse = false, setTrueVerified = false;
    bool flagSeh = false, restoreSeh = false, restoreVerified = false;
    HRESULT createRc = E_FAIL;
    bool createRcKnown = false;
    IDirect3DTexture9* shared9 = nullptr;
    IDirect3DSurface9* copySurf = nullptr;
    bool sharedProof = false;
    IDirect3DTexture9* src9 = nullptr;
    IDirect3DSurface9* srcSurf = nullptr;
    IDirect3DQuery9* ev9 = nullptr;
    ID3D11Texture2D* stage11 = nullptr;
    ID3D11Query* ev11 = nullptr;
    int d3d9Polls = -1, d3d11Polls = -1;
    bool d3d9WaitOk = false, d3d11WaitOk = false, mapOk = false;
    bool mapFailStillDrawing = false;
    UINT mapRowPitch = 0;
    long long mis1 = -1, mis2 = -1;
    long long minX1 = -1, maxX1 = -1, minX2 = -1, maxX2 = -1;
    std::string raw1, raw2;
    bool twoExact = false;
    bool unsafeRetained = false;
    int legacyCloseCalls = 0;             // must remain 0: never Close legacy handle
    bool releasedAll = true;

    void err(const std::string& s) { errors.push_back(s); }
    void hr(const std::string& api, HRESULT h)
    {
        if (FAILED(h)) {
            char b[64];
            wsprintfA(b, " hr=0x%08lX", (unsigned long)h);
            err(api + b);
        }
    }
};

// Absolute-system-path loading only; owned module handles stay in Ctx on every
// path so the later Cleanup card releases exactly what this process owns.
bool SamePathCI(const wchar_t* a, int an, const wchar_t* b, int bn)
{
    if (an <= 0 || bn <= 0 || an != bn) return false;
    return CompareStringW(LOCALE_NEUTRAL, NORM_IGNORECASE, a, an, b, bn) == CSTR_EQUAL;
}

bool LoadOneFromSystem(Ctx& c, const wchar_t* name, HMODULE* slot, const std::wstring& expected)
{
    HMODULE m = LoadLibraryExW(expected.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    *slot = m; // actual handle (or null) recorded on all fail paths for Cleanup
    if (!m) { c.err("LoadLibraryExW(SEARCH_SYSTEM32) failed"); return false; }
    wchar_t got[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, got, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { c.err("GetModuleFileNameW unavailable or truncated"); return false; }
    if (!SamePathCI(got, (int)n, expected.c_str(), (int)expected.size())) {
        c.err("loaded module path is not the expected absolute system path");
        return false;
    }
    return true;
}

bool LoadDlls(Ctx& c)
{
    wchar_t sysBuf[MAX_PATH];
    DWORD sn = GetSystemDirectoryW(sysBuf, MAX_PATH);
    if (sn == 0 || sn >= MAX_PATH) { c.err("GetSystemDirectoryW failed or truncated; no relative fallback"); return false; }
    std::wstring sysDir(sysBuf, sn);
    if (!LoadOneFromSystem(c, L"d3d9.dll", &c.d3d9, sysDir + L"\\d3d9.dll")) return false;
    if (!LoadOneFromSystem(c, L"d3d11.dll", &c.d3d11, sysDir + L"\\d3d11.dll")) return false;
    if (!LoadOneFromSystem(c, L"dxgi.dll", &c.dxgi, sysDir + L"\\dxgi.dll")) return false;
    c.pfnD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(c.d3d11, "D3D11CreateDevice");
    c.pfnCreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(c.dxgi, "CreateDXGIFactory1");
    if (!c.pfnD3D11CreateDevice || !c.pfnCreateDXGIFactory1) { c.err("d3d11/dxgi exports missing"); return false; }
    return true;
}

SIZE_T ImageSize(HMODULE m)
{
    PIMAGE_NT_HEADERS nt = ImageNtHeader(m);
    return nt ? (SIZE_T)nt->OptionalHeader.SizeOfImage : 0;
}

bool VtInModule(HMODULE m, uintptr_t vt)
{
    SIZE_T img = ImageSize(m);
    return img && AddressInExecutableImageSection(m, vt, img, sizeof(uintptr_t));
}

HWND MakeHiddenWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"G1ShtexQaWnd";
    static bool reg = false;
    if (!reg) { if (!RegisterClassExW(&wc)) return nullptr; reg = true; }
    return CreateWindowExW(0, wc.lpszClassName, L"g1", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

void Show(HWND h) { SetWindowPos(h, HWND_BOTTOM, 0, 0, 64, 64, SWP_NOACTIVATE); ShowWindow(h, SW_HIDE); }

bool PumpBounded(int msgs)
{
    int n = 0;
    MSG m;
    while (n < msgs && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); ++n; }
    return true;
}

// Real QPC usage: QueryPerformanceCounter/Frequency return BOOL, not LARGE_INTEGER.
bool TimerStart(LARGE_INTEGER& f, LARGE_INTEGER& n0)
{
    return QueryPerformanceFrequency(&f) != FALSE && f.QuadPart > 0 && QueryPerformanceCounter(&n0) != FALSE;
}
bool TimerElapsedMs(LARGE_INTEGER& f, LARGE_INTEGER& n0, DWORD& ms)
{
    LARGE_INTEGER n;
    if (QueryPerformanceCounter(&n) == FALSE) return false;
    if (n.QuadPart < n0.QuadPart) return false;
    ms = (DWORD)((n.QuadPart - n0.QuadPart) * 1000 / f.QuadPart);
    return true;
}

bool WaitEvent9(Ctx& c, int& polls, bool& ok)
{
    LARGE_INTEGER f, n0;
    polls = 0;
    ok = false;
    if (!TimerStart(f, n0)) { c.err("QueryPerformance timer unavailable (event9)"); return false; }
    while (polls < MAX_POLLS) {
        HRESULT h = c.ev9->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        if (h == S_OK) { ok = true; return true; }
        if (FAILED(h)) { c.hr("IDirect3DQuery9::GetData(event9)", h); return false; }
        ++polls;
        DWORD el = 0;
        if (!TimerElapsedMs(f, n0, el)) { c.err("QueryPerformanceCounter failed (event9 wait)"); return false; }
        if (el >= DEADLINE_MS) return false;
        Sleep(1);
        PumpBounded(32);
    }
    return false;
}

bool WaitEvent11(Ctx& c, int& polls, bool& ok)
{
    LARGE_INTEGER f, n0;
    polls = 0;
    ok = false;
    if (!TimerStart(f, n0)) { c.err("QueryPerformance timer unavailable (event11)"); return false; }
    while (polls < MAX_POLLS) {
        BOOL signaled = FALSE;
        HRESULT h = c.ctx11->GetData(c.ev11, &signaled, sizeof(BOOL), 0);
        if (h == S_OK) { ok = (signaled == TRUE); return ok; } // S_OK + TRUE BOOL = complete
        if (FAILED(h) && h != S_FALSE) { c.hr("ID3D11DeviceContext::GetData(event11)", h); return false; }
        ++polls;
        DWORD el = 0;
        if (!TimerElapsedMs(f, n0, el)) { c.err("QueryPerformanceCounter failed (event11 wait)"); return false; }
        if (el >= DEADLINE_MS) return false;
        Sleep(1);
    }
    return false;
}

// Step 1: own devices + same-adapter proof (DXGI LUID from our own Ex parent).
bool InitDevicesAndAdapter(Ctx& c)
{
    c.hwnd = MakeHiddenWindow();
    if (!c.hwnd) { c.err("own hidden 64x64 window creation failed"); return false; }
    Show(c.hwnd);
    PFN_Direct3DCreate9 create9 = (PFN_Direct3DCreate9)GetProcAddress(c.d3d9, "Direct3DCreate9");
    PFN_Direct3DCreate9Ex create9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(c.d3d9, "Direct3DCreate9Ex");
    if (!create9 || !create9Ex) { c.err("d3d9 exports missing"); return false; }
    HRESULT h;
    // PFN_Direct3DCreate9 returns IDirect3D9* directly; HRESULT is diagnostic only.
    c.d9 = create9(D3D_SDK_VERSION);
    c.hr("Direct3DCreate9", c.d9 ? S_OK : E_FAIL);
    if (!c.d9) { c.err("Direct3DCreate9 returned null parent"); return false; }
    h = create9Ex(D3D_SDK_VERSION, &c.d9ex);
    c.hr("Direct3DCreate9Ex", h);
    if (FAILED(h)) return false;
    h = c.d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &c.ai);
    c.hr("IDirect3D9::GetAdapterIdentifier", h);
    if (FAILED(h)) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = FMT22;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.hDeviceWindow = c.hwnd;
    h = c.d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, c.hwnd, BEH_ORD, &pp, &c.ord);
    c.hr("IDirect3D9::CreateDevice(ord HAL 0x44)", h);
    if (FAILED(h)) return false;
    h = c.d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, c.hwnd, BEH_EX, &pp, nullptr, &c.dex);
    c.hr("IDirect3D9Ex::CreateDeviceEx(Ex HAL 0x46)", h);
    if (FAILED(h)) return false;
    h = c.ord->GetCreationParameters(&c.cp);
    c.haveCp = SUCCEEDED(h);
    c.hr("IDirect3DDevice9::GetCreationParameters", h);
    if (!c.haveCp) return false;
    IDirect3DDevice9Ex* qi = nullptr;
    c.ownOrdIsExQI = SUCCEEDED(c.ord->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&qi));
    if (qi) qi->Release();
    qi = nullptr;
    c.ownExIsExQI = SUCCEEDED(c.dex->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&qi));
    if (qi) qi->Release();
    if (!c.ownExIsExQI || c.ownOrdIsExQI) { c.err("own device identity gate failed"); return false; }
    h = c.d9ex->GetAdapterLUID(c.cp.AdapterOrdinal, &c.luid);
    c.hr("IDirect3D9Ex::GetAdapterLUID(ordinal from actual GetCreationParameters)", h);
    if (FAILED(h)) return false;
    c.haveLuid = true;
    h = c.pfnCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&c.factory);
    c.hr("CreateDXGIFactory1", h);
    if (FAILED(h)) return false;
    int matches = 0;
    for (UINT i = 0; i < 16; ++i) {
        IDXGIAdapter* a = nullptr;
        HRESULT e = c.factory->EnumAdapters(i, &a);
        if (e == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(e)) { c.hr("EnumAdapters", e); break; }
        DXGI_ADAPTER_DESC d{};
        HRESULT g = a->GetDesc(&d);
        bool same = SUCCEEDED(g) && d.AdapterLuid.HighPart == c.luid.HighPart && d.AdapterLuid.LowPart == c.luid.LowPart;
        if (same) { ++matches; if (!c.adapter) c.adapter = a; else a->Release(); if (matches > 1) break; c.ad = d; }
        else a->Release();
    }
    c.sameAdapter = (matches == 1 && c.adapter != nullptr);
    if (!c.sameAdapter) { c.err("same-adapter LUID gate: not exactly one DXGI adapter match"); return false; }
    bool idsOk = c.ai.VendorId == c.ad.VendorId && c.ai.DeviceId == c.ad.DeviceId &&
                 c.ai.SubSysId == c.ad.SubSysId && c.ai.Revision == c.ad.Revision;
    if (!idsOk) { c.err("same-adapter gate: DXGI vs D3D9 identifier Vendor/Device/Subsystem/Revision mismatch"); return false; }
    D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    h = c.pfnD3D11CreateDevice(c.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               lvls, 3, D3D11_SDK_VERSION, &c.dev11, &c.fl, &c.ctx11);
    c.hr("D3D11CreateDevice(same-LUID adapter, UNKNOWN driver, BGRA_SUPPORT)", h);
    if (FAILED(h)) return false;
    if (!c.dev11 || !c.ctx11) { c.err("D3D11 device/context null"); return false; }
    return true;
}

// Step 2: our own shared D3D11 texture (format 88) -> legacy non-NT handle.
bool MakeShared11(Ctx& c)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1; d.Format = FMT88; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT h = c.dev11->CreateTexture2D(&d, nullptr, &c.shared11);
    c.hr("ID3D11Device::CreateTexture2D(shared 88)", h);
    if (FAILED(h)) return false;
    c.shared11->GetDesc(&c.sharedActual);
    c.sharedDescOk = c.sharedActual.Width == W && c.sharedActual.Height == H && c.sharedActual.Format == FMT88 &&
                     c.sharedActual.MipLevels == 1 && c.sharedActual.ArraySize == 1 &&
                     c.sharedActual.SampleDesc.Count == 1 && c.sharedActual.Usage == D3D11_USAGE_DEFAULT &&
                     (c.sharedActual.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) ==
                         (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
                     (c.sharedActual.MiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0;
    if (!c.sharedDescOk) { c.err("actual shared desc mismatch"); return false; }
    IDXGIResource* r = nullptr;
    h = c.shared11->QueryInterface(__uuidof(IDXGIResource), (void**)&r);
    c.hr("ID3D11Texture2D::QueryInterface(IDXGIResource)", h);
    if (FAILED(h)) return false;
    h = r->GetSharedHandle(&c.legacyHandle);
    r->Release();
    c.hr("IDXGIResource::GetSharedHandle(legacy)", h);
    if (FAILED(h) || c.legacyHandle == nullptr) { c.err("legacy shared handle invalid"); return false; }
    return true;
}

// This-process-only rediscovery adapted from the frozen Gate0 flow: Ex device
// vtable slot 125 (CheckResourceResidency), whole-221-byte executable-section
// gate, one safe 221-byte copy, 200 start positions x 4 existing masked
// signatures, unique legal hit only, off1=device->parent / off2=parent BOOL in
// the existing extraction order (never swapped, never from history). Known
// parents must equal this run's own c.d9ex / c.d9; canonical identity via
// GetDirect3D + QI(IID_IUnknown) only after address proof. Read-only: writes
// nothing, no CreateTexture, no GPU, no message pump.
void DiscoverLayout(Ctx& c)
{
    c.layoutProved = false;
    c.clsoff = 0;
    c.isoff = 0;
    c.ordParent = 0;
    c.exParent = 0;
    c.candStr.clear();
    if (!c.d3d9 || !c.dex || !c.ord || !c.d9ex || !c.d9) { c.err("DiscoverLayout prerequisites missing"); return; }
    SIZE_T img = ImageSize(c.d3d9);
    if (!img) { c.err("d3d9 image size unavailable"); return; }
    uint64_t vt = 0, crr = 0;
    if (!ReadU64LE((const void*)c.dex, &vt) || vt == 0 || (vt & 7)) { c.err("Ex device vtable pointer read rejected"); return; }
    if (vt + 125ull * 8ull < vt) { c.err("vtable slot address overflow"); return; }
    if (!ReadU64LE((const void*)(uintptr_t)(vt + 125ull * 8ull), &crr) || crr == 0 || (crr & 7)) {
        c.err("CheckResourceResidency slot read rejected");
        return;
    }
    const size_t kReadSpan = kMaxFuncScanBytes + kMaxCmpSize - 1; // 221
    if (!AddressInExecutableImageSection(c.d3d9, (uintptr_t)crr, img, kReadSpan)) {
        c.err("CheckResourceResidency not fully inside one executable section of the system d3d9.dll image; refusing to scan");
        return;
    }
    uint8_t spanBuf[kMaxFuncScanBytes + kMaxCmpSize - 1];
    if (!ReadBytesSafe((const void*)(uintptr_t)crr, spanBuf, kReadSpan)) {
        c.err("221-byte safe copy of scan range failed");
        return;
    }
    struct Hit { size_t index; size_t pattern; uint32_t a; uint32_t b; };
    std::vector<Hit> hits;
    bool overCap = false;
    for (size_t i = 0; i < kMaxFuncScanBytes && !overCap; ++i) {
        for (size_t j = 0; j < 4 && !overCap; ++j) {
            uint32_t a = 0, b = 0;
            if (MaskedSignatureMatch(&spanBuf[i], j, &a, &b)) {
                if (a > OFF_RANGE_MAX || b > OFF_RANGE_MAX) { overCap = true; continue; }
                hits.push_back({ i, j, a, b });
            }
        }
    }
    if (overCap) {
        c.err("signature matched with displacement beyond 0xFFFF cap; discovery refused outright, no later candidate may pass");
        return; // sigMatches stays unknown(-1); layoutProved stays false; nothing published
    }
    c.sigMatches = (long long)hits.size();
    for (size_t k = 0; k < hits.size(); ++k)
        c.candStr += (k ? "," : "") + std::to_string(hits[k].index) + "/" + std::to_string(hits[k].pattern) +
                     "/" + std::to_string(hits[k].a) + "/" + std::to_string(hits[k].b);
    if (hits.empty()) {
        c.err("no masked signature matched in CheckResourceResidency prologue");
        return;
    }
    if (hits.size() != 1) { c.err("multiple distinct signature matches (ambiguity)"); return; }
    const uint32_t off1 = hits[0].a; // device -> parent, existing order
    const uint32_t off2 = hits[0].b; // parent is-d3d9ex BOOL, existing order
    uint64_t ip = 0;
    if (!ReadU64LE((const uint8_t*)(const void*)c.dex + off1, &ip) || ip != (uint64_t)(uintptr_t)c.d9ex) {
        c.err("Ex internal-parent pointer does not equal this run's own Ex parent");
        return;
    }
    ip = 0;
    if (!ReadU64LE((const uint8_t*)(const void*)c.ord + off1, &ip) || ip != (uint64_t)(uintptr_t)c.d9) {
        c.err("ordinary internal-parent pointer does not equal this run's own ordinary parent");
        return;
    }
    IDirect3D9* pub = nullptr;
    HRESULT hgd = c.ord->GetDirect3D(&pub); // public method on our own device
    if (FAILED(hgd) || pub == nullptr || (uint64_t)(uintptr_t)pub != (uint64_t)(uintptr_t)c.d9 ||
        ip != (uint64_t)(uintptr_t)c.d9) {
        c.hr("IDirect3DDevice9::GetDirect3D", hgd);
        c.err("plain base-parent address consistency failed; refused COM calls on unverified internal pointer");
        if (pub) pub->Release();
        return;
    }
    bool canonicalOk = false;
    {
        IUnknown* u1 = nullptr;
        IUnknown* u2 = nullptr;
        IUnknown* u3 = nullptr;
        const HRESULT q1 = c.d9->QueryInterface(IID_IUnknown, (void**)&u1);
        const HRESULT q2 = pub->QueryInterface(IID_IUnknown, (void**)&u2);
        const HRESULT q3 = reinterpret_cast<IDirect3D9*>((uintptr_t)ip)
                              ->QueryInterface(IID_IUnknown, (void**)&u3); // address proven == c.d9
        canonicalOk = SUCCEEDED(q1) && SUCCEEDED(q2) && SUCCEEDED(q3) && u1 && u2 && u3 && u1 == u2 && u1 == u3;
        if (u1) u1->Release();
        if (u2) u2->Release();
        if (u3) u3->Release();
    }
    pub->Release();
    if (!canonicalOk) { c.err("canonical COM identity (QI IUnknown) mismatch across the three known parents"); return; }
    // BOOL gates read only at the already-proven known parents; never write here.
    uint32_t be = 0, bo = 1;
    if (!ReadU32LE((const uint8_t*)(const void*)c.d9ex + off2, &be) || be != TRUE) {
        c.err("Ex parent is-d3d9ex BOOL is not exactly TRUE at known address");
        return;
    }
    if (!ReadU32LE((const uint8_t*)(const void*)c.d9 + off2, &bo) || bo != FALSE) {
        c.err("ordinary parent is-d3d9ex BOOL is not exactly FALSE at known address");
        return;
    }
    // All gates passed: publish exactly once.
    c.clsoff = off1;
    c.isoff = off2;
    c.exParent = (uintptr_t)c.d9ex;
    c.ordParent = (uintptr_t)c.d9;
    c.layoutProved = true;
}

// POD leaf: no C++ destructors -> legal with __try/__finally (MSVC C2712).
struct FlagOut {
    BOOL original;
    HRESULT createRc;
    bool setTrueReadback;
    bool exceptionDuring;
    bool exceptionDuringRestore;
};
bool g1_writable(const void* p)
{
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery(p, &m, sizeof(m)) == 0) return false;
    if (m.State != MEM_COMMIT || (m.Protect & PAGE_GUARD)) return false;
    if (m.Protect & (PAGE_NOACCESS | 0x100)) return false;
    if ((uintptr_t)m.BaseAddress + m.RegionSize < (uintptr_t)p + 4) return false;
    DWORD b = m.Protect & 0xFF;
    return b == PAGE_READWRITE || b == PAGE_WRITECOMBINE || b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY;
}
void ToggleFlagCreateLeaf(IDirect3DDevice9* dev, void* flagAddr, IDirect3DTexture9** ppTex, HANDLE shared,
                          FlagOut* out)
{
    BOOL orig = FALSE;
    if (!g1_writable(flagAddr) || !ReadBytesSafe(flagAddr, &orig, 4) || orig != FALSE) {
        out->original = orig;
        out->exceptionDuring = true;
        return;
    }
    out->original = orig;
    __try {
        __try {
            *(volatile BOOL*)flagAddr = TRUE;
            out->setTrueReadback = (*(volatile BOOL*)flagAddr) == TRUE;
            out->createRc = dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, FMT22, D3DPOOL_DEFAULT, ppTex,
                                               &shared);
        } __finally {
            *(volatile BOOL*)flagAddr = orig;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->exceptionDuring = true;
    }
}

void FlagWriteAndOpen(Ctx& c)
{
    c.flagOriginalKnown = ReadBytesSafe((void*)(c.ordParent + c.isoff), &c.flagOriginal, 4) && c.flagOriginal == FALSE;
    if (!c.flagOriginalKnown) { c.err("pre-write proof of writable FALSE BOOL failed"); return; }
    c.flagWasFalse = true;
    c.shared9 = nullptr;
    FlagOut out = { TRUE, E_FAIL, false, false, false };
    ToggleFlagCreateLeaf(c.ord, (void*)(c.ordParent + c.isoff), &c.shared9, c.legacyHandle, &out);
    c.setTrueVerified = out.setTrueReadback;
    c.createRc = out.createRc;
    c.createRcKnown = true;
    c.flagSeh = out.exceptionDuring;
    c.restoreSeh = out.exceptionDuringRestore;
    BOOL now = TRUE;
    c.restoreVerified = !c.flagSeh && ReadBytesSafe((void*)(c.ordParent + c.isoff), &now, 4) && now == FALSE &&
                       c.flagOriginal == FALSE;
    if (!c.restoreVerified) {
        c.unsafeRetained = true;
        c.err("flag restore unproven: sharedProof=false, refs retained, no further GPU calls");
        return;
    }
    if (c.flagSeh) { c.err("SEH during scoped flag/CreateTexture"); return; }
    if (FAILED(c.createRc)) { c.hr("IDirect3DDevice9::CreateTexture(shared open, flag scoped)", c.createRc); return; }
    if (c.shared9 == nullptr) { c.err("CreateTexture returned success but shared9 is null; surface open refused"); return; }
    HRESULT h = c.shared9->GetSurfaceLevel(0, &c.copySurf);
    c.hr("IDirect3DTexture9::GetSurfaceLevel(0)", h);
    c.sharedProof = SUCCEEDED(h) && c.copySurf != nullptr;
    if (!c.sharedProof) c.err("sharedProof false: surface open failed");
}

// Source RT + StretchRect copy, GPU-stamped via event waits, no Present/Reset.
bool FillCopy(Ctx& c, const UINT quad[4])
{
    HRESULT h;
    if (!c.src9) {
        h = c.ord->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, FMT22, D3DPOOL_DEFAULT, &c.src9, nullptr);
        c.hr("IDirect3DDevice9::CreateTexture(source X8 RT)", h);
        if (FAILED(h)) return false;
        h = c.src9->GetSurfaceLevel(0, &c.srcSurf);
        c.hr("source GetSurfaceLevel", h);
        if (FAILED(h)) return false;
        h = c.ord->CreateQuery(D3DQUERYTYPE_EVENT, &c.ev9);
        c.hr("CreateQuery(EVENT)", h);
        if (FAILED(h)) return false;
    }
    const RECT q[4] = { { 0, 0, 32, 32 }, { 32, 0, 64, 32 }, { 0, 32, 32, 64 }, { 32, 32, 64, 64 } };
    for (int i = 0; i < 4; ++i) {
        h = c.ord->ColorFill(c.srcSurf, &q[i], (D3DCOLOR)quad[i]);
        c.hr("ColorFill", h);
        if (FAILED(h)) return false;
    }
    h = c.ord->StretchRect(c.srcSurf, nullptr, c.copySurf, nullptr, D3DTEXF_NONE);
    c.hr("StretchRect(ord X8 RT -> shared X8 surface)", h);
    if (FAILED(h)) return false;
    h = c.ev9->Issue(D3DISSUE_END);
    c.hr("Issue(D3DISSUE_END)", h);
    if (FAILED(h)) return false;
    if (!WaitEvent9(c, c.d3d9Polls, c.d3d9WaitOk)) { c.err("D3D9 event wait: pending past 128 polls/1000ms deadline"); return false; }
    D3D11_TEXTURE2D_DESC sd{};
    if (!c.stage11) {
        sd.Width = W; sd.Height = H; sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1;
        sd.Format = FMT88; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hs = c.dev11->CreateTexture2D(&sd, nullptr, &c.stage11);
        c.hr("CreateTexture2D(staging CPU_READ)", hs);
        if (FAILED(hs)) return false;
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        hs = c.dev11->CreateQuery(&qd, &c.ev11);
        c.hr("ID3D11Device::CreateQuery(EVENT)", hs);
        if (FAILED(hs)) return false;
    }
    c.ctx11->CopyResource(c.stage11, c.shared11);
    c.ctx11->Flush();
    c.ctx11->End(c.ev11); // issue the EVENT query
    if (!WaitEvent11(c, c.d3d11Polls, c.d3d11WaitOk)) { c.err("D3D11 event wait: pending past 128 polls/1000ms deadline"); return false; }
    return true;
}

void ReadbackCheck(Ctx& c, const UINT expect[4], long long& mis, long long& minX, long long& maxX, std::string& raw)
{
    c.mapOk = false;
    mis = -1;
    D3D11_MAPPED_SUBRESOURCE m{};
    LARGE_INTEGER f, n0;
    const bool timerOk = TimerStart(f, n0);
    HRESULT h = c.ctx11->Map(c.stage11, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    bool mapped = (h == S_OK);
    int polls = 0;
    while (!mapped) {
        if (h == DXGI_ERROR_WAS_STILL_DRAWING) {
            c.mapFailStillDrawing = true; // the only legal pending code for DO_NOT_WAIT Map
        } else {
            c.hr("ID3D11DeviceContext::Map(READ|DO_NOT_WAIT)", h); // any other HRESULT: fail, no further Map
            return;
        }
        if (!timerOk) { c.err("QueryPerformance timer unavailable (Map wait)"); return; }
        DWORD el = 0;
        if (!TimerElapsedMs(f, n0, el)) { c.err("QueryPerformanceCounter failed (Map wait)"); return; }
        if (polls >= MAX_POLLS || el >= DEADLINE_MS) { c.err("Map still drawing past 128 polls/1000ms budget"); return; }
        ++polls;
        Sleep(1);
        h = c.ctx11->Map(c.stage11, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        mapped = (h == S_OK);
    }
    struct UnmapGuard {
        ID3D11DeviceContext* ctx;
        ID3D11Texture2D* res;
        ~UnmapGuard() { ctx->Unmap(res, 0); } // exactly once per successful Map, on every exit path
    } guard = { c.ctx11, c.stage11 };
    if (m.pData == nullptr) { c.err("Map succeeded but pData is null"); return; }
    if (m.RowPitch < (UINT)W * 4u) { c.err("RowPitch < width*4"); return; }
    const size_t lastExtent = (size_t)(H - 1u) * (size_t)m.RowPitch + (size_t)W * 4u;
    if (lastExtent < (size_t)W * 4u) { c.err("readback extent SIZE_T overflow check failed"); return; }
    const uint8_t* p = (const uint8_t*)m.pData;
    long long mism = 0;
    long long mnx = 1LL << 31, mxx = -1;
    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            const uint8_t* px = p + (size_t)y * (size_t)m.RowPitch + (size_t)x * 4u;
            // bytes B,G,R,X; expect 0xRRGGBB; 4th byte is X (recorded only, never claimed as alpha)
            const UINT got = ((UINT)px[2] << 16) | ((UINT)px[1] << 8) | (UINT)px[0];
            const int qi = ((y < 32) ? 0 : 2) + ((x < 32) ? 0 : 1);
            if (got != expect[qi]) ++mism;
            if ((long long)px[3] < mnx) mnx = px[3];
            if ((long long)px[3] > mxx) mxx = px[3];
        }
    }
    char b[16];
    wsprintfA(b, "%02X%02X%02X%02X", p[0], p[1], p[2], p[3]); // first raw pixel in B,G,R,X order
    raw = b;
    c.mapRowPitch = m.RowPitch;
    mis = mism;
    minX = mnx;
    maxX = mxx;
    c.mapOk = true; // only after the complete pixel loop
}

void TwoRoundColors(Ctx& c)
{
    // D3DCOLOR 0xRRGGBB; round 2 is a permutation of round 1 so a stale frame fails.
    const UINT p1[4] = { 0x0000FF, 0x00FF00, 0xFF0000, 0xFFFFFF }; // blue, green, red, white
    const UINT p2[4] = { 0x00FF00, 0xFF0000, 0xFFFFFF, 0x0000FF }; // green, red, white, blue
    bool ok1 = FillCopy(c, p1);
    if (ok1) ReadbackCheck(c, p1, c.mis1, c.minX1, c.maxX1, c.raw1);
    bool ok2 = ok1 && FillCopy(c, p2);
    if (ok2) ReadbackCheck(c, p2, c.mis2, c.minX2, c.maxX2, c.raw2);
    c.twoExact = ok1 && ok2 && c.mapOk && c.mis1 == 0 && c.mis2 == 0;
    if (!c.twoExact) c.err("two-round color gate not fully passed");
}

void Cleanup(Ctx& c)
{
    c.releasedAll = false;
    if (c.unsafeRetained) { c.err("cleanup refused while unsafeRetained; own refs quarantined"); return; }
    struct R {
        // Release() returning nonzero is NOT treated as an error; driver-internal counts are not ours to guess.
        static void S(IUnknown** p) { if (*p) { (*p)->Release(); *p = nullptr; } }
    };
    R::S((IUnknown**)&c.copySurf);
    R::S((IUnknown**)&c.shared9);
    R::S((IUnknown**)&c.srcSurf);
    R::S((IUnknown**)&c.src9);
    R::S((IUnknown**)&c.ev9);
    R::S((IUnknown**)&c.dex);
    R::S((IUnknown**)&c.ord);
    R::S((IUnknown**)&c.ev11);
    R::S((IUnknown**)&c.stage11);
    R::S((IUnknown**)&c.shared11);
    R::S((IUnknown**)&c.ctx11);
    R::S((IUnknown**)&c.dev11);
    R::S((IUnknown**)&c.adapter);
    R::S((IUnknown**)&c.factory);
    R::S((IUnknown**)&c.d9ex);
    R::S((IUnknown**)&c.d9);
    // legacyHandle: never CloseHandle, never DuplicateHandle (close count stays 0); the opaque number is
    // dropped only after the resource COM refs it rides on are released.
    c.legacyHandle = nullptr;
    if (c.hwnd) {
        if (DestroyWindow(c.hwnd)) c.hwnd = nullptr;
        else {
            char b[32];
            wsprintfA(b, "DestroyWindow failed err=%lu", GetLastError());
            c.err(b);
        }
    }
    // Exactly one FreeLibrary per LoadDlls-owning reference, only after COM and window cleanup.
    HMODULE* slots[3] = { &c.dxgi, &c.d3d11, &c.d3d9 };
    for (int i = 0; i < 3; ++i) {
        if (*slots[i]) {
            if (FreeLibrary(*slots[i])) {
                *slots[i] = nullptr;
            } else {
                char b[48];
                wsprintfA(b, "FreeLibrary failed err=%lu (module stays mapped)", GetLastError());
                c.err(b);
            }
        }
    }
    c.releasedAll = !c.unsafeRetained && !c.copySurf && !c.shared9 && !c.srcSurf && !c.src9 && !c.ev9 &&
                   !c.dex && !c.ord && !c.ev11 && !c.stage11 && !c.shared11 && !c.ctx11 && !c.dev11 &&
                   !c.adapter && !c.factory && !c.d9ex && !c.d9 && !c.hwnd && !c.d3d9 && !c.d3d11 && !c.dxgi;
    if (!c.releasedAll) c.err("cleanup incomplete; releasedAll=false (result not fabricated)");
}

bool EmitJson(Ctx& c, const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                           INVALID_HANDLE_VALUE);
    if (f == INVALID_HANDLE_VALUE) { c.err("JSON output not brand-new or unopenable; nothing written"); return false; }
    const bool ok = c.layoutProved && c.sameAdapter && c.sharedProof && c.twoExact && c.restoreVerified &&
                    c.legacyCloseCalls == 0 && !c.unsafeRetained && c.errors.empty() && c.releasedAll &&
                    c.sharedDescOk && c.setTrueVerified;
    JsonObj j;
    j.str("scope", "OWN_DEVICE_SHARED_COLOR_ONLY_NOT_SIM_NOT_XR_NOT_FPS");
    j.boolean("layoutThisProcessProved", c.layoutProved);
    j.boolean("sameAdapterProved", c.sameAdapter);
    j.num("d3d9AdapterOrdinal", c.haveCp ? (long long)c.cp.AdapterOrdinal : -1);
    j.num("d3d9DeviceTypeActual", c.haveCp ? (long long)c.cp.DeviceType : -1);
    j.num("d3d9BehaviorActual", c.haveCp ? (long long)c.cp.BehaviorFlags : -1);
    j.num("requestedOrdBehavior", (long long)BEH_ORD);
    j.num("requestedExBehavior", (long long)BEH_EX);
    j.str("luid", c.haveLuid ? std::to_string(c.luid.HighPart) + ":" + std::to_string(c.luid.LowPart) : "null");
    j.num("dxgiVendorId", (long long)c.ad.VendorId);
    j.num("dxgiDeviceId", (long long)c.ad.DeviceId);
    j.num("dxgiSubsystemId", (long long)c.ad.SubSysId);
    j.num("dxgiRevision", (long long)c.ad.Revision);
    j.num("d3d9VendorId", (long long)c.ai.VendorId);
    j.num("d3d9DeviceId", (long long)c.ai.DeviceId);
    j.num("d3d9SubSysId", (long long)c.ai.SubSysId);
    j.num("d3d9Revision", (long long)c.ai.Revision);
    j.num("d3d11FeatureLevel", (long long)c.fl);
    j.nullableNum("d3d9SigMatches", c.sigMatches >= 0, c.sigMatches);
    j.nullableStr("d3d9CandidatePairs", !c.candStr.empty(), c.candStr);
    j.nullableNum("clsoff", c.layoutProved, (long long)c.clsoff);
    j.nullableNum("isoff", c.layoutProved, (long long)c.isoff);
    j.boolean("ownOrdIsExQI", c.ownOrdIsExQI);
    j.boolean("ownExIsExQI", c.ownExIsExQI);
    j.boolean("ordParentVtInD3d9ExecSection", c.layoutProved && c.ordVtInD3d9);
    j.num("sourceD3DFormat", 22);
    j.num("dxgiFormat", 88);
    j.boolean("actualSharedDescMatches", c.sharedDescOk);
    j.boolean("flagOriginalFalse", c.flagOriginalKnown && c.flagOriginal == FALSE);
    j.boolean("flagAfterRestoreFalse", c.restoreVerified);
    j.boolean("flagRestoreVerified", c.restoreVerified);
    j.boolean("flagSetTrueReadback", c.setTrueVerified);
    j.boolean("flagSEH", c.flagSeh);
    j.boolean("restoreSEH", c.restoreSeh);
    j.boolean("createTextureRcKnown", c.createRcKnown);
    j.nullableNum("createTextureRc", c.createRcKnown, (long long)(long)c.createRc);
    j.boolean("sharedProof", c.sharedProof);
    j.boolean("twoChangedPatternsRGBExact", c.twoExact);
    j.nullableNum("round1RGBMismatch", c.mis1 >= 0, c.mis1);
    j.nullableNum("round2RGBMismatch", c.mis2 >= 0, c.mis2);
    j.nullableStr("round1FirstRaw", !c.raw1.empty(), c.raw1);
    j.nullableStr("round2FirstRaw", !c.raw2.empty(), c.raw2);
    j.nullableNum("round1XMin", c.minX1 >= 0, c.minX1);
    j.nullableNum("round1XMax", c.minX1 >= 0, c.maxX1);
    j.nullableNum("round2XMin", c.minX2 >= 0, c.minX2);
    j.nullableNum("round2XMax", c.minX2 >= 0, c.maxX2);
    j.nullableNum("mapRowPitch", c.mapOk, (long long)c.mapRowPitch);
    j.boolean("mapOk", c.mapOk);
    j.boolean("mapFailWAS_STILL_DRAWING", c.mapFailStillDrawing);
    j.nullableNum("d3d9Polls", c.d3d9Polls >= 0, c.d3d9Polls);
    j.nullableNum("d3d11Polls", c.d3d11Polls >= 0, c.d3d11Polls);
    j.boolean("d3d9WaitWithinDeadline", c.d3d9WaitOk);
    j.boolean("d3d11WaitWithinDeadline", c.d3d11WaitOk);
    j.num("legacyHandleCloseCount", (long long)c.legacyCloseCalls);
    j.boolean("legacyHandleCloseCount0", c.legacyCloseCalls == 0);
    j.boolean("unsafeRetained", c.unsafeRetained);
    j.boolean("cleanupComplete", c.releasedAll);
    j.boolean("fpsUnknown", true);
    j.boolean("noSIMTouched", true);
    j.str("productSupport", "UNPROVEN");
    j.boolean("allGatesPassed", ok);
    j.add("errors", JsonObj::arrayOfStrings(c.errors));
    std::string s = "{" + j.body +
        ",\"limits\":\"finite driver API waits (128 polls/1000ms) are not a hard driver deadline; "
        "4th byte of 22/88 is X, not claimed as meaningful alpha\"\x7d";
    if (s.size() > (size_t)0xFFFFFFFFull) {
        CloseHandle(f); // single close on this path too
        c.err("serialized JSON exceeds DWORD write limit; refused");
        fprintf(stderr, "gate1 output refused: serialized size exceeds DWORD max; must exit non-zero\n");
        return false;
    }
    DWORD wr = 0;
    const BOOL wOk = WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr);
    const DWORD we = wOk ? 0 : GetLastError();
    const BOOL flOk = FlushFileBuffers(f);
    const DWORD fe = flOk ? 0 : GetLastError();
    const BOOL clOk = CloseHandle(f); // output file is closed exactly once on every path
    const DWORD ce = clOk ? 0 : GetLastError();
    const bool ioOk = wOk != FALSE && wr == (DWORD)s.size() && flOk != FALSE && clOk != FALSE;
    if (!ioOk) {
        c.err("output IO failed (write/flush/close); this file receipt must be rejected regardless of contents");
        fprintf(stderr, "gate1 output IO failed: write=%d wr=%lu errW=%lu errF=%lu errC=%lu; exit non-zero\n",
                (int)wOk, (unsigned long)wr, (unsigned long)we, (unsigned long)fe, (unsigned long)ce);
    }
    return ioOk;
}

void Run(Ctx& c)
{
    if (!LoadDlls(c)) return;
    if (!InitDevicesAndAdapter(c)) return;
    if (!MakeShared11(c)) return;
    DiscoverLayout(c);
    if (!c.layoutProved) return;
    FlagWriteAndOpen(c);
    if (!c.restoreVerified) return;   // unsafeRetained already flagged; refs retained
    if (!c.sharedProof) return;
    TwoRoundColors(c);
}

} // namespace g1

#if !defined(VRSIM_PRODUCTION_SHARED_HELPERS)
int wmain(int argc, wchar_t** argv)
{
    g1::Ctx c;
    c.releasedAll = false; // cleanup result must never rely on the Ctx initializer
    if (argc != 2) {
        fwprintf(stderr, L"usage: ObsD3D9SharedColorQA.exe <brand-new-output-json>\n");
        return 2;
    }
    if (GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"refusing to overwrite existing file; exit 2, nothing written\n");
        return 2;
    }
    g1::Run(c);
    if (!c.unsafeRetained) g1::Cleanup(c); // unsafe path: refs quarantined, releasedAll stays false
    const bool pass = c.layoutProved && c.sameAdapter && c.sharedProof && c.twoExact && c.restoreVerified &&
                      c.legacyCloseCalls == 0 && !c.unsafeRetained && c.errors.empty() && c.releasedAll &&
                      c.sharedDescOk && c.setTrueVerified;
    if (!g1::EmitJson(c, argv[1])) {
        fwprintf(stderr, L"gate1 output IO/serialization failed; exit 2, receipt must be rejected\n");
        return 2;
    }
    return pass ? 0 : 1; // no automatic retry; only all real gates + clean IO reach exit 0
}

#endif // !VRSIM_PRODUCTION_SHARED_HELPERS
