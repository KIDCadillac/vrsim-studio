// VRSIMNative.cpp — VRSIM Native XR Backend (in-process inside iiQWorks.Sim).
// VRSIM-PRODUCT-01: synthetic quad visual hold. No SIM scene content yet.
//
// Design constraints (from proven CARD15/Card17B2 baseline):
//  - XR_NO_PROTOTYPES: every OpenXR function resolved via xrGetInstanceProcAddr.
//  - Single OpenXR owner: one worker thread pumps events + runs the frame cycle.
//  - Session state machine: BeginSession on READY, EndSession on STOPPING,
//    frame cycle only while session running, always EndFrame after BeginFrame.
//  - Swapchain write pattern: Acquire -> Wait -> CopyResource(quadTex) -> Release.
//    Swapchain image texture pointers are cached once after swapchain creation.
//  - Key evidence export: inProcessSessionReachedVisible (via VRSIM_GetDiagnostics).
//
// Build: x64 /std:c++17, define XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11
//        XR_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN; link d3d11.lib dxgi.lib.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cmath>
#include <future>
#include <chrono>
#include <memory>
#include <fstream>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include "VRSIMNative.h"
#include "GpuProjectionPass.h" // HIGHRES-H2-GPU: worker-owned reprojection helper + wire stats
#include "SharedStereoBridge.h" // HIGHRES-H2SH-CARD1: frozen bridge interface (implementation is read-only/frozen)
#include "SharedStereoWire.h"   // HIGHRES-H2SH-CARD1: additive Pack8 wire: identity192/stop32 + ticket64/stats72 re-asserts

namespace {

// ---------------------------------------------------------------- logging --
std::mutex g_logMutex;
std::string g_logPath;

void LogLine(const char* tag, const std::string& msg)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logPath.empty())
    {
        char base[MAX_PATH];
        if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) == 0)
            strcpy_s(base, "C:\\Temp");
        g_logPath = std::string(base) + "\\VRSIM";
        CreateDirectoryA(g_logPath.c_str(), nullptr);
        g_logPath += "\\native.log";
    }
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath.c_str(), "a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02uT%02u:%02u:%02u.%03u][%s] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, tag, msg.c_str());
    fclose(f);
}

// ---------------------------------------------------------------- state ----
std::atomic<int>          g_state{ VRSIM_OFF };
std::atomic<bool>         g_stopRequested{ false };
std::thread               g_worker;
std::mutex                g_errMutex;
std::string               g_lastError;

std::mutex                g_frameMutex;
VRSIMFrameState           g_frameState{};
VRSIMEyeView              g_eyeL{};
VRSIMEyeView              g_eyeR{};

std::atomic<uint64_t>     g_quadOk{ 0 };
std::atomic<uint64_t>     g_quadFail{ 0 };
std::atomic<uint64_t>     g_beginFrameOk{ 0 };
std::atomic<bool>         g_reachedVisible{ false };
std::atomic<int>          g_xrState{ XR_SESSION_STATE_UNKNOWN };

HMODULE                   g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_gpa = nullptr;

XrInstance                g_inst = XR_NULL_HANDLE;
XrSystemId                g_sysId = XR_NULL_SYSTEM_ID;
XrSession                 g_sess = XR_NULL_HANDLE;
XrSpace                   g_localSpace = XR_NULL_HANDLE;
XrSwapchain               g_swap[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
std::vector<ID3D11Texture2D*> g_swapTex[2]; // ALL images of each swapchain (write by acquired idx)
ID3D11Texture2D*          g_quadTex[2] = { nullptr, nullptr };
ID3D11Device*             g_dev = nullptr;
ID3D11DeviceContext*      g_ctx = nullptr;
uint32_t                  g_swapW = 0, g_swapH = 0;
int64_t                   g_swapFormat = 0;
// HIGHRES-H2-COLOR: frozen per-Create session color contract. VRSIM_Create reads
// VRSIM_RAW_COLORSPACE exactly once while the lifecycle is serialized (no worker
// yet); false = legacy (87 selection + default behavior), true = explicit sRGB
// raw-source declaration (91 required). Never reread per frame; cleared with the
// session teardown so a new session can never inherit an old contract.
bool                      g_sessionColorSrgb = false;
// HIGHRES-H2SH-CARD1: the ACTUAL runtime-required adapter LUID + minimum
// feature level, frozen per session by VRSIM_Create's device block (merged
// from the reviewed device-runtime-compat candidate) and cleared with the
// session teardown. The shared bridge Init validates against exactly this
// LUID — never a default adapter, a source-inferred LUID or a cross-adapter
// fallback.
LUID                      g_runtimeRequiredLuid{};
bool                      g_runtimeRequiredLuidValid = false;
D3D_FEATURE_LEVEL         g_runtimeMinFeatureLevel = (D3D_FEATURE_LEVEL)0;
XrEnvironmentBlendMode    g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
char                      g_runtimeName[XR_MAX_RUNTIME_NAME_SIZE + 1] = "?";
char                      g_systemName[XR_MAX_SYSTEM_NAME_SIZE + 1] = "?";
uint32_t                  g_apiVersion = 0;

std::wstring               g_loaderPathOverride;
std::string               g_loaderPathUsed;

// session init handshake: Create (caller thread) waits for the worker's session phase
std::promise<int>         g_initPromise;
uint64_t                  g_waitErrCount = 0;   // worker-only, throttles WaitFrame error logs

// ---- PHASE 12/13/14: frame-loop counters (all atomics; worker writes, diagnostics read) ----
std::atomic<uint64_t>     g_pollLoopCount{ 0 };
std::atomic<uint64_t>     g_sessionReadyEvents{ 0 };
std::atomic<uint64_t>     g_beginSessionAttempts{ 0 };
std::atomic<uint64_t>     g_beginSessionSuccess{ 0 };
std::atomic<uint64_t>     g_waitFrameEntered{ 0 };
std::atomic<uint64_t>     g_waitFrameReturned{ 0 };
std::atomic<uint64_t>     g_waitFrameSuccess{ 0 };
std::atomic<uint64_t>     g_waitFrameFailed{ 0 };
std::atomic<long long>    g_waitFrameEnteredAtMs{ 0 };  // epoch ms; 0 = not currently inside
std::atomic<long long>    g_lastWaitFrameResult{ 0 };
std::atomic<long long>    g_lastPredictedDisplayTime{ 0 };
std::atomic<long long>    g_lastPredictedDisplayPeriod{ 0 };
std::atomic<int>          g_lastShouldRender{ 0 };
std::atomic<uint64_t>     g_beginFrameAttempts{ 0 };
std::atomic<uint64_t>     g_beginFrameFailed{ 0 };
std::atomic<uint64_t>     g_zeroLayerEndFrames{ 0 };
std::atomic<uint64_t>     g_quadLayerAttempts{ 0 };
std::atomic<uint64_t>     g_quadFillFail{ 0 };   // acquire/wait/copy failed for some eye
std::atomic<uint64_t>     g_shouldRenderTrue{ 0 };
std::atomic<uint64_t>     g_shouldRenderFalse{ 0 };
std::atomic<long long>    g_lastEndFrameResult{ 0 };

// ---- PHASE 19: first-frame timeline (epoch ms; 0 = not reached) ----
std::atomic<long long>    g_tBase{ 0 };          // VRSIM_Create entry
std::atomic<long long>    g_tWorkerStarted{ 0 };     // T0
std::atomic<long long>    g_tInstanceCreated{ 0 };   // T1
std::atomic<long long>    g_tSessionCreated{ 0 };    // T2
std::atomic<long long>    g_tReadyEvent{ 0 };        // T3
std::atomic<long long>    g_tBeginSessionEntered{ 0 };  // T4
std::atomic<long long>    g_tBeginSessionReturned{ 0 }; // T5
std::atomic<long long>    g_tFirstWaitFrameEntered{ 0 };// T6
std::atomic<long long>    g_tFirstWaitFrameReturned{ 0 };// T7
std::atomic<long long>    g_tFirstBeginFrameReturned{ 0 };// T8
std::atomic<long long>    g_tFirstEndFrameReturned{ 0 };  // T9
std::atomic<long long>    g_tSynchronizedEvent{ 0 };      // T10
std::atomic<long long>    g_tVisibleEvent{ 0 };           // T11
std::atomic<long long>    g_tFirstShouldRenderTrue{ 0 };  // T12
std::atomic<long long>    g_tFirstQuadEndFrameOk{ 0 };    // T13

// ---- PHASE 1: official enum naming (never hand-write the mapping again) ----
const char* SessionStateName(XrSessionState s)
{
    switch (s)
    {
    case XR_SESSION_STATE_UNKNOWN:      return "XR_SESSION_STATE_UNKNOWN";
    case XR_SESSION_STATE_IDLE:         return "XR_SESSION_STATE_IDLE";
    case XR_SESSION_STATE_READY:        return "XR_SESSION_STATE_READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "XR_SESSION_STATE_SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:      return "XR_SESSION_STATE_VISIBLE";
    case XR_SESSION_STATE_FOCUSED:      return "XR_SESSION_STATE_FOCUSED";
    case XR_SESSION_STATE_STOPPING:     return "XR_SESSION_STATE_STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "XR_SESSION_STATE_LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:      return "XR_SESSION_STATE_EXITING";
    default:                            return "XR_SESSION_STATE_INVALID";
    }
}

// ---- PHASE 18: session event timeline JSONL (each line flushed on write) ----
void LogSessionEvent(XrSessionState oldS, XrSessionState newS, bool beginCalled, long long beginResult)
{
    char base[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) == 0) strcpy_s(base, "C:\\Temp");
    std::string p = std::string(base) + "\\VRSIM";
    CreateDirectoryA(p.c_str(), nullptr);
    p += "\\session-events.jsonl";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "a") != 0 || !f) return;
    SYSTEMTIME st; GetSystemTime(&st);
    fprintf(f,
        "{\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\",\"threadId\":%llu,"
        "\"oldStateRaw\":%d,\"oldStateName\":\"%s\","
        "\"newStateRaw\":%d,\"newStateName\":\"%s\","
        "\"beginSessionCalled\":%s,\"beginSessionResult\":%lld}\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        (unsigned long long)GetCurrentThreadId(),
        (int)oldS, SessionStateName(oldS), (int)newS, SessionStateName(newS),
        beginCalled ? "true" : "false", beginResult);
    fclose(f);
}

// ---- PHASE 19 support: frame-state.csv (throttled per ~100 frames) ----
void LogFrameCsv(uint64_t serial, XrSessionState st, int shouldRender, long long endResult)
{
    static uint64_t lines = 0;
    if (serial != 1 && serial % 100 != 0) return;
    char base[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) == 0) strcpy_s(base, "C:\\Temp");
    std::string p = std::string(base) + "\\VRSIM";
    CreateDirectoryA(p.c_str(), nullptr);
    p += "\\frame-state.csv";
    FILE* f = nullptr;
    bool head = false;
    { std::ifstream probe(p); head = !probe.good(); }
    if (fopen_s(&f, p.c_str(), "a") != 0 || !f) return;
    if (head) fprintf(f, "utcMs,serial,stateRaw,stateName,shouldRender,quadOk,quadFail,endResult\n");
    fprintf(f, "%llu,%llu,%d,%s,%d,%llu,%llu,%lld\n",
        (unsigned long long)GetTickCount64(), (unsigned long long)serial, (int)st,
        SessionStateName(st), shouldRender,
        (unsigned long long)g_quadOk.load(), (unsigned long long)g_quadFail.load(), endResult);
    fclose(f);
}

void SetErr(const std::string& m)
{
    std::lock_guard<std::mutex> lk(g_errMutex);
    g_lastError = m;
    LogLine("ERR", m);
}
void SetState(int s) { g_state = s; }

std::string GetLastErrorCopy()
{
    std::lock_guard<std::mutex> lk(g_errMutex);
    return g_lastError;
}

std::string XrResultName(XrResult r)
{
    // xrResultToString needs an instance for custom results; root names are enough here
    return "xr" + std::to_string((int)r);
}

// ------------------------------------------------- function pointer table --
#define XR_FN(x) PFN_##x x = nullptr;
XR_FN(xrCreateInstance) XR_FN(xrDestroyInstance)
XR_FN(xrGetInstanceProperties)
XR_FN(xrGetSystem) XR_FN(xrGetSystemProperties)
XR_FN(xrEnumerateEnvironmentBlendModes)
XR_FN(xrGetViewConfigurationProperties) XR_FN(xrEnumerateViewConfigurationViews)
XR_FN(xrCreateSession) XR_FN(xrDestroySession)
XR_FN(xrCreateReferenceSpace) XR_FN(xrDestroySpace)
XR_FN(xrPollEvent)
XR_FN(xrBeginSession) XR_FN(xrEndSession) XR_FN(xrRequestExitSession)
XR_FN(xrWaitFrame) XR_FN(xrBeginFrame) XR_FN(xrEndFrame)
XR_FN(xrLocateViews)
XR_FN(xrCreateSwapchain) XR_FN(xrDestroySwapchain)
XR_FN(xrEnumerateSwapchainFormats) XR_FN(xrEnumerateSwapchainImages)
XR_FN(xrAcquireSwapchainImage) XR_FN(xrWaitSwapchainImage) XR_FN(xrReleaseSwapchainImage)
PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11Requirements = nullptr;
#undef XR_FN

bool ResolveFns(std::string& err)
{
#define RES(x) do { g_gpa(g_inst, #x, (PFN_xrVoidFunction*)&x); if (!(x)) { err = #x " missing"; return false; } } while (0)
    RES(xrCreateInstance); RES(xrDestroyInstance); RES(xrGetInstanceProperties);
    RES(xrGetSystem); RES(xrGetSystemProperties); RES(xrEnumerateEnvironmentBlendModes);
    RES(xrGetViewConfigurationProperties); RES(xrEnumerateViewConfigurationViews);
    RES(xrCreateSession); RES(xrDestroySession); RES(xrCreateReferenceSpace); RES(xrDestroySpace);
    RES(xrPollEvent); RES(xrBeginSession); RES(xrEndSession); RES(xrRequestExitSession);
    RES(xrWaitFrame); RES(xrBeginFrame); RES(xrEndFrame); RES(xrLocateViews);
    RES(xrCreateSwapchain); RES(xrDestroySwapchain); RES(xrEnumerateSwapchainFormats);
    RES(xrEnumerateSwapchainImages); RES(xrAcquireSwapchainImage);
    RES(xrWaitSwapchainImage); RES(xrReleaseSwapchainImage);
#undef RES
    g_gpa(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&xrGetD3D11Requirements);
    if (!xrGetD3D11Requirements) { err = "XR_KHR_D3D11_enable missing"; return false; }
    return true;
}

// -------------------------------------------------------- loader discovery --
bool FileExists(const std::string& p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string DiscoverLoader()
{
    // registry ActiveRuntime JSON sits next to the loader DLL for PC runtimes
    const char* keys[] = {
        "SOFTWARE\\Khronos\\OpenXR\\1",
        "SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1",
    };
    for (HKEY hive : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        for (const char* k : keys)
        {
            char json[MAX_PATH * 2] = {0}; DWORD sz = sizeof(json); DWORD type = 0;
            if (RegGetValueA(hive, k, "ActiveRuntime", RRF_RT_REG_SZ, &type, json, &sz) == ERROR_SUCCESS)
            {
                std::string dir = json;
                const size_t bs = dir.find_last_of("\\/");
                if (bs != std::string::npos)
                {
                    std::string cand = dir.substr(0, bs) + "\\openxr_loader.dll";
                    if (FileExists(cand)) return cand;
                }
            }
        }
    const char* known[] = {
        "C:\\Program Files\\Oculus\\Support\\oculus-runtime\\openxr_loader.dll",
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll",
    };
    for (const char* k : known) if (FileExists(k)) return k;
    return "openxr_loader.dll"; // default DLL search
}

// -------------------------------------------------------- synthetic quad ----
// L eye: magenta checkerboard; R eye: cyan checkerboard; both: white cross.
// Distinct tints let the user verify stereo separation later.
void FillQuadPixels(std::vector<uint8_t>& px, uint32_t w, uint32_t h, bool left)
{
    px.assign((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t* p = &px[((size_t)y * w + x) * 4]; // BGRA byte order
            const bool a = ((x / 128) + (y / 128)) % 2 == 0;
            if (left) { p[0] = a ? 255 : 40; p[1] = 0;   p[2] = a ? 40 : 255; }          // magenta
            else      { p[0] = a ? 40 : 255; p[1] = a ? 255 : 40; p[2] = a ? 40 : 255; } // cyan-ish
            p[3] = 255;
        }
    const uint32_t cw = 24;
    for (uint32_t y = h / 2 - cw; y < h / 2 + cw; ++y)
        for (uint32_t x = 0; x < w; ++x)
        { uint8_t* p = &px[((size_t)y * w + x) * 4]; p[0] = 255; p[1] = 255; p[2] = 255; }
    for (uint32_t x = w / 2 - cw; x < w / 2 + cw; ++x)
        for (uint32_t y = 0; y < h; ++y)
        { uint8_t* p = &px[((size_t)y * w + x) * 4]; p[0] = 255; p[1] = 255; p[2] = 255; }
}

ID3D11Texture2D* CreateQuadTexture(ID3D11Device* dev, uint32_t w, uint32_t h,
                                   int64_t format, bool left, std::string& err)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = (DXGI_FORMAT)format;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> px;
    FillQuadPixels(px, w, h, left);
    D3D11_SUBRESOURCE_DATA init{ px.data(), w * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    const HRESULT hr = dev->CreateTexture2D(&d, &init, &tex);
    if (FAILED(hr) || !tex) { err = "CreateTexture2D quad hr=0x" + std::to_string((unsigned long)hr); return nullptr; }
    return tex;
}

// solid-color DEFAULT-usage texture (PRODUCT-02 diagnostic frames: yellow/red)
ID3D11Texture2D* CreateSolidTexture(ID3D11Device* dev, uint32_t w, uint32_t h,
                                    int64_t format, uint8_t b, uint8_t g, uint8_t r, std::string& err)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = (DXGI_FORMAT)format;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_DEFAULT;              // CPU-writable via UpdateSubresource
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = b; px[i+1] = g; px[i+2] = r; px[i+3] = 255; }
    D3D11_SUBRESOURCE_DATA init{ px.data(), w * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&d, &init, &tex);
    if (FAILED(hr) || !tex) { err = "CreateSolidTexture hr=0x" + std::to_string((unsigned long)hr); return nullptr; }
    return tex;
}

bool EnumerateSwapchainTextures(XrSwapchain swap, std::vector<ID3D11Texture2D*>& out)
{
    // two-call idiom: capacity 0 returns count; capacity < count returns SIZE_INSUFFICIENT
    out.clear();
    uint32_t cnt = 0;
    if (xrEnumerateSwapchainImages(swap, 0, &cnt, nullptr) != XR_SUCCESS || cnt == 0) return false;
    std::vector<XrSwapchainImageD3D11KHR> imgs(cnt);
    for (auto& im : imgs) im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    if (xrEnumerateSwapchainImages(swap, cnt, &cnt,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())) != XR_SUCCESS) return false;
    for (uint32_t i = 0; i < cnt; ++i) out.push_back(imgs[i].texture);
    return !out.empty() && out[0] != nullptr;
}

// ------------------------------------------------------------- frame loop --
// PHASE 5 (02BC): live tracking evidence counters
static std::atomic<unsigned long long> g_poseSerial{ 0 };
static std::atomic<uint64_t> g_poseSamples{ 0 };
static std::atomic<uint64_t> g_poseChanges{ 0 };
static double g_lastPoseL[3] = { 0, 0, 0 };   // worker-only
static bool g_haveLastPose = false;
// PHASE 14-19 (02C-FIX): pose sample ring — each located frame's XR pose/fov kept so a
// submitted image can be paired with the pose it was RENDERED at (reprojection correctness)
struct PoseSample { unsigned long long serial; XrPosef pl, pr; XrFovf fl, fr; bool valid; };
static const int POSE_RING = 256;
static PoseSample g_poseRing[POSE_RING];
static uint32_t g_poseRingIdx = 0; // worker-only
// HIGHRES-H2-GPU: worker writes, ABI stats readers read -> atomics
static std::atomic<unsigned long long> g_lastLayerPoseSerial{ 0 }; // exact serial whose pose is in the layer
static std::atomic<double> g_lastConsumedYawDeg{ -9999 };          // 02-LIVE-PIPELINE: yaw of last consumed frame
static std::atomic<uint64_t> g_consumedYawChanges{ 0 };
static std::atomic<uint64_t> g_poseImageMismatch{ 0 };
// PHASE 1/2 (02C-CALIBRATION): XR_EXT_user_presence — the generic "user is wearing" signal.
// FOCUSED only means input focus; it must never be treated as HEADSET_WORN.
static std::atomic<int> g_userPresenceAvail{ 0 };   // extension was in the runtime list
static std::atomic<int> g_userPresenceSupported{ 0 }; // system property supportsUserPresence
static std::atomic<int> g_userPresent{ -1 };        // -1 = no event yet, 0/1 = last event
static uint64_t g_userPresenceEvents = 0;           // worker-only
static XrPosef g_layerPl{}, g_layerPr{}; static XrFovf g_layerFl{}, g_layerFr{};
static bool g_haveLayerPose = false;

void PublishViews(const XrView views[2])
{
    // 0.1.2.2 (user directive): the OFFICIAL runtime already defines the frame — XR LOCAL
    // is Y-up right-handed with -Z forward, semantically identical to the VC camera basis
    // (right/up/back). Publish RAW XR values (mm, x,y,z,w) with ZERO conversion. The old
    // CARD16 Rx(+90) rig was an abstract reference frame, NOT the VC world frame (VC is
    // Y-up) — reusing it as a world mapping was the root of the floor-looking bug family.
    std::lock_guard<std::mutex> lk(g_frameMutex);
    ++g_poseSerial;
    ++g_poseSamples;
    {
        const double dx = views[0].pose.position.x * 1000.0 - g_lastPoseL[0];
        const double dy = views[0].pose.position.y * 1000.0 - g_lastPoseL[1];
        const double dz = views[0].pose.position.z * 1000.0 - g_lastPoseL[2];
        if (g_haveLastPose && std::sqrt(dx*dx + dy*dy + dz*dz) > 0.5) ++g_poseChanges;
        g_lastPoseL[0] = views[0].pose.position.x * 1000.0;
        g_lastPoseL[1] = views[0].pose.position.y * 1000.0;
        g_lastPoseL[2] = views[0].pose.position.z * 1000.0;
        g_haveLastPose = true;
    }
    for (int e = 0; e < 2; ++e)
    {
        VRSIMEyeView& o = (e == 0) ? g_eyeL : g_eyeR;
        const XrPosef& pose = views[e].pose;
        o.posMm[0] = pose.position.x * 1000.0;
        o.posMm[1] = pose.position.y * 1000.0;
        o.posMm[2] = pose.position.z * 1000.0;
        o.quatXyzw[0] = pose.orientation.x;
        o.quatXyzw[1] = pose.orientation.y;
        o.quatXyzw[2] = pose.orientation.z;
        o.quatXyzw[3] = pose.orientation.w;
        o.fovLeft  = views[e].fov.angleLeft;
        o.fovRight = views[e].fov.angleRight;
        o.fovUp    = views[e].fov.angleUp;
        o.fovDown  = views[e].fov.angleDown;
    }
}

// Session phase runs ON THE WORKER THREAD so the session is created, begun and
// frame-cycled by one thread (proven baseline pattern; also removes any runtime
// thread-affinity risk for xrWaitFrame/xrBeginFrame/xrEndFrame).
// Errors: SetErr + return -2; resources are destroyed by the caller (VRSIM_Create)
// after joining. Returns 0 when session + space + swapchains + quad are ready.
int SessionInitPhase()
{
    // ---- session ----
    XrGraphicsBindingD3D11KHR gfx{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    gfx.device = g_dev;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gfx;
    sci.systemId = g_sysId;
    XrResult r = xrCreateSession(g_inst, &sci, &g_sess);
    if (r != XR_SUCCESS) { SetErr("xrCreateSession " + XrResultName(r)); return -2; }
    g_tSessionCreated = (long long)GetTickCount64();

    // ---- reference space (LOCAL, identity) ----
    XrReferenceSpaceCreateInfo rsi{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation = { 0, 0, 0, 1 };
    rsi.poseInReferenceSpace.position = { 0, 0, 0 };
    r = xrCreateReferenceSpace(g_sess, &rsi, &g_localSpace);
    if (r != XR_SUCCESS) { SetErr("xrCreateReferenceSpace " + XrResultName(r)); return -2; }

    // ---- view config + swapchain size ----
    XrViewConfigurationView eye[2]{ {}, {} };
    eye[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    eye[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    uint32_t vc = 2;
    r = xrEnumerateViewConfigurationViews(g_inst, g_sysId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, eye);
    if (r != XR_SUCCESS || vc != 2 || eye[0].recommendedImageRectWidth == 0)
    { SetErr("xrEnumerateViewConfigurationViews " + XrResultName(r)); return -2; }

    // ---- swapchain format ----
    // HIGHRES-H2-COLOR: legacy/default prefers BGRA8 (87) exactly as before. In
    // the opt-in sRGB contract the runtime must offer B8G8R8A8_UNORM_SRGB (91);
    // no silent 87 fallback and no formats[0] assumption — absence fails init.
    uint32_t fn = 0;
    int64_t format = 0;
    if (xrEnumerateSwapchainFormats(g_sess, 0, &fn, nullptr) == XR_SUCCESS && fn > 0)
    {
        std::vector<int64_t> fmts(fn);
        if (xrEnumerateSwapchainFormats(g_sess, fn, &fn, fmts.data()) == XR_SUCCESS)
        {
            if (g_sessionColorSrgb)
            {
                for (int64_t f : fmts) if (f == (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { format = f; break; }
            }
            else
            {
                format = fmts[0];
                for (int64_t f : fmts) if (f == 87) { format = 87; break; }
            }
        }
    }
    if (format == 0)
    {
        SetErr(g_sessionColorSrgb
                   ? "sRGB color contract: runtime offers no B8G8R8A8_UNORM_SRGB (91) swapchain format"
                   : "no swapchain formats");
        return -2;
    }
    g_swapFormat = format;
    LogLine("XR", std::string("color contract sourceEncoding=") +
            (g_sessionColorSrgb ? "srgb" : "legacy") +
            " swapFormat=" + std::to_string(g_swapFormat));

    for (int e = 0; e < 2; ++e)
    {
        XrSwapchainCreateInfo si{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        si.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        si.format = format;
        si.width = eye[e].recommendedImageRectWidth;
        si.height = eye[e].recommendedImageRectHeight;
        si.sampleCount = 1;
        si.mipCount = 1;
        si.arraySize = 1;
        si.faceCount = 1;
        r = xrCreateSwapchain(g_sess, &si, &g_swap[e]);
        if (r != XR_SUCCESS) { SetErr("xrCreateSwapchain[" + std::to_string(e) + "] " + XrResultName(r)); return -2; }
        if (!EnumerateSwapchainTextures(g_swap[e], g_swapTex[e]))
        { SetErr("swapchain image enumerate failed"); return -2; }
        std::string qerr;
        g_quadTex[e] = CreateQuadTexture(g_dev, si.width, si.height, format, e == 0, qerr);
        if (!g_quadTex[e]) { SetErr(qerr); return -2; }
        // pre-fill every image so no uninitialized content can ever be composited
        for (auto* t : g_swapTex[e]) g_ctx->CopyResource(t, g_quadTex[e]);
    }
    g_swapW = eye[0].recommendedImageRectWidth;
    g_swapH = eye[0].recommendedImageRectHeight;
    return 0;
}

// ---- PRODUCT-02: stereo frame exchange (triple-slot ring, single OpenXR owner) ----
// Producer = C# VCFrameProducer (UI/VC thread). Consumer = the XR worker thread.
// Slot states: 0=FREE 1=WRITING 2=READY 3=READING. LATEST_POSE_WINS: consumer takes
// the highest-serial READY slot; producer may steal the lowest-serial READY slot.
struct StereoSlot
{
    std::atomic<int> state;
    unsigned long long frameSerial;
    unsigned long long poseSerial;
    long long timestampNs;
    int width, height, stride;
    unsigned long long leftSum; // additive pixel fingerprint (source-dependency evidence)
    double cameraYawDeg;        // 02-LIVE-PIPELINE: program-driven camera yaw at render time
    // HIGHRES-H2-GPU: immutable raw-GPU payload. Only valid while the slot is
    // exclusively owned (WRITING/READING). mode: 0=CPU, 1=rawGPU with plans.
    int mode;                             // VRSIM::kGpuSlotModeCpu / kGpuSlotModeRawGpu / 2=shared
    unsigned long long epoch;             // ring epoch captured at a gated claim (0 for CPU)
    VRSIM::ProjectionPlanV1 leftPlan;     // meaningful only when mode==rawGpu
    VRSIM::ProjectionPlanV1 rightPlan;    // meaningful only when mode==rawGpu
    // HIGHRES-H2SH-CARD1: shared-mode metadata (mode==2), stored BY VALUE.
    // The bridge ticket + the frozen capture identity live here; NO pixel
    // arrays and NO borrowed source-UI surface pointers are ever retained.
    // Valid only while the slot is exclusively owned; cleared on proven FREE.
    VRSIM::SharedPreparedPairV1  sharedTicket;
    VRSIM::SharedCaptureIdentityV1 sharedIdentity;
};
// HIGHRES-H2SH-CARD1 additive slot mode. The frozen GpuProjectionPass.h
// constants (CPU=0, rawGPU=1) are unchanged; mode 2 is native-internal only
// and is never advertised on Plan56/Stats120.
static const int kGpuSlotModeSharedV1 = 2;
static const int RING_SLOTS = 3;
static StereoSlot g_ring[RING_SLOTS];
static std::vector<uint8_t> g_ringL[RING_SLOTS], g_ringR[RING_SLOTS];
// HIGHRES-H2-GPU: short-hold mutex serializing every slot scan+claim and the
// READY-publish / FREE-release transitions. NEVER held across memcpy, vector
// resize, D3D calls or any other lock/wait. A WRITING or READING slot is
// exclusively owned by its claimer and its payload is immutable until release.
static std::mutex g_ringMutex;
static std::atomic<unsigned long long> g_latestReadySerial{ 0 };
static std::atomic<bool> g_sceneMode{ false };
static std::atomic<int> g_pendingSceneW{ 0 }, g_pendingSceneH{ 0 };
static ID3D11Texture2D* g_sceneTex[2] = { nullptr, nullptr };
static ID3D11Texture2D* g_redTex[2] = { nullptr, nullptr };
static bool g_uploadFailed = false; // worker-only
static std::atomic<uint64_t> g_submitOk{ 0 }, g_submitFail{ 0 }, g_d3dUploads{ 0 };
static std::atomic<uint64_t> g_vcLayerOk{ 0 }, g_reusedFrames{ 0 };
static std::atomic<uint64_t> g_framesDropped{ 0 };
static std::atomic<unsigned long long> g_lastConsumedSerial{ 0 };
// PHASE 3: explicit render source mode (no fuzzy bool)
static const int RSM_SYNTHETIC_TEST = 0, RSM_VC_SCENE = 1;
static std::atomic<int> g_renderSource{ RSM_SYNTHETIC_TEST };
static std::atomic<bool> g_vcSceneRequested{ false };
static std::atomic<bool> g_illegalFallback{ false };

// ---- HIGHRES-H2-GPU integration state ------------------------------------
// Cumulative counters never reset mid-process; active identity resets on a new
// session (VRSIM_Create with no prior worker) and on every scene rebuild.
static std::mutex                 g_gpuStatsMutex;      // short hold: copy/update only
static VRSIM::GpuPipelineStatsV1  g_gpuStats{};         // guarded by g_gpuStatsMutex
static VRSIM::GpuProjectionPass   g_gpuPass;            // worker-owned; helper is lock-free
static bool                       g_activeSourceGpu = false; // worker-only
static bool                       g_gpuPassReady = false;    // worker-only
static uint32_t                   g_gpuPassW = 0, g_gpuPassH = 0; // worker-only
static DXGI_FORMAT                g_gpuPassFmt = DXGI_FORMAT_B8G8R8A8_UNORM; // worker-only: plane format of the initialized resources

// Ring admission/epoch (guarded by g_ringMutex). Raw submits may only claim while
// admission is open (successfully initialized session); every open/close bumps the
// epoch so a claim that straddles Stop/Destroy publishes as stale (released, not
// queued). CPU submit path is intentionally NOT gated (old path stays usable).
static unsigned long long         g_ringEpoch = 0;
static bool                       g_ringAdmission = false;

// ---- HIGHRES-H2SH-CARD1: shared stereo bridge session state ----------------
// The bridge is an independent frozen class with its OWN mutable lifetime
// gate, atomic busy counters and real event drain; this block only holds
// native-side metadata mirrors and NEVER performs GPU/COM/wait work under
// g_sharedMetaMutex. Lock order (documented, mandatory):
//   g_ringMutex -> g_sharedMetaMutex -> (bridge's internal short gates)
// Never the reverse. g_ringMutex is only ever held across pure atomic/POD
// metadata work; the bridge calls allowed inside a ring section are the
// metadata-only grants (CommitPreparedOnSourceUI / DiscardPreparedOnSourceUI
// / cached Stats), which per the frozen bridge contract run no D3D API and no
// driver wait. Every real GPU transaction (Initialize/PrepareOnSourceUI)
// happens OUTSIDE both gates.
constexpr bool kVRSIMSharedActivationEnabled = true; // stays false until all three cards compile+review

// Per-slot shared phase (guarded by g_sharedMetaMutex). Ring slot states stay
// 0=FREE 1=WRITING 2=READY 3=READING; a shared PREPARED ticket is carried by
// ring WRITING (exclusively owned by source UI), a granted COMMITTED ticket by
// ring READY. Phase adds the bridge-side granularity the ring cannot express.
enum SharedSlotPhaseV1 {
    kSharedPhaseNone = 0,
    kSharedPhasePreparing = 1,   // ring WRITING reserved; bridge Prepare in flight
    kSharedPhasePrepared = 2,    // bridge PREPARED ticket recorded, awaiting COMMIT or DISCARD
    kSharedPhaseCommitted = 3,   // one linearized grant: bridge COMMITTED + ring READY
    kSharedPhaseReading = 4,     // worker lease (Card2; never reclaimed by source UI discard)
    kSharedPhaseInflight = 5,    // worker EndRead done, awaiting real event drain (Card2)
    kSharedPhaseQuarantined = 6  // unproven: retained, no reuse, no generic free
};
static std::mutex                 g_sharedMetaMutex;    // short pure-metadata gate; NO GPU/COM/wait
static VRSIM::SharedStereoBridge  g_sharedBridge;       // borrowed XR device/context live until its sourceFinalize contract
static int                        g_sharedPhase[RING_SLOTS] = {};
static bool                       g_sharedBound = false;          // bridge InitializeOnSourceUI succeeded
static bool                       g_sharedAdmissionClosed = false;
static bool                       g_sharedWorkerDrained = false;  // set only by Card2 worker drain proof
static bool                       g_sharedSourceFinalized = false;
static bool                       g_sharedQuarantined = false;
static bool                       g_sharedFinalizedOnce = false;  // a closed owner is NOT reusable
static unsigned long long         g_sharedBoundEpoch = 0;
static uint32_t                   g_sharedBoundGeneration = 0;
static long long                  g_sharedLastBridgeHresult = 0;  // preserved exact bridge HRESULT

// HIGHRES-H2SH-CARD2B: shared close/teardown lifecycle mirrors. Every flag is
// pure metadata under g_sharedMetaMutex (ring -> meta order); the ONLY bridge
// call ever made inside a native gate is the reviewed pure-metadata
// CloseAdmission. No GPU/COM/wait/Release is ever run under these gates, and
// no gate is ever held across InitializeOnSourceUI/PrepareOnSourceUI/
// DiscardPreparedOnSourceUI/TryFinalizeOnSourceUI.
static bool                       g_sharedLifetimeStarted = false; // first paired-FREE grant taken (incl. PREPARING / failed-Init gap)
static bool                       g_sharedClosing = false;         // close linearized with grant/Commit/first-claim
static bool                       g_sharedTeardownRequested = false; // Destroy entered
static int                        g_sharedSourceActiveCalls = 0;   // in-flight source-UI Prepare calls
static bool                       g_sharedFinalizeBusy = false;    // TryFinalizeOnSourceUI in flight (RAII)
static bool                       g_sharedWorkerNoContext = false; // worker exited: proven no further context access
static bool                       g_sharedWorkerCleanupDone = false; // worker GpuWorkerCleanup actually succeeded (next card)

// SCENE-FREEZE-1: private scene-change metadata (NOT ABI, no header, no export).
// Read and written ONLY under one joint ring -> meta gate. This card may only
// INVALIDATE readiness: nothing here is ever set true from g_sceneMode, a
// requested 1536 size, or WorkerApplySceneMode returning normally.
static bool                       g_sceneChangeBusy = false;      // a worker rebuild owns the queue
static bool                       g_sharedScene1536Ready = false; // next card publishes, never this one
static unsigned long long         g_sceneReadyEpoch = 0;          // epoch a readiness would belong to

// Forward decl for the locked shared close. It MUST live in the SAME nested
// unnamed namespace as the real stats-helper definition below (reopening an
// unnamed namespace at the same scope refers to the SAME entity): declaring it
// directly in the outer unnamed namespace created a distinct, never-defined
// entity (LNK2019). No duplicate/empty helper is introduced here.
namespace {
void GpuStatsResetActiveLocked(); // caller already holds g_gpuStatsMutex
}

// RAII over one source-UI shared active call. ArmLocked() runs inside the
// ring -> meta grant gate (caller already holds g_sharedMetaMutex); the
// destructor drops the count on EVERY exit, including exceptions.
struct SharedActiveCallGuard
{
    bool armed = false;
    void ArmLocked() { armed = true; ++g_sharedSourceActiveCalls; }
    ~SharedActiveCallGuard()
    {
        if (armed)
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            --g_sharedSourceActiveCalls;
        }
    }
};

struct FinalizeBusyGuard
{
    bool armed = false;
    void ArmLocked() { armed = true; g_sharedFinalizeBusy = true; }
    ~FinalizeBusyGuard()
    {
        if (armed)
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedFinalizeBusy = false;
        }
    }
};

// Caller MUST already hold g_ringMutex AND g_sharedMetaMutex (ring -> meta).
// Publishes g_sharedWorkerDrained ONLY on real evidence: cached worker-exit
// proof, zero source active calls, no quarantine, and all three slots proven
// paired FREE (native ring FREE + phase None + the bridge's actual cached
// FREE). Never inferred from elapsed time; never published while anything is
// READING/INFLIGHT/PREPARING/PREPARED/COMMITTED.
void SharedUpdateDrainLocked(const VRSIM::SharedBridgeStatsV1& bs)
{
    if (g_sharedWorkerDrained || !g_sharedWorkerNoContext || !g_sharedWorkerCleanupDone) return;
    if (g_sharedSourceActiveCalls != 0 || g_sharedQuarantined) return;
    for (int i = 0; i < RING_SLOTS; ++i)
    {
        if (g_sharedPhase[i] != kSharedPhaseNone) return;
        if (g_ring[i].state.load() != 0) return;
        if (i < (int)VRSIM::kStereoBridgeSlotCountV1 &&
            bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree) return;
    }
    g_sharedWorkerDrained = true;
}

// Caller MUST already hold g_ringMutex AND g_sharedMetaMutex. Linearized
// shared close: sets closing, closes the ring admission (Locked variant: the
// stats helpers are NEVER re-entered under the meta gate — the permitted
// nesting is ring -> meta -> gpuStats, matching RingSetAdmission's ring ->
// gpuStats order), and runs the reviewed pure-metadata bridge CloseAdmission.
// Idempotent; the original boundEpoch/generation and every stored slot
// epoch/ticket stay untouched. Shared by Stop/Destroy/RequestDrain/Commit/
// first-grant/ERROR/LOSS/worker-exit so close vs. grant is one linearization.
void SharedCloseLocked(bool teardown)
{
    g_sharedClosing = true;
    if (teardown) g_sharedTeardownRequested = true;
    if (!g_sharedAdmissionClosed)
    {
        g_sharedAdmissionClosed = true;
        const HRESULT chr = g_sharedBridge.CloseAdmission(); // pure metadata, gate-legal
        g_sharedLastBridgeHresult = (long long)chr;
    }
    if (g_ringAdmission)
    {
        g_ringAdmission = false;
        ++g_ringEpoch;
        std::lock_guard<std::mutex> slk(g_gpuStatsMutex);
        g_gpuStats.sessionEpoch = g_ringEpoch;
        g_gpuStats.reserved &= ~1u;
        GpuStatsResetActiveLocked();
    }
}

// Lock-taking wrapper of SharedCloseLocked (no native gate held by callers).
void SharedCloseLifecycle(bool teardown)
{
    std::lock_guard<std::mutex> rlk(g_ringMutex);
    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
    SharedCloseLocked(teardown);
}

// True while ANY real shared lifecycle state exists: a lifetime lease (even
// unbound/PREPARING), an active source call, an outstanding phase, a finalize
// in progress, or quarantine. Under ONE ring -> meta gate; when it holds the
// close runs atomically inside the same gate (no check->firstPrepare gap).
bool SharedLeaseOrStateLocked()
{
    if (g_sharedLifetimeStarted || g_sharedBound || g_sharedQuarantined ||
        g_sharedFinalizeBusy || g_sharedSourceActiveCalls > 0) return true;
    for (int i = 0; i < RING_SLOTS; ++i)
        if (g_sharedPhase[i] != kSharedPhaseNone) return true;
    return false;
}

// HIGHRES-H2SH-CARD2B: closes the shared lifecycle ONLY when a lease/state
// exists, so a session that never touched shared (legacy Create failure,
// natural ERROR, plain Stop/Destroy) can never pre-close the bridge master
// admission for a future session. PREPARING / active-call / unbound lifetime
// all count as real pending and DO close.
void SharedCloseLifecycleIfLease(bool teardown)
{
    std::lock_guard<std::mutex> rlk(g_ringMutex);
    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
    if (SharedLeaseOrStateLocked()) SharedCloseLocked(teardown);
}

// Legacy reset/lifecycle guard: a shared lifetime lease (from the FIRST
// paired-FREE grant, including PREPARING / the bound=false init gap / a failed
// Init) must never be wiped by a legacy Create/Destroy reset until a REAL
// source finalize (bridge S_OK) proved everything empty. Quarantine blocks
// regardless of bound. An Init failure never clears lifetimeStarted by
// inferring Initialized()==false.
bool SharedLegacyResetBlocked()
{
    std::lock_guard<std::mutex> lk(g_sharedMetaMutex);
    if (g_sharedQuarantined || g_sharedFinalizeBusy) return true;
    if (g_sharedSourceActiveCalls > 0) return true;
    if (g_sharedLifetimeStarted && !g_sharedSourceFinalized) return true;
    for (int i = 0; i < RING_SLOTS; ++i)
        if (g_sharedPhase[i] != kSharedPhaseNone) return true;
    return false;
}

// Shared reservation: paired FREE ONLY (ring state FREE AND the bridge's
// cached slot state FREE). This MUST NOT reuse RingClaimSlotLocked: that
// helper's lowest-serial READY steal branch is forbidden for shared tickets,
// and a ring FREE alone is not proof while the bridge slot is still
// PREPARED/COMMITTED/READING/INFLIGHT. -> WRITING on success, -1 otherwise.
int RingClaimSharedPairedFreeLocked(const VRSIM::SharedBridgeStatsV1& bridgeStats,
                                    VRSIM::SharedPreparedPairV1* outGranted)
{
    for (int i = 0; i < RING_SLOTS; ++i)
    {
        if (g_ring[i].state.load() != 0) continue;
        if (i >= (int)VRSIM::kStereoBridgeSlotCountV1) break;
        if (bridgeStats.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree) continue;
        g_ring[i].state.store(1);
        if (outGranted) outGranted->slotIndex = (uint32_t)i; // caller records metadata next
        return i;
    }
    return -1;
}

// Caller MUST already hold g_ringMutex. Used by the linearized COMMIT grant so
// neither RingPublishReady nor RingSetAdmission is re-entered recursively.
void RingPublishReadyLocked(int slot, unsigned long long frameSerial)
{
    g_ring[slot].state.store(2);
    g_latestReadySerial = frameSerial;
}

// Caller MUST already hold g_ringMutex AND g_sharedMetaMutex (ring -> meta).
// HIGHRES-H2SH-CARD2B: the lock moved OUT of this helper — the first shared
// grant records PREPARING/WRITING inside one linearized ring -> meta gate, so
// a self-taken meta lock here would recursively lock the same mutex (UB).
// Record the reserved shared WRITING slot + preparing phase + frozen capture
// identity (captureEndQpc stays 0 until the owner's single completion
// timestamp at COMMIT).
void RingRecordSharedPreparingLocked(int slot, unsigned long long epoch,
                                     const VRSIM::SharedCaptureIdentityV1& id)
{
    StereoSlot& s = g_ring[slot];
    s.mode = kGpuSlotModeSharedV1;
    s.epoch = epoch;
    s.frameSerial = id.frameSerial;
    s.poseSerial = id.poseSerial;
    s.cameraYawDeg = id.cameraYawDeg;
    s.leftPlan = id.leftPlan;      // same frozen plans the identity carries (validator reuse)
    s.rightPlan = id.rightPlan;
    // HIGHRES-H2SH-CARD2: record the FULL source dimensions. stride 0 is the
    // explicit "no CPU payload promised" marker: shared slots never carry or
    // deep-copy CPU pixel bytes; legacy byte validation never runs on them.
    s.width = (int)VRSIM::kStereoBridgeWidthV1;
    s.height = (int)VRSIM::kStereoBridgeHeightV1;
    s.stride = 0;
    s.timestampNs = 0;             // shared slots carry no CPU timestamp; QPC lives in identity
    s.leftSum = 0;                 // no CPU fingerprint on the shared route
    s.sharedTicket = VRSIM::SharedPreparedPairV1{}; // zeroed: named fields + tail padding
    s.sharedIdentity = id;
    g_sharedPhase[slot] = kSharedPhasePreparing;
}

// ---- PRODUCT-02 stats POD (C ABI) ----
struct VRSIMStereoStats
{
    unsigned long long submitOk, submitFail, d3dUploads, vcLayerOk, reusedFrames;
    unsigned long long latestFrameSerial, quadOkCount;
    int sceneMode;
    int ringFree;
    // v2 (0.1.1.2)
    unsigned long long framesDropped, lastConsumedSerial;
    int renderSourceMode;      // 0=SYNTHETIC_TEST 1=VC_SCENE
    int illegalFallback;       // ILLEGAL_SYNTHETIC_FALLBACK
    int requestedRenderSource; // what the plugin asked for
    // v3 (0.1.1.3) PHASE 5 tracking evidence
    unsigned long long poseSerial, poseSamples, poseChanges;
    // v4 (0.1.1.4) pose/image association
    unsigned long long poseImageMismatch, lastLayerPoseSerial;
    // v5 (0.1.1.7) user presence (02C-CALIBRATION)
    int userPresenceAvailable, userPresenceSupported, isUserPresent;
    // v6 (0.1.1.8) live-pipeline content evidence
    double lastConsumedYawDeg;
    unsigned long long consumedYawChanges;
    // v7 (0.1.2.0)
    unsigned long long beginFrameFailed;
    unsigned long long d3dUploadCount;
};

namespace {

// HIGHRES-H2-GPU: forward decls (defined with the ring helpers below) so the
// scene-rebuild path can reset the worker-owned helper + active identity.
void GpuStatsResetActive();
void GpuStatsResetActiveLocked();      // caller already holds g_gpuStatsMutex
void CreateResetStaleState();          // serialized Create, no previous worker

// SCENE-FREEZE-1: queue-vs-first-Prepare is ONE transaction. The busy flag is
// armed inside the SAME joint gate that takes both pending dimensions, so no
// EnableSceneMode can re-queue past a rebuild that already owns the queue. The
// destructor re-takes ring -> meta and returns busy on EVERY exit (early
// return, normal end, exception). Never destroy it while a gate is held.
struct SceneChangeBusyGuard
{
    bool armed = false;
    unsigned long long epoch = 0;
    void ArmLocked() // caller holds ring + meta
    {
        armed = true;
        g_sceneChangeBusy = true;
        epoch = (unsigned long long)g_ringEpoch; // original epoch recorded
    }
    ~SceneChangeBusyGuard()
    {
        if (!armed) return;
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        g_sceneChangeBusy = false;
    }
};

bool WorkerApplySceneMode(int w, int h); // same namespace, defined next

// Worker-thread only. ONE joint gate performs: two-dimension pending check,
// admission/lease decision, epoch record, busy arm, readiness invalidation and
// the two-dimension take + pending clear. ALL real work (the original rebuild
// body, every SetErr/LogLine, any GPU/XR/COM call) runs OUTSIDE the gates.
void WorkerApplyPendingSceneMode()
{
    SceneChangeBusyGuard busy;
    int w = 0, h = 0;
    bool take = false;
    const char* why = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        if (g_pendingSceneW.load() <= 0 || g_pendingSceneH.load() <= 0)
        {
            // SF2 (Root-approved): the common idle path queues nothing, so it
            // must not open the log file once per frame. why stays nullptr.
        }
        else if (g_sceneChangeBusy)
        {
            // busy already owns the queue: the request STAYS pending, untouched
            why = "scene rebuild busy: request stays queued";
        }
        else if (g_stopRequested.load() || !g_ringAdmission ||
                 SharedLeaseOrStateLocked() || g_sharedClosing ||
                 g_sharedTeardownRequested || g_sharedAdmissionClosed)
        {
            // shared/close won: drop THIS stale request only. No Reset/Release,
            // no GPU claim, no readiness identity erased (a live shared lease's
            // own readiness stays exactly as it was), no claim of success.
            g_pendingSceneW.store(0);
            g_pendingSceneH.store(0);
            why = "shared lease or native close won: scene request dropped";
        }
        else
        {
            w = g_pendingSceneW.load();
            h = g_pendingSceneH.load();
            g_pendingSceneW.store(0);
            g_pendingSceneH.store(0);
            g_sharedScene1536Ready = false; // invalidate first; republished only below
            g_sceneReadyEpoch = 0;
            busy.ArmLocked();
            take = true;
        }
    }
    if (!take)
    {
        if (why) LogLine("XR", std::string("scene request not applied: ") + why);
        return; // guard unarmed, or nothing taken: no rebuild, no fake success
    }
    // SF2: the actual body, the readiness publication and the result log are ONE
    // try/catch while busy is still armed. An exception may never leave
    // busy=true (the guard still owns it) nor keep a just-published readiness.
    // No GPU/COM/XR call and no real logging happens under a gate.
    try
    {
        const bool verified = WorkerApplySceneMode(w, h); // all real work, gates closed
        bool published = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
            if (busy.armed && g_sceneChangeBusy &&
                busy.epoch == (unsigned long long)g_ringEpoch &&
                !g_stopRequested.load() && g_ringAdmission &&
                !SharedLeaseOrStateLocked() && !g_sharedQuarantined &&
                !g_sharedFinalizeBusy && !g_sharedSourceFinalized &&
                !g_sharedTeardownRequested && !g_sharedFinalizedOnce &&
                g_pendingSceneW.load() == 0 && g_pendingSceneH.load() == 0 &&
                g_sceneMode.load() && verified &&
                w == (int)VRSIM::kStereoBridgeWidthV1 && h == (int)VRSIM::kStereoBridgeHeightV1)
            {
                // the ONLY place this card sets readiness true: this round's
                // verified native bridge-profile rebuild, under this round's epoch
                g_sharedScene1536Ready = true;
                g_sceneReadyEpoch = busy.epoch;
                published = true;
            }
        }
        LogLine("XR", std::string("scene rebuild returned ") + std::to_string(w) + "x" +
                std::to_string(h) + " epoch=" + std::to_string(busy.epoch) +
                " verified=" + std::to_string(verified ? 1 : 0) +
                " ready=" + std::to_string(published ? 1 : 0));
    }
    catch (...)
    {
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedScene1536Ready = false; // never carry a readiness over an exception
            g_sceneReadyEpoch = 0;
        }
        LogLine("XR", std::string("scene rebuild threw ") + std::to_string(w) + "x" +
                std::to_string(h) + "; readiness stays false"); // best effort, outside gates
    }
}

// worker-only: recreate swapchains at the scene size + scene textures (between
// frames). Runs ONLY while the SF1 busy guard is alive and NEVER under a native
// gate. SF2 returns this round's ACTUAL verified fact: both eyes' swapchains
// exist, every enumerated swap texture and the scene texture are non-null and
// their real GetDesc descriptors match this round's w/h, the session's ACTUAL
// swapchain format, ArraySize 1 and SampleDesc.Count 1. A requested size or a
// non-null pointer is never accepted as proof. Legacy sizes keep the original
// rebuild behavior; readiness for the shared profile is decided by the caller.
bool WorkerApplySceneMode(int w, int h)
{
    if (!g_dev || !g_ctx) return false;
    LogLine("XR", "ApplySceneMode " + std::to_string(w) + "x" + std::to_string(h));
    // HIGHRES-H2-GPU: scene rebuild invalidates the worker-owned helper and the
    // active GPU identity/layer pose (helper re-initializes lazily at new dims).
    g_gpuPass.Reset();
    g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
    g_activeSourceGpu = false;
    g_haveLayerPose = false; g_lastLayerPoseSerial = 0;
    GpuStatsResetActive();
    for (int e = 0; e < 2; ++e)
    {
        if (g_swap[e] != XR_NULL_HANDLE) { xrDestroySwapchain(g_swap[e]); g_swap[e] = XR_NULL_HANDLE; }
        if (g_quadTex[e]) { g_quadTex[e]->Release(); g_quadTex[e] = nullptr; }
        // release the previous scene/red textures so a rebuild cannot leak them
        if (g_sceneTex[e]) { g_sceneTex[e]->Release(); g_sceneTex[e] = nullptr; }
        if (g_redTex[e]) { g_redTex[e]->Release(); g_redTex[e] = nullptr; }
        g_swapTex[e].clear();
        XrSwapchainCreateInfo si{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        si.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        si.format = g_swapFormat;
        si.width = (uint32_t)w; si.height = (uint32_t)h;
        si.sampleCount = 1; si.mipCount = 1; si.arraySize = 1; si.faceCount = 1;
        XrResult r = xrCreateSwapchain(g_sess, &si, &g_swap[e]);
        if (r != XR_SUCCESS)
        {
            SetErr("scene xrCreateSwapchain[" + std::to_string(e) + "] " + XrResultName(r));
            g_sceneMode = false;
            return false;
        }
        if (!EnumerateSwapchainTextures(g_swap[e], g_swapTex[e]))
        { SetErr("scene texture enumerate failed"); g_sceneMode = false; return false; }
        // PHASE 4: VC_SCENE mode NEVER shows the checkerboard. Before the first VC
        // frame: SOLID YELLOW = SCENE_MODE_ACTIVE_BUT_NO_VC_FRAME.
        std::string qerr;
        g_sceneTex[e] = CreateSolidTexture(g_dev, si.width, si.height, g_swapFormat, 0, 255, 255, qerr); // BGR: yellow
        if (!g_sceneTex[e]) { SetErr("scene texture: " + qerr); g_sceneMode = false; g_renderSource = RSM_SYNTHETIC_TEST; return false; }
        for (auto* t : g_swapTex[e]) g_ctx->CopyResource(t, g_sceneTex[e]);
        g_redTex[e] = CreateSolidTexture(g_dev, si.width, si.height, g_swapFormat, 0, 0, 255, qerr); // BGR: red
    }
    g_swapW = (uint32_t)w; g_swapH = (uint32_t)h;
    g_sceneMode = true;
    g_renderSource = RSM_VC_SCENE;
    LogLine("XR", "scene mode ACTIVE (renderSource=VC_SCENE, waiting=YELLOW) " + std::to_string(w) + "x" + std::to_string(h));

    // SCENE-FREEZE-2: verify BOTH eyes from the real descriptors (worker thread,
    // no native gate held). The session's actual swapchain format is what the
    // swapchains were created with; the shared SOURCE format is a different
    // contract and is NEVER forced onto these outputs here.
    const DXGI_FORMAT wantFmt = (DXGI_FORMAT)g_swapFormat;
    for (int e = 0; e < 2; ++e)
    {
        if (g_swap[e] == XR_NULL_HANDLE)
        { SetErr("scene verify: swapchain handle missing eye=" + std::to_string(e)); return false; }
        if (g_swapTex[e].empty())
        { SetErr("scene verify: no swapchain textures enumerated eye=" + std::to_string(e)); return false; }
        if (!g_sceneTex[e])
        { SetErr("scene verify: scene texture missing eye=" + std::to_string(e)); return false; }
        D3D11_TEXTURE2D_DESC sd{};
        g_sceneTex[e]->GetDesc(&sd);
        if ((int)sd.Width != w || (int)sd.Height != h || sd.Format != wantFmt ||
            sd.ArraySize != 1 || sd.SampleDesc.Count != 1)
        {
            SetErr("scene verify: scene descriptor mismatch eye=" + std::to_string(e) +
                   " w=" + std::to_string((unsigned)sd.Width) +
                   " h=" + std::to_string((unsigned)sd.Height) +
                   " fmt=" + std::to_string((unsigned)sd.Format) +
                   " array=" + std::to_string((unsigned)sd.ArraySize) +
                   " samples=" + std::to_string((unsigned)sd.SampleDesc.Count));
            return false;
        }
        for (size_t k = 0; k < g_swapTex[e].size(); ++k)
        {
            ID3D11Texture2D* t = g_swapTex[e][k];
            if (!t)
            { SetErr("scene verify: null swapchain texture eye=" + std::to_string(e) +
                     " idx=" + std::to_string((unsigned long long)k)); return false; }
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            const bool swapFormatCompatible = d.Format == wantFmt ||
                ((wantFmt == DXGI_FORMAT_B8G8R8A8_UNORM || wantFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
                 d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
            if ((int)d.Width != w || (int)d.Height != h || !swapFormatCompatible ||
                d.ArraySize != 1 || d.SampleDesc.Count != 1)
            {
                SetErr("scene verify: swap descriptor mismatch eye=" + std::to_string(e) +
                       " idx=" + std::to_string((unsigned long long)k) +
                       " w=" + std::to_string((unsigned)d.Width) +
                       " h=" + std::to_string((unsigned)d.Height) +
                       " fmt=" + std::to_string((unsigned)d.Format) +
                       " array=" + std::to_string((unsigned)d.ArraySize) +
                       " samples=" + std::to_string((unsigned)d.SampleDesc.Count));
                return false;
            }
        }
    }
    return true; // this round verified for BOTH eyes at w x h
}

// ---- HIGHRES-H2-GPU: ring claim protocol helpers (all transitions under
// g_ringMutex; scan+claim in one critical section so producer steal, consumer
// pick and multi-producer claims cannot race on the same slot) ----

// core scan+claim; caller MUST already hold g_ringMutex. -> WRITING.
// Prefer a FREE slot; else steal the LOWEST-serial READY slot (latest-wins).
// Never touches WRITING/READING (exclusively owned).
int RingClaimSlotLocked()
{
    for (int i = 0; i < RING_SLOTS; ++i)
        if (g_ring[i].state.load() == 0) { g_ring[i].state.store(1); return i; }
    int victim = -1; unsigned long long victimSerial = ~0ull;
    for (int i = 0; i < RING_SLOTS; ++i)
    {
        const int st = g_ring[i].state.load();
        // HIGHRES-H2SH-CARD1: a shared-mode READY slot is a live COMMITTED
        // ticket — the latest-wins steal must NEVER pick it as a victim.
        if (st == 2 && g_ring[i].mode != kGpuSlotModeSharedV1 &&
            g_ring[i].frameSerial < victimSerial)
        { victim = i; victimSerial = g_ring[i].frameSerial; }
    }
    if (victim < 0) return -1;
    g_ring[victim].state.store(1);
    ++g_framesDropped; // stolen before consumption (latest-wins policy)
    return victim;
}

// producer: prefer a FREE slot; otherwise steal the LOWEST-serial READY slot
// (latest-wins). Never touches WRITING/READING (exclusively owned). -> WRITING.
// HIGHRES-H2SH-CARD2B: the legacy CPU claim is mutually exclusive with a
// shared lifetime INSIDE the existing ring -> meta short gate (no pre-gate
// snapshot): once any shared lease exists (lifetimeStarted, including the
// bound=false init gap / PREPARING, or closing/finalizing), no new CPU claim
// is granted. Default non-shared sessions keep the untouched behavior.
int RingClaimForWrite()
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        if (g_sharedLifetimeStarted || g_sharedClosing || g_sharedFinalizeBusy)
            return -1;
    }
    return RingClaimSlotLocked();
}

// consumer: claim the newest READY slot -> READING; -1 when none ready
int RingClaimForRead()
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    int best = -1; unsigned long long bestSerial = 0;
    for (int i = 0; i < RING_SLOTS; ++i)
        // HIGHRES-H2SH-CARD1: the legacy CPU/raw consumer never reads a shared
        // ticket slot; shared consumption is the Card2 worker path only.
        if (g_ring[i].state.load() == 2 && g_ring[i].mode != kGpuSlotModeSharedV1 &&
            (best < 0 || g_ring[i].frameSerial > bestSerial))
        { best = i; bestSerial = g_ring[i].frameSerial; }
    if (best >= 0) g_ring[best].state.store(3);
    return best;
}

// exclusive owner releases WRITING/READING -> FREE
void RingReleaseToFree(int slot)
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    // HIGHRES-H2SH-CARD2: a shared-ticket slot NEVER returns to FREE through
    // this generic path. Shared FREE requires the bridge's actual cached FREE
    // plus the same slot+ticket+epoch+generation provenance, established only
    // by WorkerPollSharedCompletion on a proven S_OK+TRUE GetData; anything
    // else is retained/quarantined, never a fabricated Free.
    if (g_ring[slot].mode == kGpuSlotModeSharedV1) return;
    g_ring[slot].state.store(0);
}

// producer publishes a fully written slot: READY + latest serial atomically
void RingPublishReady(int slot, unsigned long long frameSerial)
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    g_ring[slot].state.store(2);
    g_latestReadySerial = frameSerial;
}

// ---- HIGHRES-H2-GPU admission-gated claim/publish (raw submit only) --------
// Claim only while admission is open AND the caller's captured epoch still equals
// the current ring epoch (a capture made against a stale session is rejected here,
// never claimed). The claim records the epoch on the slot so a Stop/Destroy that
// races the payload copy makes the later publish stale (released, not queued).
// -> slot index (WRITING) + *outEpoch; -1 when closed, epoch mismatch, or no slot;
// kRingClaimGatedSharedRoute (-3) when a shared lifetime owns the ring
// (HIGHRES-H2SH-CARD2B: checked in the SAME ring -> meta short gate, closing the
// old snapshot->claim gap against a racing first shared grant).
static const int kRingClaimGatedSharedRoute = -3;
int RingClaimGated(unsigned long long expectedEpoch, unsigned long long* outEpoch)
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        if (g_sharedLifetimeStarted || g_sharedClosing || g_sharedFinalizeBusy)
            return kRingClaimGatedSharedRoute;
    }
    if (!g_ringAdmission) return -1;
    if (expectedEpoch != g_ringEpoch) return -1; // no old capture accepted by a fresh epoch
    const int slot = RingClaimSlotLocked();
    if (slot >= 0 && outEpoch) *outEpoch = g_ringEpoch;
    return slot;
}

// Publish only if the epoch/admission are unchanged since the claim. On a stale
// window the WRITING slot is returned to FREE (payload discarded) and false is
// returned. Never held across the payload copy.
// HIGHRES-H2SH-CARD2B: a failed legacy publish may only FREE its own still-held
// NON-shared WRITING claim — a shared-ticket slot is never cleared here.
bool RingPublishGated(int slot, unsigned long long frameSerial, unsigned long long epoch)
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    if (g_ring[slot].mode == kGpuSlotModeSharedV1) return false; // never touch shared tickets
    if (!g_ringAdmission || epoch != g_ringEpoch)
    { g_ring[slot].state.store(0); return false; }
    g_ring[slot].state.store(2);
    g_latestReadySerial = frameSerial;
    return true;
}

// Consumer snapshot of the current admission/epoch under the ring mutex, so a raw
// slot can be validated (slot epoch == live epoch AND admission open) before any
// GPU work and again before committing metadata.
void RingSnapshotAdmissionEpoch(bool* open, unsigned long long* epoch)
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    if (open) *open = g_ringAdmission;
    if (epoch) *epoch = g_ringEpoch;
}

// Open (successfully initialized session) / close (Stop/Destroy/Create-reset/
// worker exit). Bump the epoch on every transition so in-flight raw claims
// linearize against it, then coherently sync the wire stats (session epoch +
// reserved admission bit) and CLEAR the active identity + layer-success-last
// fields under the short stats lock. Never D3D or large copies under either lock.
void RingSetAdmissionLocked(bool open)
{
    g_ringAdmission = open;
    ++g_ringEpoch;
    const unsigned long long epoch = g_ringEpoch;
    std::lock_guard<std::mutex> slk(g_gpuStatsMutex);
    g_gpuStats.sessionEpoch = epoch;
    if (open) g_gpuStats.reserved |= 1u; else g_gpuStats.reserved &= ~1u;
    GpuStatsResetActiveLocked();
}

void RingSetAdmission(bool open)
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    RingSetAdmissionLocked(open);
}

// HIGHRES-H2 state-recovery fix: admission-open + no pending Destroy + not ERROR.
// Caller MUST hold g_ringMutex so an explicit Stop (which closes admission under
// this same lock) can never be undone by a racing event-branch state promotion.
bool LiveSessionAllowedLocked()
{
    return g_ringAdmission && !g_stopRequested.load() &&
           g_state.load() != VRSIM_ERROR;
}

// ---- HIGHRES-H2-GPU stats helpers (short g_gpuStatsMutex hold; no D3D/copy) --
// Locked variant: caller already holds g_gpuStatsMutex (used by RingSetAdmission,
// which nests the ring lock and must NOT re-enter the locking wrapper).
void GpuStatsResetActiveLocked()
{
    g_gpuStats.activeSource = VRSIM::kGpuActiveSourceNone;
    g_gpuStats.activeFrameSerial = 0;
    g_gpuStats.activePoseSerial = 0;
    g_gpuStats.activeWidth = 0;
    g_gpuStats.activeHeight = 0;
    g_gpuStats.lastSuccessfulLayerFrameSerial = 0;
    g_gpuStats.lastSuccessfulLayerPoseSerial = 0;
}
void GpuStatsResetActive()
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex);
    GpuStatsResetActiveLocked();
}
void GpuStatsRawAccepted()
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex); ++g_gpuStats.rawAccepted;
}
void GpuStatsRawRejected()
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex); ++g_gpuStats.rawRejected;
}
void GpuStatsExactPoseRejected()
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex); ++g_gpuStats.exactPoseRejected;
}
void GpuStatsPairFailed(uint64_t dispatchTicks)
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex);
    ++g_gpuStats.gpuPairFailed;
    g_gpuStats.lastGpuDispatchQpcTicks = dispatchTicks;
}
void GpuStatsCommitGpu(uint64_t frameSerial, uint64_t poseSerial,
                       uint32_t w, uint32_t h, uint64_t dispatchTicks)
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex);
    ++g_gpuStats.gpuPairCommitted;
    g_gpuStats.activeSource = VRSIM::kGpuActiveSourceGpuLive;
    g_gpuStats.activeFrameSerial = frameSerial;
    g_gpuStats.activePoseSerial = poseSerial;
    g_gpuStats.activeWidth = w;
    g_gpuStats.activeHeight = h;
    g_gpuStats.lastGpuDispatchQpcTicks = dispatchTicks;
}
void GpuStatsCommitCpu(uint64_t frameSerial, uint64_t poseSerial, uint32_t w, uint32_t h,
                       uint32_t source)
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex);
    g_gpuStats.activeSource = source;
    g_gpuStats.activeFrameSerial = frameSerial;
    g_gpuStats.activePoseSerial = poseSerial;
    g_gpuStats.activeWidth = w;
    g_gpuStats.activeHeight = h;
}
// Records a layer actually delivered to the runtime. HIGHRES-H2 safetyfix: the
// LIVE-pair gate is enforced HERE under g_gpuStatsMutex (no caller may read
// g_gpuStats unlocked): the committed source must be CPU- or GPU-live, both
// active serials nonzero, and the wire admission reserved bit0 (session open)
// set — a closed/newEpoch session cleared them under the ring->stats lock order.
// Then increments uniqueLiveLayerOk only for a distinct active frame serial
// (dedup vs repeated runtime ticks).
void GpuStatsRecordLayerDelivered()
{
    std::lock_guard<std::mutex> lk(g_gpuStatsMutex);
    if (g_gpuStats.activeSource != VRSIM::kGpuActiveSourceCpuLive &&
        g_gpuStats.activeSource != VRSIM::kGpuActiveSourceGpuLive) return;
    if (g_gpuStats.activeFrameSerial == 0 || g_gpuStats.activePoseSerial == 0) return;
    if ((g_gpuStats.reserved & 1u) == 0) return;
    const uint64_t af = g_gpuStats.activeFrameSerial;
    if (af > 0 && af != g_gpuStats.lastSuccessfulLayerFrameSerial)
    {
        ++g_gpuStats.uniqueLiveLayerOk;
        g_gpuStats.lastSuccessfulLayerFrameSerial = af;
        g_gpuStats.lastSuccessfulLayerPoseSerial = g_gpuStats.activePoseSerial;
    }
}

// worker-only: lazily (re)initialize the helper at the current scene size.
// HIGHRES-H2-COLOR: plane format follows the frozen session color contract
// (91 when sRGB, else the legacy 87) and must match the selected swapchain
// format exactly — no silent mismatch. Rebuilds on dimension or format change.
bool EnsureGpuPassReady(uint32_t w, uint32_t h)
{
    if (!g_dev || !g_ctx) return false;
    const DXGI_FORMAT expectedFormat = g_sessionColorSrgb
        ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (g_swapFormat != (int64_t)expectedFormat) return false;
    if (g_gpuPassReady && g_gpuPassW == w && g_gpuPassH == h && g_gpuPassFmt == expectedFormat) return true;
    const HRESULT hr = g_gpuPass.Initialize(g_dev, g_ctx, w, h, expectedFormat);
    if (FAILED(hr))
    { g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0; g_gpuPassFmt = DXGI_FORMAT_B8G8R8A8_UNORM; return false; }
    const bool firstInit = !g_gpuPassReady;
    g_gpuPassReady = true; g_gpuPassW = w; g_gpuPassH = h; g_gpuPassFmt = expectedFormat;
    if (firstInit || expectedFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
        LogLine("XR", "gpu pass helper init sourceEncoding=" + std::string(g_sessionColorSrgb ? "srgb" : "legacy") +
                " sourceSrvFormat=" + std::to_string((int64_t)expectedFormat) +
                " outputRtvFormat=" + std::to_string((int64_t)expectedFormat) +
                " swapFormat=" + std::to_string(g_swapFormat));
    return true;
}

// worker-only: does a raw plan's tangent set match the EXACT pose FOV and the
// canonical square capture envelope (ty=max(U,-D,max(-L,R)/aspect), tx=aspect*ty)?
bool RawPlanMatchesPose(const VRSIM::ProjectionPlanV1& p, const XrFovf& fov, double aspect)
{
    auto tanClose = [](double got, double expected)
    {
        const double tol = 1e-5 * (std::max)(1.0, std::fabs(expected));
        return std::fabs(got - expected) <= tol;
    };
    if (!tanClose(p.tanL, std::tan(fov.angleLeft)))  return false;
    if (!tanClose(p.tanR, std::tan(fov.angleRight))) return false;
    if (!tanClose(p.tanU, std::tan(fov.angleUp)))    return false;
    if (!tanClose(p.tanD, std::tan(fov.angleDown)))  return false;
    const double coverX = (std::max)(-p.tanL, p.tanR);
    const double ty = (std::max)((std::max)(p.tanU, -p.tanD), coverX / aspect);
    const double tx = aspect * ty;
    if (std::fabs(p.ty - ty) > 1e-6) return false;
    if (std::fabs(p.tx - tx) > 1e-6) return false;
    return true;
}

// Submit-time structural validation of a copied raw plan (called on the caller
// thread BEFORE any ring claim). Checks the wire header, FOV sign convention
// (left/down negative, right/up positive), positive coverage, the square-envelope
// half tangents, and that every value survives a float conversion (the shader
// constants are float) with finite, nonzero directions and positive half tans.
// The exact tangent/FOV pairing against the live pose is verified later by
// RawPlanMatchesPose on the worker; this rejects only ill-formed plans early.
bool ValidatePlanForSubmit(const VRSIM::ProjectionPlanV1& p, double aspect)
{
    if (p.structBytes != VRSIM::kProjectionPlanV1Bytes) return false;
    if (p.version != VRSIM::kProjectionPlanV1Version) return false;
    if (!(aspect > 0.0 && std::isfinite(aspect))) return false;
    if (!std::isfinite(p.tanL) || !std::isfinite(p.tanR) ||
        !std::isfinite(p.tanU) || !std::isfinite(p.tanD) ||
        !std::isfinite(p.tx)   || !std::isfinite(p.ty)) return false;
    // FOV sign convention (VC-style radians baked to tangents)
    if (!(p.tanL < 0.0)) return false;
    if (!(p.tanR > 0.0)) return false;
    if (!(p.tanU > 0.0)) return false;
    if (!(p.tanD < 0.0)) return false;
    // positive coverage / extent
    if (!(p.tanR - p.tanL > 0.0)) return false;
    if (!(p.tanU - p.tanD > 0.0)) return false;
    // canonical half tangents must be positive
    if (!(p.tx > 0.0 && p.ty > 0.0)) return false;
    // float conversion: finite, nonzero directions, positive half tans (shader floats)
    const float fl = (float)p.tanL, fr = (float)p.tanR, fu = (float)p.tanU, fd = (float)p.tanD;
    const float ftx = (float)p.tx, fty = (float)p.ty;
    if (!std::isfinite(fl) || !std::isfinite(fr) || !std::isfinite(fu) || !std::isfinite(fd) ||
        !std::isfinite(ftx) || !std::isfinite(fty)) return false;
    if (fl == 0.0f || fr == 0.0f || fu == 0.0f || fd == 0.0f) return false;
    if (!(ftx > 0.0f && fty > 0.0f)) return false;
    return true;
}

// worker-only scene source for a given eye. Returns a BORROWED pointer (never
// Released by callers). When the active source is GPU this is the reprojection
// Output() EVEN IF NULL: an active-GPU session must never silently fall back to
// the CPU scene texture with GPU pose metadata (that would show a mismatched
// pair). Otherwise the CPU scene / synthetic quad texture.
// HIGHRES-H2SH-CARD2B P1-7 fix: the pure cached shared-lifetime decision runs
// BEFORE the whole function (g_sceneMode included) and is keyed on
// lifetimeStarted (covering PREPARING and the bound=false init gap), not just
// bound. Once any shared lease exists the ONLY valid source is an active-GPU
// Output with a valid active layer pose; anything else is nullptr — the shared
// route NEVER falls back to the stale CPU sceneTex or the synthetic quadTex.
// Sessions without any shared lease keep the untouched scene/quad selection.
// Pure metadata read under the short shared gate; NO COM/GPU/device call while
// the gate is held.
ID3D11Texture2D* SceneSource(int eye)
{
    bool sharedLease = false;
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        sharedLease = g_sharedLifetimeStarted || g_sharedBound;
    }
    if (sharedLease)
    {
        if (!g_activeSourceGpu || !g_haveLayerPose) return nullptr;
        return g_gpuPass.Output(eye);
    }
    if (g_sceneMode)
    {
        if (g_activeSourceGpu) return g_gpuPass.Output(eye);
        return g_sceneTex[eye];
    }
    return g_quadTex[eye];
}

// worker-thread cleanup: release the worker-owned helper and invalidate the
// active GPU identity/layer pose. Called on worker exit (incl. init failure)
// before the caller's context release, and again (idempotently) after join.
void GpuWorkerCleanup()
{
    g_gpuPass.Reset();
    g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
    g_activeSourceGpu = false;
    g_haveLayerPose = false; g_lastLayerPoseSerial = 0;
    GpuStatsResetActive();
}

// Serialized Create contract: called ONLY when no previous worker exists (Create
// rejects when joinable) and the producer is stopped, so touching worker-owned
// fields here is safe. Clears stale ring slots, the pose ring, layer/consumed
// serials, pending scene dims and the active GPU identity/flags so a new session
// never reuses an old session's pose or a stolen raw slot's payload. Cumulative
// counters (rawAccepted/rawRejected/gpuPair*/uniqueLiveLayerOk/...) are preserved.
void CreateResetStaleState()
{
    {
        std::lock_guard<std::mutex> lk(g_ringMutex);
        for (int i = 0; i < RING_SLOTS; ++i)
        {
            g_ring[i].state.store(0);
            g_ring[i].epoch = 0;
        }
        g_latestReadySerial = 0;
    }
    for (int i = 0; i < POSE_RING; ++i) g_poseRing[i].valid = false;
    g_poseRingIdx = 0;
    g_haveLayerPose = false;
    g_lastLayerPoseSerial = 0;
    g_lastConsumedSerial = 0;
    g_pendingSceneW = 0; g_pendingSceneH = 0;
    g_sceneMode = false;
    g_renderSource = RSM_SYNTHETIC_TEST;
    g_vcSceneRequested = false;
    // active GPU identity + wire identity (helper holds no live resources here:
    // the prior worker already ran GpuWorkerCleanup on exit)
    g_activeSourceGpu = false;
    g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
    std::lock_guard<std::mutex> slk(g_gpuStatsMutex);
    GpuStatsResetActiveLocked();
}

// consumed by the worker between frames: claim newest READY slot, validate,
// then D3D upload. HIGHRES-H2-GPU: exact-pose matching BEFORE any write; on
// every reject path active state (textures, layer pose, counters) stays untouched.
void WorkerConsumeLatestFrame()
{
    const int best = RingClaimForRead();
    if (best < 0) { ++g_reusedFrames; return; }
    StereoSlot& s = g_ring[best];

    // ---- validate BEFORE any UpdateSubresource / state mutation ----
    bool ok = true;
    if (s.width != (int)g_swapW || s.height != (int)g_swapH) ok = false;
    const uint64_t rowBytes = (uint64_t)s.width * 4ull;
    if (s.stride <= 0 || (uint64_t)s.stride < rowBytes) ok = false;
    const size_t payload = (size_t)s.stride * (size_t)s.height;
    if (g_ringL[best].size() < payload || g_ringR[best].size() < payload) ok = false;
    if (!std::isfinite(s.cameraYawDeg)) ok = false;
    if (s.frameSerial <= g_lastConsumedSerial.load()) ok = false; // stale/duplicate
    if (s.frameSerial == 0) ok = false;

    const bool rawGpu = (s.mode == (int)VRSIM::kGpuSlotModeRawGpu);
    if (ok && rawGpu && s.poseSerial == 0) ok = false; // raw frames need an exact pose

    // raw consumer: the slot's claim epoch must still be the live epoch AND
    // admission must still be open BEFORE any GPU work — never commit a capture
    // made against a stopped/stale session.
    bool rawEpochStale = false;
    if (ok && rawGpu)
    {
        bool admitOpen = false; unsigned long long liveEpoch = 0;
        RingSnapshotAdmissionEpoch(&admitOpen, &liveEpoch);
        if (!admitOpen || s.epoch != liveEpoch) { rawEpochStale = true; ok = false; }
    }

    // live frames must pair with the EXACT PoseSample serial; no nearest-older fallback
    const bool legacyPreview = (!rawGpu && s.poseSerial == 0); // STATIC PREVIEW ONLY
    const PoseSample* pose = nullptr;
    bool poseMissing = false;
    if (ok && !legacyPreview)
    {
        for (int i = 0; i < POSE_RING; ++i)
        {
            const PoseSample& ps = g_poseRing[i];
            if (ps.valid && ps.serial == s.poseSerial) { pose = &ps; break; }
        }
        if (!pose) { poseMissing = true; ok = false; }
    }
    // raw-GPU: plan tangents + canonical envelope must match the exact pose FOV
    bool rawPlanMismatch = false;
    if (ok && rawGpu)
    {
        const double aspect = (double)s.width / (double)s.height;
        rawPlanMismatch = !RawPlanMatchesPose(s.leftPlan, pose->fl, aspect) ||
                          !RawPlanMatchesPose(s.rightPlan, pose->fr, aspect);
        if (rawPlanMismatch) ok = false;
    }
    if (!ok)
    {
        if (rawGpu)
        {
            // invalid / missing serial / plan mismatch: drop, keep old texture/active
            GpuStatsExactPoseRejected();
            ++g_framesDropped;
        }
        else if (poseMissing) { ++g_poseImageMismatch; ++g_framesDropped; }
        RingReleaseToFree(best);
        return; // keep prior textures/layer pose/consumed serial; no d3dUploads++
    }

    if (rawGpu)
    {
        // ---- worker-owned GPU reprojection dispatch (CPU enqueue timed) ----
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        const bool ready = EnsureGpuPassReady((uint32_t)s.width, (uint32_t)s.height);
        HRESULT hr = E_FAIL;
        if (ready)
        {
            hr = g_gpuPass.RenderPair(g_ringL[best].data(), g_ringL[best].size(),
                                      g_ringR[best].data(), g_ringR[best].size(),
                                      (uint32_t)s.stride, s.leftPlan, s.rightPlan);
        }
        QueryPerformanceCounter(&t1);
        const uint64_t dispatchTicks = (uint64_t)(t1.QuadPart - t0.QuadPart);
        ID3D11Texture2D* outL = g_gpuPass.Output(0);
        ID3D11Texture2D* outR = g_gpuPass.Output(1);
        if (FAILED(hr) || !outL || !outR)
        {
            // failure: do NOT switch active source/pose/frame
            GpuStatsPairFailed(dispatchTicks);
            RingReleaseToFree(best);
            return;
        }
        // The epoch may have moved while the helper was dispatching. The FINAL
        // admission/epoch check AND every commit (worker source/pose/FOV/frame
        // counters + the GPU stats commit) run in ONE short g_ringMutex section,
        // so a Stop can neither close/newEpoch/clearStats between check and commit
        // nor let a new epoch repopulate the old active identity. Lock order is
        // ring -> gpuStats (same as RingSetAdmission). No D3D or large copies
        // under the lock; RingReleaseToFree runs only AFTER the scope.
        bool dropped = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            if (!g_ringAdmission || s.epoch != g_ringEpoch)
            {
                // stale/closed: invalidate the worker GPU identity + exact pair
                dropped = true;
                g_activeSourceGpu = false;   // never display a wrong pair
                g_haveLayerPose = false;
                g_lastLayerPoseSerial = 0;
                GpuStatsResetActive();       // stats lock nested in allowed order
            }
            else
            {
                // ---- commit GPU source + exact layer pose/FOV/frame together ----
                g_activeSourceGpu = true;
                ++g_d3dUploads;
                if (s.cameraYawDeg != g_lastConsumedYawDeg.load()) ++g_consumedYawChanges;
                g_lastConsumedYawDeg = s.cameraYawDeg;
                g_lastConsumedSerial = s.frameSerial;
                g_layerPl = pose->pl; g_layerPr = pose->pr;
                g_layerFl = pose->fl; g_layerFr = pose->fr;
                g_lastLayerPoseSerial = pose->serial; // == s.poseSerial (exact)
                g_haveLayerPose = true;
                GpuStatsCommitGpu(s.frameSerial, pose->serial,
                                  (uint32_t)s.width, (uint32_t)s.height, dispatchTicks);
            }
        }
        if (dropped)
        {
            GpuStatsPairFailed(dispatchTicks);
            ++g_framesDropped;
        }
        RingReleaseToFree(best);
        return;
    }

    if (!g_sceneTex[0] || !g_sceneTex[1]) { RingReleaseToFree(best); return; }
    const D3D11_BOX box{ 0, 0, 0, (UINT)s.width, (UINT)s.height, 1 };
    g_ctx->UpdateSubresource(g_sceneTex[0], 0, &box, g_ringL[best].data(), s.stride, 0);
    g_ctx->UpdateSubresource(g_sceneTex[1], 0, &box, g_ringR[best].data(), s.stride, 0);
    if (FAILED(g_dev->GetDeviceRemovedReason()))
    {
        g_uploadFailed = true; // flag only: live failure NEVER paints the red fallback
        RingReleaseToFree(best);
        return; // no successful-upload count, no pose/consumed commits
    }
    g_uploadFailed = false;

    // ---- commit pose/FOV + consumed state together (worker-only fields) ----
    g_activeSourceGpu = false; // CPU path selected
    ++g_d3dUploads;
    if (s.cameraYawDeg != g_lastConsumedYawDeg.load()) ++g_consumedYawChanges;
    g_lastConsumedYawDeg = s.cameraYawDeg;
    g_lastConsumedSerial = s.frameSerial;
    if (legacyPreview)
    {
        // static preview must never inherit a previous live layer pose
        g_haveLayerPose = false;
        g_lastLayerPoseSerial = 0;
        GpuStatsCommitCpu(s.frameSerial, 0, (uint32_t)s.width, (uint32_t)s.height,
                          VRSIM::kGpuActiveSourceStaticPreview);
    }
    else
    {
        g_layerPl = pose->pl; g_layerPr = pose->pr;
        g_layerFl = pose->fl; g_layerFr = pose->fr;
        g_lastLayerPoseSerial = pose->serial; // == s.poseSerial (exact)
        g_haveLayerPose = true;
        GpuStatsCommitCpu(s.frameSerial, pose->serial,
                          (uint32_t)s.width, (uint32_t)s.height,
                          VRSIM::kGpuActiveSourceCpuLive);
    }
    static bool firstLogged = false;
    if (!firstLogged) { firstLogged = true;
        LogLine("XR", "first VC frame uploaded serial=" + std::to_string(s.frameSerial) +
            " " + std::to_string(s.width) + "x" + std::to_string(s.height) +
            (legacyPreview ? " (legacy static-preview poseSerial=0)" :
             " poseSerial=" + std::to_string(s.poseSerial))); }
    RingReleaseToFree(best);
}

// ---- HIGHRES-H2SH-CARD2: worker shared-GPU consumer ------------------------
// Worker-thread only. The bridge ticket travels BY VALUE with the exact
// original ticket (never a slot index). SRVs returned by BeginReadOnWorker are
// BORROWED: never Released, never stored on the native slot. Every successful
// BeginRead is paired with EndReadOnWorker on the SAME worker context,
// including rejected or nothing-projected reads, and End runs only AFTER the
// actual RenderSharedPair commands. Only a successful End moves the native
// slot to INFLIGHT; the ring slot returns to FREE exclusively through
// WorkerPollSharedCompletion's proven bridge cached FREE + exact same
// slot/ticket/epoch/generation provenance. No elapsed/sleep ever infers GPU
// completion, and no legacy counter (uniqueLiveLayerOk authority) is touched:
// the sole content/FPS authority remains the existing successful xrEndFrame +
// both-eye fill + distinct activeFrameSerial path.

// Recycle real bridge completion events. Called EVERY worker iteration,
// including session-not-running, shouldRender=false and no-new-frame cases,
// so an INFLIGHT ticket can never be starved of its release proof.
void WorkerPollSharedCompletion()
{
    bool bound = false;
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        bound = g_sharedBound;
    }
    if (!bound) return;

    const HRESULT hr = g_sharedBridge.PollCompletedOnWorker();
    if (hr != S_OK && hr != S_FALSE)
    {
        // Unexpected HRESULT: the bridge retains internally. Mirror the
        // quarantine, keep every INFLIGHT/READING slot reserved, NEVER
        // fabricate a Free.
        const bool bq = g_sharedBridge.Quarantined();
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        g_sharedLastBridgeHresult = (long long)hr;
        if (bq)
        {
            g_sharedQuarantined = true;
            for (int i = 0; i < RING_SLOTS; ++i)
                if (g_sharedPhase[i] == kSharedPhaseInflight ||
                    g_sharedPhase[i] == kSharedPhaseReading)
                    g_sharedPhase[i] = kSharedPhaseQuarantined;
        }
        return;
    }

    // The bridge releases INFLIGHT -> FREE internally only on its actual
    // S_OK + TRUE GetData; consult the cached stats for that proven fact.
    VRSIM::SharedBridgeStatsV1 bs{};
    g_sharedBridge.Stats(&bs); // pure cached atomics; no D3D/wait under the gate
    std::lock_guard<std::mutex> rlk(g_ringMutex);
    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
    g_sharedLastBridgeHresult = (long long)hr;
    for (int i = 0; i < RING_SLOTS; ++i)
    {
        if (g_sharedPhase[i] != kSharedPhaseInflight) continue;
        if (bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree) continue; // pending: retain
        StereoSlot& s = g_ring[i];
        const VRSIM::SharedPreparedPairV1& t = s.sharedTicket;
        // FREE回收必须同时满足：bridge 实际 cached FREE（上面已判）+ 同一 slot +
        // 原 ticket + 原 epoch + 原 generation 的完整 provenance。
        const bool proven =
            s.mode == kGpuSlotModeSharedV1 && s.state.load() == 3 &&
            t.slotIndex == (uint32_t)i && t.ticket != 0 && t.epoch != 0 && t.generation != 0 &&
            t.epoch == g_sharedBoundEpoch && t.generation == g_sharedBoundGeneration &&
            s.epoch == t.epoch;
        if (!proven)
        {
            // Bridge says FREE but the native pairing is not the exact same
            // provenance: retain + quarantine instead of a fabricated recycle.
            g_sharedQuarantined = true;
            g_sharedPhase[i] = kSharedPhaseQuarantined;
            continue;
        }
        s.state.store(0);
        s.epoch = 0;
        s.mode = (int)VRSIM::kGpuSlotModeCpu;
        s.sharedTicket = VRSIM::SharedPreparedPairV1{};
        s.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
        g_sharedPhase[i] = kSharedPhaseNone;
    }
}

// Consume one shared COMMITTED ticket (ring READY + phase Committed). The
// legacy CPU/raw consumer never reaches this path and shared slots are never
// claimed by it; a bound session takes this consumer exclusively.
void WorkerConsumeSharedFrame()
{
    int slot = -1;
    VRSIM::SharedPreparedPairV1 ticket{};
    VRSIM::SharedCaptureIdentityV1 id{};
    unsigned long long claimEpoch = 0;
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        // Fair drain: pick the OLDEST shared READY ticket (lowest frame
        // serial), so a newest-frame pick can never abandon an older COMMITTED
        // ticket ahead of its read/end.
        int best = -1; unsigned long long bestSerial = 0;
        for (int i = 0; i < RING_SLOTS; ++i)
        {
            if (g_ring[i].state.load() != 2) continue;
            if (g_ring[i].mode != kGpuSlotModeSharedV1) continue;
            if (best < 0 || g_ring[i].frameSerial < bestSerial) { best = i; bestSerial = g_ring[i].frameSerial; }
        }
        if (best < 0) { ++g_reusedFrames; return; } // honest reused: never fabricate a frame from old bytes
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        // HIGHRES-H2SH-CARD2B: no new BeginRead claim once the shared lifecycle
        // is closing/closed (linearized with the close in this same ring -> meta
        // gate). Unread COMMITTED/PREPARED tickets stay READY for the source
        // UI's exact-ticket discard; the worker only Polls existing INFLIGHT.
        if (g_sharedClosing || g_sharedAdmissionClosed) return;
        if (g_sharedPhase[best] != kSharedPhaseCommitted) return; // leave READY untouched
        StereoSlot& s = g_ring[best];
        // one short metadata gate: snapshot this round's ticket/identity/
        // dimensions/epoch BY VALUE, then register the worker READING lease
        slot = best;
        ticket = s.sharedTicket;
        id = s.sharedIdentity;
        claimEpoch = s.epoch;
        s.state.store(3);
        g_sharedPhase[slot] = kSharedPhaseReading;
    }

    StereoSlot& s = g_ring[slot]; // worker-exclusive while READING

    // ---- every validation runs BEFORE any projection ----
    bool valid = true;
    if (ticket.version != VRSIM::kStereoBridgeVersionV1 ||
        ticket.structBytes != (uint32_t)sizeof(VRSIM::SharedPreparedPairV1) ||
        ticket.slotIndex != (uint32_t)slot ||
        ticket.ticket == 0 || ticket.epoch == 0 || ticket.generation == 0 ||
        ticket.width != VRSIM::kStereoBridgeWidthV1 ||
        ticket.height != VRSIM::kStereoBridgeHeightV1 ||
        ticket.format88 != VRSIM::kStereoBridgeFormat88V1) valid = false;
    if (id.structBytes != VRSIM::kSharedCaptureIdentityV1Bytes ||
        id.version != VRSIM::kSharedCaptureIdentityV1Version ||
        id.reserved != 0) valid = false;
    if (id.frameSerial == 0 || id.poseSerial == 0) valid = false;
    if (!std::isfinite(id.cameraYawDeg)) valid = false;
    // identity agrees with the ring slot's recorded metadata (by-value compare)
    if (s.frameSerial != id.frameSerial || s.poseSerial != id.poseSerial ||
        s.epoch != claimEpoch || claimEpoch == 0 ||
        s.mode != kGpuSlotModeSharedV1 ||
        s.width != (int)VRSIM::kStereoBridgeWidthV1 ||
        s.height != (int)VRSIM::kStereoBridgeHeightV1) valid = false;
    if (!VRSIM::SharedPlanBitsEqual(s.leftPlan, id.leftPlan) ||
        !VRSIM::SharedPlanBitsEqual(s.rightPlan, id.rightPlan)) valid = false;
    // ticket, identity and the bound session epoch/generation must all agree
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        if (!g_sharedBound || g_sharedQuarantined) valid = false;
        if (ticket.epoch != g_sharedBoundEpoch || ticket.generation != g_sharedBoundGeneration) valid = false;
        if (id.epoch != ticket.epoch || id.generation != ticket.generation) valid = false;
    }
    if (valid && id.frameSerial <= g_lastConsumedSerial.load()) valid = false; // stale/duplicate
    // EXACT PoseSample.serial lookup in the worker-owned ring: NO latest/
    // nearest fallback; source UI never gains an unlocked poseRing access.
    const PoseSample* pose = nullptr;
    if (valid)
    {
        for (int i = 0; i < POSE_RING; ++i)
        {
            const PoseSample& ps = g_poseRing[i];
            if (ps.valid && ps.serial == id.poseSerial) { pose = &ps; break; }
        }
        if (!pose) valid = false;
    }
    // structural plan validation + both eyes' exact pose FOV match (locked
    // square 1536 profile => aspect exactly 1.0; validators unchanged)
    if (valid)
    {
        const double aspect = 1.0;
        const auto planStructOk = [&aspect](const VRSIM::ProjectionPlanV1& p)
        {
            if (!ValidatePlanForSubmit(p, aspect)) return false;
            if (!(p.tx >= (std::max)(-p.tanL, p.tanR))) return false;
            if (!(p.ty >= (std::max)(p.tanU, -p.tanD))) return false;
            if (std::fabs(p.tx / p.ty - aspect) > 1e-6) return false;
            return true;
        };
        valid = planStructOk(id.leftPlan) && planStructOk(id.rightPlan) &&
                RawPlanMatchesPose(id.leftPlan, pose->fl, aspect) &&
                RawPlanMatchesPose(id.rightPlan, pose->fr, aspect);
    }

    ID3D11ShaderResourceView* srvL = nullptr;
    ID3D11ShaderResourceView* srvR = nullptr;
    const HRESULT bhr = g_sharedBridge.BeginReadOnWorker(ticket, &srvL, &srvR);
    if (bhr != S_OK)
    {
        // No lease: NEVER clear the slot on the strength of the native
        // READING mark alone.
        const bool bq = g_sharedBridge.Quarantined();
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        g_sharedLastBridgeHresult = (long long)bhr;
        if (bq)
        {
            g_sharedQuarantined = true;
            g_sharedPhase[slot] = kSharedPhaseQuarantined; // retain, no reuse
        }
        else if (g_sharedAdmissionClosed &&
                 g_sharedPhase[slot] == kSharedPhaseReading &&
                 g_ring[slot].state.load() == 3 &&
                 VRSIM::SharedTicketFieldsEqual(ticket, g_ring[slot].sharedTicket))
        {
            // A closed-denied COMMITTED stays READY/COMMITTED for the source
            // UI's exact-ticket discard — never a generic FREE here.
            g_ring[slot].state.store(2);
            g_sharedPhase[slot] = kSharedPhaseCommitted;
        }
        else
        {
            // unexpected refusal with admission open: retain + quarantine
            g_sharedQuarantined = true;
            g_sharedPhase[slot] = kSharedPhaseQuarantined;
        }
        return;
    }

    // Lease GRANTED: srvL/srvR are borrowed persistent pointers (never
    // Released, never stored); EndRead on this same worker context is now
    // mandatory on every path below.
    HRESULT rhr = E_FAIL;
    bool rendered = false;
    bool renderThrew = false;
    uint64_t dispatchTicks = 0;
    if (valid && srvL && srvR)
    {
        // P1-1: EnsureGpuPassReady/RenderSharedPair can throw from std::string/
        // vector work (log concatenation). Catch here so the mandatory
        // EndReadOnWorker below ALWAYS runs and no exception escapes the
        // worker. A throw mid-dispatch leaves the frozen helper's active pair
        // INDETERMINATE: treat it as a new-image attempt (invalidate below).
        try
        {
            if (EnsureGpuPassReady(VRSIM::kStereoBridgeWidthV1, VRSIM::kStereoBridgeHeightV1))
            {
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                // frozen actual signature: exactly four arguments, no width/height
                rhr = g_gpuPass.RenderSharedPair(srvL, srvR, id.leftPlan, id.rightPlan);
                QueryPerformanceCounter(&t1);
                dispatchTicks = (uint64_t)(t1.QuadPart - t0.QuadPart);
                rendered = (rhr == S_OK);
            }
        }
        catch (...)
        {
            renderThrew = true;
            rendered = false;
        }
    }

    // End AFTER the real RenderSharedPair commands — also for a failed
    // projection, a pure drain-without-projection read and a render path that
    // threw. P1-1: End's OWN exception is caught here — never retried on the
    // same lease, the ticket is retained as unproven/quarantined (never a
    // generic FREE) and nothing escapes the worker. No logging inside either
    // catch: std::string work could throw again.
    HRESULT ehr = E_FAIL;
    try
    {
        ehr = g_sharedBridge.EndReadOnWorker(ticket);
    }
    catch (...)
    {
        ehr = E_FAIL;
    }
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        g_sharedLastBridgeHresult = (long long)ehr;
        if (ehr == S_OK) g_sharedPhase[slot] = kSharedPhaseInflight; // only End success -> INFLIGHT
        else { g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined; }
    }

    if (!valid)
    {
        // validation-rejected COMMITTED: lease drained without projecting; the
        // poll releases it on the proven bridge FREE. No image was attempted,
        // so the previous active pair stays valid; this is NOT a projection
        // success and never counted as one.
        GpuStatsExactPoseRejected();
        ++g_framesDropped;
        return;
    }

    // P1-2: the frozen helper switches ACTIVE to the new images on S_OK.
    // Whenever a new image was attempted (S_OK switch or indeterminate throw)
    // but the metadata commit does NOT happen (End failed/threw, outputs
    // missing, device removed, stale/closed final gate), the native active
    // pose/frame/stats must be invalidated so the new image can never keep
    // being delivered under the OLD pose. An ordinary RenderSharedPair HRESULT
    // failure with no rebuild/throw preserves the old pair per the frozen
    // helper contract.
    const bool newImageAttempted = rendered || renderThrew;
    ID3D11Texture2D* outL = nullptr;
    ID3D11Texture2D* outR = nullptr;
    bool deviceOk = false;
    bool outputQueryThrew = false;
    if (rendered && !renderThrew)
    {
        try
        {
            outL = g_gpuPass.Output(0);
            outR = g_gpuPass.Output(1);
            deviceOk = g_dev && SUCCEEDED(g_dev->GetDeviceRemovedReason());
        }
        catch (...)
        {
            outputQueryThrew = true;
        }
    }
    const bool commitEligible =
        rendered && !renderThrew && !outputQueryThrew && outL && outR &&
        deviceOk && ehr == S_OK;

    bool committed = false;
    if (commitEligible)
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        StereoSlot& cs = g_ring[slot];
        const bool same =
            g_ringAdmission && claimEpoch == g_ringEpoch &&
            cs.state.load() == 3 && cs.mode == kGpuSlotModeSharedV1 &&
            cs.epoch == claimEpoch &&
            VRSIM::SharedTicketFieldsEqual(ticket, cs.sharedTicket) &&
            ticket.epoch == g_sharedBoundEpoch &&
            ticket.generation == g_sharedBoundGeneration &&
            g_sharedPhase[slot] == kSharedPhaseInflight &&
            !g_sharedQuarantined;
        if (same)
        {
            g_activeSourceGpu = true;
            ++g_d3dUploads;
            if (id.cameraYawDeg != g_lastConsumedYawDeg.load()) ++g_consumedYawChanges;
            g_lastConsumedYawDeg = id.cameraYawDeg;
            g_lastConsumedSerial = id.frameSerial;
            g_layerPl = pose->pl; g_layerPr = pose->pr;
            g_layerFl = pose->fl; g_layerFr = pose->fr;
            g_lastLayerPoseSerial = pose->serial; // == id.poseSerial (exact)
            g_haveLayerPose = true;
            GpuStatsCommitGpu(id.frameSerial, pose->serial,
                              VRSIM::kStereoBridgeWidthV1, VRSIM::kStereoBridgeHeightV1,
                              dispatchTicks);
            committed = true;
        }
    }
    if (committed) return;
    if (newImageAttempted)
    {
        // P1-2: S_OK switch or indeterminate throw WITHOUT a metadata commit
        // (End failed/threw, outputs missing, device removed, stale/closed
        // final gate): clear the active GPU identity under the short ring ->
        // gpuStats gate so a shared-bound fill gets nullptr (P1-3
        // SceneSource) instead of the new image under the old pose. The
        // ticket was already End-drained above or marked quarantined; the
        // slot/phase stay reserved, never a generic FREE.
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        g_activeSourceGpu = false;
        g_haveLayerPose = false;
        g_lastLayerPoseSerial = 0;
        GpuStatsResetActive(); // ring -> gpuStats nesting, established order
        GpuStatsPairFailed(dispatchTicks); // same failure accounting as the legacy stale drop
        ++g_framesDropped;
    }
    else
    {
        // ordinary projection failure (no helper switch): the previous
        // successful pair and its exact pose remain active, as on the legacy
        // raw path; never presented as a successful shared projection.
        GpuStatsPairFailed(dispatchTicks);
    }
}

// Worker loop consumption selection: PURE metadata bound decision, no GPU
// initialization on the query path. A shared LEASE (lifetimeStarted from the
// first paired-FREE grant, including PREPARING / the bound=false init gap, or
// bound) MUST use the shared consumer (no raw-byte/CPU fallback); every other
// session keeps the untouched legacy WorkerConsumeLatestFrame behavior.
void WorkerConsumeSceneFrame()
{
    bool sharedLease = false;
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        sharedLease = g_sharedLifetimeStarted || g_sharedBound;
    }
    if (sharedLease) WorkerConsumeSharedFrame();
    else WorkerConsumeLatestFrame();
}

// ---- HIGHRES-H2 FIX: bounded worker-owned diagnostic readback --------------
// VRSIM_DumpSceneTexture (exported) never touches the device: the calling thread
// only validates, queues ONE request and waits (<=1000ms) on a promise. ALL D3D
// work (staging CreateTexture/CopyResource/Map/Unmap) happens on the worker
// thread between frames. Heap shared ownership keeps a caller timeout safe: the
// worker still completes and releases the request without any caller pointer.
struct DumpResult
{
    int rc = -1;
    int w = 0, h = 0;
    std::vector<unsigned char> bytes; // owned CPU copy; valid only when rc == 0
};

struct DumpRequest
{
    int eye = 0;
    int cap = 0;
    uint64_t generation = 0;                  // dump-session generation at queue time
    std::shared_ptr<std::atomic<bool>> canceled;
    std::shared_ptr<std::promise<DumpResult>> promise;
};

std::mutex                      g_dumpMutex;
std::shared_ptr<DumpRequest>    g_dumpSlot;   // non-null while g_dumpStage != 0
int                             g_dumpStage = 0; // 0 idle, 1 pending, 2 in-flight (worker)
std::atomic<bool>               g_dumpAccepting{ false };
std::atomic<uint64_t>           g_dumpGeneration{ 0 };

void DumpCompleteWith(const std::shared_ptr<DumpRequest>& req, DumpResult&& res)
{
    if (!req || !req->promise) return;
    try { req->promise->set_value(std::move(res)); } catch (...) {}
}

// Stop / Destroy / worker-exit / Create-reset: refuse new requests, cancel the
// not-yet-consumed one (rc -6) and flag an in-flight one so the worker fails it
// explicitly. Generation bump keeps stale requests out of the next session.
void DumpCancelPending()
{
    std::shared_ptr<DumpRequest> pending;
    {
        // generation/admission flips share g_dumpMutex with the slot selection
        // and the worker's commit recheck, so cancel-vs-complete linearizes
        std::lock_guard<std::mutex> lk(g_dumpMutex);
        ++g_dumpGeneration;
        g_dumpAccepting = false;
        if (g_dumpStage == 1) { pending = g_dumpSlot; g_dumpSlot.reset(); g_dumpStage = 0; }
        else if (g_dumpStage == 2 && g_dumpSlot) g_dumpSlot->canceled->store(true);
    }
    if (pending)
    {
        DumpResult res; res.rc = -6;
        DumpCompleteWith(pending, std::move(res));
    }
}

struct StagingTexGuard
{
    ID3D11Texture2D* t = nullptr;
    ~StagingTexGuard() { if (t) t->Release(); }
};

// worker-thread only: fill res with an owned CPU copy of one scene eye texture
void WorkerFillDump(DumpRequest& req, DumpResult& res)
{
    if (req.canceled->load() || g_stopRequested.load() ||
        req.generation != g_dumpGeneration.load() || !g_dumpAccepting.load())
    { res.rc = -6; return; }
    ID3D11Texture2D* src = SceneSource(req.eye); // projected GPU output when active-GPU
    if (!src || !g_dev || !g_ctx) { res.rc = -2; return; }
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    { res.rc = -4; return; } // unknown pixel layout: never guess
    if (d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1)
    { res.rc = -4; return; }
    const uint64_t rowBytes = (uint64_t)d.Width * 4ull;
    const uint64_t total = rowBytes * (uint64_t)d.Height;
    if (d.Width == 0 || d.Height == 0 || total > (64ull * 1024ull * 1024ull))
    { res.rc = -4; return; }
    if ((uint64_t)req.cap < total) { res.rc = -3; return; }
    StagingTexGuard sg; // released on every path, including Map failure
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &sg.t)) || !sg.t)
    { res.rc = -4; return; }
    g_ctx->CopyResource(sg.t, src); // returns void; device state checked below
    if (FAILED(g_dev->GetDeviceRemovedReason()))
    { res.rc = -4; return; }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(sg.t, 0, D3D11_MAP_READ, 0, &m)))
    { res.rc = -5; return; } // failed Map needs no Unmap
    try
    {
        res.bytes.resize((size_t)total);
        for (UINT y = 0; y < d.Height; ++y)
            memcpy(res.bytes.data() + (size_t)y * rowBytes,
                   (const unsigned char*)m.pData + (size_t)y * m.RowPitch,
                   (size_t)rowBytes);
    }
    catch (...) { g_ctx->Unmap(sg.t, 0); res.bytes.clear(); res.rc = -4; return; }
    g_ctx->Unmap(sg.t, 0);
    res.w = (int)d.Width; res.h = (int)d.Height; res.rc = 0;
}

// worker-thread only, called between frames even when the session is not
// running, so every caller wait fails in bounded time
void WorkerPumpDumpRequest()
{
    std::shared_ptr<DumpRequest> req;
    {
        std::lock_guard<std::mutex> lk(g_dumpMutex);
        if (g_dumpStage != 1) return;
        req = g_dumpSlot;
        g_dumpStage = 2;
    }
    DumpResult res;
    try { WorkerFillDump(*req, res); }
    catch (...) { res = DumpResult{}; res.rc = -4; }
    {
        // All D3D work and Unmap are done. Re-validate admission under the same
        // mutex that Stop/Destroy use to cancel, then commit the final result and
        // release the slot atomically. set_value never blocks and no waiter holds
        // g_dumpMutex, so publishing inside the lock is safe.
        std::lock_guard<std::mutex> lk(g_dumpMutex);
        if (req->canceled->load() || g_stopRequested.load() ||
            req->generation != g_dumpGeneration.load() || !g_dumpAccepting.load())
        {
            res = DumpResult{}; res.rc = -6; // drop owned bytes; Stop raced after fill
        }
        DumpCompleteWith(req, std::move(res));
        if (g_dumpSlot == req) g_dumpSlot.reset();
        if (g_dumpStage == 2) g_dumpStage = 0;
    }
}

} // namespace

void WorkerMain()
{
try
{
    LogLine("XR", "worker enter");
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        g_sharedWorkerCleanupDone = false;
        g_sharedWorkerNoContext = false;
        g_sharedWorkerDrained = false;
    }

    auto exitCleanup = [&]() -> void
    {
        unsigned long long frozenEpoch = 0;
        uint32_t frozenGen = 0;
        bool hadLease = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            hadLease = SharedLeaseOrStateLocked();
            if (hadLease)
            {
                SharedCloseLocked(false);
            }
            else
            {
                g_sharedClosing = true;
                if (g_ringAdmission) RingSetAdmissionLocked(false);
            }
            frozenEpoch = g_sharedBoundEpoch;
            frozenGen = g_sharedBoundGeneration;
        }
        DumpCancelPending();

        auto fullProofLocked = [&](const VRSIM::SharedBridgeStatsV1& bs) -> bool
        {
            if (!g_sharedClosing) return false;
            if (g_ringAdmission) return false;
            if (g_sharedSourceActiveCalls != 0) return false;
            if (g_sharedFinalizeBusy) return false;
            if (g_sharedQuarantined) return false;
            if (hadLease)
            {
                if (!g_sharedLifetimeStarted) return false;
                if (g_sharedBoundEpoch != frozenEpoch) return false;
                if (g_sharedBoundGeneration != frozenGen) return false;
                if (!g_sharedAdmissionClosed) return false;
            }
            else
            {
                if (g_sharedLifetimeStarted || g_sharedBound) return false;
            }
            for (int i = 0; i < RING_SLOTS; ++i)
            {
                if (g_sharedPhase[i] != kSharedPhaseNone) return false;
                if (g_ring[i].state.load() != 0) return false;
                if (g_ring[i].mode == kGpuSlotModeSharedV1) return false;
                if (i < (int)VRSIM::kStereoBridgeSlotCountV1 &&
                    bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree)
                    return false;
            }
            return true;
        };

        bool drainProven = false;
        const long long startMs = (long long)GetTickCount64();
        for (int iter = 0; iter < 128; ++iter)
        {
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                VRSIM::SharedBridgeStatsV1 bs{};
                g_sharedBridge.Stats(&bs);
                if (fullProofLocked(bs)) { drainProven = true; break; }
            }
            if ((long long)GetTickCount64() - startMs >= 1000) break;
            WorkerPollSharedCompletion();
            Sleep(8);
        }
        if (!drainProven)
        {
            LogLine("XR", "worker exit: drain unproven; CleanupDone stays false");
            return;
        }

        GpuWorkerCleanup();

        bool postMismatch = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            VRSIM::SharedBridgeStatsV1 bs{};
            g_sharedBridge.Stats(&bs);
            if (fullProofLocked(bs))
            {
                g_sharedWorkerCleanupDone = true;
                g_sharedWorkerNoContext = true;
                SharedUpdateDrainLocked(bs);
            }
            else
            {
                postMismatch = true;
            }
        }
        if (postMismatch)
            LogLine("XR", "worker post-cleanup state mismatch; proof not published");
    };

    int rc = SessionInitPhase();
    bool admit = false;
    {
        std::lock_guard<std::mutex> lk(g_dumpMutex);
        ++g_dumpGeneration;
        const bool open = (rc == 0) && !g_stopRequested.load() &&
                          (g_state.load() == VRSIM_STARTING);
        g_dumpAccepting = open;
        admit = open;
        if (open) SetState(VRSIM_SYNCHRONIZED);
    }
    RingSetAdmission(admit);
    try { g_initPromise.set_value(rc); } catch (...) {}
    if (rc != 0)
    {
        exitCleanup();
        LogLine("XR", "worker exit (init failed)");
        return;
    }
    LogLine("XR", "session init ok swap=" + std::to_string(g_swapW) + "x" + std::to_string(g_swapH) +
            " fmt=" + std::to_string(g_swapFormat));

    bool sessionBegun = false;
    uint64_t serial = 0;
    uint64_t visibleLogged = 0;
    uint64_t firstWaitLogged = 0;
    XrView views[2]{};
    views[0].type = XR_TYPE_VIEW; views[1].type = XR_TYPE_VIEW;
    g_tWorkerStarted = (long long)GetTickCount64();

    while (!g_stopRequested)
    {
        ++g_pollLoopCount;
        // HIGHRES-H2SH-CARD2: recycle shared bridge completion events at the
        // TOP of EVERY worker iteration — including session-not-running,
        // shouldRender=false and no-new-frame passes — so an INFLIGHT ticket
        // is never starved by content-conditional branches.
        WorkerPollSharedCompletion();
        // HIGHRES-H2SH-CARD2B: shared closing is processed BEFORE the XR frame
        // / xrWaitFrame path: once a shared lease exists AND the lifecycle is
        // closed, no new BeginRead and no new frames run; an already-Begun
        // frame always Ends inside this iteration (pairing unchanged), and
        // existing INFLIGHT tickets are serviced by the bounded exit
        // maintenance. A session that never took a shared lease keeps the
        // untouched legacy Stop/teardown pacing (closing alone is not a lease).
        bool sharedStopNow = false;
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            sharedStopNow = g_sharedLifetimeStarted &&
                            (g_sharedClosing || g_sharedTeardownRequested);
        }
        if (sharedStopNow)
        {
            LogLine("XR", "shared closing: worker stops new frames before WaitFrame");
            break;
        }
        // ---- pump all pending events ----
        for (;;)
        {
            XrEventDataBuffer ev{}; ev.type = XR_TYPE_EVENT_DATA_BUFFER;
            const XrResult r = xrPollEvent(g_inst, &ev);
            if (r != XR_SUCCESS) break;
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
                const int oldRaw = g_xrState.exchange((int)sc->state);
                LogLine("XR", std::string("sessionState ") + std::to_string(oldRaw) +
                        " -> " + std::to_string((int)sc->state) + " (" + SessionStateName(sc->state) + ")");
                bool beginCalled = false;
                long long beginResult = -1;
                switch (sc->state)
                {
                case XR_SESSION_STATE_READY:
                    ++g_sessionReadyEvents;
                    if (g_tReadyEvent == 0) g_tReadyEvent = (long long)GetTickCount64();
                    if (!sessionBegun)
                    {
                        bool mayBegin = false;
                        {
                            // precheck under the ring lock Stop/Destroy use to close
                            // admission; NO XR call may happen while holding it
                            std::lock_guard<std::mutex> lk(g_ringMutex);
                            mayBegin = LiveSessionAllowedLocked();
                        }
                        if (!mayBegin)
                        {
                            LogLine("XR", "READY ignored: admission closed or ERROR (explicit Stop wins)");
                            break;
                        }
                        ++g_beginSessionAttempts;
                        beginCalled = true;
                        g_tBeginSessionEntered = (long long)GetTickCount64();
                        XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        const XrResult br = xrBeginSession(g_sess, &bi); // outside any lock
                        beginResult = (long long)br;
                        g_tBeginSessionReturned = (long long)GetTickCount64();
                        sessionBegun = (br == XR_SUCCESS);
                        if (sessionBegun) ++g_beginSessionSuccess;
                        LogLine("XR", std::string("BeginSession result=") + std::to_string((int)br) +
                                (sessionBegun ? " (OK) -> frame loop gate OPEN" : " FAILED"));
                        if (sessionBegun)
                        {
                            // If Stop closed admission during the XR call, leave the
                            // state alone (no resurrection): stopRequested/Destroy owns
                            // this now-begun session's teardown.
                            std::lock_guard<std::mutex> lk(g_ringMutex);
                            if (LiveSessionAllowedLocked()) SetState(VRSIM_SYNCHRONIZED);
                        }
                    }
                    break;
                case XR_SESSION_STATE_SYNCHRONIZED:
                    if (g_tSynchronizedEvent == 0) g_tSynchronizedEvent = (long long)GetTickCount64();
                    {
                        // direct set (no monotonic ratchet): must recover from a
                        // natural STOPPING(7) -> IDLE -> READY -> begin cycle
                        std::lock_guard<std::mutex> lk(g_ringMutex);
                        if (sessionBegun && LiveSessionAllowedLocked()) SetState(VRSIM_SYNCHRONIZED);
                    }
                    break;
                case XR_SESSION_STATE_VISIBLE:
                    if (g_tVisibleEvent == 0) g_tVisibleEvent = (long long)GetTickCount64();
                    if (!g_reachedVisible.exchange(true))
                        LogLine("XR", "IN_PROCESS_SESSION_REACHED_VISIBLE=true");
                    {
                        std::lock_guard<std::mutex> lk(g_ringMutex);
                        if (sessionBegun && LiveSessionAllowedLocked()) SetState(VRSIM_VISIBLE);
                    }
                    break;
                case XR_SESSION_STATE_FOCUSED:
                    {
                        std::lock_guard<std::mutex> lk(g_ringMutex);
                        if (sessionBegun && LiveSessionAllowedLocked()) SetState(VRSIM_RUNNING);
                    }
                    break;
                case XR_SESSION_STATE_STOPPING:
                    if (sessionBegun)
                    {
                        const XrResult er = xrEndSession(g_sess); // outside any lock
                        sessionBegun = false;
                        LogLine("XR", "EndSession " + XrResultName(er));
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_ringMutex);
                        if (g_state.load() != VRSIM_ERROR) SetState(VRSIM_STOPPING);
                    }
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    // HIGHRES-H2SH-CARD2B: natural runtime LOSS/EXITING runs
                    // the SAME close responsibility as Stop/Destroy (pure
                    // metadata close; never an auto re-open of a shared owner).
                    SharedCloseLifecycleIfLease(false);
                    g_stopRequested = true;
                    break;
                default: break;
                }
                LogSessionEvent((XrSessionState)oldRaw, sc->state, beginCalled, beginResult);
            }
            else if (ev.type == XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT)
            {
                const auto* up = reinterpret_cast<const XrEventDataUserPresenceChangedEXT*>(&ev);
                g_userPresent = up->isUserPresent ? 1 : 0;
                g_userPresenceEvents++;
                LogLine("XR", "userPresence -> " + std::to_string(g_userPresent.load()) +
                        " (events=" + std::to_string(g_userPresenceEvents) + ")");
            }
        }
        if (g_stopRequested) break;

        // PRODUCT-02 / SCENE-FREEZE-1: the queue take, admission decision,
        // epoch record and busy arm are ONE joint-gate transaction inside the
        // helper; the call site must never exchange the pending dims itself.
        WorkerApplyPendingSceneMode();
        // PHASE 15: VC_SCENE requested but synthetic source still active = illegal fallback
        if (g_vcSceneRequested && !g_sceneMode && !g_illegalFallback)
        {
            g_illegalFallback = true;
            LogLine("XR", "ILLEGAL_SYNTHETIC_FALLBACK=true (VC_SCENE requested but scene mode inactive)");
        }
        // HIGHRES-H2 FIX: bounded diagnostic readback on THIS thread only, after
        // any scene-mode recreation and before the sessionRunning gate, so the
        // caller's wait fails in bounded time even while the session is idle.
        WorkerPumpDumpRequest();
        // ---- PHASE 3/4 FIX: the frame loop gate is sessionBegun ONLY ----
        // (0.1.0.6 deadlocked here: gate required state >= SYNCHRONIZED while the
        //  runtime establishes synchronization THROUGH the frame loop itself.)
        // Gate A (frame loop runs) = sessionBegun && not STOPPING/EXITING/LOSS_PENDING
        // Gate B (layers submitted) = fs.shouldRender == XR_TRUE
        const int st = g_xrState;
        const bool sessionRunning = sessionBegun &&
            st != XR_SESSION_STATE_STOPPING &&
            st != XR_SESSION_STATE_EXITING &&
            st != XR_SESSION_STATE_LOSS_PENDING;
        if (!sessionRunning) { Sleep(10); continue; }

        // ---- WaitFrame (entry/exit instrumented: distinguishes not-called vs blocked vs failed) ----
        ++g_waitFrameEntered;
        g_waitFrameEnteredAtMs = (long long)GetTickCount64();
        if (g_tFirstWaitFrameEntered == 0) g_tFirstWaitFrameEntered = g_waitFrameEnteredAtMs.load();
        XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        XrResult r = xrWaitFrame(g_sess, &fwi, &fs);
        g_waitFrameReturned = g_waitFrameEntered.load(); // single consumer thread
        g_waitFrameEnteredAtMs = 0;
        g_lastWaitFrameResult = (long long)r;
        if (g_tFirstWaitFrameReturned == 0) g_tFirstWaitFrameReturned = (long long)GetTickCount64();
        if (r != XR_SUCCESS)
        {
            ++g_waitFrameFailed;
            ++g_waitErrCount;
            if (g_waitErrCount <= 3 || g_waitErrCount % 500 == 0)
                LogLine("XR", "WaitFrame " + XrResultName(r) + " count=" + std::to_string(g_waitErrCount));
            Sleep(10);
            continue;
        }
        ++g_waitFrameSuccess;
        g_lastPredictedDisplayTime = (long long)fs.predictedDisplayTime;
        g_lastPredictedDisplayPeriod = (long long)fs.predictedDisplayPeriod;
        g_lastShouldRender = (fs.shouldRender == XR_TRUE) ? 1 : 0;
        if (fs.shouldRender == XR_TRUE)
        {
            ++g_shouldRenderTrue;
            if (g_tFirstShouldRenderTrue == 0) g_tFirstShouldRenderTrue = (long long)GetTickCount64();
        }
        else ++g_shouldRenderFalse;
        if (firstWaitLogged == 0)
        {
            firstWaitLogged = 1;
            LogLine("XR", "first WaitFrame OK displayTime=" + std::to_string(fs.predictedDisplayTime) +
                    " period=" + std::to_string(fs.predictedDisplayPeriod) +
                    " shouldRender=" + std::to_string((int)fs.shouldRender));
        }

        XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
        ++g_beginFrameAttempts;
        r = xrBeginFrame(g_sess, &fbi);
        if (r != XR_SUCCESS)
        {
            ++g_beginFrameFailed;
            // 0.1.2.0: the runtime discards frames while SYNCHRONIZED (headset not worn).
            // The ring must still drain so scene textures stay current for when VISIBLE
            // arrives — otherwise every content frame committed during this period is stolen.
            if (g_sceneMode)
            {
                WorkerApplyPendingSceneMode(); // same transaction as the between-frames site
                WorkerConsumeSceneFrame(); // CARD2: shared-bound sessions take the shared consumer
            }
            if (g_beginFrameFailed <= 3 || g_beginFrameFailed % 500 == 0)
                LogLine("XR", "BeginFrame " + XrResultName(r) + " count=" + std::to_string(g_beginFrameFailed.load()));
            Sleep(10);
            continue;
        }
        if (g_tFirstBeginFrameReturned == 0) g_tFirstBeginFrameReturned = (long long)GetTickCount64();
        ++serial;
        ++g_beginFrameOk;

        {
            std::lock_guard<std::mutex> lk(g_frameMutex);
            g_frameState.frameSerial = serial;
            g_frameState.displayTime = (long long)fs.predictedDisplayTime;
            g_frameState.displayPeriod = (long long)fs.predictedDisplayPeriod;
            g_frameState.shouldRender = (fs.shouldRender == XR_TRUE) ? 1 : 0;
            g_frameState.trackingValid = 0;
        }

        bool viewsValid = false;
        if (fs.shouldRender == XR_TRUE)
        {
            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = g_localSpace;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            uint32_t n = 2;
            r = xrLocateViews(g_sess, &vli, &vs, 2, &n, views);
            viewsValid = (r == XR_SUCCESS && n == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT));
            if (viewsValid)
            {
                PublishViews(views);
                std::lock_guard<std::mutex> lk(g_frameMutex);
                g_frameState.trackingValid = 1;
                // record pose sample for later image/pose pairing (PHASE 14-19)
                PoseSample& ps = g_poseRing[g_poseRingIdx % POSE_RING];
                ps.serial = g_poseSerial.load(); ps.pl = views[0].pose; ps.pr = views[1].pose;
                ps.fl = views[0].fov; ps.fr = views[1].fov; ps.valid = true;
                g_poseRingIdx++;
            }
        }

        // ---- fill swapchains + build projection layer when rendering ----
        const XrCompositionLayerBaseHeader* layers[1] = {};
        XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerProjectionView pv[2]{};
        pv[0].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        pv[1].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        uint32_t layerCount = 0;

        // PRODUCT-02: in scene mode, consume the newest VC stereo frame into scene textures
        // BEFORE the acquire/copy cycle (LATEST_POSE_WINS / latest completed frame wins)
        // HIGHRES-H2SH-CARD2: pure metadata bound-selection; a shared-bound
        // session never falls back to the legacy byte consumer here.
        if (g_sceneMode) WorkerConsumeSceneFrame();

        bool bothFillOk = false;
        bool fatalChannel = false; // acquire/wait/release failure: tear the channel down after this EndFrame
        // HIGHRES-H2 safetyfix: a closed/stopping native session must never submit
        // raw scene layers. Snapshot ring admission in a SHORT lock (no D3D under
        // it) and gate the fill/layer build on it plus the stop/state flags. An
        // in-flight pre-Stop frame still ends balanced: EndFrame with layerCount 0
        // or the already-coherent pair — never a new raw/old-pose submission.
        bool sessionAdmitting = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            sessionAdmitting = g_ringAdmission;
        }
        if (fs.shouldRender == XR_TRUE && viewsValid && sessionAdmitting &&
            !g_stopRequested.load() && g_state.load() != VRSIM_STOPPING)
        {
            ++g_quadLayerAttempts;
            bool ok = true;
            for (int e = 0; e < 2 && ok; ++e)
            {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                // HIGHRES-H2 FIX-2: ANY non-SUCCESS acquire result is FATAL for this
                // channel — including XR_SESSION_LOSS_PENDING, which is a POSITIVE
                // result: the image MAY ALREADY be acquired, so the old "nothing
                // held, transient, no release" assumption was wrong and retrying
                // acquire on a losing session just repeats errors. Do NOT Release
                // without a completed Wait here; leave the image to the
                // xrDestroySwapchain teardown after this worker exits. This frame
                // still ends balanced with layerCount 0, then the channel stops.
                const XrResult ar = xrAcquireSwapchainImage(g_swap[e], &ai, &idx);
                if (ar != XR_SUCCESS)
                {
                    fatalChannel = true;
                    SetErr("xrAcquireSwapchainImage failed eye=" + std::to_string(e) +
                           " " + XrResultName(ar));
                    RingSetAdmission(false);
                    g_stopRequested = true;
                    SetState(VRSIM_ERROR); // GetState must not keep reporting RUNNING
                    // HIGHRES-H2SH-CARD2B: a natural runtime ERROR closes the
                    // shared lifecycle with the same responsibility and never
                    // auto-revives the shared owner.
                    SharedCloseLifecycleIfLease(false);
                    ok = false;
                    break; // no Wait/Copy/Release on this eye; idx unused
                }
                // HIGHRES-H2 followup: NEVER release an image whose Wait failed or timed
                // out (the old unconditional best-effort release was wrong). Wait with an
                // INFINITE timeout; a spurious XR_TIMEOUT_EXPIRED is retried on the SAME
                // acquired image a bounded number of times.
                bool waited = false;
                XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    const XrResult wr = xrWaitSwapchainImage(g_swap[e], &wi);
                    if (wr == XR_SUCCESS) { waited = true; break; }
                    if (wr != XR_TIMEOUT_EXPIRED) break; // non-timeout error: stop retrying
                }
                if (!waited)
                {
                    // Still timed out / errored: leave the image acquired (no Release),
                    // record a fatal channel error, stop admitting and exit the loop after
                    // this EndFrame. xrDestroySwapchain on teardown reclaims the image.
                    // HIGHRES-H2 FIX-2: also publish VRSIM_ERROR — a fatal channel fault
                    // must never leave GetState reporting RUNNING. Error text is kept.
                    fatalChannel = true;
                    SetErr("xrWaitSwapchainImage failed/timed out eye=" + std::to_string(e));
                    RingSetAdmission(false);
                    g_stopRequested = true;
                    SetState(VRSIM_ERROR);
                    SharedCloseLifecycleIfLease(false); // CARD2B: ERROR closes, never revives
                    ok = false;
                    break;
                }
                ID3D11Texture2D* src = SceneSource(e); // GPU output when active-GPU (may be null)
                if (idx < g_swapTex[e].size() && g_swapTex[e][idx] && src)
                    g_ctx->CopyResource(g_swapTex[e][idx], src);
                else
                    ok = false; // missing source/image: cannot guarantee content
                // release exactly once after a successful wait+copy; a failed release is
                // itself fatal (no second Release attempt) and stops the channel.
                // HIGHRES-H2 FIX-2: a failed release also publishes VRSIM_ERROR so
                // GetState cannot keep reporting RUNNING after the channel stops.
                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                if (xrReleaseSwapchainImage(g_swap[e], &ri) != XR_SUCCESS)
                {
                    fatalChannel = true;
                    SetErr("xrReleaseSwapchainImage failed eye=" + std::to_string(e));
                    RingSetAdmission(false);
                    g_stopRequested = true;
                    SetState(VRSIM_ERROR);
                    SharedCloseLifecycleIfLease(false); // CARD2B: ERROR closes, never revives
                    ok = false;
                    break;
                }
            }
            // HIGHRES-H2-GPU: build/submit the projection layer ONLY after BOTH eyes
            // acquired/waited/copied/released with a valid source and no device removal.
            // Any failure -> layerCount stays 0 (never a partial old/new eye layer).
            if (ok && SUCCEEDED(g_dev->GetDeviceRemovedReason()))
            {
                bothFillOk = true;
                for (int e = 0; e < 2; ++e)
                {
                    if (g_sceneMode && g_haveLayerPose)
                    {
                        // PHASE 16: the image was rendered at its own pose — submit THAT pose
                        pv[e].pose = (e == 0) ? g_layerPl : g_layerPr;
                        pv[e].fov  = (e == 0) ? g_layerFl : g_layerFr;
                    }
                    else
                    {
                        pv[e].pose = views[e].pose; // synthetic quad mode: fresh pose is fine
                        pv[e].fov = views[e].fov;
                    }
                    pv[e].subImage.swapchain = g_swap[e];
                    pv[e].subImage.imageRect.offset.x = 0;
                    pv[e].subImage.imageRect.offset.y = 0;
                    pv[e].subImage.imageRect.extent.width = (int32_t)g_swapW;
                    pv[e].subImage.imageRect.extent.height = (int32_t)g_swapH;
                    pv[e].subImage.imageArrayIndex = 0;
                }
                proj.space = g_localSpace;
                proj.viewCount = 2;
                proj.views = pv;
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                layerCount = 1;
            }
            else { ++g_quadFillFail; } // failed fill: no layer, never counted as content
        }

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = g_blend;
        fei.layerCount = layerCount;
        fei.layers = layerCount > 0 ? layers : nullptr;
        r = xrEndFrame(g_sess, &fei);
        g_lastEndFrameResult = (long long)r;
        if (g_tFirstEndFrameReturned == 0) g_tFirstEndFrameReturned = (long long)GetTickCount64();
        if (layerCount == 0) ++g_zeroLayerEndFrames;
        if (r != XR_SUCCESS)
        {
            if (layerCount > 0) ++g_quadFail; // quad-layer EndFrame failure
            LogLine("XR", "EndFrame " + XrResultName(r) + " serial=" + std::to_string(serial));
        }
        else
        {
            if (layerCount > 0)
            {
                ++g_quadOk; // quad-layer EndFrame success (the PHASE 23 gate counter)
                if (g_sceneMode) ++g_vcLayerOk; // VC-scene layer success (the 02A gate counter)
                if (g_tFirstQuadEndFrameOk == 0) g_tFirstQuadEndFrameOk = (long long)GetTickCount64();
                // HIGHRES-H2 followup: unique delivered-to-runtime DISTINCT content.
                // Counts only when BOTH eyes filled, live scene mode, a valid exact
                // layer pose and the session is not stopping; repeats never
                // increment. The LIVE-pair gate (CPU/GPU-live committed source,
                // nonzero active serials, open admission bit) is enforced INSIDE
                // GpuStatsRecordLayerDelivered under the stats lock — never read
                // g_gpuStats here. Static-preview and synthetic frames never
                // increment uniqueLiveLayerOk.
                if (bothFillOk && g_sceneMode && g_haveLayerPose && !g_stopRequested.load())
                    GpuStatsRecordLayerDelivered();
            }
            if (visibleLogged == 0 && g_reachedVisible)
            {
                visibleLogged = 1;
                LogLine("XR", "first layer EndFrame OK serial=" + std::to_string(serial) +
                        " " + std::to_string(g_swapW) + "x" + std::to_string(g_swapH));
            }
        }
        LogFrameCsv(serial, (XrSessionState)st, (int)fs.shouldRender, (long long)r);
        // A wait/release channel fault already set stopRequested + closed admission and
        // submitted this frame with layerCount 0; exit so Destroy's teardown runs cleanly.
        if (fatalChannel) break;
    }
    exitCleanup();
    LogLine("XR", "worker exit");
}
catch (...)
{
    g_stopRequested = true;
    try {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        if (SharedLeaseOrStateLocked()) SharedCloseLocked(false);
        else { g_sharedClosing = true; if (g_ringAdmission) RingSetAdmissionLocked(false); }
        if (g_sharedLifetimeStarted) g_sharedQuarantined = true;
        g_sharedWorkerCleanupDone = false;
        g_sharedWorkerNoContext = false;
        g_sharedWorkerDrained = false;
    } catch (...) {}
    try { g_initPromise.set_value(-3); } catch (...) {}
    try { LogLine("XR", "worker exit (exception; flags remain false, quarantine if lease)"); } catch (...) {}
}
}

// ---------------------------------------------------------------- cleanup --
// HIGHRES-H2SH-CARD2B: returns FALSE on an honest teardown PENDING: the
// central teardown owns its own atomic hard gate (close-of-new-leases + block
// evaluation in ONE ring -> meta gate, so no check->firstPrepare race
// survives), and a refusal clears NO state, joins NOTHING, releases NO
// context/device/swapchain references — everything is retained until a real
// finalize (or process exit). Callers must never ignore the refusal and may
// not report OFF/success on it.
bool DestroyXRResources()
{
    // D1: the FIRST joint gate closes native entry unconditionally, then proves
    // eligibility from actual cached state. Refusal keeps every resource, the
    // original bind identity and all lifetime mirrors; only the closing /
    // teardown / admission changes survive a refusal (they are the request).
    bool legacyNoLease = false;
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order

        g_stopRequested = true;
        g_sharedClosing = true;
        g_sharedTeardownRequested = true;
        RingSetAdmissionLocked(false);

        // A real lease/state closes the bridge master; a truly never-used legacy
        // route closes only native entry. Absence of bound alone is not proof.
        const bool lease = SharedLeaseOrStateLocked();
        legacyNoLease = !lease;
        if (lease) SharedCloseLocked(true);

        VRSIM::SharedBridgeStatsV1 bs{};
        g_sharedBridge.Stats(&bs); // cached, no GPU

        bool blocked = g_sharedQuarantined || g_sharedFinalizeBusy ||
                       g_sharedSourceActiveCalls != 0 || g_worker.joinable();
        for (int i = 0; !blocked && i < RING_SLOTS; ++i)
        {
            if (g_sharedPhase[i] != kSharedPhaseNone) { blocked = true; break; }
            if (g_ring[i].mode == kGpuSlotModeSharedV1) { blocked = true; break; }
            const auto st = g_ring[i].state.load();
            if (st == 0) continue;
            // A shared lease requires ALL native slots FREE; WRITING / unknown
            // always refuses. READY(2)/READING(3) survive only as legacy caches.
            if (lease || (st != 2 && st != 3)) { blocked = true; break; }
        }
        for (int i = 0; !blocked && i < (int)VRSIM::kStereoBridgeSlotCountV1; ++i)
            if (bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree) { blocked = true; break; }
        if (!blocked && lease)
        {
            // The shared route releases only on the worker's real, proven exit.
            blocked = !(g_sharedWorkerCleanupDone && g_sharedWorkerNoContext &&
                        g_sharedWorkerDrained && g_sharedSourceFinalized &&
                        g_sharedAdmissionClosed && g_sharedClosing &&
                        g_sharedTeardownRequested);
        }
        if (blocked) return false;
    }

    // HIGHRES-H2-COLOR: clear the frozen session color contract here (serialized
    // cleanup, only after the worker has joined) so a new Create can never
    // inherit the previous session's sRGB selection. Failed-Create paths call
    // this same function after joining the worker.
    g_sessionColorSrgb = false;
    g_gpuPass.Reset();
    g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
    g_activeSourceGpu = false;
    for (int e = 0; e < 2; ++e)
    {
        if (g_swap[e] != XR_NULL_HANDLE) { xrDestroySwapchain(g_swap[e]); g_swap[e] = XR_NULL_HANDLE; }
        if (g_quadTex[e]) { g_quadTex[e]->Release(); g_quadTex[e] = nullptr; }
        if (g_sceneTex[e]) { g_sceneTex[e]->Release(); g_sceneTex[e] = nullptr; }
        if (g_redTex[e]) { g_redTex[e]->Release(); g_redTex[e] = nullptr; }
        g_swapTex[e].clear();
    }
    if (g_localSpace != XR_NULL_HANDLE) { xrDestroySpace(g_localSpace); g_localSpace = XR_NULL_HANDLE; }
    if (g_sess != XR_NULL_HANDLE) { xrDestroySession(g_sess); g_sess = XR_NULL_HANDLE; }
    if (g_inst != XR_NULL_HANDLE) { xrDestroyInstance(g_inst); g_inst = XR_NULL_HANDLE; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    if (g_loader) { FreeLibrary(g_loader); g_loader = nullptr; }
    g_gpa = nullptr;

    // HIGHRES-H2SH-CARD1/CARD2B/D1: the frozen runtime-required LUID/minimum-
    // feature metadata and the shared lifetime mirrors clear only AFTER the
    // real GPU/XR releases have returned normally. g_sharedFinalizedOnce
    // PERMANENTLY refuses any re-Initialize of this bridge owner: a fresh owned
    // QA host is required for a future shared session. g_sharedClosing,
    // g_sharedTeardownRequested, g_sharedAdmissionClosed, native admission and
    // the original bind epoch/generation stay as they are: only a genuinely
    // empty next Create may reset the native legacy doors.
    g_runtimeRequiredLuid = LUID{};
    g_runtimeRequiredLuidValid = false;
    g_runtimeMinFeatureLevel = (D3D_FEATURE_LEVEL)0;

    if (!legacyNoLease)
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        if (g_sharedFinalizedOnce || g_sharedSourceFinalized)
        {
            g_sharedLifetimeStarted = false;
            g_sharedBound = false;
            g_sharedSourceFinalized = false;
            g_sharedWorkerCleanupDone = false;
            g_sharedWorkerNoContext = false;
            g_sharedWorkerDrained = false;
            for (int i = 0; i < RING_SLOTS; ++i) g_sharedPhase[i] = kSharedPhaseNone;
        }
        return true;
    }

    // Approved truly no-lease legacy route: the actual releases above are the
    // cleanup authority (the joined worker left no context owner). Re-prove the
    // caches are empty before demoting leftover CPU/raw READY(2)/READING(3) so
    // a later refusal can never claim retained full ownership. Shared flags are
    // never fabricated here.
    bool demoted = false;
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        if (!SharedLeaseOrStateLocked() && !g_worker.joinable() &&
            !g_ringAdmission && g_sharedClosing && g_sharedTeardownRequested &&
            g_sharedSourceActiveCalls == 0 && !g_sharedQuarantined && !g_sharedFinalizeBusy)
        {
            VRSIM::SharedBridgeStatsV1 bs{};
            g_sharedBridge.Stats(&bs);
            bool ok = true;
            for (int i = 0; ok && i < RING_SLOTS; ++i)
            {
                if (g_sharedPhase[i] != kSharedPhaseNone) { ok = false; break; }
                if (g_ring[i].mode == kGpuSlotModeSharedV1) { ok = false; break; }
                const auto st = g_ring[i].state.load();
                if (st != 0 && st != 2 && st != 3) { ok = false; break; }
            }
            for (int i = 0; ok && i < (int)VRSIM::kStereoBridgeSlotCountV1; ++i)
                if (bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree) { ok = false; break; }
            if (ok)
            {
                for (int i = 0; i < RING_SLOTS; ++i)
                {
                    const auto st = g_ring[i].state.load();
                    if (st == 2 || st == 3) { g_ring[i].state.store(0); g_ring[i].epoch = 0; }
                }
                demoted = true;
            }
        }
    }
    if (!demoted)
    {
        // Honest failure: the real release already happened, but the final
        // cached-empty proof did not. Do not report OFF or retained ownership.
        SetErr("native resources released but cached-empty recheck failed: leftover slot state retained");
        LogLine("XR", "DestroyXRResources post-release empty recheck refused");
        return false;
    }
    return true; // D1: honest full-cleanup completion (refusals return false above)
}

} // namespace

// ================================================================== C ABI ==
extern "C" {

// PHASE 2: build identity (never guess versions from filenames again)
typedef struct
{
    char semanticVersion[32];
    char buildTimestamp[32];
    unsigned int featureFlags; // bit0 = PRODUCT02_SCENE_MODE_SUPPORTED, bit1 = GPU RAW API
} VRSIMBuildInfo;

__declspec(dllexport) int VRSIM_GetBuildInfo(VRSIMBuildInfo* out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    strcpy_s(out->semanticVersion, "0.1.4.0");
    strcpy_s(out->buildTimestamp, __TIMESTAMP__);
    out->featureFlags = 1u | 2u;
    return 0;
}

// PHASE 17 + HIGHRES-H2 FIX: QA readback of the exact texture consumed for OpenXR
// submission. ABI unchanged. The calling thread performs NO D3D work and never
// touches g_ctx/g_dev/g_sceneTex: it queues one worker-owned request, waits at
// most 1000ms, then copies the request's OWNED CPU bytes. No UI callback, no
// frame loop on the caller, no retention of caller pointers past return.
// rc: -1 bad args | -2 session/scene not ready | -3 cap too small
//     -4 staging/format/internal failure | -5 Map failed
//     -6 canceled by Stop/Destroy or stale session generation | -7 busy
//     -8 worker did not complete within 1000ms
__declspec(dllexport) int VRSIM_DumpSceneTexture(int eye, unsigned char* buf, int cap, int* outW, int* outH)
{
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!buf || cap <= 0 || !outW || !outH) return -1;
    if (eye != 0 && eye != 1) return -1;
    try
    {
        if (!g_dumpAccepting.load()) return -2; // before init / after stop-destroy
        auto req = std::make_shared<DumpRequest>();
        req->eye = eye;
        req->cap = cap;
        req->generation = g_dumpGeneration.load();
        req->canceled = std::make_shared<std::atomic<bool>>(false);
        req->promise = std::make_shared<std::promise<DumpResult>>();
        std::future<DumpResult> fut = req->promise->get_future();
        {
            std::lock_guard<std::mutex> lk(g_dumpMutex);
            if (g_dumpStage != 0) return -7; // at most one pending OR in-flight dump
            if (!g_dumpAccepting.load() || req->generation != g_dumpGeneration.load())
                return -2;
            g_dumpSlot = req;
            g_dumpStage = 1;
        }
        if (fut.wait_for(std::chrono::milliseconds(1000)) != std::future_status::ready)
        {
            // worker still owns the request via shared_ptr; flag it canceled so it
            // fails fast and releases staging. The caller retains nothing past return.
            req->canceled->store(true);
            return -8;
        }
        DumpResult res = fut.get();
        if (res.rc != 0) return res.rc;
        if (res.w <= 0 || res.h <= 0 || res.bytes.empty() ||
            (uint64_t)res.w * (uint64_t)res.h * 4ull != (uint64_t)res.bytes.size() ||
            res.bytes.size() > (size_t)cap)
            return -4;
        {
            // HIGHRES-H2 FIX: final CPU-byte delivery is linearized against Stop
            // under the same g_dumpMutex the worker's commit recheck uses. Only the
            // memcpy and out-dims are inside the lock: no D3D, allocation, or wait.
            std::lock_guard<std::mutex> lk(g_dumpMutex);
            if (req->canceled->load() || req->generation != g_dumpGeneration.load() ||
                !g_dumpAccepting.load() || g_stopRequested.load())
                return -6; // canceled mid-delivery; out dims stay zero
            memcpy(buf, res.bytes.data(), res.bytes.size());
            *outW = res.w;
            *outH = res.h;
        }
        return 0;
    }
    catch (...) { return -4; } // C ABI never throws
}

__declspec(dllexport) int VRSIM_SubmitStereoFrame(
    const unsigned char* left, const unsigned char* right,
    int width, int height, int stride,
    unsigned long long frameSerial, unsigned long long poseSerial, double cameraYawDeg)
{
    // Native only bounds the generic source size (nonnull, 1..2048 per side,
    // stride >= row bytes, total <= 32 MiB). It does NOT detect scene/profile
    // mismatch; H2 must ensure the selected and actual scene size agree.
    if (!left || !right) { ++g_submitFail; return -1; }
    if (width < 1 || width > 2048 || height < 1 || height > 2048) { ++g_submitFail; return -1; }
    const uint64_t rowBytes = (uint64_t)width * 4ull;
    if (stride <= 0 || (uint64_t)stride < rowBytes) { ++g_submitFail; return -1; }
    const size_t need = (size_t)stride * (size_t)height;
    if (need > 32ull * 1024ull * 1024ull) { ++g_submitFail; return -1; }
    try
    {
        // scan+claim (FREE first, else lowest-serial READY steal) atomically
        const int slot = RingClaimForWrite();
        if (slot < 0) { ++g_submitFail; return -2; }
        try
        {
            // WRITING slot is exclusively owned: payload work OUTSIDE g_ringMutex,
            // never after return (caller pointers are not retained)
            if (g_ringL[slot].size() != need) g_ringL[slot].resize(need);
            if (g_ringR[slot].size() != need) g_ringR[slot].resize(need);
            memcpy(g_ringL[slot].data(), left, need);
            memcpy(g_ringR[slot].data(), right, need);
            LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
            StereoSlot& s = g_ring[slot];
            s.frameSerial = frameSerial; s.poseSerial = poseSerial; s.cameraYawDeg = cameraYawDeg;
            s.timestampNs = qpc.QuadPart; s.width = width; s.height = height; s.stride = stride;
            s.mode = (int)VRSIM::kGpuSlotModeCpu; // CPU path (existing behavior)
            s.epoch = 0;                           // CPU slots carry no ring epoch
            s.leftPlan = VRSIM::ProjectionPlanV1{}; // a stolen raw slot must not keep a plan
            s.rightPlan = VRSIM::ProjectionPlanV1{};
            unsigned long long sum = 0;
            { const unsigned int* p = reinterpret_cast<const unsigned int*>(g_ringL[slot].data());
              for (size_t i = 0; i < need / 4; i += 97) sum += p[i]; }
            s.leftSum = sum;
        }
        catch (...)
        {
            // resize/copy failed: release the claimed WRITING slot back to FREE
            RingReleaseToFree(slot);
            ++g_submitFail;
            return -3;
        }
        RingPublishReady(slot, frameSerial);
        ++g_submitOk;
        return 0;
    }
    catch (...) { ++g_submitFail; return -3; } // C ABI never throws
}

// ---- HIGHRES-H2-GPU: wire stats snapshot (v1, 120 bytes) -------------------
// Coherent copy of the shared g_gpuStats under the short stats lock: session
// epoch, reserved admission bit and active identity stay consistent because
// RingSetAdmission updates them together. No lock held across the memcpy.
__declspec(dllexport) int VRSIM_GetGpuPipelineStatsV1(void* out, uint32_t capacity)
{
    if (!out || capacity < (uint32_t)sizeof(VRSIM::GpuPipelineStatsV1)) return -1;
    try
    {
        VRSIM::GpuPipelineStatsV1 local{};
        {
            std::lock_guard<std::mutex> lk(g_gpuStatsMutex);
            local = g_gpuStats;
        }
        local.structBytes = (uint32_t)sizeof(VRSIM::GpuPipelineStatsV1);
        local.version = VRSIM::kGpuPipelineStatsV1Version;
        memcpy(out, &local, sizeof(local));
        return 0;
    }
    catch (...) { return -3; } // C ABI never throws
}

// ---- HIGHRES-H2-GPU: raw stereo submit (admission/epoch-gated) -------------
// Validates caller bounds + both projection plans BEFORE any claim; rejects are
// counted in wire stats. The claimed WRITING slot is exclusively owned: deep
// copy + metadata happen outside every lock and no caller pointer is retained.
// A Stop/Destroy straddling the payload copy makes the gated publish fail (slot
// released by RingPublishGated, never queued). No D3D work on this thread.
__declspec(dllexport) int VRSIM_SubmitRawStereoV1(
    const unsigned char* left, uint64_t leftBytes,
    const unsigned char* right, uint64_t rightBytes,
    int width, int height, int stride,
    uint64_t frameSerial, uint64_t poseSerial, uint64_t capturedSessionEpoch,
    double cameraYawDeg,
    const VRSIM::ProjectionPlanV1* leftPlan,
    const VRSIM::ProjectionPlanV1* rightPlan)
{
    if (!left || !right || !leftPlan || !rightPlan) { GpuStatsRawRejected(); return -1; }
    if (width < 2 || width > 2048 || height < 2 || height > 2048) { GpuStatsRawRejected(); return -1; }
    const uint64_t rowBytes = (uint64_t)width * 4ull;
    if (stride <= 0 || (uint64_t)stride < rowBytes) { GpuStatsRawRejected(); return -1; }
    // dims/stride already bounded above => product < 2^42, no uint64 overflow
    const uint64_t need = (uint64_t)stride * (uint64_t)height;
    if (need > 32ull * 1024ull * 1024ull) { GpuStatsRawRejected(); return -1; }
    if (leftBytes < need || rightBytes < need) { GpuStatsRawRejected(); return -1; }
    if (frameSerial == 0 || poseSerial == 0 || capturedSessionEpoch == 0) { GpuStatsRawRejected(); return -1; }
    if (!std::isfinite(cameraYawDeg)) { GpuStatsRawRejected(); return -1; }
    // HIGHRES-H2SH-CARD1: a bound shared session rejects mixed legacy
    // byte-submit traffic (-5): shared ticket slots are never contaminated by
    // CPU-pixel transports, and the shared route never consumes a leftover
    // CPU/raw ticket as shared content. Pure metadata check, no GPU work.
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        if (g_sharedBound) { GpuStatsRawRejected(); return -5; }
    }

    // copy both plans once; validate the copies (caller pointers never retained)
    const VRSIM::ProjectionPlanV1 lp = *leftPlan;
    const VRSIM::ProjectionPlanV1 rp = *rightPlan;
    const double aspect = (double)width / (double)height;
    const auto planOk = [aspect](const VRSIM::ProjectionPlanV1& p)
    {
        if (!ValidatePlanForSubmit(p, aspect)) return false;
        // square-envelope half tangents must actually cover the FOV...
        if (!(p.tx >= (std::max)(-p.tanL, p.tanR))) return false;
        if (!(p.ty >= (std::max)(p.tanU, -p.tanD))) return false;
        // ...and sit on the profile aspect line (ty>0/finite guaranteed by the
        // structural validator, so the division is safe)
        if (std::fabs(p.tx / p.ty - aspect) > 1e-6) return false;
        return true;
    };
    if (!planOk(lp) || !planOk(rp)) { GpuStatsRawRejected(); return -1; }

    int slot = -1;
    bool published = false;
    try
    {
        unsigned long long claimedEpoch = 0;
        slot = RingClaimGated(capturedSessionEpoch, &claimedEpoch);
        if (slot < 0) { GpuStatsRawRejected(); return -2; } // closed / stale epoch / ring full
        try
        {
            // WRITING slot exclusively owned: payload work outside g_ringMutex
            if (g_ringL[slot].size() != need) g_ringL[slot].resize((size_t)need);
            if (g_ringR[slot].size() != need) g_ringR[slot].resize((size_t)need);
            memcpy(g_ringL[slot].data(), left, need);
            memcpy(g_ringR[slot].data(), right, need);
            LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
            StereoSlot& s = g_ring[slot];
            s.frameSerial = frameSerial; s.poseSerial = poseSerial; s.cameraYawDeg = cameraYawDeg;
            s.timestampNs = qpc.QuadPart; s.width = width; s.height = height; s.stride = stride;
            s.mode = (int)VRSIM::kGpuSlotModeRawGpu;
            s.epoch = claimedEpoch;
            s.leftPlan = lp; s.rightPlan = rp;
            unsigned long long sum = 0;
            { const unsigned int* p = reinterpret_cast<const unsigned int*>(g_ringL[slot].data());
              for (size_t i = 0; i < need / 4; i += 97) sum += p[i]; }
            s.leftSum = sum;
        }
        catch (...)
        {
            // resize/copy failed: release the claimed WRITING slot back to FREE
            RingReleaseToFree(slot);
            GpuStatsRawRejected();
            return -3;
        }
        published = RingPublishGated(slot, frameSerial, claimedEpoch);
        if (!published) { GpuStatsRawRejected(); return -4; } // Stop straddled the copy
        ++g_submitOk;
        GpuStatsRawAccepted();
        return 0;
    }
    catch (...)
    {
        if (slot >= 0 && !published) RingReleaseToFree(slot);
        GpuStatsRawRejected();
        return -3; // C ABI never throws
    }
}

// ===================== HIGHRES-H2SH-CARD1: shared stereo exports ============
// Wire/entry layer only. Shared activation is HARD-DISABLED for every
// intermediate card: the entry points below refuse with
// kSharedExportUnavailable BEFORE any GPU/COM/bridge call, advertise no
// feature flag, and never initialize the GPU on capability/state queries.
// Return codes: 0 completed | 1 pending | -1 bad argument | -2 unavailable |
// -3 stale | -4 quarantined | -5 error (exact bridge HRESULT preserved in
// g_sharedLastBridgeHresult; bridge S_FALSE is never treated as completion).
// Helpers are file-static (internal linkage) so no C-linkage type surprises.

// Structural COMMIT-time identity check: everything except the single
// completion field is validated against the STORED PREPARE image by
// SharedIdentityEqualExceptCompletion; here we only bound the completion
// bracket itself.
static bool SharedWireValidateCommitShape(const VRSIM::SharedCaptureIdentityV1& id)
{
    if (id.structBytes != VRSIM::kSharedCaptureIdentityV1Bytes) return false;
    if (id.version != VRSIM::kSharedCaptureIdentityV1Version) return false;
    if (id.reserved != 0) return false;
    if (!std::isfinite(id.cameraYawDeg)) return false;
    // 0 < poseBegin <= poseEnd <= captureBegin < captureEnd
    if (!(id.poseSampleBeginQpc > 0)) return false;
    if (!(id.poseSampleBeginQpc <= id.poseSampleEndQpc)) return false;
    if (!(id.poseSampleEndQpc <= id.captureBeginQpc)) return false;
    if (!(id.captureBeginQpc < id.captureEndQpc)) return false;
    return true;
}

// Structural PREPARE-time identity check (capacity/version/finite/plan/exact
// captured pose). captureEndQpc MUST be 0: the completion timestamp is
// supplied exactly once at COMMIT.
static bool SharedWireValidatePrepareIdentity(const VRSIM::SharedCaptureIdentityV1& id)
{
    if (id.structBytes != VRSIM::kSharedCaptureIdentityV1Bytes) return false;
    if (id.version != VRSIM::kSharedCaptureIdentityV1Version) return false;
    if (id.reserved != 0) return false;
    if (id.frameSerial == 0 || id.poseSerial == 0 || id.epoch == 0 || id.generation == 0) return false;
    if (id.captureEndQpc != 0) return false;
    if (!(id.poseSampleBeginQpc > 0)) return false;
    if (!(id.poseSampleBeginQpc <= id.poseSampleEndQpc)) return false;
    if (!(id.poseSampleEndQpc <= id.captureBeginQpc)) return false;
    if (!std::isfinite(id.cameraYawDeg)) return false;
    // exact captured pose must already exist (never latest/nearest): the
    // serial is bounded by the published pose counter; the worker re-pairs
    // against the EXACT PoseSample at read time (Card2).
    if (id.poseSerial > g_poseSerial.load()) return false;
    if (id.poseSerial == 0) return false;
    // locked 1536x1536 square profile => aspect exactly 1.0; reuse the
    // reviewed raw-submit plan validators unchanged
    const double aspect = 1.0;
    auto planOk = [aspect](const VRSIM::ProjectionPlanV1& p)
    {
        if (!ValidatePlanForSubmit(p, aspect)) return false;
        if (!(p.tx >= (std::max)(-p.tanL, p.tanR))) return false;
        if (!(p.ty >= (std::max)(p.tanU, -p.tanD))) return false;
        if (std::fabs(p.tx / p.ty - aspect) > 1e-6) return false;
        return true;
    };
    return planOk(id.leftPlan) && planOk(id.rightPlan);
}

static bool SharedWireTicketShapeOk(const VRSIM::SharedPreparedPairV1& t)
{
    return t.version == VRSIM::kStereoBridgeVersionV1 &&
           t.structBytes == (uint32_t)sizeof(VRSIM::SharedPreparedPairV1) &&
           t.slotIndex < VRSIM::kStereoBridgeSlotCountV1 &&
           t.generation != 0 && t.epoch != 0 && t.ticket != 0 &&
           t.width == VRSIM::kStereoBridgeWidthV1 &&
           t.height == VRSIM::kStereoBridgeHeightV1 &&
           t.format88 == VRSIM::kStereoBridgeFormat88V1;
}

// Release a still-WRITING reserved shared slot back to FREE after a proven
// non-quarantine failure. Caller MUST already hold g_ringMutex (order: ring
// -> meta; no recursive g_ringMutex acquisition).
static void RingReleaseSharedWritingLocked(int slot, unsigned long long claimEpoch)
{
    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
    StereoSlot& s = g_ring[slot];
    if (s.state.load() == 1 && s.mode == kGpuSlotModeSharedV1 && s.epoch == claimEpoch)
    {
        s.state.store(0);
        s.mode = (int)VRSIM::kGpuSlotModeCpu;
        s.epoch = 0;
        s.sharedTicket = VRSIM::SharedPreparedPairV1{};
        s.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
        g_sharedPhase[slot] = kSharedPhaseNone;
    }
}

static std::string SharedHrName(HRESULT hr)
{
    return "0x" + std::to_string((unsigned long)hr);
}

// Source UI only, inside the owner double-lock callback. The two borrowed
// IDirect3DSurface9 handle NUMBERS are valid only during THIS call: they are
// passed straight to the bridge and are NEVER cached in the slot. Grants a
// PREPARED ticket into a paired FREE ring+bridge slot (READY steal is
// forbidden here; RingClaimSharedPairedFreeLocked consults ONLY state==0 plus
// the bridge's cached FREE state).
__declspec(dllexport) int VRSIM_PrepareSharedStereoV1(
    unsigned long long leftSurface, unsigned long long rightSurface,
    const void* identity, unsigned int identityCapacityBytes,
    void* outTicket64, unsigned int ticketCapacityBytes)
{
    // HARD gate first: while activation is disabled nothing below ever runs —
    // no bridge entry, no GPU/COM call, no accidental init on a cache query.
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportUnavailable;
    if (!identity || !outTicket64) return VRSIM::kSharedExportBadArgument;
    if (identityCapacityBytes != VRSIM::kSharedCaptureIdentityV1Bytes) return VRSIM::kSharedExportBadArgument;
    if (ticketCapacityBytes != VRSIM::kSharedTicketExportBytesV1) return VRSIM::kSharedExportBadArgument;
    if (leftSurface == 0 || rightSurface == 0) return VRSIM::kSharedExportBadArgument;
    int slot = -1;
    unsigned long long claimEpoch = 0;
    // HIGHRES-H2SH-CARD2B: one source-UI active call, armed inside the grant
    // gate and dropped exactly once on EVERY exit (including exceptions).
    SharedActiveCallGuard activeCall;
    try
    {
        VRSIM::SharedCaptureIdentityV1 id{};
        memcpy(&id, identity, sizeof(id)); // 192 B fully named explicit layout (no implicit padding)
        if (!SharedWireValidatePrepareIdentity(id)) return VRSIM::kSharedExportBadArgument;

        // ---- first reservation: ONE ring -> meta short gate, linearized
        // against close/Stop/Commit/legacy claim. Before this gate returns
        // NOTHING borrows g_dev/g_ctx/LUID and NO COM/QI/Init runs. ----
        bool bound = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
            if (g_sharedQuarantined) return VRSIM::kSharedExportQuarantined;
            if (g_sharedSourceFinalized || g_sharedFinalizedOnce)
                return VRSIM::kSharedExportUnavailable; // a finalized owner is NOT reusable
            if (g_sharedClosing || g_sharedAdmissionClosed || g_sharedTeardownRequested ||
                g_sharedFinalizeBusy)
                return VRSIM::kSharedExportStale;
            if (g_sharedSourceActiveCalls > 0) return VRSIM::kSharedExportPending;
            if (g_stopRequested.load()) return VRSIM::kSharedExportStale;
            if (g_state.load() == VRSIM_ERROR) return VRSIM::kSharedExportStale;
            if (g_state.load() != VRSIM_RUNNING) return VRSIM::kSharedExportPending; // actual running session
            if (!g_ringAdmission) return VRSIM::kSharedExportStale;
            if (id.epoch != g_ringEpoch) return VRSIM::kSharedExportStale;
            bound = g_sharedBound;
            // HIGHRES-H2SH-CARD2C: the lease identity FREEZES at the FIRST
            // reservation. A failedInit + proven-FREE rollback keeps
            // g_sharedLifetimeStarted true while g_sharedBound stays false, so
            // this gate keys off the lifetime flag, NOT the bound mirror: any
            // later call carrying a different epoch/generation is Stale. A
            // same-identity retry while bound==false stays legal; if the bridge
            // itself is closed the Init/Prepare below honestly refuses it —
            // this never opens an extra door.
            if (g_sharedLifetimeStarted &&
                (id.epoch != g_sharedBoundEpoch || id.generation != g_sharedBoundGeneration))
                return VRSIM::kSharedExportStale; // one frozen epoch/generation per lease
            // never default-adapter, never a source-inferred LUID (fields read,
            // nothing called while the gate is held)
            if (!g_dev || !g_ctx || !g_runtimeRequiredLuidValid)
                return VRSIM::kSharedExportUnavailable;
            // SCENE-FREEZE-2: a FIRST shared lease is built only on a
            // worker-VERIFIED native bridge-profile scene source. Busy, ANY
            // pending dimension, readiness false, or readiness from another
            // epoch is ordinary backpressure: Pending with no lease, no bridge
            // entry, no ticket, no quarantine and no StickyError, before the
            // paired-FREE claim, the epoch/generation write, lifetimeStarted,
            // the activeCall arm and any dev/context borrow. An existing lease
            // keeps its frozen profile and never re-reads these caches (a
            // resize is already refused at the EnableSceneMode gate).
            if (!g_sharedLifetimeStarted &&
                (g_sceneChangeBusy ||
                 g_pendingSceneW.load() != 0 || g_pendingSceneH.load() != 0 ||
                 !g_sharedScene1536Ready ||
                 g_sceneReadyEpoch != (unsigned long long)g_ringEpoch))
                return VRSIM::kSharedExportPending;
            VRSIM::SharedBridgeStatsV1 bs{};
            g_sharedBridge.Stats(&bs); // pure cached atomics; no D3D/wait under the gate
            if (!bound)
            {
                // FIRST shared grant: every old non-shared CPU/raw slot must be
                // FREE, no shared phase may exist, and all bridge slots cached
                // FREE. Mutual exclusion with legacy claim/publish in this same
                // gate; an unfinished old slot is never cleared here; READY
                // steal stays forbidden (claim consults state==0 only).
                bool allFree = true;
                for (int i = 0; i < RING_SLOTS; ++i)
                {
                    if (g_ring[i].state.load() != 0 ||
                        g_sharedPhase[i] != kSharedPhaseNone) { allFree = false; break; }
                    if (i < (int)VRSIM::kStereoBridgeSlotCountV1 &&
                        bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree)
                    { allFree = false; break; }
                }
                if (!allFree) return VRSIM::kSharedExportPending;
            }
            slot = RingClaimSharedPairedFreeLocked(bs, nullptr);
            if (slot < 0) return VRSIM::kSharedExportPending; // no paired FREE; NEVER steal READY
            claimEpoch = g_ringEpoch;
            // The ORIGINAL lease epoch/generation is written on the TRUE FIRST
            // reservation ONLY; a same-identity retry never rewrites it, so a
            // later Stop bumping the live ring epoch can NEVER replace this
            // lease's frozen original identity.
            if (!g_sharedLifetimeStarted)
            {
                g_sharedBoundEpoch = id.epoch;
                g_sharedBoundGeneration = id.generation;
                g_sharedLifetimeStarted = true; // lease from the FIRST grant; an Init failure never clears it
            }
            RingRecordSharedPreparingLocked(slot, claimEpoch, id); // caller now holds ring + meta
            activeCall.ArmLocked();
        }

        // Rollback of the still-PREPARING reservation: only the bridge's ACTUAL
        // cached FREE for the exact slot + epoch/WRITING/shared match releases
        // it; anything else retains + quarantines. The lifetime lease is NEVER
        // cleared here (no inferring emptiness from Initialized()==false).
        auto rollbackOrQuarantine = [&]() -> bool
        {
            VRSIM::SharedBridgeStatsV1 rbs{};
            g_sharedBridge.Stats(&rbs); // cached query outside the native gate
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
            StereoSlot& rs = g_ring[slot];
            const bool pairedFree =
                slot < (int)VRSIM::kStereoBridgeSlotCountV1 &&
                rbs.slotState[slot] == (uint32_t)VRSIM::kSharedStereoFree;
            const bool exact =
                rs.state.load() == 1 && rs.mode == kGpuSlotModeSharedV1 &&
                rs.epoch == claimEpoch &&
                g_sharedPhase[slot] == kSharedPhasePreparing;
            if (pairedFree && exact)
            {
                rs.state.store(0);
                rs.epoch = 0;
                rs.mode = (int)VRSIM::kGpuSlotModeCpu;
                rs.sharedTicket = VRSIM::SharedPreparedPairV1{};
                rs.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
                g_sharedPhase[slot] = kSharedPhaseNone;
                return true;
            }
            g_sharedQuarantined = true;
            g_sharedPhase[slot] = kSharedPhaseQuarantined;
            return false;
        };

        // First valid PREPARE initializes the bridge exactly once, OUTSIDE both
        // native gates (the bridge runs its own short lifetime/grant gates).
        if (!bound)
        {
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; LUID luid{};
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                dev = g_dev; ctx = g_ctx; luid = g_runtimeRequiredLuid; // borrowed AFTER the lease
            }
            const HRESULT ihr = g_sharedBridge.InitializeOnSourceUI(
                dev, ctx, luid, VRSIM::kStereoBridgeWidthV1, VRSIM::kStereoBridgeHeightV1,
                id.generation, id.epoch, leftSurface, rightSurface);
            if (ihr != S_OK)
            {
                const bool q = g_sharedBridge.Quarantined();
                {
                    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                    g_sharedLastBridgeHresult = (long long)ihr;
                    if (q) { g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined; }
                }
                if (q)
                {
                    SetErr("shared bridge init quarantined hr=" + SharedHrName(ihr));
                    return VRSIM::kSharedExportQuarantined; // unproven: slot + lease retained
                }
                const bool rb = rollbackOrQuarantine();
                SetErr("shared bridge init failed hr=" + SharedHrName(ihr) +
                       (rb ? " (reservation rolled back on proven FREE; lease retained for a real empty finalize)"
                           : " (reservation retained/quarantined)"));
                return rb ? VRSIM::kSharedExportError : VRSIM::kSharedExportQuarantined;
            }
            // back INSIDE the same gate: re-verify original slot/epoch/identity
            // and the close state before any further step.
            bool goPrepare = false;
            bool closedMidway = false;
            int lostRc = 0;
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                g_sharedLastBridgeHresult = (long long)ihr;
                const bool stillOurs =
                    g_sharedLifetimeStarted && !g_sharedQuarantined && !g_sharedFinalizeBusy &&
                    g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                    g_ring[slot].state.load() == 1 &&
                    g_ring[slot].mode == kGpuSlotModeSharedV1 &&
                    g_ring[slot].epoch == claimEpoch &&
                    g_sharedPhase[slot] == kSharedPhasePreparing;
                if (!stillOurs)
                {
                    g_sharedQuarantined = true;
                    g_sharedPhase[slot] = kSharedPhaseQuarantined;
                    lostRc = VRSIM::kSharedExportQuarantined;
                }
                else
                {
                    // Init created REAL owned resources: publish the bound mirror as
                    // a fact (needed for Discard/Finalize paths), even on close.
                    g_sharedBound = true;
                    closedMidway = g_sharedClosing || g_sharedAdmissionClosed ||
                                   g_sharedTeardownRequested || g_sharedFinalizeBusy ||
                                   !g_ringAdmission || claimEpoch != g_ringEpoch ||
                                   g_stopRequested.load();
                    goPrepare = !closedMidway;
                }
            }
            if (lostRc) // error text built OUTSIDE the native gate
            {
                SetErr("shared init grant identity lost mid-flight");
                return lostRc;
            }
            if (closedMidway)
            {
                // Close won before Prepare: NO Prepare call, NO PREPARED publish.
                // No ticket exists yet, so only a proven paired-FREE rollback of
                // the reservation runs; the lease stays for the real empty
                // finalize path.
                rollbackOrQuarantine();
                SetErr("shared lifecycle closed during init: prepare refused, lease pending finalize");
                return VRSIM::kSharedExportStale;
            }
            if (!goPrepare) // unreachable; keep compiler honest
                return VRSIM::kSharedExportStale;
        }

        // Real GPU transaction: outside g_ringMutex AND g_sharedMetaMutex.
        VRSIM::SharedPreparedPairV1 granted{};
        const HRESULT prh = g_sharedBridge.PrepareOnSourceUI((uint32_t)slot, leftSurface, rightSurface, &granted);

        // HIGHRES-H2SH-CARD2C: SAVE FIRST, regardless of prh. The frozen bridge
        // fills *out with the REAL ticket (SharedStereoBridge.cpp:519) BEFORE
        // its final RestoreAndVerify gate (:553), so a FAILED return can still
        // carry a non-zero granted ticket. Inside ONE ring -> meta gate the
        // full non-zero grant is recorded on the ORIGINAL slot (same slot /
        // epoch / identity; phase stays PREPARING) BEFORE any HRESULT /
        // actual-quarantine / shape / close classification below. A slot whose
        // original claim is gone is never overwritten (no wrong-slot write
        // masking a new identity); what is saved here is never reset later.
        if (granted.ticket != 0)
        {
            bool ticketCaptured = false;
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                StereoSlot& cs = g_ring[slot];
                const bool stillOriginalClaim =
                    g_sharedLifetimeStarted &&
                    g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                    cs.state.load() == 1 && cs.mode == kGpuSlotModeSharedV1 &&
                    cs.epoch == claimEpoch &&
                    g_sharedPhase[slot] == kSharedPhasePreparing;
                if (stillOriginalClaim)
                {
                    cs.sharedTicket = granted; // full original grant, kept even on FAILED
                    ticketCaptured = true;
                }
            }
            if (!ticketCaptured)
            {
                // LIVE ticket but the original claim was lost between gates:
                // fail closed — retain + quarantine, never Discard/rollback an
                // unknown, never write over a foreign slot identity.
                const bool qLost = g_sharedBridge.Quarantined();
                {
                    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                    g_sharedLastBridgeHresult = (long long)prh;
                    g_sharedQuarantined = true;
                    g_sharedPhase[slot] = kSharedPhaseQuarantined;
                }
                SetErr(std::string("shared prepare live ticket lost original slot") +
                       (qLost ? " (bridge quarantined)" : " (retained)"));
                return VRSIM::kSharedExportQuarantined;
            }
        }

        if (prh != S_OK)
        {
            const bool q = g_sharedBridge.Quarantined(); // the bridge's ACTUAL state
            if (granted.ticket != 0)
            {
                if (q)
                {
                    // FAILED + actual quarantine + real ticket: the ticket stays
                    // recorded on the slot; NO Discard, NO rollback to FREE on
                    // unknown — unproven GPU work does NOT free the slot.
                    {
                        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                        g_sharedLastBridgeHresult = (long long)prh;
                        g_sharedQuarantined = true;
                        g_sharedPhase[slot] = kSharedPhaseQuarantined;
                    }
                    SetErr("shared prepare quarantined with live ticket hr=" + SharedHrName(prh));
                    return VRSIM::kSharedExportQuarantined;
                }
                // FAILED + non-zero ticket + proven NOT quarantined: the exact
                // original ticket is Discarded on the source UI OUTSIDE the
                // native gates; FREE happens only on actual Discard S_OK + the
                // bridge's cached FREE for this slot + the original
                // slot/epoch/ticket/phase all matching. Anything else retains
                // the saved ticket + quarantines.
                const HRESULT drh = g_sharedBridge.DiscardPreparedOnSourceUI(granted);
                VRSIM::SharedBridgeStatsV1 dbs{};
                g_sharedBridge.Stats(&dbs); // cached query outside the native gate
                bool freed = false;
                {
                    std::lock_guard<std::mutex> rlk(g_ringMutex);
                    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta
                    g_sharedLastBridgeHresult = (long long)drh;
                    StereoSlot& rs = g_ring[slot];
                    const bool pairedFree =
                        drh == S_OK && slot < (int)VRSIM::kStereoBridgeSlotCountV1 &&
                        dbs.slotState[slot] == (uint32_t)VRSIM::kSharedStereoFree;
                    const bool exact =
                        g_sharedLifetimeStarted && !g_sharedQuarantined && !g_sharedFinalizeBusy &&
                        g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                        rs.state.load() == 1 && rs.mode == kGpuSlotModeSharedV1 &&
                        rs.epoch == claimEpoch &&
                        g_sharedPhase[slot] == kSharedPhasePreparing &&
                        VRSIM::SharedTicketFieldsEqual(granted, rs.sharedTicket);
                    if (pairedFree && exact)
                    {
                        rs.state.store(0);
                        rs.epoch = 0;
                        rs.mode = (int)VRSIM::kGpuSlotModeCpu;
                        rs.sharedTicket = VRSIM::SharedPreparedPairV1{};
                        rs.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
                        g_sharedPhase[slot] = kSharedPhaseNone;
                        freed = true;
                    }
                    else
                    {
                        g_sharedQuarantined = true;
                        g_sharedPhase[slot] = kSharedPhaseQuarantined;
                    }
                }
                SetErr("shared prepare failed with granted ticket hr=" + SharedHrName(prh) +
                       " (discard hr=" + SharedHrName(drh) + ")");
                return freed ? VRSIM::kSharedExportError : VRSIM::kSharedExportQuarantined;
            }
            // FAILED + zero ticket: the already-reviewed PREPARING rollback.
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                g_sharedLastBridgeHresult = (long long)prh;
                if (q) { g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined; }
            }
            if (q)
            {
                SetErr("shared prepare quarantined hr=" + SharedHrName(prh));
                return VRSIM::kSharedExportQuarantined;
            }
            const bool rb = rollbackOrQuarantine();
            SetErr("shared prepare failed hr=" + SharedHrName(prh));
            return rb ? VRSIM::kSharedExportError : VRSIM::kSharedExportQuarantined;
        }

        const bool grantOk =
            granted.version == VRSIM::kStereoBridgeVersionV1 &&
            granted.structBytes == (uint32_t)sizeof(VRSIM::SharedPreparedPairV1) &&
            granted.slotIndex == (uint32_t)slot &&
            granted.generation == id.generation &&
            granted.epoch == id.epoch &&
            granted.ticket != 0 &&
            granted.width == VRSIM::kStereoBridgeWidthV1 &&
            granted.height == VRSIM::kStereoBridgeHeightV1 &&
            granted.format88 == VRSIM::kStereoBridgeFormat88V1;

        // ---- Classify AFTER the save-first capture above: the original ticket
        // is already on the slot. Only this gate's success may publish PREPARED;
        // a post-close success is rolled back or quarantined, never published,
        // the stored ticket is never rewritten or lost, and the new live epoch
        // never replaces the original identity. ----
        bool published = false;
        int lostRc2 = 0;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            StereoSlot& ws = g_ring[slot];
            const bool stillOurs =
                g_sharedLifetimeStarted && g_sharedBound &&
                !g_sharedQuarantined && !g_sharedFinalizeBusy &&
                g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                ws.state.load() == 1 && ws.mode == kGpuSlotModeSharedV1 &&
                ws.epoch == claimEpoch &&
                g_sharedPhase[slot] == kSharedPhasePreparing;
            g_sharedLastBridgeHresult = (long long)prh;
            if (!stillOurs)
            {
                // The grant (if any real ticket) is ALREADY recorded on the
                // original slot by the save-first gate above. Never rewrite the
                // slot here: a lost claim may carry a NEW identity that a stale
                // write must not mask. Fail closed: retain + quarantine.
                g_sharedQuarantined = true;
                g_sharedPhase[slot] = kSharedPhaseQuarantined;
                lostRc2 = VRSIM::kSharedExportQuarantined;
            }
            else if (granted.ticket != 0 &&
                     !VRSIM::SharedTicketFieldsEqual(granted, ws.sharedTicket))
            {
                // The saved original ticket changed between the two gates:
                // unknown state — keep what is stored, retain + quarantine.
                g_sharedQuarantined = true;
                g_sharedPhase[slot] = kSharedPhaseQuarantined;
                lostRc2 = VRSIM::kSharedExportQuarantined;
            }
            else
            {
                // No ticket rewrite: the save-first gate already stored the
                // grant (or nothing for a zero-ticket shape violation).
                g_sharedPhase[slot] = kSharedPhasePrepared;
                const bool closedMidway =
                    g_sharedClosing || g_sharedAdmissionClosed || g_sharedTeardownRequested ||
                    g_sharedFinalizeBusy || !g_ringAdmission || claimEpoch != g_ringEpoch ||
                    g_stopRequested.load();
                published = grantOk && !closedMidway;
            }
        }
        if (lostRc2) // error text built OUTSIDE the native gate
        {
            SetErr("shared prepare lost its grant identity mid-flight");
            return lostRc2;
        }

        if (!published)
        {
            // Close won / bad shape: exact original-ticket source UI Discard runs
            // OUTSIDE the native gate (the bridge holds its own short grant
            // gate). Only actual S_OK + the matching bridge cached FREE + the
            // same gate-proven original slot ticket/epoch may roll back;
            // anything unknown is retained + quarantined.
            const HRESULT drh = g_sharedBridge.DiscardPreparedOnSourceUI(granted);
            VRSIM::SharedBridgeStatsV1 bs2{};
            g_sharedBridge.Stats(&bs2); // cached outside the gate
            bool rolledBack = false;
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
                g_sharedLastBridgeHresult = (long long)drh;
                StereoSlot& rs = g_ring[slot];
                const bool pairedFree =
                    granted.slotIndex == (uint32_t)slot && drh == S_OK &&
                    slot < (int)VRSIM::kStereoBridgeSlotCountV1 &&
                    bs2.slotState[slot] == (uint32_t)VRSIM::kSharedStereoFree;
                const bool exact =
                    rs.state.load() == 1 && rs.mode == kGpuSlotModeSharedV1 &&
                    rs.epoch == claimEpoch &&
                    VRSIM::SharedTicketFieldsEqual(granted, rs.sharedTicket);
                if (pairedFree && exact)
                {
                    rs.state.store(0);
                    rs.epoch = 0;
                    rs.mode = (int)VRSIM::kGpuSlotModeCpu;
                    rs.sharedTicket = VRSIM::SharedPreparedPairV1{};
                    rs.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
                    g_sharedPhase[slot] = kSharedPhaseNone;
                    rolledBack = true;
                }
                else
                {
                    // unproven closure: the saved ticket + slot stay, quarantine
                    g_sharedQuarantined = true;
                    g_sharedPhase[slot] = kSharedPhaseQuarantined;
                }
            }
            SetErr("shared prepare rollback (discard hr=" + SharedHrName(drh) + ")");
            if (!rolledBack) return VRSIM::kSharedExportQuarantined;
            return grantOk ? VRSIM::kSharedExportStale   // close linearly won: honest stale, ticket reclaimed
                           : VRSIM::kSharedExportError;  // unexpected grant shape discarded cleanly
        }

        unsigned char tbuf[64];
        VRSIM::SharedTicketSerializeZeroedV1(tbuf, granted); // tail 60..63 serialized zero
        memcpy(outTicket64, tbuf, 64);
        return VRSIM::kSharedExportCompleted;
    }
    catch (...)
    {
        // C ABI never throws. Fail closed: if a slot was reserved, quarantine
        // the tracking rather than freeing unproven GPU-adjacent state. The
        // lifetime LEASE stays (never inferred empty) and the armed active call
        // is dropped exactly once by the SharedActiveCallGuard destructor.
        if (slot >= 0)
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedQuarantined = true;
            g_sharedPhase[slot] = kSharedPhaseQuarantined;
        }
        SetErr("shared prepare threw");
        return VRSIM::kSharedExportError;
    }
}

// Source UI only, AFTER the typed owner returned through both Unlock
// operations and the managed post guards (never inside the locked callback).
// The caller resubmits the ORIGINAL ticket + identity with captureEndQpc set
// exactly once; every other field/bit pattern must match the stored PREPARE
// image. All validation, the metadata-only CommitPreparedOnSourceUI bridge
// grant, identity publication, ring READY, phase COMMITTED and the stats
// counter execute in ONE linearized ring -> meta gate against the drain close.
__declspec(dllexport) int VRSIM_CommitSharedStereoV1(
    const void* ticket64, unsigned int ticketCapacityBytes,
    const void* identity, unsigned int identityCapacityBytes)
{
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportUnavailable;
    if (!ticket64 || !identity) return VRSIM::kSharedExportBadArgument;
    if (ticketCapacityBytes != VRSIM::kSharedTicketExportBytesV1) return VRSIM::kSharedExportBadArgument;
    if (identityCapacityBytes != VRSIM::kSharedCaptureIdentityV1Bytes) return VRSIM::kSharedExportBadArgument;
    int slot = -1;
    try
    {
        VRSIM::SharedPreparedPairV1 t{};
        {
            unsigned char tb[64];
            memcpy(tb, ticket64, 64);
            memcpy(&t, tb, 60);
        }
        if (!SharedWireTicketShapeOk(t)) return VRSIM::kSharedExportBadArgument;
        VRSIM::SharedCaptureIdentityV1 id{};
        memcpy(&id, identity, sizeof(id));
        if (!SharedWireValidateCommitShape(id)) return VRSIM::kSharedExportBadArgument;
        slot = (int)t.slotIndex;

        int rc = VRSIM::kSharedExportError;
        HRESULT commitHr = E_FAIL;
        bool bridgeCalled = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);

            if (!g_sharedLifetimeStarted || !g_sharedBound)
            { rc = VRSIM::kSharedExportUnavailable; }
            else if (g_sharedQuarantined || g_sharedFinalizedOnce || g_sharedSourceFinalized)
            { rc = VRSIM::kSharedExportQuarantined; }
            else if (g_sharedClosing || g_sharedAdmissionClosed || g_sharedTeardownRequested ||
                     g_sharedFinalizeBusy || g_stopRequested.load())
            { rc = VRSIM::kSharedExportStale; }
            else if (g_state.load() != VRSIM_RUNNING)
            { rc = VRSIM::kSharedExportStale; }
            else if (!g_ringAdmission || g_ring[slot].epoch != g_ringEpoch)
            { rc = VRSIM::kSharedExportStale; }
            else if (g_ring[slot].state.load() != 1 ||
                     g_ring[slot].mode != kGpuSlotModeSharedV1)
            { rc = VRSIM::kSharedExportStale; }
            else if (g_sharedPhase[slot] != kSharedPhasePrepared)
            { rc = VRSIM::kSharedExportStale; }
            else if (g_sharedBoundEpoch != id.epoch ||
                     g_sharedBoundGeneration != id.generation)
            { rc = VRSIM::kSharedExportStale; }
            else if (!VRSIM::SharedTicketFieldsEqual(t, g_ring[slot].sharedTicket))
            { rc = VRSIM::kSharedExportStale; }
            else if (g_ring[slot].sharedIdentity.captureEndQpc != 0)
            { rc = VRSIM::kSharedExportStale; }
            else if (!VRSIM::SharedIdentityEqualExceptCompletion(
                         g_ring[slot].sharedIdentity, id))
            { rc = VRSIM::kSharedExportStale; }
            else
            {
                bridgeCalled = true;
                commitHr = g_sharedBridge.CommitPreparedOnSourceUI(
                    g_ring[slot].sharedTicket);
                g_sharedLastBridgeHresult = (long long)commitHr;
                if (commitHr == S_OK)
                {
                    g_ring[slot].sharedIdentity = id;
                    RingPublishReadyLocked(slot, g_ring[slot].frameSerial);
                    g_sharedPhase[slot] = kSharedPhaseCommitted;
                    ++g_submitOk;
                    GpuStatsRawAccepted();
                    rc = VRSIM::kSharedExportCompleted;
                }
                else
                {
                    const bool q = g_sharedBridge.Quarantined();
                    if (q)
                    {
                        g_sharedQuarantined = true;
                        g_sharedPhase[slot] = kSharedPhaseQuarantined;
                    }
                    rc = q ? VRSIM::kSharedExportQuarantined
                           : VRSIM::kSharedExportError;
                }
            }
        }

        if (rc != VRSIM::kSharedExportCompleted)
        {
            if (bridgeCalled)
                SetErr("shared commit refused hr=" + SharedHrName(commitHr));
            return rc;
        }
        return rc;
    }
    catch (...)
    {
        if (slot >= 0)
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedQuarantined = true;
            g_sharedPhase[slot] = kSharedPhaseQuarantined;
        }
        SetErr("shared commit threw");
        return VRSIM::kSharedExportError;
    }
}

// Source UI only. Exact-ticket checked; unknown tickets are never dropped.
// PREPARED may be discarded while admission is open; a never-read COMMITTED
// only after CloseAdmission. READING/INFLIGHT are left for the worker event
// drain. The ring slot returns to FREE only on a PROVEN paired bridge FREE.
// SharedActiveCallGuard arms inside the initial gate and covers the entire
// source Discard until the final paired-FREE gate completes (or the try exits).
__declspec(dllexport) int VRSIM_DiscardSharedStereoV1(const void* ticket64, unsigned int ticketCapacityBytes)
{
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportUnavailable;
    if (!ticket64) return VRSIM::kSharedExportBadArgument;
    if (ticketCapacityBytes != VRSIM::kSharedTicketExportBytesV1) return VRSIM::kSharedExportBadArgument;
    int slot = -1;
    try
    {
        VRSIM::SharedPreparedPairV1 t{};
        {
            unsigned char tb[64];
            memcpy(tb, ticket64, 64);
            memcpy(&t, tb, 60);
        }
        if (!SharedWireTicketShapeOk(t)) return VRSIM::kSharedExportBadArgument;
        slot = (int)t.slotIndex;

        VRSIM::SharedPreparedPairV1 stored{};
        SharedActiveCallGuard activeCall;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            if (!g_sharedLifetimeStarted || !g_sharedBound)
                return VRSIM::kSharedExportUnavailable;
            if (g_sharedFinalizeBusy)
                return VRSIM::kSharedExportPending;
            if (g_sharedQuarantined)
                return VRSIM::kSharedExportQuarantined;
            const int phase = g_sharedPhase[slot];
            if (phase == kSharedPhaseNone)
                return VRSIM::kSharedExportStale;
            if (phase == kSharedPhasePreparing)
                return VRSIM::kSharedExportPending;
            if (phase == kSharedPhaseReading || phase == kSharedPhaseInflight)
                return VRSIM::kSharedExportPending;
            if (!VRSIM::SharedTicketFieldsEqual(t, g_ring[slot].sharedTicket))
                return VRSIM::kSharedExportStale;
            if (g_sharedBoundEpoch != t.epoch || g_sharedBoundGeneration != t.generation)
                return VRSIM::kSharedExportStale;
            if (g_ring[slot].mode != kGpuSlotModeSharedV1)
                return VRSIM::kSharedExportStale;
            if (phase == kSharedPhasePrepared)
            {
                if (g_ring[slot].state.load() != 1)
                    return VRSIM::kSharedExportStale;
            }
            else
            {
                if (!g_sharedAdmissionClosed)
                    return VRSIM::kSharedExportPending;
                if (g_ring[slot].state.load() != 2)
                    return VRSIM::kSharedExportPending;
            }
            stored = g_ring[slot].sharedTicket;
            activeCall.ArmLocked();
        }

        const HRESULT hr = g_sharedBridge.DiscardPreparedOnSourceUI(stored);

        if (hr != S_OK)
        {
            const bool q = g_sharedBridge.Quarantined();
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                g_sharedLastBridgeHresult = (long long)hr;
                if (q) { g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined; }
            }
            SetErr("shared discard refused hr=" + SharedHrName(hr));
            return q ? VRSIM::kSharedExportQuarantined : VRSIM::kSharedExportError;
        }

        int rc = VRSIM::kSharedExportCompleted;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            VRSIM::SharedBridgeStatsV1 bs2{};
            g_sharedBridge.Stats(&bs2);
            StereoSlot& s = g_ring[slot];
            const int phase = g_sharedPhase[slot];
            const bool phaseOk = (phase == kSharedPhasePrepared) ||
                                 (phase == kSharedPhaseCommitted && g_sharedAdmissionClosed);
            const bool freeOk = (slot < (int)VRSIM::kStereoBridgeSlotCountV1) &&
                                bs2.slotState[slot] == (uint32_t)VRSIM::kSharedStereoFree;
            const bool ticketOk = s.mode == kGpuSlotModeSharedV1 &&
                                  VRSIM::SharedTicketFieldsEqual(stored, s.sharedTicket) &&
                                  s.sharedTicket.epoch == stored.epoch &&
                                  s.sharedTicket.generation == stored.generation &&
                                  s.epoch == stored.epoch;
            const int ringState = s.state.load();
            g_sharedLastBridgeHresult = (long long)hr;
            if (g_sharedQuarantined)          rc = VRSIM::kSharedExportQuarantined;
            else if (!phaseOk)                rc = VRSIM::kSharedExportPending;
            else if (!freeOk)                 rc = VRSIM::kSharedExportPending;
            else if (!ticketOk || (ringState != 1 && ringState != 2))
                                              rc = VRSIM::kSharedExportStale;
            else
            {
                s.state.store(0);
                s.epoch = 0;
                s.mode = (int)VRSIM::kGpuSlotModeCpu;
                s.sharedTicket = VRSIM::SharedPreparedPairV1{};
                s.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
                g_sharedPhase[slot] = kSharedPhaseNone;
                SharedUpdateDrainLocked(bs2);
            }
        }
        return rc;
    }
    catch (...)
    {
        if (slot >= 0)
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedQuarantined = true;
            g_sharedPhase[slot] = kSharedPhaseQuarantined;
        }
        SetErr("shared discard threw");
        return VRSIM::kSharedExportError;
    }
}

// Closes shared admissions. NO D3D/context calls from this caller: the bridge
// CloseAdmission is its own atomic-once short gate. The worker event drain
// proof (INFLIGHT -> FREE on S_OK+TRUE) is the Card2 worker path; this export
// only linearizes the close and reports honest pending where appropriate.
// Activation false: honest Completed 0 (no flags, no bridge call, same as before).
// Activation true with real lease: ONE ring->meta gate runs SharedCloseLocked(false)
// which calls bridge CloseAdmission (pure metadata, gate-legal). Original
// boundEpoch/generation and every stored ticket/phase identity stay untouched.
// Activation true with NO lease (never-shared or failed-before-first-grant):
// only native g_sharedClosing=true + RingSetAdmissionLocked(false) close the gate
// for future grants; no bridge CloseAdmission, no fake admissionClosed/flags.
// Returns the actual bridge S_OK/S_FALSE as Completed; otherwise Error.
__declspec(dllexport) int VRSIM_RequestSharedDrainV1()
{
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportCompleted;
    try
    {
        long long closeHr = 0;
        bool hadLease = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            if (SharedLeaseOrStateLocked())
            {
                hadLease = true;
                SharedCloseLocked(false);
                closeHr = g_sharedLastBridgeHresult;
            }
            else
            {
                g_sharedClosing = true;
                RingSetAdmissionLocked(false);
            }
        }
        if (hadLease)
        {
            if (closeHr == (long long)S_OK || closeHr == (long long)S_FALSE)
                return VRSIM::kSharedExportCompleted;
            SetErr("shared drain CloseAdmission failed hr=" +
                   SharedHrName((HRESULT)closeHr));
            return VRSIM::kSharedExportError;
        }
        return VRSIM::kSharedExportCompleted;
    }
    catch (...) { SetErr("shared drain threw"); return VRSIM::kSharedExportError; }
}

// Purely cached read; never executes D3D/COM/wait and never infers a flag from
// elapsed time. While no shared session is bound the state is reported as the
// honest empty image (all-zero flags/epoch), NEVER a fabricated drained/finalized.
__declspec(dllexport) int VRSIM_GetSharedStopStateV1(void* out, unsigned int capacityBytes)
{
    if (!out) return VRSIM::kSharedExportBadArgument;
    if (capacityBytes < VRSIM::kSharedStopStateV1Bytes) return VRSIM::kSharedExportBadArgument;
    try
    {
        VRSIM::SharedStopStateV1 st{}; // value-init zeroes every byte incl. reserved at 28
        st.structBytes = VRSIM::kSharedStopStateV1Bytes;
        st.version = VRSIM::kSharedStopStateV1Version;
        // ONE short ring -> meta gate for the whole snapshot: flags, bound
        // epoch/generation and the pending count come from a single coherent
        // window (no mixed-generation between two lock phases). Pure atomic/
        // POD reads: no GPU/COM/wait. Unbound state stays the honest empty
        // image (flags 0), NEVER a fabricated drained/finalized.
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
            uint32_t flags = 0;
            if (g_sharedAdmissionClosed) flags |= VRSIM::kSharedStopFlagAdmissionClosed;
            if (g_sharedWorkerDrained)   flags |= VRSIM::kSharedStopFlagWorkerDrained;
            if (g_sharedSourceFinalized) flags |= VRSIM::kSharedStopFlagSourceFinalized;
            if (g_sharedQuarantined)     flags |= VRSIM::kSharedStopFlagQuarantined;
            st.flags = flags;
            st.boundEpoch = g_sharedBoundEpoch;
            st.boundGeneration = g_sharedBoundGeneration;
            uint32_t pending = 0;
            for (int i = 0; i < RING_SLOTS; ++i)
                if (g_ring[i].mode == kGpuSlotModeSharedV1 && g_ring[i].state.load() != 0 &&
                    g_sharedPhase[i] != kSharedPhaseNone)
                    ++pending;
            st.pendingSlots = pending;
        }
        static_assert(sizeof(VRSIM::SharedStopStateV1) == 32, "stop32 has no implicit padding");
        memcpy(out, &st, sizeof(st));
        return VRSIM::kSharedExportCompleted;
    }
    catch (...) { return VRSIM::kSharedExportError; }
}

// Cached bridge counters only (the bridge Answers from atomics; safe even
// before any Initialize because the Pimpl is never freed). Exported 72 bytes
// are serialized with the alignment padding at 20..23 proven zero.
__declspec(dllexport) int VRSIM_GetSharedBridgeStatsV1(void* out, unsigned int capacityBytes)
{
    if (!out) return VRSIM::kSharedExportBadArgument;
    if (capacityBytes < VRSIM::kSharedBridgeStatsExportBytesV1) return VRSIM::kSharedExportBadArgument;
    try
    {
        VRSIM::SharedBridgeStatsV1 bs{};
        g_sharedBridge.Stats(&bs);
        bs.version = VRSIM::kStereoBridgeVersionV1;                 // fixed contract header
        bs.structBytes = (uint32_t)sizeof(VRSIM::SharedBridgeStatsV1);
        unsigned char buf[72];
        VRSIM::SharedBridgeStatsSerializeZeroedV1(buf, bs);
        memcpy(out, buf, 72);
        return VRSIM::kSharedExportCompleted;
    }
    catch (...) { return VRSIM::kSharedExportError; }
}

// Same source UI thread, after the worker-drained proof and source capture
// busy=0. While no shared lifetime lease exists (never Prepare attempted) this
// reports the honest Unavailable. A real lease that is still PREPARING or
// has activeCalls cannot finalize: the initial gate proves all conditions
// inside ONE ring->meta lock, arms FinalizeBusyGuard, and only then releases
// the gates to call bridge TryFinalizeOnSourceUI (COM/GPU outside). On S_OK a
// second same-identity ring->meta gate re-validates before recording finalized.
// Activation false and ABI are unchanged.
__declspec(dllexport) int VRSIM_FinalizeSharedSourceUIV1()
{
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportUnavailable;
    int rc = VRSIM::kSharedExportError;
    unsigned long long savedEpoch = 0;
    uint32_t savedGen = 0;
    FinalizeBusyGuard busyGuard;
    try
    {
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);

            if (g_sharedSourceFinalized)
                return VRSIM::kSharedExportCompleted;
            if (!g_sharedLifetimeStarted)
                return VRSIM::kSharedExportUnavailable;
            if (g_sharedQuarantined)
                return VRSIM::kSharedExportQuarantined;
            if (g_sharedFinalizeBusy)
                return VRSIM::kSharedExportPending;
            if (!g_sharedAdmissionClosed || !g_sharedWorkerDrained ||
                !g_sharedWorkerNoContext || !g_sharedWorkerCleanupDone)
                return VRSIM::kSharedExportPending;
            if (g_sharedSourceActiveCalls != 0)
                return VRSIM::kSharedExportPending;

            VRSIM::SharedBridgeStatsV1 bs{};
            g_sharedBridge.Stats(&bs);
            bool allFree = true;
            for (int i = 0; i < RING_SLOTS; ++i)
            {
                if (g_sharedPhase[i] != kSharedPhaseNone ||
                    g_ring[i].state.load() != 0 ||
                    g_ring[i].mode == kGpuSlotModeSharedV1)
                { allFree = false; break; }
                if (i < (int)VRSIM::kStereoBridgeSlotCountV1 &&
                    bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree)
                { allFree = false; break; }
            }
            if (!allFree)
                return VRSIM::kSharedExportPending;

            savedEpoch = g_sharedBoundEpoch;
            savedGen = g_sharedBoundGeneration;
            busyGuard.ArmLocked();
        }

        const HRESULT hr = g_sharedBridge.TryFinalizeOnSourceUI();

        if (hr == S_OK)
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedLastBridgeHresult = (long long)hr;
            if (g_sharedLifetimeStarted &&
                g_sharedBoundEpoch == savedEpoch &&
                g_sharedBoundGeneration == savedGen &&
                g_sharedSourceActiveCalls == 0 &&
                !g_sharedQuarantined)
            {
                bool stillAllFree = true;
                for (int i = 0; i < RING_SLOTS; ++i)
                    if (g_sharedPhase[i] != kSharedPhaseNone ||
                        g_ring[i].state.load() != 0)
                    { stillAllFree = false; break; }
                if (stillAllFree)
                {
                    g_sharedSourceFinalized = true;
                    g_sharedFinalizedOnce = true;
                    rc = VRSIM::kSharedExportCompleted;
                }
                else
                {
                    rc = VRSIM::kSharedExportError;
                }
            }
            else
            {
                rc = VRSIM::kSharedExportError;
            }
        }
        else if (hr == S_FALSE)
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedLastBridgeHresult = (long long)hr;
            rc = VRSIM::kSharedExportPending;
        }
        else
        {
            const bool q = g_sharedBridge.Quarantined();
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                g_sharedLastBridgeHresult = (long long)hr;
                if (q) g_sharedQuarantined = true;
            }
            SetErr("shared finalize failed hr=" + SharedHrName(hr));
            rc = q ? VRSIM::kSharedExportQuarantined : VRSIM::kSharedExportError;
        }
        return rc;
    }
    catch (...)
    {
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedQuarantined = true;
        }
        SetErr("shared finalize threw");
        return VRSIM::kSharedExportError;
    }
}

__declspec(dllexport) int VRSIM_EnableSceneMode(int width, int height)
{
    if (width < 64 || height < 64 || width > 2048 || height > 2048) return -1; // size contract only
    // SCENE-FREEZE-1: queue-vs-first-Prepare is ONE ring -> meta transaction.
    // Once ANY real shared lease/state exists (lifetime lease, active call,
    // outstanding phase, finalizeBusy, quarantine, finalizedOnce, closing /
    // teardown / bridge admissionClosed) the request is refused here and is
    // NEVER written to the queue, including a same-size 1536 request; the
    // refusal does not touch the bridge master. A normal prelease session in
    // STARTING / SYNCHRONIZED / VISIBLE / RUNNING with native admission still
    // open DOES queue, so startup is not gated on RUNNING alone. STOPPING /
    // ERROR / OFF or a closed native admission is refused.
    bool accepted = false;
    bool sharedRefuse = false;
    const char* why = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        if (SharedLeaseOrStateLocked() || g_sharedQuarantined || g_sharedFinalizeBusy ||
            g_sharedFinalizedOnce || g_sharedClosing || g_sharedTeardownRequested ||
            g_sharedAdmissionClosed)
        {
            sharedRefuse = true;
            why = "shared lease/lifecycle already engaged: scene request refused, queue untouched";
        }
        else if (g_stopRequested.load() || !g_ringAdmission ||
                 g_state.load() == VRSIM_OFF || g_state.load() == VRSIM_STOPPING ||
                 g_state.load() == VRSIM_ERROR)
        {
            why = "session not live or native admission closed: scene request refused";
        }
        else
        {
            // latest request wins the queue; readiness is only invalidated here
            g_pendingSceneW = width;
            g_pendingSceneH = height;
            g_vcSceneRequested = true;
            g_sharedScene1536Ready = false;
            g_sceneReadyEpoch = 0;
            accepted = true;
        }
    }
    if (!accepted)
    {
        SetErr(std::string("VRSIM_EnableSceneMode refused: ") + why);
        return sharedRefuse ? -2 : -3; // non-zero: never reported as applied
    }
    return 0;
}

__declspec(dllexport) int VRSIM_GetStereoStats(VRSIMStereoStats* out)
{
    if (!out) return -1;
    int freeSlots = 0;
    for (int i = 0; i < RING_SLOTS; ++i) if (g_ring[i].state.load() == 0) ++freeSlots;
    out->submitOk = g_submitOk.load(); out->submitFail = g_submitFail.load();
    out->d3dUploads = g_d3dUploads.load(); out->vcLayerOk = g_vcLayerOk.load();
    out->reusedFrames = g_reusedFrames.load();
    out->latestFrameSerial = g_latestReadySerial.load();
    out->quadOkCount = g_quadOk.load();
    out->sceneMode = g_sceneMode.load() ? 1 : 0;
    out->ringFree = freeSlots;
    out->framesDropped = g_framesDropped.load();
    out->lastConsumedSerial = g_lastConsumedSerial.load();
    out->renderSourceMode = g_renderSource.load();
    out->illegalFallback = g_illegalFallback.load() ? 1 : 0;
    out->requestedRenderSource = g_vcSceneRequested.load() ? RSM_VC_SCENE : RSM_SYNTHETIC_TEST;
    out->poseSerial = g_poseSerial.load();
    out->poseSamples = g_poseSamples.load();
    out->poseChanges = g_poseChanges.load();
    out->poseImageMismatch = g_poseImageMismatch.load();
    out->lastLayerPoseSerial = g_lastLayerPoseSerial.load();
    out->userPresenceAvailable = g_userPresenceAvail.load();
    out->userPresenceSupported = g_userPresenceSupported.load();
    out->isUserPresent = g_userPresent.load();
    out->lastConsumedYawDeg = g_lastConsumedYawDeg.load();
    out->consumedYawChanges = g_consumedYawChanges.load();
    out->beginFrameFailed = g_beginFrameFailed.load();
    out->d3dUploadCount = g_d3dUploads.load();
    return 0;
}

__declspec(dllexport) int VRSIM_SetLoaderPathW(const wchar_t* absolutePath)
{
    if (!absolutePath || !absolutePath[0]) { g_loaderPathOverride.clear(); return 0; }
    g_loaderPathOverride.assign(absolutePath, absolutePath + wcslen(absolutePath));
    return 0;
}

__declspec(dllexport) int VRSIM_Create()
{
    if (g_worker.joinable()) { SetErr("already created"); return -1; }
    // HIGHRES-H2SH-CARD1/D2: ONE short ring->meta permission gate replaces the
    // old separate guard sequence. A bound, un-drained or quarantined shared
    // bridge tracking is NEVER wiped by a legacy Create reset; the closed
    // owner cannot be reused here (a fresh owned QA host is required) and
    // g_sharedFinalizedOnce permanently forbids a second shared Initialize.
    // Only a genuinely EMPTY session may open a legacy door: no lease/state,
    // no bound/lifetime, no quarantine/finalizeBusy/active calls, every native
    // slot FREE/None/not-SharedV1, every cached bridge slot FREE, and the
    // actual runtime resources absent. A retained ERROR/WAITING state proves
    // nothing. Nothing is reset to manufacture emptiness.
    bool emptyGrant = false;
    const char* deny = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        VRSIM::SharedBridgeStatsV1 bs{};
        g_sharedBridge.Stats(&bs); // pure cached stats: no GPU/COM/SDK call here
        bool ok = !g_worker.joinable() &&
                  !SharedLeaseOrStateLocked() &&
                  !g_sharedQuarantined && !g_sharedFinalizeBusy &&
                  g_sharedSourceActiveCalls == 0 &&
                  !g_sharedBound && !g_sharedLifetimeStarted;
        if (ok)
        {
            for (int i = 0; i < RING_SLOTS; ++i)
            {
                if (g_ring[i].state.load() != 0 ||
                    g_ring[i].mode == kGpuSlotModeSharedV1 ||
                    g_sharedPhase[i] != kSharedPhaseNone)
                { deny = "native ring slot not empty"; ok = false; break; }
            }
        }
        if (ok)
        {
            for (int i = 0; i < (int)VRSIM::kStereoBridgeSlotCountV1; ++i)
            {
                if (bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree)
                { deny = "shared bridge slot not free"; ok = false; break; }
            }
        }
        if (ok)
        {
            for (int e = 0; e < 2; ++e)
            {
                if (g_swap[e] != XR_NULL_HANDLE || g_quadTex[e] || g_sceneTex[e] ||
                    g_redTex[e] || !g_swapTex[e].empty())
                { deny = "previous session XR/D3D resources retained"; ok = false; break; }
            }
        }
        if (ok && (g_loader != nullptr || g_inst != XR_NULL_HANDLE ||
                   g_sess != XR_NULL_HANDLE || g_localSpace != XR_NULL_HANDLE ||
                   g_dev != nullptr || g_ctx != nullptr || g_gpuPassReady))
        { deny = "previous session loader/device/adapter resources retained"; ok = false; }
        if (ok)
        {
            // Native ring admission closes FIRST so no old raw submit can be
            // admitted; then only the native legacy doors open for this session.
            // g_sharedAdmissionClosed / bridge master / g_sharedFinalizedOnce
            // are deliberately not reopened here.
            RingSetAdmissionLocked(false);
            g_sharedClosing = false;
            g_sharedTeardownRequested = false;
            g_stopRequested = false;
            emptyGrant = true;
        }
    }
    if (!emptyGrant)
    {
        SetErr(std::string("VRSIM_Create refused, nothing reset: ") +
               (deny ? deny : "session state not genuinely empty"));
        return -4;
    }
    DumpCancelPending(); // no stale dump request may cross into a new session
    // HIGHRES-H2-GPU: no previous worker exists and the producer is stopped
    // (serialized Create contract) -> clear stale ring slots, pose ring, layer/
    // consumed serials, pending scene dims and the active GPU identity AFTER the
    // empty permission grant (never inside the gate: no recursive g_ringMutex).
    // Cumulative counters are preserved. Admission stays closed until the
    // existing valid SessionInitPhase opens it.
    CreateResetStaleState();
    // D2: every failed-Create cleanup goes through this checked path: the
    // original actual failure string is preserved, and a central refusal keeps
    // its own truthful diagnostic instead of claiming retained ownership
    // (central cleanup can refuse after a partial release).
    const auto centralCleanup = [&]() -> bool {
        const std::string original(GetLastErrorCopy());
        if (DestroyXRResources()) return true;
        const std::string detail(GetLastErrorCopy());
        SetErr(original + " | central cleanup refused/pending: " +
               (detail.empty() ? std::string("cleanup refused/pending") : detail));
        return false;
    };
    // HIGHRES-H2-COLOR: freeze the session color contract exactly once here —
    // never reread per frame. Absent/empty/exact "legacy" => false; exact
    // "srgb" => true; every other token fails the Create BEFORE any runtime
    // resource is loaded. An overlong value is invalid (never silently
    // truncated): reject on the first query, read exactly once.
    g_sessionColorSrgb = false; // never inherit an old choice if this Create fails
    {
        wchar_t envBuf[64];
        const DWORD n = GetEnvironmentVariableW(L"VRSIM_RAW_COLORSPACE", envBuf, (DWORD)(sizeof(envBuf) / sizeof(envBuf[0])));
        std::wstring val;
        if (n == 0)
        {
            val.clear(); // absent or empty
        }
        else if (n < (DWORD)(sizeof(envBuf) / sizeof(envBuf[0])))
        {
            val.assign(envBuf, n);
        }
        else
        {
            SetErr("VRSIM_RAW_COLORSPACE is not a valid color contract token (overlong value)");
            SetState(VRSIM_ERROR);
            return -1;
        }
        if (val.empty() || val == L"legacy") g_sessionColorSrgb = false;
        else if (val == L"srgb") g_sessionColorSrgb = true;
        else
        {
            std::string tok;
            const int mb = WideCharToMultiByte(CP_UTF8, 0, val.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (mb > 0) { std::vector<char> s((size_t)mb, 0); WideCharToMultiByte(CP_UTF8, 0, val.c_str(), -1, s.data(), mb, nullptr, nullptr); tok = s.data(); }
            SetErr("VRSIM_RAW_COLORSPACE must be exactly 'legacy' or 'srgb', got '" + tok + "'");
            SetState(VRSIM_ERROR);
            return -1;
        }
    }
    SetState(VRSIM_STARTING);
    g_tBase = (long long)GetTickCount64();
    LogLine("XR", "VRSIM_Create begin pid=" + std::to_string(GetCurrentProcessId()));

    // ---- loader ----
    std::string loaderPath;
    if (!g_loaderPathOverride.empty())
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, g_loaderPathOverride.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
        std::vector<char> mb((size_t)n, 0);
        WideCharToMultiByte(CP_UTF8, 0, g_loaderPathOverride.c_str(), -1,
                            mb.data(), n, nullptr, nullptr);
        loaderPath = mb.data();
    }
    else loaderPath = DiscoverLoader();
    g_loader = LoadLibraryExA(loaderPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!g_loader)
    {
        SetErr("loader load failed: " + loaderPath + " gle=" + std::to_string(GetLastError()));
        SetState(VRSIM_ERROR);
        return -2;
    }
    g_loaderPathUsed = loaderPath;
    g_gpa = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!g_gpa) { SetErr("xrGetInstanceProcAddr not found"); SetState(VRSIM_ERROR); return -2; }

    // xrCreateInstance must be resolved BEFORE ResolveFns (which needs a live instance)
    g_gpa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    if (!xrCreateInstance) { SetErr("xrCreateInstance missing"); SetState(VRSIM_ERROR); return -2; }

    // ---- instance (requests XR_KHR_D3D11_enable + XR_EXT_user_presence when available) ----
    const char* exts[2] = { "XR_KHR_D3D11_enable", nullptr };
    uint32_t extCount = 1;
    {
        uint32_t n = 0;
        PFN_xrEnumerateInstanceExtensionProperties enumProps = nullptr;
        g_gpa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumProps);
        if (enumProps)
        {
            if (enumProps(nullptr, 0, &n, nullptr) == XR_SUCCESS && n > 0)
            {
                std::vector<XrExtensionProperties> props(n);
                for (auto& eprop : props) { eprop.type = XR_TYPE_EXTENSION_PROPERTIES; eprop.next = nullptr; }
                if (enumProps(nullptr, n, &n, props.data()) == XR_SUCCESS)
                {
                    for (const auto& eprop : props)
                        if (strcmp(eprop.extensionName, XR_EXT_USER_PRESENCE_EXTENSION_NAME) == 0)
                        { g_userPresenceAvail = 1; exts[1] = XR_EXT_USER_PRESENCE_EXTENSION_NAME; extCount = 2; break; }
                }
            }
        }
        LogLine("XR", "userPresence extension available=" + std::to_string(g_userPresenceAvail.load()));
    }
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "VRSIM");
    strcpy_s(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "iiQWorks.Sim");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ici.enabledExtensionCount = extCount;
    ici.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ici, &g_inst);
    if (r != XR_SUCCESS)
    {
        SetErr("xrCreateInstance " + XrResultName(r));
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }
    std::string ferr;
    if (!ResolveFns(ferr))
    {
        SetErr(ferr);
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }
    g_tInstanceCreated = (long long)GetTickCount64();

    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    if (xrGetInstanceProperties(g_inst, &ip) == XR_SUCCESS)
    {
        memcpy(g_runtimeName, ip.runtimeName, XR_MAX_RUNTIME_NAME_SIZE);
        g_runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = 0;
        g_apiVersion = XR_VERSION_MAJOR(ip.runtimeVersion) * 1000000u
                     + XR_VERSION_MINOR(ip.runtimeVersion) * 1000u
                     + XR_VERSION_PATCH(ip.runtimeVersion);
    }

    // ---- system ----
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(g_inst, &sgi, &g_sysId);
    if (r != XR_SUCCESS)
    {
        SetErr("xrGetSystem (HMD not available) " + XrResultName(r));
        if (!centralCleanup()) { SetState(VRSIM_ERROR); return -4; } // never an HMD retry on refused cleanup
        SetState(VRSIM_WAITING_FOR_HMD);
        return 1; // caller may retry when HMD wakes
    }
    XrSystemUserPresencePropertiesEXT presenceProps{ XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT };
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    if (g_userPresenceAvail) sp.next = &presenceProps;
    if (xrGetSystemProperties(g_inst, g_sysId, &sp) == XR_SUCCESS)
    {
        memcpy(g_systemName, sp.systemName, XR_MAX_SYSTEM_NAME_SIZE);
        g_systemName[XR_MAX_SYSTEM_NAME_SIZE] = 0;
        if (g_userPresenceAvail)
        {
            g_userPresenceSupported = presenceProps.supportsUserPresence ? 1 : 0;
            LogLine("XR", "userPresence system support=" + std::to_string(g_userPresenceSupported.load()));
        }
    }

    // ---- blend mode ----
    uint32_t bn = 0;
    xrEnumerateEnvironmentBlendModes(g_inst, g_sysId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &bn, &g_blend);

    // ---- D3D11 device (dedicated; BGRA required by swapchains) ----
    // HIGHRES-H2SH-CARD1: strict merge of the reviewed device-runtime-compat
    // candidate block (that file SHA 5C4A19F7…): fail closed on the actual
    // xrGetD3D11Requirements HRESULT, select the EXACT runtime-required LUID via
    // EnumAdapters1, create on THAT adapter with D3D_DRIVER_TYPE_UNKNOWN and the
    // runtime minimum feature level, and allow only the documented E_INVALIDARG
    // single-element 11_0 retry on the SAME adapter when the minimum permits it.
    // No default-adapter selection, no source-inferred LUID, no cross-adapter
    // fallback. The actual runtimeRequiredLuid is frozen in session metadata for
    // the shared bridge's four-way adapter proof.
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XrResult rr = xrGetD3D11Requirements(g_inst, g_sysId, &req);
    LogLine("XR", "xrGetD3D11Requirements rc=" + XrResultName(rr));
    if (rr != XR_SUCCESS)
    {
        SetErr("xrGetD3D11Requirements " + XrResultName(rr));
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }
    LogLine("XR", "runtime D3D11 requirements adapterLuid high="
        + std::to_string((unsigned long long)req.adapterLuid.HighPart)
        + " low=" + std::to_string((unsigned long long)req.adapterLuid.LowPart)
        + " minFeatureLevel=" + std::to_string((unsigned)((D3D_FEATURE_LEVEL)req.minFeatureLevel)));
    g_runtimeRequiredLuid = req.adapterLuid;
    g_runtimeMinFeatureLevel = (D3D_FEATURE_LEVEL)req.minFeatureLevel;
    g_runtimeRequiredLuidValid = true; // frozen session metadata; cleared by DestroyXRResources

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr) || !factory)
    {
        if (factory) factory->Release();
        SetErr("CreateDXGIFactory1 hr=" + std::to_string((unsigned long)hr));
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }

    IDXGIAdapter1* best = nullptr;
    DXGI_ADAPTER_DESC1 bestDesc{};
    for (UINT i = 0; i < 64; ++i)
    {
        IDXGIAdapter1* ad = nullptr;
        HRESULT ehr = factory->EnumAdapters1(i, &ad);
        if (ehr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ehr) || !ad)
        {
            if (ad) ad->Release();
            factory->Release();
            SetErr("DXGI EnumAdapters1 idx=" + std::to_string(i)
                + " hr=" + std::to_string((unsigned long)ehr));
            const bool cleaned = centralCleanup();
            SetState(VRSIM_ERROR);
            return cleaned ? -2 : -4;
        }
        DXGI_ADAPTER_DESC1 d{};
        HRESULT dhr = ad->GetDesc1(&d);
        if (FAILED(dhr))
        {
            ad->Release();
            factory->Release();
            SetErr("IDXGIAdapter1::GetDesc1 idx=" + std::to_string(i)
                + " hr=" + std::to_string((unsigned long)dhr));
            const bool cleaned = centralCleanup();
            SetState(VRSIM_ERROR);
            return cleaned ? -2 : -4;
        }
        const bool match = (d.AdapterLuid.LowPart == req.adapterLuid.LowPart)
            && (d.AdapterLuid.HighPart == req.adapterLuid.HighPart);
        if (match)
        {
            best = ad;      // ownership transferred to D3D11CreateDevice path
            bestDesc = d;
            break;          // matched adapter must be the last reference kept
        }
        ad->Release();      // rejected adapter released exactly once
    }
    factory->Release();     // factory released on every path, exactly once
    if (!best)
    {
        SetErr("DXGI adapter match for runtime LUID not found high="
            + std::to_string((unsigned long long)req.adapterLuid.HighPart)
            + " low=" + std::to_string((unsigned long long)req.adapterLuid.LowPart));
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }
    LogLine("XR", "DXGI adapter matched desc luid high="
        + std::to_string((unsigned long long)bestDesc.AdapterLuid.HighPart)
        + " low=" + std::to_string((unsigned long long)bestDesc.AdapterLuid.LowPart));

    D3D_FEATURE_LEVEL minFL = (D3D_FEATURE_LEVEL)req.minFeatureLevel;
    const D3D_FEATURE_LEVEL baseFL[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL fls[2];
    UINT flCount = 0;
    if (minFL > D3D_FEATURE_LEVEL_11_1)
    {
        fls[flCount++] = minFL;   // request only the actual runtime minimum
    }
    else
    {
        for (UINT i = 0; i < 2; ++i)
            if (baseFL[i] >= minFL) fls[flCount++] = baseFL[i];
    }

    BOOL restrictedRetry = FALSE;
    D3D_FEATURE_LEVEL actual = D3D_FEATURE_LEVEL(0);
    hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, flCount, D3D11_SDK_VERSION,
        &g_dev, &actual, &g_ctx);
    if (FAILED(hr) || !g_dev || !g_ctx)
    {
        // If a partial call wrote globals, drop them safely before retry.
        if (g_dev) { g_dev->Release(); g_dev = nullptr; }
        if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
        if (hr == E_INVALIDARG && minFL <= D3D_FEATURE_LEVEL_11_0)
        {
            // One restricted retry: SAME adapter, single-element 11_0 list.
            const D3D_FEATURE_LEVEL fl110[] = { D3D_FEATURE_LEVEL_11_0 };
            actual = D3D_FEATURE_LEVEL(0);
            hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl110, 1, D3D11_SDK_VERSION,
                &g_dev, &actual, &g_ctx);
            restrictedRetry = TRUE;
        }
    }
    best->Release();        // adapter reference released exactly once
    if (FAILED(hr) || !g_dev || !g_ctx)
    {
        SetErr("D3D11CreateDevice(runtime adapter) hr="
            + std::to_string((unsigned long)hr));
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }

    // Actual feature level proof from the created device itself.
    const D3D_FEATURE_LEVEL got = g_dev->GetFeatureLevel();
    LogLine("XR", "D3D11 device created actualFeatureLevel="
        + std::to_string((unsigned)got)
        + " reportedAtCreate=" + std::to_string((unsigned)actual)
        + " restrictedRetry=" + std::to_string(restrictedRetry ? 1 : 0));
    if (got < minFL || got < D3D_FEATURE_LEVEL_11_0)
    {
        SetErr("D3D11 feature level below runtime requirement got="
            + std::to_string((unsigned)got)
            + " min=" + std::to_string((unsigned)minFL));
        g_dev->Release(); g_dev = nullptr;
        if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }

    // ---- worker owns session creation + frame loop on ONE thread ----
    // D2: the only legitimate g_stopRequested reset is the empty entry grant; a
    // Stop linearized after entry must not be overwritten this late.
    g_initPromise = std::promise<int>();
    std::future<int> initF = g_initPromise.get_future();
    g_worker = std::thread(WorkerMain);
    auto st = initF.wait_for(std::chrono::seconds(20));
    if (st != std::future_status::ready)
    {
        SetErr("session init timeout (20s)");
        g_stopRequested = true;
        if (g_worker.joinable()) g_worker.join();
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }
    const int wrc = initF.get();
    if (wrc != 0)
    {
        if (g_worker.joinable()) g_worker.join();
        const bool cleaned = centralCleanup(); // authoritative even after the join
        SetState(VRSIM_ERROR);
        return cleaned ? wrc : -4;
    }
    LogLine("XR", "VRSIM_Create ok runtime=" + std::string(g_runtimeName) +
            " system=" + std::string(g_systemName) +
            " swap=" + std::to_string(g_swapW) + "x" + std::to_string(g_swapH) +
            " fmt=" + std::to_string(g_swapFormat) +
            " loader=" + g_loaderPathUsed);
    return 0;
}

__declspec(dllexport) int VRSIM_Start()
{
    return (int)g_state.load(); // worker already runs after Create
}

__declspec(dllexport) int VRSIM_Poll()  { return (int)g_state.load(); }
__declspec(dllexport) int VRSIM_GetState() { return (int)g_state.load(); }

__declspec(dllexport) int VRSIM_GetFrameState(VRSIMFrameState* out)
{
    if (!out) return -1;
    std::lock_guard<std::mutex> lk(g_frameMutex);
    *out = g_frameState;
    return 0;
}

__declspec(dllexport) int VRSIM_GetViews(VRSIMEyeView* outL, VRSIMEyeView* outR)
{
    if (!outL || !outR) return -1;
    std::lock_guard<std::mutex> lk(g_frameMutex);
    *outL = g_eyeL;
    *outR = g_eyeR;
    return 0;
}

__declspec(dllexport) int VRSIM_Stop()
{
    LogLine("XR", "VRSIM_Stop requested");
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        if (SharedLeaseOrStateLocked())
        {
            SharedCloseLocked(false);
        }
        else
        {
            g_sharedClosing = true;
            RingSetAdmissionLocked(false);
        }
    }
    DumpCancelPending();
    const int st = g_xrState;
    if (g_sess != XR_NULL_HANDLE &&
        (st == XR_SESSION_STATE_SYNCHRONIZED || st == XR_SESSION_STATE_VISIBLE ||
         st == XR_SESSION_STATE_FOCUSED))
    {
        const XrResult r = xrRequestExitSession(g_sess);
        LogLine("XR", "RequestExitSession " + XrResultName(r));
    }
    SetState(VRSIM_STOPPING);
    return 0;
}

__declspec(dllexport) int VRSIM_Destroy()
{
    // D1: no SharedLegacyResetBlocked() guesswork. One short joint gate closes
    // native entry and snapshots the ACTUAL cached eligibility. Pending returns
    // happen outside the gates and never clear references, join or go OFF.
    bool lease = false;
    bool blocked = false;
    const char* reason = nullptr;
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
        g_stopRequested = true;
        g_sharedClosing = true;
        g_sharedTeardownRequested = true;
        RingSetAdmissionLocked(false);
        lease = SharedLeaseOrStateLocked();
        if (lease) SharedCloseLocked(true); // bridge close only for real lease/state

        VRSIM::SharedBridgeStatsV1 bs{};
        g_sharedBridge.Stats(&bs); // cached, no GPU

        if (g_sharedQuarantined) { blocked = true; reason = "shared session quarantined"; }
        if (!blocked && g_sharedFinalizeBusy) { blocked = true; reason = "shared finalize busy"; }
        if (!blocked && g_sharedSourceActiveCalls != 0) { blocked = true; reason = "source calls active"; }
        for (int i = 0; !blocked && i < RING_SLOTS; ++i)
        {
            if (g_sharedPhase[i] != kSharedPhaseNone)
            { blocked = true; reason = "slot phase not none"; break; }
            if (g_ring[i].mode == kGpuSlotModeSharedV1)
            { blocked = true; reason = "slot in shared mode"; break; }
            const auto st = g_ring[i].state.load();
            if (st == 0) continue;
            // Shared route: ALL native slots must be FREE. Legacy no-lease route:
            // READY(2)/READING(3) are allowed here only so the existing off-UI
            // join below can finish their old worker ownership; WRITING(1) and
            // any unknown state refuse.
            if (lease) { blocked = true; reason = "shared lease slot not free"; break; }
            if (st != 2 && st != 3) { blocked = true; reason = "slot writing/unknown"; break; }
        }
        for (int i = 0; !blocked && i < (int)VRSIM::kStereoBridgeSlotCountV1; ++i)
        {
            if (bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree)
            { blocked = true; reason = "bridge slot not free"; break; }
        }
        if (!blocked && lease)
        {
            // The shared route reaches join only after the worker's proven exit
            // (no source-UI join while that UI is still needed to drain/discard).
            // The original bind epoch/generation are retained, never zeroed here.
            if (!(g_sharedWorkerCleanupDone && g_sharedWorkerNoContext &&
                  g_sharedWorkerDrained && g_sharedSourceFinalized &&
                  g_sharedAdmissionClosed))
            { blocked = true; reason = "shared worker/source cleanup not proven"; }
        }
    }

    LogLine("XR", "VRSIM_Destroy begin");
    if (blocked)
    {
        SetErr(reason);
        LogLine("XR", "VRSIM_Destroy pending (eligibility not proven)");
        return 1;
    }

    DumpCancelPending(); // cancel near-Destroy requests before joining the worker
    if (g_worker.joinable()) g_worker.join();
    // The central joint gate is authoritative and rechecks after the join; this
    // snapshot does not guarantee success. No OFF and no success log on refusal.
    if (!DestroyXRResources())
    {
        const std::string central(GetLastErrorCopy());
        if (central.empty())
            SetErr("central teardown refused/pending; inspect actual lifecycle state");
        LogLine("XR", "VRSIM_Destroy pending (DestroyXRResources refused)");
        return 1;
    }
    SetState(VRSIM_OFF);
    LogLine("XR", "VRSIM_Destroy ok");
    return 0;
}

__declspec(dllexport) const char* VRSIM_GetLastError()
{
    static std::string snap;
    snap = GetLastErrorCopy();
    return snap.c_str();
}

__declspec(dllexport) int VRSIM_GetDiagnostics(char* buf, int cap)
{
    if (!buf || cap <= 0) return -1;
    auto esc = [](const char* s)
    {
        std::string o;
        for (const char* c = s; *c; ++c)
        {
            const unsigned char ch = (unsigned char)*c;
            if (ch == '"' || ch == '\\') continue;
            if (ch < 0x20 || ch > 0x7E) o += '?'; else o += (char)ch;
        }
        return o;
    };
    std::string j = "{";
    j += "\"pid\":" + std::to_string(GetCurrentProcessId());
    j += ",\"state\":" + std::to_string((int)g_state.load());
    const int xrRaw = (int)g_xrState.load();
    j += ",\"xrState\":" + std::to_string(xrRaw);
    j += ",\"xrStateName\":\"" + std::string(SessionStateName((XrSessionState)xrRaw)) + "\"";
    j += ",\"inProcessSessionReachedVisible\":" + std::string(g_reachedVisible ? "true" : "false");
    // ---- PHASE 12/13/14 counters ----
    j += ",\"pollLoopCount\":" + std::to_string(g_pollLoopCount.load());
    j += ",\"sessionReadyEvents\":" + std::to_string(g_sessionReadyEvents.load());
    j += ",\"beginSessionAttempts\":" + std::to_string(g_beginSessionAttempts.load());
    j += ",\"beginSessionSuccess\":" + std::to_string(g_beginSessionSuccess.load());
    j += ",\"waitFrameEntered\":" + std::to_string(g_waitFrameEntered.load());
    j += ",\"waitFrameReturned\":" + std::to_string(g_waitFrameReturned.load());
    j += ",\"waitFrameSuccess\":" + std::to_string(g_waitFrameSuccess.load());
    j += ",\"waitFrameFailed\":" + std::to_string(g_waitFrameFailed.load());
    j += ",\"lastWaitFrameResult\":" + std::to_string(g_lastWaitFrameResult.load());
    const long long enteredAt = g_waitFrameEnteredAtMs.load();
    j += ",\"waitFrameStuckMs\":" + std::to_string(
        enteredAt > 0 ? (long long)GetTickCount64() - enteredAt : 0);
    j += ",\"lastPredictedDisplayTime\":" + std::to_string(g_lastPredictedDisplayTime.load());
    j += ",\"lastPredictedDisplayPeriod\":" + std::to_string(g_lastPredictedDisplayPeriod.load());
    j += ",\"lastShouldRender\":" + std::to_string(g_lastShouldRender.load());
    j += ",\"shouldRenderTrue\":" + std::to_string(g_shouldRenderTrue.load());
    j += ",\"shouldRenderFalse\":" + std::to_string(g_shouldRenderFalse.load());
    j += ",\"beginFrameAttempts\":" + std::to_string(g_beginFrameAttempts.load());
    j += ",\"beginFrameSuccess\":" + std::to_string(g_beginFrameOk.load());
    j += ",\"beginFrameFailed\":" + std::to_string(g_beginFrameFailed.load());
    j += ",\"zeroLayerEndFrames\":" + std::to_string(g_zeroLayerEndFrames.load());
    j += ",\"quadLayerAttempts\":" + std::to_string(g_quadLayerAttempts.load());
    j += ",\"quadFillFail\":" + std::to_string(g_quadFillFail.load());
    j += ",\"lastEndFrameResult\":" + std::to_string(g_lastEndFrameResult.load());
    // ---- PHASE 19 timeline (relative ms from VRSIM_Create entry; -1 = not reached) ----
    const long long base = g_tBase.load();
    auto rel = [&](const std::atomic<long long>& a)
    {
        const long long v = a.load();
        return v == 0 ? -1 : v - base;
    };
    j += ",\"timelineMs\":{"
        "\"t0WorkerStarted\":" + std::to_string(rel(g_tWorkerStarted)) +
        ",\"t1InstanceCreated\":" + std::to_string(rel(g_tInstanceCreated)) +
        ",\"t2SessionCreated\":" + std::to_string(rel(g_tSessionCreated)) +
        ",\"t3ReadyEvent\":" + std::to_string(rel(g_tReadyEvent)) +
        ",\"t4BeginSessionEntered\":" + std::to_string(rel(g_tBeginSessionEntered)) +
        ",\"t5BeginSessionReturned\":" + std::to_string(rel(g_tBeginSessionReturned)) +
        ",\"t6FirstWaitFrameEntered\":" + std::to_string(rel(g_tFirstWaitFrameEntered)) +
        ",\"t7FirstWaitFrameReturned\":" + std::to_string(rel(g_tFirstWaitFrameReturned)) +
        ",\"t8FirstBeginFrameReturned\":" + std::to_string(rel(g_tFirstBeginFrameReturned)) +
        ",\"t9FirstEndFrameReturned\":" + std::to_string(rel(g_tFirstEndFrameReturned)) +
        ",\"t10SynchronizedEvent\":" + std::to_string(rel(g_tSynchronizedEvent)) +
        ",\"t11VisibleEvent\":" + std::to_string(rel(g_tVisibleEvent)) +
        ",\"t12FirstShouldRenderTrue\":" + std::to_string(rel(g_tFirstShouldRenderTrue)) +
        ",\"t13FirstQuadEndFrameOk\":" + std::to_string(rel(g_tFirstQuadEndFrameOk)) +
        "}";
    j += ",\"runtimeName\":\"" + esc(g_runtimeName) + "\"";
    j += ",\"systemName\":\"" + esc(g_systemName) + "\"";
    j += ",\"runtimeVersion\":" + std::to_string(g_apiVersion);
    j += ",\"swapWidth\":" + std::to_string(g_swapW);
    j += ",\"swapHeight\":" + std::to_string(g_swapH);
    j += ",\"swapFormat\":" + std::to_string(g_swapFormat);
    j += ",\"blendMode\":" + std::to_string((int)g_blend);
    j += ",\"beginFrames\":" + std::to_string(g_beginFrameOk.load());
    j += ",\"quadOk\":" + std::to_string(g_quadOk.load());
    j += ",\"quadFail\":" + std::to_string(g_quadFail.load());
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        j += ",\"frameSerial\":" + std::to_string(g_frameState.frameSerial);
        j += ",\"shouldRender\":" + std::to_string(g_frameState.shouldRender);
        j += ",\"trackingValid\":" + std::to_string(g_frameState.trackingValid);
        // IPD from live eye positions (mm), 0 if not tracked yet
        const double dx = g_eyeR.posMm[0] - g_eyeL.posMm[0];
        const double dy = g_eyeR.posMm[1] - g_eyeL.posMm[1];
        const double dz = g_eyeR.posMm[2] - g_eyeL.posMm[2];
        j += ",\"ipdMm\":" + std::to_string(std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    j += ",\"loaderPath\":\"" + esc(g_loaderPathUsed.c_str()) + "\"";
    {
        std::lock_guard<std::mutex> lk(g_errMutex);
        j += ",\"lastError\":\"" + esc(g_lastError.c_str()) + "\"";
    }
    j += "}";
    const int need = (int)j.size() + 1;
    if (need > cap) { buf[0] = 0; return -need; }
    memcpy(buf, j.c_str(), (size_t)need);
    return (int)j.size();
}

} // extern "C"
