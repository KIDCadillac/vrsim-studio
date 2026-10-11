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
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
// D11 lease-owned output GPU timing observer: DXGI_FORMAT constants only
// (no new macro, no ABI/wire change; comment-metadata CSV output only).
#include <dxgiformat.h>
#endif
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
#include "RuntimeDeviceCapabilities.h" // DEVICE-CAPS-01: pure capability policy shared verbatim with the qa fixtures

// OUTPUT4096 N2: the QA output-4096 symbol is only meaningful on top of the
// full mixed-source QA stack; an incomplete combination is a compile error.
#if defined(VRSIM_QA_OUTPUT_4096)
#if !defined(VRSIM_SHARED_3072) || !defined(VRSIM_QA_MIXED_SOURCE) || \
    !defined(VRSIM_SOURCE_4096_QA) || !defined(VRSIM_QA_D9_BEGIN_POLL)
#error "VRSIM_QA_OUTPUT_4096 requires VRSIM_SHARED_3072, VRSIM_QA_MIXED_SOURCE, VRSIM_SOURCE_4096_QA and VRSIM_QA_D9_BEGIN_POLL"
#endif
#endif

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
// DEVICE-CAPS-01: authoritative PER-EYE output extents for the projection
// subImage. Worker-owned like g_swapW/H (written only by SessionInitPhase and
// WorkerApplySceneMode on the single XR worker, read at submission on that same
// thread). Legacy g_swapW/H remain the left-eye scene payload contract
// (WorkerConsumeLatestFrame / logs / ABI) but are NOT the authority for the
// right-eye subImage: an asymmetric runtime recommendation can make a
// left-extent right subImage invalid.
uint32_t                  g_eyeOutW[2] = { 0, 0 };
uint32_t                  g_eyeOutH[2] = { 0, 0 };
// DEVICE-CAPS-01: per-session runtime device capability cache. Filled ONLY from
// actual successful queries (xrGetSystemProperties for the system max on the
// Create thread before the worker exists; xrEnumerateViewConfigurationViews for
// both eyes on the worker). Cleared at CreateResetStaleState and on the actual
// non-refused DestroyXRResources path so old runtime capabilities can never
// admit a new runtime. Lifecycle-serialized like g_sessionColorSrgb: never
// read/written across threads while a worker runs.
VRSIMCaps::DeviceCapsV1   g_deviceCaps{};
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

// ---- HIGHRES-H2-DIAG-FB: XR_FB_color_space OBSERVATION + bounded opt-in -----
// Diagnostics never switch color by themselves: the official PFN types are used,
// xrSetColorSpaceFB is resolved/called ONLY under the exact frozen
// VRSIM_XR_COLORSPACE=rec709 opt-in (see REC709 block below); no gamma/
// saturation/luminance/whitepoint correction; the existing raw sRGB / SRV93 /
// RTV91 / swap91 contract is untouched. Every fact is an atomic primitive or
// fixed static text: the XR worker may publish while a JSON reader reads, with
// no std::string/vector ever shared across threads.
const int kDiagMaxColorSpaces = 64;
PFN_xrEnumerateColorSpacesFB xrEnumerateColorSpacesFB = nullptr; // nullable/optional; never a mandatory RES entry
// frozen diagnostic selection: 0 absent/legacy | 1 exact 'obs-shared' | 2 invalid token (never fatal)
std::atomic<int>          g_diagFbPipelineSelected{ 0 };
// instance extension enumeration facts (a failed enumeration is unknown, never unsupported)
std::atomic<int>          g_diagFbExtEnumRc{ 0 };         // actual count-call XrResult
std::atomic<int>          g_diagFbExtEnumRcValid{ 0 };
std::atomic<int>          g_diagFbExtEnumDataRc{ 0 };     // actual data-call XrResult (recorded separately)
std::atomic<int>          g_diagFbExtEnumDataRcValid{ 0 };
std::atomic<int>          g_diagFbAvailable{ 0 };
std::atomic<int>          g_diagFbAvailableValid{ 0 };
std::atomic<int>          g_diagFbEnabled{ 0 };
// GetProcAddr facts for the enumeration function
std::atomic<int>          g_diagFbGetProcRc{ 0 };
std::atomic<int>          g_diagFbGetProcRcValid{ 0 };
std::atomic<int>          g_diagFbFunctionPresent{ 0 };
// device NATIVE gamut from system properties — NOT the app/session colorspace
std::atomic<int>          g_diagNativeColorSpace{ -1 };
std::atomic<int>          g_diagNativeColorSpaceValid{ 0 };
// session supported colorspace two-call enumeration (once after a successful xrCreateSession)
std::atomic<int>          g_diagColorCountRc{ 0 };
std::atomic<int>          g_diagColorCountRcValid{ 0 };
std::atomic<uint32_t>     g_diagColorCountOutput{ 0 };    // actual count-call countOutput, kept even when > capacity
std::atomic<int>          g_diagColorDataRc{ 0 };
std::atomic<int>          g_diagColorDataRcValid{ 0 };
std::atomic<uint32_t>     g_diagSupportedCount{ 0 };
std::atomic<int>          g_diagSupportedListValid{ 0 };
std::atomic<int>          g_diagSupportedColorSpaces[kDiagMaxColorSpaces];
// ---- HIGHRES-H2-REC709 OPT-IN (VRSIM_XR_COLORSPACE=rec709, obs-shared only) --
// The official Set PFN is resolved ONLY when FB was actually enabled AND the
// exact lowercase token was frozen; it is invoked at most once by the sole XR
// worker. Absent/other tokens and legacy paths never resolve or call it.
// Nothing here touches gamma/shader/format91; a failed/unavailable Set keeps
// appColorSpace=RUNTIME_SELECTED_UNKNOWN with an honest skip reason.
PFN_xrSetColorSpaceFB     xrSetColorSpaceFB = nullptr; // nullable/optional; never a mandatory RES entry
std::atomic<int>          g_diagSetRequested{ 0 };     // frozen token: 0 none/absent | 1 exact 'rec709' | 2 invalid (never fatal)
std::atomic<int>          g_diagSetGetProcRc{ 0 };     // actual GPA rc for xrSetColorSpaceFB
std::atomic<int>          g_diagSetGetProcRcValid{ 0 };
std::atomic<int>          g_diagSetFunctionPresent{ 0 };
std::atomic<int>          g_diagSetAttempted{ 0 };     // 1 only after the single real invocation
std::atomic<int>          g_diagSetResult{ 0 };        // actual XrResult of that single call
std::atomic<int>          g_diagSetResultValid{ 0 };
std::atomic<int>          g_diagAppliedColorSpace{ -1 }; // REC709(2) ONLY on exact XR_SUCCESS
std::atomic<int>          g_diagAppliedValid{ 0 };
std::atomic<int>          g_diagSetSkipReason{ 0 };    // honest reason when no Set was made

const char* FbSetSkipReasonName(int r)
{
    switch (r)
    {
    case 0: return "NONE";
    case 1: return "NO_REC709_REQUEST";
    case 2: return "EXTENSION_NOT_ENABLED";
    case 3: return "SET_FUNCTION_MISSING";
    case 4: return "SUPPORTED_ENUMERATION_NOT_VALID";
    case 5: return "REC709_NOT_IN_SUPPORTED_LIST";
    case 6: return "SET_ATTEMPT_FAILED_RC_IN_setResult";
    case 7: return "INVALID_REC709_REQUEST";
    case 8: return "READBACK_PIPELINE_NOT_OBS_SHARED";
    case 9: return "SOURCE_CONTRACT_NOT_SRGB";
    default: return "UNKNOWN";
    }
}

const char* FbColorSpaceName(int cs)
{
    switch (cs)
    {
    case XR_COLOR_SPACE_UNMANAGED_FB:  return "XR_COLOR_SPACE_UNMANAGED_FB";
    case XR_COLOR_SPACE_REC2020_FB:    return "XR_COLOR_SPACE_REC2020_FB";
    case XR_COLOR_SPACE_REC709_FB:     return "XR_COLOR_SPACE_REC709_FB";
    case XR_COLOR_SPACE_RIFT_CV1_FB:   return "XR_COLOR_SPACE_RIFT_CV1_FB";
    case XR_COLOR_SPACE_RIFT_S_FB:     return "XR_COLOR_SPACE_RIFT_S_FB";
    case XR_COLOR_SPACE_QUEST_FB:      return "XR_COLOR_SPACE_QUEST_FB";
    case XR_COLOR_SPACE_P3_FB:         return "XR_COLOR_SPACE_P3_FB";
    case XR_COLOR_SPACE_ADOBE_RGB_FB:  return "XR_COLOR_SPACE_ADOBE_RGB_FB";
    default:                           return "XR_COLOR_SPACE_FB_UNKNOWN";
    }
}

// ---- committed diagnostics snapshot (private POD, no ABI/export change) ----
// One coherent whole image of every FB fact plus the bounded 64-value list.
// Writers publish only at COMPLETED stage boundaries (extension scan done /
// GPA result done / system-property result done / worker enumeration done);
// those stages are serialized on this path: the caller thread runs the first
// three before any worker exists, the worker runs the last while VRSIM_Create
// is parked on the init promise, and Reset runs only at the safe fresh-Create
// point or the real post-join cleanup. GetDiagnostics copies the ENTIRE image
// inside one short dedicated gate and builds all JSON/strings OUTSIDE it; it
// never reads the per-atomic working fields again. The gate is held only for
// the pure POD copy: no XR/GPA call, no LogLine, no JSON/string, no allocation
// under it, and never the ring/meta gates.
struct FbColorSpaceDiagSnapshot
{
    int pipelineSelected;
    int extCountRc;         int extCountRcValid;
    int extDataRc;          int extDataRcValid;
    int available;          int availableValid;           int enabled;
    int getProcRc;          int getProcRcValid;           int functionPresent;
    int nativeColorSpace;   int nativeColorSpaceValid;
    int supportedCountRc;   int supportedCountRcValid;
    unsigned int supportedCountOutput;
    int supportedDataRc;    int supportedDataRcValid;
    unsigned int supportedCount;
    int supportedListValid;
    // REC709 opt-in facts (request / Set PFN / single attempt / result / applied)
    int setRequested;
    int setGetProcRc;       int setGetProcRcValid;    int setFunctionPresent;
    int setAttempted;
    int setResult;          int setResultValid;
    int appliedColorSpace;  int appliedValid;
    int setSkipReason;
    int supportedColorSpaces[kDiagMaxColorSpaces];
};

std::mutex g_diagSnapMutex; // short pure-copy gate, separate from lifecycle gates

FbColorSpaceDiagSnapshot DefaultFbDiagSnapshot()
{
    FbColorSpaceDiagSnapshot s{};
    s.nativeColorSpace = -1;
    s.appliedColorSpace = -1;
    for (int i = 0; i < kDiagMaxColorSpaces; ++i) s.supportedColorSpaces[i] = -1;
    return s;
}

FbColorSpaceDiagSnapshot g_diagCommitted{ DefaultFbDiagSnapshot() };

// Builds the complete image from the atomic working fields OUTSIDE the gate
// (every payload+valid pair and the count+list belong to this ONE copy; list
// bounded to 64; a list copy happens only while listValid, otherwise cleared
// to -1), then replaces the committed image exactly once inside the gate.
void PublishFbDiagnosticsSnapshot()
{
    FbColorSpaceDiagSnapshot s{};
    s.pipelineSelected = g_diagFbPipelineSelected.load();
    s.extCountRc = g_diagFbExtEnumRc.load();
    s.extCountRcValid = g_diagFbExtEnumRcValid.load();
    s.extDataRc = g_diagFbExtEnumDataRc.load();
    s.extDataRcValid = g_diagFbExtEnumDataRcValid.load();
    s.available = g_diagFbAvailable.load();
    s.availableValid = g_diagFbAvailableValid.load();
    s.enabled = g_diagFbEnabled.load();
    s.getProcRc = g_diagFbGetProcRc.load();
    s.getProcRcValid = g_diagFbGetProcRcValid.load();
    s.functionPresent = g_diagFbFunctionPresent.load();
    s.nativeColorSpace = g_diagNativeColorSpace.load();
    s.nativeColorSpaceValid = g_diagNativeColorSpaceValid.load();
    s.supportedCountRc = g_diagColorCountRc.load();
    s.supportedCountRcValid = g_diagColorCountRcValid.load();
    s.supportedCountOutput = g_diagColorCountOutput.load();
    s.supportedDataRc = g_diagColorDataRc.load();
    s.supportedDataRcValid = g_diagColorDataRcValid.load();
    s.setRequested = g_diagSetRequested.load();
    s.setGetProcRc = g_diagSetGetProcRc.load();
    s.setGetProcRcValid = g_diagSetGetProcRcValid.load();
    s.setFunctionPresent = g_diagSetFunctionPresent.load();
    s.setAttempted = g_diagSetAttempted.load();
    s.setResult = g_diagSetResult.load();
    s.setResultValid = g_diagSetResultValid.load();
    s.appliedColorSpace = g_diagAppliedColorSpace.load();
    s.appliedValid = g_diagAppliedValid.load();
    s.setSkipReason = g_diagSetSkipReason.load();
    const unsigned int rawCount = (unsigned int)g_diagSupportedCount.load();
    s.supportedCount = rawCount;
    s.supportedListValid = g_diagSupportedListValid.load();
    if (s.supportedListValid)
    {
        const unsigned int lim = rawCount < (unsigned int)kDiagMaxColorSpaces
            ? rawCount : (unsigned int)kDiagMaxColorSpaces;
        for (unsigned int i = 0; i < lim; ++i) s.supportedColorSpaces[i] = g_diagSupportedColorSpaces[i].load();
        for (unsigned int i = lim; i < (unsigned int)kDiagMaxColorSpaces; ++i) s.supportedColorSpaces[i] = -1;
    }
    else
    {
        for (int i = 0; i < kDiagMaxColorSpaces; ++i) s.supportedColorSpaces[i] = -1;
    }
    std::lock_guard<std::mutex> lk(g_diagSnapMutex);
    g_diagCommitted = s; // one full committed replacement
}

// Reset only at the safe serialized fresh-Create point and after an ACTUAL
// non-refused cleanup; a retained refused cleanup never resets this.
void ResetFbColorSpaceDiagnostics()
{
    g_diagFbPipelineSelected = 0;
    g_diagFbExtEnumRc = 0;
    g_diagFbExtEnumRcValid = 0;
    g_diagFbExtEnumDataRc = 0;
    g_diagFbExtEnumDataRcValid = 0;
    g_diagFbAvailable = 0;
    g_diagFbAvailableValid = 0;
    g_diagFbEnabled = 0;
    g_diagFbGetProcRc = 0;
    g_diagFbGetProcRcValid = 0;
    g_diagFbFunctionPresent = 0;
    g_diagNativeColorSpace = -1;
    g_diagNativeColorSpaceValid = 0;
    g_diagColorCountRc = 0;
    g_diagColorCountRcValid = 0;
    g_diagColorCountOutput = 0;
    g_diagColorDataRc = 0;
    g_diagColorDataRcValid = 0;
    g_diagSupportedCount = 0;
    g_diagSupportedListValid = 0;
    for (int i = 0; i < kDiagMaxColorSpaces; ++i) g_diagSupportedColorSpaces[i] = -1;
    xrEnumerateColorSpacesFB = nullptr;
    g_diagSetRequested = 0;
    g_diagSetGetProcRc = 0;
    g_diagSetGetProcRcValid = 0;
    g_diagSetFunctionPresent = 0;
    g_diagSetAttempted = 0;
    g_diagSetResult = 0;
    g_diagSetResultValid = 0;
    g_diagAppliedColorSpace = -1;
    g_diagAppliedValid = 0;
    g_diagSetSkipReason = 0;
    xrSetColorSpaceFB = nullptr;
    // the committed image reverts to the full unknown/default snapshot in ONE
    // replacement (short POD-copy gate only; reset points are already serialized)
    {
        std::lock_guard<std::mutex> lk(g_diagSnapMutex);
        g_diagCommitted = DefaultFbDiagSnapshot();
    }
}

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
struct PoseSample { unsigned long long serial; XrPosef pl, pr; XrFovf fl, fr; bool valid;
#if defined(VRSIM_QA_FRAME_TRACE)
    // QA-trace only: the ORIGINAL locate-time facts of this exact successful
    // sample (actual predicted target + the real xrLocateViews QPC window).
    // Never a fabricated serial*period target. Signed XrTime/XrDuration ns is
    // preserved as-is. Ordinary builds: struct unchanged.
    long long qaDisplayTime; long long qaDisplayPeriod;
    long long qaLocateBeginQpc; long long qaLocateEndQpc;
#endif
};
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

#if defined(VRSIM_QA_FRAME_TRACE)
// ============================================================================
// VRSIM_QA_FRAME_TRACE — bounded evidence ONLY. Zero change to rendering,
// lifecycle, ownership or color behavior: the ordinary build without this
// define compiles NONE of this block. Runtime stays default-OFF until
// VRSIM_QA_FRAME_TRACE_FILE names a fresh absolute local .csv path; the path
// is frozen and every buffer reset EXACTLY ONCE per successful-eligibility
// VRSIM_Create before worker creation (never on a refused Create/Destroy).
// The worker writes fixed POD rows into a preallocated array: no heap
// growth, no I/O, no extra lock per frame; overflow is counted honestly and
// means the capture is INCOMPLETE, never a PASS. Domains: QPC ticks vs XrTime
// nanoseconds are recorded separately and NEVER subtracted across domains;
// nothing here is GPU time or motion-to-photon.
// ============================================================================
struct QaFrameRowV1
{
    int kind;                // 0 endframe (every ACTUAL xrEndFrame attempt), 1 wait_failure, 2 begin_failure
    int result;              // the ACTUAL XrResult of that call (no invented layer)
    int layerCount;          // ACTUAL submitted layer count (failure rows: 0)
    int shouldRender;        // actual fs.shouldRender at this point
    int trackingValid;       // actual viewsValid (position+orientation valid bits)
    int haveLayerPose;       // actual g_haveLayerPose at submission time
    int activeSourceGpu;     // actual g_activeSourceGpu at submission time
    int sourceKind;          // 0 none, 1 shared, 2 legacy(cpu/raw-live pair), 3 synthetic
    int sharedIdentityValid; // 1 only when the submitted scene genuinely carries a
                             // coherent shared commit identity (never a stale one)
    int contentClass;        // 0 n/a (ALWAYS for failures/zero-layer), 1 new, 2 reused —
                             // filled/updated ONLY on r==XR_SUCCESS && layerCount>0
    unsigned long long currentLocatedPoseSerial; // ACTUAL g_poseSerial when views located this
                             // frame (0 when this frame located nothing)
    unsigned long long frameSerial;        // worker runtime serial (BeginFrame-success count)
    unsigned long long activeFrameSerial;  // consumed content serial (id.frameSerial when shared)
    unsigned long long layerPoseSerial;    // g_lastLayerPoseSerial actually in the layer
    long long targetDisplayTimeNs;   // THIS call's fs.predictedDisplayTime (signed XrTime ns)
    long long targetDisplayPeriodNs; // THIS call's fs.predictedDisplayPeriod (signed XrDuration ns)
    unsigned long long idEpoch;
    unsigned long long idGeneration;
    long long sampleDisplayTimeNs;   // ORIGINAL PoseSample target at locate time (signed XrTime ns)
    long long sampleDisplayPeriodNs; // ORIGINAL PoseSample period (signed XrDuration ns)
    long long callBeginQpc;    // actual xrWait/xrBegin/xrEndFrame call window (QPC ticks)
    long long callEndQpc;
    long long consumeBeginQpc; // actual WorkerConsumeSceneFrame window (QPC ticks)
    long long consumeEndQpc;
    long long waitBeginQpc;    // THIS frame's actual SUCCESSFUL xrWaitFrame window (QPC ticks)
    long long waitEndQpc;
    long long beginBeginQpc;   // THIS frame's actual SUCCESSFUL xrBeginFrame window (QPC ticks)
    long long beginEndQpc;
    long long sampleLocateBeginQpc; // original xrLocateViews window of the cached pose (QPC ticks)
    long long sampleLocateEndQpc;
    long long idPoseBeginQpc;  // shared identity's original pose-sample window
    long long idPoseEndQpc;
    long long idCaptureBeginQpc; // shared identity's capture window (source side)
    long long idCaptureEndQpc;
    double curL[7];  // current located left  pose (x y z qx qy qz qw)
    double curR[7];  // current located right pose
    double layL[7];  // ACTUAL pv[0].pose frozen immediately BEFORE xrEndFrame (zero when layerCount==0)
    double layR[7];  // ACTUAL pv[1].pose frozen immediately BEFORE xrEndFrame (zero when layerCount==0)
};
static const uint32_t kQaTraceCapacity = 32768u; // fixed bounded capacity: 120 s 120 Hz measurement + startup/warmup + teardown (about 273 s at 120 Hz); rows beyond capacity still counted as overflow and the capture stays INCOMPLETE
static QaFrameRowV1 g_qaTraceRows[kQaTraceCapacity];
static uint32_t g_qaTraceCount = 0;
static uint32_t g_qaTraceOverflow = 0;
static bool g_qaTraceEnabled = false;
static std::wstring g_qaTracePath;
static long long g_qaTraceQpcFreq = 0;
static long long g_qaTraceStartQpc = 0;
static long long g_qaTraceEndQpc = 0;
// worker-only trace cache: the COMPLETE identity + the EXACT matching
// PoseSample, copied BY VALUE inside the successful shared commit. Never a
// pointer into the ring, never a later arbitrary Stats re-snapshot.
static VRSIM::SharedCaptureIdentityV1 g_qaTraceSharedId{};
static PoseSample g_qaTraceSharedPose{};
static bool g_qaTraceSharedValid = false;
// last SUCCESSFUL delivered identity (worker-only; reused-vs-new authority)
static int g_qaTraceLastOkKind = -1;
static unsigned long long g_qaTraceLastOkFrame = 0;
static unsigned long long g_qaTraceLastOkPose = 0;

long long QaTraceQpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (long long)t.QuadPart; }

std::string QaWToU8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int mb = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (mb <= 0) return std::string();
    std::string s((size_t)mb, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], mb, nullptr, nullptr);
    return s;
}

void QaTraceAddRow(const QaFrameRowV1& row)
{
    if (!g_qaTraceEnabled) return;
    if (g_qaTraceCount < kQaTraceCapacity) g_qaTraceRows[g_qaTraceCount++] = row;
    else ++g_qaTraceOverflow; // honest: rows beyond capacity are LOST, capture incomplete
}

// Frozen once per Create AFTER the existing eligibility gates passed and
// BEFORE the worker exists. Trace stays OFF unless every rule below holds;
// refusals only log a reason — no existing gate or error return is touched.
void QaTraceFreezeAndResetBody()
{
    g_qaTraceEnabled = false;
    g_qaTracePath.clear();
    g_qaTraceCount = 0; g_qaTraceOverflow = 0;
    g_qaTraceSharedValid = false;
    g_qaTraceSharedId = VRSIM::SharedCaptureIdentityV1{};
    g_qaTraceSharedPose = PoseSample{};
    g_qaTraceLastOkKind = -1; g_qaTraceLastOkFrame = 0; g_qaTraceLastOkPose = 0;
    g_qaTraceStartQpc = 0; g_qaTraceEndQpc = 0;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    g_qaTraceQpcFreq = (long long)fq.QuadPart;
    wchar_t buf[512];
    const DWORD n = GetEnvironmentVariableW(L"VRSIM_QA_FRAME_TRACE_FILE", buf,
                                            (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return; // absent/empty: default OFF, silent
    if (n >= (DWORD)(sizeof(buf) / sizeof(buf[0])))
    {
        LogLine("TRACE", "OFF: VRSIM_QA_FRAME_TRACE_FILE overlong (never truncated)");
        return;
    }
    const std::wstring p(buf, n);
    const wchar_t* why = nullptr;
    if (p.size() < 6) why = L"path too short";
    else if (p[0] == L'\\') why = L"UNC or device path rejected";
    else
    {
        const wchar_t d0 = p[0];
        const bool drive = ((d0 >= L'A' && d0 <= L'Z') || (d0 >= L'a' && d0 <= L'z')) &&
                           p[1] == L':' && p[2] == L'\\';
        if (!drive) why = L"not an absolute local drive path";
        else if (p.find(L"..") != std::wstring::npos) why = L"path contains '..'";
        else
        {
            wchar_t e3 = p[p.size() - 3], e2 = p[p.size() - 2], e1 = p[p.size() - 1];
            if (e3 >= L'A' && e3 <= L'Z') e3 = (wchar_t)(e3 + 32);
            if (e2 >= L'A' && e2 <= L'Z') e2 = (wchar_t)(e2 + 32);
            if (e1 >= L'A' && e1 <= L'Z') e1 = (wchar_t)(e1 + 32);
            if (p[p.size() - 4] != L'.' || e3 != L'c' || e2 != L's' || e1 != L'v')
                why = L"suffix is not .csv";
        }
    }
    if (!why)
    {
        const size_t s = p.rfind(L'\\');
        std::wstring parent = p.substr(0, s);
        if (parent.size() == 2) parent += L"\\"; // drive root ("C:\\file.csv")
        const DWORD patt = GetFileAttributesW(parent.c_str());
        if (patt == INVALID_FILE_ATTRIBUTES || !(patt & FILE_ATTRIBUTE_DIRECTORY))
            why = L"parent directory does not exist";
        else if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            why = L"destination already exists (fresh file required)";
    }
    if (why)
    {
        LogLine("TRACE", std::string("OFF: ") + QaWToU8(why) +
                " path='" + QaWToU8(p) + "' (Unicode env/path APIs; no directories created)");
        return;
    }
    g_qaTracePath = p;
    g_qaTraceEnabled = true;
    g_qaTraceStartQpc = QaTraceQpcNow();
    LogLine("TRACE", "armed once for this session; capacity=" +
            std::to_string((unsigned long long)kQaTraceCapacity) +
            " qpcFrequency=" + std::to_string(g_qaTraceQpcFreq) +
            " (export only after a fully successful VRSIM_Destroy; default-OFF otherwise)");
}

// P1 diagnostic boundary: a trace failure may NEVER throw into VRSIM_Create.
// The outermost catch keeps only no-throw fallbacks; a freeze failure means
// the trace is disabled and nothing else.
void QaTraceFreezeAndReset() noexcept
{
    try { QaTraceFreezeAndResetBody(); }
    catch (...)
    {
        g_qaTraceEnabled = false;
        try { LogLine("TRACE", "freeze threw: trace disabled for this session (diagnosticFAIL)"); }
        catch (...) { OutputDebugStringW(L"VRSIM QA frame trace: freeze failed; trace disabled\n"); }
    }
}

static const char* kQaTraceCsvHeader =
    "kind,kindText,frameSerial,callBeginQpc,callEndQpc,result,layerCount,shouldRender,"
    "trackingValid,haveLayerPose,activeSourceGpu,sourceKind,sharedIdentityValid,contentClass,"
    "currentLocatedPoseSerial,"
    "activeFrameSerial,layerPoseSerial,targetDisplayTimeNs,targetDisplayPeriodNs,idEpoch,idGeneration,"
    "sampleDisplayTimeNs,sampleDisplayPeriodNs,consumeBeginQpc,consumeEndQpc,"
    "waitBeginQpc,waitEndQpc,beginBeginQpc,beginEndQpc,"
    "sampleLocateBeginQpc,sampleLocateEndQpc,idPoseBeginQpc,idPoseEndQpc,idCaptureBeginQpc,idCaptureEndQpc,"
    "curLx,curLy,curLz,curLqx,curLqy,curLqz,curLqw,"
    "curRx,curRy,curRz,curRqx,curRqy,curRqz,curRqw,"
    "layLx,layLy,layLz,layLqx,layLqy,layLqz,layLqw,"
    "layRx,layRy,layRz,layRqx,layRqy,layRqz,layRqw\n";

#if defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING)
// Forward decl for the D9 GPU timing observer comment metadata. It MUST live
// at the SAME textual scope as QaTraceExportCsvBody (the outer unnamed
// namespace stays open; see the CARD2B forward-decl note below): declaring it
// in a different nested scope would create a distinct, never-defined entity
// (LNK2019). Pure cached serialization: no C export, no PInvoke, no ABI.
static std::string QaD9GpuTimingMetaLines();
#endif
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
// D11 lease-owned output GPU timing observer — SAME textual-scope rule as the
// D9 forward decl above (unnamed namespace stays open; a definition at a
// different nested scope is a distinct entity). Pure cached serialization and
// no-throw lifecycle hooks: no C export, no PInvoke, no ABI/wire change.
static void QaD11GpuTimingFreezeAndReset() noexcept;
static std::string QaD11GpuTimingMetaLines();
static void QaD11GpuTimingWorkerInit(ID3D11Device* dev, ID3D11DeviceContext* ctx) noexcept;
struct QaD11GpuObsHandleV1; // defined with the observer POD below (same scope)
static void QaD11GpuTimingClose(const QaD11GpuObsHandleV1& h, long long renderHr,
                                bool renderReturned, bool renderThrew);
static void QaD11GpuTimingResolveOne() noexcept; // R4: ONE GetData per call; the normal-loop callsite is R5
static void QaD11GpuTimingStamp(const QaD11GpuObsHandleV1& h, long long ehr,
                                bool endReadReturned, bool endReadThrew) noexcept;
static void QaD11GpuTimingFinalize() noexcept;
#endif
// Runs EXACTLY ONCE, only on the VRSIM_Destroy path that already joined the
// worker, got DestroyXRResources()==true and set state OFF. It can never
// change Destroy's return value; a failed/partial export is diagnosticFAIL.
// All allocations complete BEFORE the handle is opened; CloseHandle runs on
// every opened-handle path; result logging happens after close inside a
// nested no-throw guard.
void QaTraceExportCsvBody()
{
    if (!g_qaTraceEnabled) return;
    g_qaTraceEnabled = false; // one-shot: no further rows, no second export
    g_qaTraceEndQpc = QaTraceQpcNow();
    const uint32_t rows = g_qaTraceCount;
    const uint32_t overflow = g_qaTraceOverflow;
    DWORD gle = 0;
    bool ok = false;
    try
    {
        std::string out;
        out += "#schema=1\n";
        out += "#define=VRSIM_QA_FRAME_TRACE\n";
        out += "#capacity=" + std::to_string((unsigned long long)kQaTraceCapacity) + "\n";
        out += "#rowCount=" + std::to_string((unsigned long long)rows) + "\n";
        out += "#overflowRows=" + std::to_string((unsigned long long)overflow) + "\n";
        out += "#incomplete=" + std::string(overflow > 0 ? "true" : "false") + "\n";
        out += "#qpcFrequency=" + std::to_string(g_qaTraceQpcFreq) + "\n";
        out += "#traceStartQpcTicks=" + std::to_string(g_qaTraceStartQpc) + "\n";
        out += "#traceEndQpcTicks=" + std::to_string(g_qaTraceEndQpc) + "\n";
        out += "#clockDomains=QpcTicks for call/wait/begin/consume/locate/capture windows; signed XrTimeNanoseconds only for *Ns columns; NEVER subtract across domains; not GPU time, not motion-to-photon\n";
        out += "#kindLegend=endframe|wait_failure|begin_failure sourceKind=0none|1shared|2legacy|3synthetic contentClass=0na|1new|2reused(vs last SUCCESSFUL delivery, r==XR_SUCCESS&&layerCount>0)\n";
#if defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING)
        // Comment metadata ONLY, before the ORIGINAL header: the 63 columns
        // and all data rows stay byte-unchanged. Pure cached reads via the
        // internal accessor: zero COM/GPU/SDK calls during export, which
        // itself still runs only after the original successful Destroy/OFF/
        // worker-join path.
        out += QaD9GpuTimingMetaLines();
#endif
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        // D11 observer comment metadata ONLY, beside the restored D9 block and
        // before the unchanged ORIGINAL 63-column header; zero D3D/COM/SDK
        // calls here; its own independent four-macro gate.
        out += QaD11GpuTimingMetaLines();
#endif
        out += kQaTraceCsvHeader;
        auto du = [](double v) { char b[64]; snprintf(b, sizeof b, "%.17g", v); return std::string(b); };
        for (uint32_t i = 0; i < rows; ++i)
        {
            const QaFrameRowV1& rw = g_qaTraceRows[i];
            out += std::to_string(rw.kind);
            out += ','; out += (rw.kind == 0 ? "endframe" : (rw.kind == 1 ? "wait_failure" : "begin_failure"));
            out += ','; out += std::to_string(rw.frameSerial);
            out += ','; out += std::to_string(rw.callBeginQpc);
            out += ','; out += std::to_string(rw.callEndQpc);
            out += ','; out += std::to_string(rw.result);
            out += ','; out += std::to_string(rw.layerCount);
            out += ','; out += std::to_string(rw.shouldRender);
            out += ','; out += std::to_string(rw.trackingValid);
            out += ','; out += std::to_string(rw.haveLayerPose);
            out += ','; out += std::to_string(rw.activeSourceGpu);
            out += ','; out += std::to_string(rw.sourceKind);
            out += ','; out += std::to_string(rw.sharedIdentityValid);
            out += ','; out += std::to_string(rw.contentClass);
            out += ','; out += std::to_string(rw.currentLocatedPoseSerial);
            out += ','; out += std::to_string(rw.activeFrameSerial);
            out += ','; out += std::to_string(rw.layerPoseSerial);
            out += ','; out += std::to_string(rw.targetDisplayTimeNs);
            out += ','; out += std::to_string(rw.targetDisplayPeriodNs);
            out += ','; out += std::to_string(rw.idEpoch);
            out += ','; out += std::to_string(rw.idGeneration);
            out += ','; out += std::to_string(rw.sampleDisplayTimeNs);
            out += ','; out += std::to_string(rw.sampleDisplayPeriodNs);
            out += ','; out += std::to_string(rw.consumeBeginQpc);
            out += ','; out += std::to_string(rw.consumeEndQpc);
            out += ','; out += std::to_string(rw.waitBeginQpc);
            out += ','; out += std::to_string(rw.waitEndQpc);
            out += ','; out += std::to_string(rw.beginBeginQpc);
            out += ','; out += std::to_string(rw.beginEndQpc);
            out += ','; out += std::to_string(rw.sampleLocateBeginQpc);
            out += ','; out += std::to_string(rw.sampleLocateEndQpc);
            out += ','; out += std::to_string(rw.idPoseBeginQpc);
            out += ','; out += std::to_string(rw.idPoseEndQpc);
            out += ','; out += std::to_string(rw.idCaptureBeginQpc);
            out += ','; out += std::to_string(rw.idCaptureEndQpc);
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k < 7; ++k) { out += ','; out += du(e == 0 ? rw.curL[k] : rw.curR[k]); }
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k < 7; ++k) { out += ','; out += du(e == 0 ? rw.layL[k] : rw.layR[k]); }
            out += '\n';
        }
        const HANDLE h = CreateFileW(g_qaTracePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) gle = GetLastError();
        else
        {
            ok = true;
            size_t off = 0;
            while (off < out.size())
            {
                DWORD written = 0;
                const size_t rem = out.size() - off;
                const DWORD chunk = (DWORD)(rem > 65536u ? 65536u : rem);
                if (!WriteFile(h, out.data() + off, chunk, &written, nullptr) || written == 0)
                { gle = GetLastError(); ok = false; break; }
                off += written;
            }
            if (!CloseHandle(h)) { if (!gle) gle = GetLastError(); ok = false; }
        }
    }
    catch (...)
    {
        ok = false; // build/allocation failure: diagnosticFAIL, never success
    }
    // The result log runs OUTSIDE the allocation try and after the handle was
    // closed, inside a nested no-throw guard: diagnostics must never propagate
    // into VRSIM_Destroy; the export can never alter its unchanged return.
    try
    {
        LogLine("TRACE", std::string("export ") + (ok ? "ok" : "FAIL=diagnosticFAIL neverSuccess") +
                " win32Error=" + std::to_string((unsigned long)gle) +
                " rowCount=" + std::to_string((unsigned long long)rows) +
                " overflowRows=" + std::to_string((unsigned long long)overflow) +
                " capacity=" + std::to_string((unsigned long long)kQaTraceCapacity) +
                " qpcFrequency=" + std::to_string(g_qaTraceQpcFreq) +
                " traceStartQpc=" + std::to_string(g_qaTraceStartQpc) +
                " traceEndQpc=" + std::to_string(g_qaTraceEndQpc) +
                " schemaVersion=1 define=VRSIM_QA_FRAME_TRACE" +
                " path='" + QaWToU8(g_qaTracePath) + "'");
    }
    catch (...)
    {
        OutputDebugStringW(L"VRSIM QA frame trace: export result log unavailable (diagnostic only)\n");
    }
}

// P1 diagnostic boundary: the export must never throw into VRSIM_Destroy.
void QaTraceExportCsv() noexcept
{
    try { QaTraceExportCsvBody(); }
    catch (...) { OutputDebugStringW(L"VRSIM QA frame trace: export failed (diagnosticFAIL; Destroy return unchanged)\n"); }
}
#endif // VRSIM_QA_FRAME_TRACE

#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_PREPARE_WAIT)
// ============================================================================
// VRSIM_QA_PREPARE_WAIT — optional pure-NUMERIC Prepare/Wait telemetry under a
// SECOND compile macro on top of VRSIM_QA_FRAME_TRACE: either macro absent
// compiles NONE of this block. Runtime default OFF; ONLY the exact env value
// "1" of VRSIM_QA_PREPARE_WAIT enables it for the whole session (no trim, no
// case fallback, absent/empty/other/overlong = OFF; no dependency on
// g_qaTraceEnabled or any trace path). A legal VRSIM_Create (after the
// existing eligibility gates, before the worker exists) freezes and resets
// this numeric cache exactly once; refused Creates never reset it, and
// Destroy/Stop/Finalize NEVER clear it, so a closed measurement window keeps
// the original provenance. Zero changes to wait policy, grants, tickets, FPU
// gating, lifecycle, GPU/COM calls, or the worker-owned frame-trace rows /
// cache / schema above. OFF: the bridge call receives nullptr (zero added
// sampling work) and nothing is published. ON: publication is a by-value
// numeric copy under ONE short dedicated SRWLOCK — never nested with the ring
// or meta locks, never held across any SDK/COM/wait/log/string/allocation.
// ============================================================================
struct QaPwPhaseAccum
{
    unsigned long long timedSamples; // incremented ONLY for mask==1 + matching frozen QPF
    unsigned long long ticksSum;     // raw QPC ticks of valid samples only; never an inferred 0
};
struct QaPwCache
{
    bool enabled;                    // exact env "1"
    bool available;                  // frozen QPF succeeded; failure = OFF/unavailable, never fails Create
    unsigned long long qpcFrequency;
    bool haveProvenance;             // FIRST observed identity (original epoch, not post-Stop)
    unsigned long long firstEpoch;
    unsigned long long firstGeneration;
    unsigned long long provenanceMismatchCalls; // later calls whose epoch/gen differ (flagged, never relabeled)
    // ACTUAL returned PrepareOnSourceUI calls; classification on the real prh
    // (never from sample masks alone: a public early return can leave
    // maskBridgeHresult 0 while prh is the true observed code)
    unsigned long long bridgeCalls;
    unsigned long long bridgeSOk;    // exact prh == S_OK (PREPARED grant; NOT commit, NOT display, NOT new-content FPS)
    unsigned long long bridgeFailed; // FAILED(prh) (a non-zero ticket may still exist; NOT "no ticket")
    unsigned long long bridgeOther;  // remaining actual codes
    // totals of the five REAL wait counters (accumulated ONLY when the sample's
    // maskWaitExecuted==1; never inferred from d9Polls, never fabricated)
    unsigned long long getDataCalls;
    unsigned long long flushGetDataCalls;
    unsigned long long sFalseReturns;
    unsigned long long otherPendingReturns;
    unsigned long long sleep1Calls;
    // separate phase denominators; NO averaging by bridgeCalls
    QaPwPhaseAccum copy;    // maskCopyWallSum + copyWallTicksSum (left+right completed real copy CPU walls)
    QaPwPhaseAccum issue;   // maskIssueWall + issueWallTicks
    QaPwPhaseAccum wait;    // maskWaitWall + waitWallTicks (helper n0->exit window; excludes its
                            // initial QPF and error logging; CPU wall ONLY, NOT GPU time / NOT full Prepare latency)
    QaPwPhaseAccum getData; // maskGetDataWall + getDataWallTicksSum (A2 mask = EVERY real call interval measured)
    QaPwPhaseAccum sleep;   // maskSleepWall + sleepWallTicksSum
    unsigned long long timingInvalidCalls; // wait observed with at least one phase lacking a valid measurement
    bool counterOverflow;   // a checked addition failed: old values retained, NEVER wrapped
    bool incomplete;        // overflow or provenance mismatch: aggregates not usable as a complete window
    // FULL private retention of the LAST actual returned call
    bool lastValid;
    VRSIM::PrepareWaitSample lastSample; // by value; no pointers/COM/surfaces/handles retained
    long long lastBridgeHresult;         // the real observed prh (always for a published last)
    unsigned long long lastTicket;       // real granted.ticket: OBSERVED only — not delivered, not COMMIT, not display
    bool lastTicketNonzero;              // nonzero ticket on a FAILED prh does NOT imply the ticket is absent
    unsigned long long lastEpoch;
    unsigned long long lastGeneration;
    unsigned long long lastFrameSerial;
    unsigned long long lastPoseSerial;
    long long lastCaptureBeginQpc;
    long long lastCaptureEndQpc;         // ORIGINAL at PREPARE is 0; preserved 0; stage label below
    bool lastProvenanceValid;
};
static QaPwCache g_pwCache{};
static SRWLOCK g_pwLock = SRWLOCK_INIT;

// Checked unsigned addition: on overflow the OLD value is retained and the
// caller marks the cache incomplete; nothing wraps, throws or changes any
// original outcome.
static inline bool QaPwCheckedAdd(unsigned long long& a, unsigned long long b)
{
    if (b > (0xFFFFFFFFFFFFFFFFull - a)) return false;
    a += b;
    return true;
}

// Legal-Create freeze/reset: numeric env read + ONE QPF, then a full by-value
// cache replacement under the exclusive gate. no-throw: any failure simply
// leaves telemetry OFF/unavailable and NEVER fails VRSIM_Create.
static void QaPrepareWaitFreezeAndReset() noexcept
{
    try
    {
        QaPwCache fresh{};
        wchar_t buf[8];
        const DWORD n = GetEnvironmentVariableW(L"VRSIM_QA_PREPARE_WAIT", buf,
                                                (DWORD)(sizeof(buf) / sizeof(buf[0])));
        if (n == 1u && buf[0] == L'1') // exact "1" ONLY; overlong (n>=8) is OFF, never truncated
        {
            LARGE_INTEGER fq;
            if (QueryPerformanceFrequency(&fq) != 0 && fq.QuadPart > 0)
            {
                fresh.enabled = true;
                fresh.available = true;
                fresh.qpcFrequency = (unsigned long long)fq.QuadPart;
            }
        }
        AcquireSRWLockExclusive(&g_pwLock);
        g_pwCache = fresh;
        ReleaseSRWLockExclusive(&g_pwLock);
    }
    catch (...) { /* freeze failure: telemetry stays OFF; Create return untouched */ }
}

// The enable state is frozen by a legal Create BEFORE the worker/source UI
// can drive any Prepare; afterwards it is read-only for the session, so the
// plain read below is lifecycle-ordered and race-safe by construction.
static inline bool QaPrepareWaitActive()
{
    return g_pwCache.enabled && g_pwCache.available;
}

// Pure-numeric publication from the deferred guard's destructor. no-throw;
// gate held only for fixed-width copies/additions. mask==0 phases NEVER
// increment timedSamples and NEVER add a fake 0; a later success cannot
// revalidate an already-invalid phase total because timedSamples only moves
// with fully valid samples.
static void QaPrepareWaitPublish(const VRSIM::PrepareWaitSample& s, long long prh,
                                unsigned long long ticket,
                                unsigned long long epoch, unsigned long long generation,
                                unsigned long long frameSerial, unsigned long long poseSerial,
                                long long captureBeginQpc, long long captureEndQpc) noexcept
{
    try
    {
        AcquireSRWLockExclusive(&g_pwLock);
        QaPwCache& c = g_pwCache;
        bool ovf = false;
        const bool firstHere = !c.haveProvenance;
        if (firstHere)
        {
            c.haveProvenance = true; c.firstEpoch = epoch; c.firstGeneration = generation;
        }
        else if (epoch != c.firstEpoch || generation != c.firstGeneration)
        {
            if (!QaPwCheckedAdd(c.provenanceMismatchCalls, 1ull)) ovf = true;
            c.incomplete = true; // mismatch is flagged; nothing is relabeled
        }
        const bool lastProv = (epoch != 0ull) && (generation != 0ull) &&
                              (firstHere || (epoch == c.firstEpoch && generation == c.firstGeneration));
        if (!QaPwCheckedAdd(c.bridgeCalls, 1ull)) ovf = true;
        if (prh == (long long)S_OK) { if (!QaPwCheckedAdd(c.bridgeSOk, 1ull)) ovf = true; }
        else if (prh < 0) { if (!QaPwCheckedAdd(c.bridgeFailed, 1ull)) ovf = true; } // FAILED(prh)
        else { if (!QaPwCheckedAdd(c.bridgeOther, 1ull)) ovf = true; }
        // StageB2: copy and Issue have INDEPENDENT denominators. The frozen
        // bridge sets both real timing masks BEFORE the conditional Wait, so a
        // failed Issue (Wait never called) still retains the actual copy/Issue
        // samples. Each phase accumulates ONLY on its own mask==1 plus a sample
        // QPF that is valid and equals the frozen frequency; nothing is ever
        // accumulated as an inferred zero.
        const bool freqOk = (s.maskQpcFrequency != 0u) && (s.qpcFrequency == c.qpcFrequency);
        const bool copyOk = (s.maskCopyWallSum != 0u) && freqOk;
        const bool issueOk = (s.maskIssueWall != 0u) && freqOk;
        if (copyOk)
        { if (!QaPwCheckedAdd(c.copy.timedSamples, 1ull)) ovf = true;
          if (!QaPwCheckedAdd(c.copy.ticksSum, s.copyWallTicksSum)) ovf = true; }
        if (issueOk)
        { if (!QaPwCheckedAdd(c.issue.timedSamples, 1ull)) ovf = true;
          if (!QaPwCheckedAdd(c.issue.ticksSum, s.issueWallTicks)) ovf = true; }
        bool timingValid = copyOk && issueOk;
        if (s.maskWaitExecuted != 0u) // absent mask = unknown, never measured zero
        {
            if (!QaPwCheckedAdd(c.getDataCalls, s.getDataCalls)) ovf = true;
            if (!QaPwCheckedAdd(c.flushGetDataCalls, s.flushGetDataCalls)) ovf = true;
            if (!QaPwCheckedAdd(c.sFalseReturns, s.sFalseReturns)) ovf = true;
            if (!QaPwCheckedAdd(c.otherPendingReturns, s.otherPendingReturns)) ovf = true;
            if (!QaPwCheckedAdd(c.sleep1Calls, s.sleep1Calls)) ovf = true;
            const bool waitOk = (s.maskWaitWall != 0u) && freqOk;
            const bool gdOk = (s.maskGetDataWall != 0u) && freqOk;
            const bool slOk = (s.maskSleepWall != 0u) && freqOk;
            if (waitOk)
            { if (!QaPwCheckedAdd(c.wait.timedSamples, 1ull)) ovf = true;
              if (!QaPwCheckedAdd(c.wait.ticksSum, s.waitWallTicks)) ovf = true; }
            if (gdOk)
            { if (!QaPwCheckedAdd(c.getData.timedSamples, 1ull)) ovf = true;
              if (!QaPwCheckedAdd(c.getData.ticksSum, s.getDataWallTicksSum)) ovf = true; }
            if (slOk)
            { if (!QaPwCheckedAdd(c.sleep.timedSamples, 1ull)) ovf = true;
              if (!QaPwCheckedAdd(c.sleep.ticksSum, s.sleepWallTicksSum)) ovf = true; }
            timingValid = timingValid && waitOk && gdOk && slOk;
        }
        // ONE timing-invalid increment per call, and only for a call that
        // actually attempted at least one timed phase; a refusal that never
        // reached any phase stays unknown, not an invalid measurement.
        const bool attempted = (s.maskCopyWallSum != 0u) || (s.maskIssueWall != 0u) ||
                               (s.maskLeftCopy != 0u) || (s.maskRightCopy != 0u) ||
                               (s.maskIssue != 0u) || (s.maskWaitExecuted != 0u);
        if (attempted && !timingValid) { if (!QaPwCheckedAdd(c.timingInvalidCalls, 1ull)) ovf = true; }
        c.lastValid = true;
        c.lastSample = s;
        c.lastBridgeHresult = prh; // the REAL observed return, even if maskBridgeHresult==0
        c.lastTicket = ticket;
        c.lastTicketNonzero = (ticket != 0ull);
        c.lastEpoch = epoch; c.lastGeneration = generation;
        c.lastFrameSerial = frameSerial; c.lastPoseSerial = poseSerial;
        c.lastCaptureBeginQpc = captureBeginQpc;
        c.lastCaptureEndQpc = captureEndQpc; // original PREPARE value (0); never fabricated
        c.lastProvenanceValid = lastProv;
        if (ovf) { c.counterOverflow = true; c.incomplete = true; } // retained values, no wrap
        ReleaseSRWLockExclusive(&g_pwLock);
    }
    catch (...) { /* publication failure: diagnostics merely absent for one call */ }
}

// Stack-only deferred guard: armed immediately after the ACTUAL bridge call
// returns, publishes at scope exit — strictly AFTER the original
// save-first/classification work on every early return. Never touches the
// ticket path, never releases refs, never calls any lifecycle/GPU method.
struct QaPrepareWaitGuard
{
    void Arm(const VRSIM::PrepareWaitSample& sampleIn, HRESULT prhIn,
             unsigned long long ticketIn, const VRSIM::SharedCaptureIdentityV1& idIn) noexcept
    {
        sample = sampleIn; prh = (long long)prhIn; ticket = ticketIn;
        epoch = idIn.epoch; generation = (unsigned long long)idIn.generation;
        frameSerial = idIn.frameSerial; poseSerial = idIn.poseSerial;
        captureBeginQpc = idIn.captureBeginQpc;
        captureEndQpc = idIn.captureEndQpc; // 0 at PREPARE by wire contract; preserved
        armed = true;
    }
    ~QaPrepareWaitGuard() noexcept
    {
        if (!armed) return; // OFF/unarmed: zero work, zero lock
        QaPrepareWaitPublish(sample, prh, ticket, epoch, generation,
                             frameSerial, poseSerial, captureBeginQpc, captureEndQpc);
    }
    bool armed = false;
    VRSIM::PrepareWaitSample sample{};
    long long prh = 0;
    unsigned long long ticket = 0;
    unsigned long long epoch = 0, generation = 0, frameSerial = 0, poseSerial = 0;
    long long captureBeginQpc = 0, captureEndQpc = 0;
};

// Full by-value snapshot under ONE short shared gate; serialization runs by
// the caller OUTSIDE it. no-throw; failure = snapshot unavailable.
static bool QaPrepareWaitSnapshot(QaPwCache& out) noexcept
{
    try
    {
        AcquireSRWLockShared(&g_pwLock);
        out = g_pwCache;
        ReleaseSRWLockShared(&g_pwLock);
        return true;
    }
    catch (...) { return false; }
}

// Serialization of an ALREADY-SNAPSHOTTED cache (allocations happen only here,
// outside every numeric gate). May throw on allocation: the caller labels the
// child unavailable; nothing here ever touches Native state. Unknown values
// emit null; measured 0 appears only for a valid measured phase. Raw QPC
// ticks only — no NaN/Inf, no ms conversion, no FPS claim.
static std::string QaPrepareWaitJsonBody(const QaPwCache& c)
{
    auto su = [](unsigned long long v) { return std::to_string(v); };
    auto sl = [](long long v) { return std::to_string(v); };
    auto sb = [](bool v) { return std::string(v ? "true" : "false"); };
    std::string s = "{";
    s += "\"schemaVersion\":1";
    s += ",\"enabled\":" + sb(c.enabled);
    s += ",\"available\":" + sb(c.available);
    s += ",\"qpcFrequency\":" + (c.available ? su(c.qpcFrequency) : std::string("null"));
    s += ",\"firstEpoch\":" + (c.haveProvenance ? su(c.firstEpoch) : std::string("null"));
    s += ",\"firstGeneration\":" + (c.haveProvenance ? su(c.firstGeneration) : std::string("null"));
    s += ",\"provenanceMismatchCalls\":" + su(c.provenanceMismatchCalls);
    s += ",\"bridgeCalls\":" + su(c.bridgeCalls);
    s += ",\"bridgeSOk\":" + su(c.bridgeSOk);
    s += ",\"bridgeFailed\":" + su(c.bridgeFailed);
    s += ",\"bridgeOther\":" + su(c.bridgeOther);
    s += ",\"getDataCalls\":" + su(c.getDataCalls);
    s += ",\"flushGetDataCalls\":" + su(c.flushGetDataCalls);
    s += ",\"sFalseReturns\":" + su(c.sFalseReturns);
    s += ",\"otherPendingReturns\":" + su(c.otherPendingReturns);
    s += ",\"sleep1Calls\":" + su(c.sleep1Calls);
    // StageB2: every real *TimedSamples count is emitted (including 0); the
    // corresponding *TicksSum is JSON null when that phase has NO timed
    // sample (unknown/no accumulation), and only ever a real unsigned sum
    // (including a measured 0) once timedSamples > 0.
    s += ",\"copyTimedSamples\":" + su(c.copy.timedSamples) +
         ",\"copyTicksSum\":" + (c.copy.timedSamples > 0ull ? su(c.copy.ticksSum) : std::string("null"));
    s += ",\"issueTimedSamples\":" + su(c.issue.timedSamples) +
         ",\"issueTicksSum\":" + (c.issue.timedSamples > 0ull ? su(c.issue.ticksSum) : std::string("null"));
    s += ",\"waitTimedSamples\":" + su(c.wait.timedSamples) +
         ",\"waitTicksSum\":" + (c.wait.timedSamples > 0ull ? su(c.wait.ticksSum) : std::string("null"));
    s += ",\"getDataTimedSamples\":" + su(c.getData.timedSamples) +
         ",\"getDataTicksSum\":" + (c.getData.timedSamples > 0ull ? su(c.getData.ticksSum) : std::string("null"));
    s += ",\"sleepTimedSamples\":" + su(c.sleep.timedSamples) +
         ",\"sleepTicksSum\":" + (c.sleep.timedSamples > 0ull ? su(c.sleep.ticksSum) : std::string("null"));
    s += ",\"timingInvalidCalls\":" + su(c.timingInvalidCalls);
    s += ",\"counterOverflow\":" + sb(c.counterOverflow);
    s += ",\"incomplete\":" + sb(c.incomplete);
    if (!c.lastValid)
    {
        s += ",\"last\":null";
    }
    else
    {
        const VRSIM::PrepareWaitSample& m = c.lastSample;
        unsigned long long mb = 0;
        mb |= (unsigned long long)(m.maskBridgeHresult & 1u) << 0;
        mb |= (unsigned long long)(m.maskLeftCopy & 1u) << 1;
        mb |= (unsigned long long)(m.maskRightCopy & 1u) << 2;
        mb |= (unsigned long long)(m.maskIssue & 1u) << 3;
        mb |= (unsigned long long)(m.maskWaitExecuted & 1u) << 4;
        mb |= (unsigned long long)(m.maskQpcFrequency & 1u) << 5;
        mb |= (unsigned long long)(m.maskLeftWall & 1u) << 6;
        mb |= (unsigned long long)(m.maskRightWall & 1u) << 7;
        mb |= (unsigned long long)(m.maskIssueWall & 1u) << 8;
        mb |= (unsigned long long)(m.maskWaitWall & 1u) << 9;
        mb |= (unsigned long long)(m.maskGetDataWall & 1u) << 10;
        mb |= (unsigned long long)(m.maskSleepWall & 1u) << 11;
        mb |= (unsigned long long)(m.maskCopyWallSum & 1u) << 12;
        mb |= (unsigned long long)(m.lastGetDataHrValid & 1u) << 13;
        s += ",\"last\":{\"stage\":\"BRIDGE_RETURNED_NOT_NATIVE_ACCEPTED_NOT_COMMIT_NOT_DISPLAY\"";
        s += ",\"provenanceValid\":" + sb(c.lastProvenanceValid);
        s += ",\"epoch\":" + su(c.lastEpoch) + ",\"generation\":" + su(c.lastGeneration);
        s += ",\"frameSerial\":" + su(c.lastFrameSerial) + ",\"poseSerial\":" + su(c.lastPoseSerial);
        s += ",\"captureBeginQpc\":" + sl(c.lastCaptureBeginQpc);
        s += ",\"captureEndQpc\":" + sl(c.lastCaptureEndQpc); // ORIGINAL 0 at PREPARE; preserved, never a completion claim
        s += ",\"bridgeHr\":" + sl(c.lastBridgeHresult) + ",\"bridgeHrIsRealPrh\":true";
        s += ",\"sampleBridgeHr\":" + (m.maskBridgeHresult ? sl((long long)m.bridgeHresult) : std::string("null"));
        s += ",\"ticketObserved\":" + su(c.lastTicket);
        s += ",\"ticketObservedIsNotDelivery\":true";
        s += ",\"maskBits\":" + su(mb) +
             ",\"maskBitsLegend\":\"0bridge,1leftCopy,2rightCopy,3issue,4waitEntered,5qpf,6leftWall,7rightWall,8issueWall,9waitWall,10getDataWall,11sleepWall,12copyWallSum,13lastGetHr\"";
        s += ",\"leftCopyCalls\":" + (m.maskLeftCopy ? su(m.leftCopyCalls) : std::string("null"));
        s += ",\"leftCopyHr\":" + (m.maskLeftCopy ? sl((long long)m.leftCopyHr) : std::string("null"));
        s += ",\"rightCopyCalls\":" + (m.maskRightCopy ? su(m.rightCopyCalls) : std::string("null"));
        s += ",\"rightCopyHr\":" + (m.maskRightCopy ? sl((long long)m.rightCopyHr) : std::string("null"));
        s += ",\"issueCalls\":" + (m.maskIssue ? su(m.issueCalls) : std::string("null"));
        s += ",\"issueHr\":" + (m.maskIssue ? sl((long long)m.issueHr) : std::string("null"));
        s += ",\"lastGetDataHr\":" + (m.lastGetDataHrValid ? sl((long long)m.lastGetDataHr) : std::string("null"));
        s += ",\"leftWallTicks\":" + (m.maskLeftWall ? su(m.leftCopyWallTicks) : std::string("null"));
        s += ",\"rightWallTicks\":" + (m.maskRightWall ? su(m.rightCopyWallTicks) : std::string("null"));
        s += ",\"copyWallTicks\":" + (m.maskCopyWallSum ? su(m.copyWallTicksSum) : std::string("null"));
        s += ",\"issueWallTicks\":" + (m.maskIssueWall ? su(m.issueWallTicks) : std::string("null"));
        s += ",\"waitWallTicks\":" + (m.maskWaitWall ? su(m.waitWallTicks) : std::string("null"));
        s += ",\"getDataWallTicks\":" + (m.maskGetDataWall ? su(m.getDataWallTicksSum) : std::string("null"));
        s += ",\"sleepWallTicks\":" + (m.maskSleepWall ? su(m.sleepWallTicksSum) : std::string("null"));
        s += ",\"sampleQpcFrequency\":" + (m.maskQpcFrequency ? su(m.qpcFrequency) : std::string("null"));
        s += "}";
    }
    s += ",\"limits\":\"ticks are raw QPC CPU wall windows not GPU time not full Prepare latency; waitWallTicks is helper n0-exit window excluding its initial QPF and error logging; bridgeCalls are actual returned Prepare calls never new content frames; ticketObserved is not delivery or COMMIT; phase sums use only mask-valid frequency-matched samples\"";
    s += "}";
    return s;
}
#endif // VRSIM_QA_FRAME_TRACE && VRSIM_QA_PREPARE_WAIT

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

    // ---- HIGHRES-H2-DIAG-FB: ONE bounded two-call enumeration of the session's
    // FB-supported color spaces, on this sole XR worker, directly after the
    // actually successful xrCreateSession. Never per-frame, no ring/meta lock is
    // held across any XR call, no retry; this block itself never calls
    // xrSetColorSpaceFB — its validated list is the PRECONDITION for the single
    // bounded REC709 opt-in Set in the block immediately below.
    // A non-SUCCESS result keeps the raw code and the unknown state; success is
    // never fabricated and the default mode never reaches this block.
    if (g_diagFbEnabled.load() != 0 && xrEnumerateColorSpacesFB != nullptr)
    {
        uint32_t cnt = 0;
        const XrResult cr = xrEnumerateColorSpacesFB(g_sess, 0, &cnt, nullptr);
        g_diagColorCountRc = (int)cr;
        g_diagColorCountRcValid = 1;
        bool dataCallMade = false;
        if (cr == XR_SUCCESS)
        {
            g_diagColorCountOutput = cnt; // actual count-call output, kept even when > capacity
            if (cnt == 0)
            {
                // honest empty list on a successful count call
                g_diagSupportedCount = 0;
                g_diagSupportedListValid = 1;
            }
            else if (cnt <= (uint32_t)kDiagMaxColorSpaces)
            {
                XrColorSpaceFB list[kDiagMaxColorSpaces];
                uint32_t got = 0;
                const XrResult dr = xrEnumerateColorSpacesFB(g_sess, cnt, &got, list);
                g_diagColorDataRc = (int)dr;
                g_diagColorDataRcValid = 1;
                dataCallMade = true;
                if (dr == XR_SUCCESS && got <= cnt)
                {
                    for (uint32_t i = 0; i < got; ++i)
                        g_diagSupportedColorSpaces[i] = (int)list[i];
                    g_diagSupportedCount = got; // count published AFTER elements
                    g_diagSupportedListValid = 1;
                }
            }
            else
            {
                // cnt > bounded capacity: the ACTUAL count is retained (never 0),
                // listValid stays false; no data call, no retry, no growth.
                g_diagSupportedCount = cnt;
                g_diagSupportedListValid = 0;
            }
        }
        std::string facts = "FB supported spaces countRc=" + std::to_string((int)g_diagColorCountRc.load()) +
            " countOutput=" + std::to_string((unsigned long long)cnt) +
            " dataRcRecorded=" + std::to_string(g_diagColorDataRcValid.load()) +
            " dataRc=" + std::to_string(dataCallMade ? (int)g_diagColorDataRc.load() : 0) +
            " listValid=" + std::to_string(g_diagSupportedListValid.load()) +
            " supportedCount=" + std::to_string((unsigned long long)g_diagSupportedCount.load());
        if (g_diagSupportedListValid.load() != 0)
        {
            const uint32_t fc = (uint32_t)g_diagSupportedCount.load();
            for (uint32_t i = 0; i < fc; ++i)
            {
                const int cs = g_diagSupportedColorSpaces[i].load();
                facts += " " + std::to_string(cs) + "(" + FbColorSpaceName(cs) + ")";
            }
        }
        LogLine("DIAG-FB", facts + " appColorSpace=RUNTIME_SELECTED_UNKNOWN setAttempted=" +
                std::to_string(g_diagSetAttempted.load()));
        PublishFbDiagnosticsSnapshot(); // stage boundary: worker enumeration block fully complete (facts already logged outside any gate)
    }

    // ---- HIGHRES-H2-REC709 OPT-IN: at most ONE official xrSetColorSpaceFB
    // invocation, on this sole XR worker, directly after the supported-space
    // enumeration. Attempted only when ALL hold: exact frozen 'rec709' request,
    // FB actually enabled, Set PFN present, supported enumeration list valid and
    // XR_COLOR_SPACE_REC709_FB (2) present in it. Every other combination is an
    // honest logged skip. applied=REC709/valid is published ONLY on exact
    // XR_SUCCESS; failure keeps the real rc, RUNTIME_SELECTED_UNKNOWN and never
    // falls back to another color space. Default/legacy/absent never reach a Set.
    // No lock is held across the XR call; this runs once, never per frame.
    if (g_diagSetRequested.load() == 1)
    {
        int skip = 0;
        if (g_diagFbPipelineSelected.load() != 1) skip = 8;      // obs-shared only
        else if (!g_sessionColorSrgb) skip = 9;                  // sRGB source contract only
        else if (g_diagFbEnabled.load() == 0) skip = 2;
        else if (g_diagSetFunctionPresent.load() == 0) skip = 3;
        else if (g_diagSupportedListValid.load() == 0) skip = 4;
        else
        {
            uint32_t lim = (uint32_t)g_diagSupportedCount.load();
            if (lim > (uint32_t)kDiagMaxColorSpaces) lim = (uint32_t)kDiagMaxColorSpaces;
            bool rec709Supported = false;
            for (uint32_t i = 0; i < lim; ++i)
            {
                if (g_diagSupportedColorSpaces[i].load() == (int)XR_COLOR_SPACE_REC709_FB)
                { rec709Supported = true; break; }
            }
            if (!rec709Supported) skip = 5;
        }
        if (skip == 0)
        {
            const XrResult sr = xrSetColorSpaceFB(g_sess, XR_COLOR_SPACE_REC709_FB);
            g_diagSetAttempted = 1;
            g_diagSetResult = (int)sr;
            g_diagSetResultValid = 1;
            if (sr == XR_SUCCESS)
            {
                g_diagAppliedColorSpace = (int)XR_COLOR_SPACE_REC709_FB;
                g_diagAppliedValid = 1;
            }
            else g_diagSetSkipReason = 6;
            LogLine("DIAG-FB", "xrSetColorSpaceFB REC709 attempted once rc=" + std::to_string((int)sr) +
                    " appliedValid=" + std::to_string(g_diagAppliedValid.load()) +
                    " (exact-success-only; failure keeps RUNTIME_SELECTED_UNKNOWN, no fallback setter)");
        }
        else
        {
            g_diagSetSkipReason = skip;
            LogLine("DIAG-FB", std::string("REC709 set skipped: ") + FbSetSkipReasonName(skip) +
                    " (appColorSpace stays RUNTIME_SELECTED_UNKNOWN)");
        }
        PublishFbDiagnosticsSnapshot(); // stage boundary: worker Set transaction complete
    }

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
    if (r != XR_SUCCESS || vc != 2)
    { SetErr("xrEnumerateViewConfigurationViews " + XrResultName(r)); return -2; }
    // DEVICE-CAPS-01: validate BOTH eyes' recommended AND maxImageRect extents
    // against the actual system max cached by VRSIM_Create, then freeze the
    // per-session capability cache from this actual successful enumeration.
    // Fail closed BEFORE any swapchain creation: no clamp, no left-eye-only
    // assumption, no creation outside the applicable maxima.
    VRSIMCaps::DeviceCapsV1 caps{};
    caps.systemMaxValid = g_deviceCaps.systemMaxValid;
    caps.systemMaxWidth = g_deviceCaps.systemMaxWidth;
    caps.systemMaxHeight = g_deviceCaps.systemMaxHeight;
    for (int e = 0; e < 2; ++e)
    {
        caps.eye[e].recommendedWidth  = eye[e].recommendedImageRectWidth;
        caps.eye[e].recommendedHeight = eye[e].recommendedImageRectHeight;
        caps.eye[e].maxRectWidth      = eye[e].maxImageRectWidth;
        caps.eye[e].maxRectHeight     = eye[e].maxImageRectHeight;
    }
    const int capsDecision = VRSIMCaps::CapsAdmitInitialSwapchains(caps);
    if (capsDecision != VRSIMCaps::kCapsOk)
    {
        SetErr("view configuration capability gate refused decision=" + std::to_string(capsDecision) +
               " eye0 rec=" + std::to_string(caps.eye[0].recommendedWidth) + "x" + std::to_string(caps.eye[0].recommendedHeight) +
               " maxRect=" + std::to_string(caps.eye[0].maxRectWidth) + "x" + std::to_string(caps.eye[0].maxRectHeight) +
               " eye1 rec=" + std::to_string(caps.eye[1].recommendedWidth) + "x" + std::to_string(caps.eye[1].recommendedHeight) +
               " maxRect=" + std::to_string(caps.eye[1].maxRectWidth) + "x" + std::to_string(caps.eye[1].maxRectHeight) +
               " systemMax=" + std::to_string(caps.systemMaxWidth) + "x" + std::to_string(caps.systemMaxHeight) +
               " systemMaxValid=" + std::to_string(caps.systemMaxValid ? 1 : 0));
        return -2;
    }
    caps.valid = true;
    g_deviceCaps = caps; // per-session cache: only this actual successful query admits this runtime
    LogLine("XR", "caps eye0 rec=" + std::to_string(caps.eye[0].recommendedWidth) + "x" + std::to_string(caps.eye[0].recommendedHeight) +
            " maxRect=" + std::to_string(caps.eye[0].maxRectWidth) + "x" + std::to_string(caps.eye[0].maxRectHeight) +
            " eye1 rec=" + std::to_string(caps.eye[1].recommendedWidth) + "x" + std::to_string(caps.eye[1].recommendedHeight) +
            " maxRect=" + std::to_string(caps.eye[1].maxRectWidth) + "x" + std::to_string(caps.eye[1].maxRectHeight) +
            " systemMax=" + std::to_string(caps.systemMaxWidth) + "x" + std::to_string(caps.systemMaxHeight) +
            " (both eyes validated before initial swapchain creation)");

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
    g_swapW = eye[0].recommendedImageRectWidth;  // legacy left-eye scene payload contract (kept)
    g_swapH = eye[0].recommendedImageRectHeight;
    // DEVICE-CAPS-01: retain the ACTUAL per-eye output extents; the projection
    // submit uses each eye's own extent, never the left extents for both eyes.
    for (int e = 0; e < 2; ++e)
    {
        g_eyeOutW[e] = eye[e].recommendedImageRectWidth;
        g_eyeOutH[e] = eye[e].recommendedImageRectHeight;
    }
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
#if defined(VRSIM_QA_MIXED_SOURCE)
// HIGHRES-H2 mixed-source QA mode key (worker-only): true ONLY while the shared
// g_gpuPass was (re)initialized via the QA split entry (4096x4096 source into
#if defined(VRSIM_QA_OUTPUT_4096)
// the 4096x4096 output profile). Never a new pass/context/thread: the same
#else
// the 3072x3072 output profile). Never a new pass/context/thread: the same
#endif
// g_gpuPass helper and the same single XR worker are used. The raw CPU-byte
// EnsureGpuPassReady path refuses to reuse such a pass and resets first.
static bool                       g_gpuPassMixedSourceQa = false;
#endif

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

#if defined(VRSIM_QA_D9_BEGIN_POLL)
// SOURCE-UI-BEGIN-POLL-01: one FIXED native async ledger for the opt-in QA
// Begin/Poll/Cancel facade. Guarded by g_sharedMetaMutex (ring -> meta order).
// It NEVER caches borrowed surface numbers, COM pointers or an allocating
// queue: only the durable occupancy flag, the original source thread, the
// original ring slot + frozen claim epoch, the full immutable identity192 and
// the exact bridge token32. `occupied` is the cross-call durable lease that
// keeps PREPARING visible to reset/drain/finalize BETWEEN UI callbacks; the
// per-call SharedActiveCallGuard count is dropped on every return and is NOT a
// substitute for it. Reset to empty only on a proven grant-publish, a proven
// paired-FREE rollback or a proven drained clear; never blindly on worker exit.
struct QaAsyncLedgerV1
{
    bool occupied = false;                     // durable reservation/token lives across calls
    unsigned long sourceThread = 0;            // original source UI thread id
    int slot = -1;                             // original ring slot
    unsigned long long claimEpoch = 0;         // ring epoch frozen at reservation
    VRSIM::SharedCaptureIdentityV1 identity{}; // full immutable identity192
    VRSIM::QaBeginTokenV1 token{};             // exact bridge token32 (valid when hasToken)
    bool hasToken = false;                     // a real S_FALSE/Pending token was saved
};
static QaAsyncLedgerV1 g_qaLedger;
#endif // VRSIM_QA_D9_BEGIN_POLL

// CARD2B_QA_DIAGNOSTICS_BEGIN
#if defined(VRSIM_QA_D9_BEGIN_POLL)
struct QaBeginPollNativeSnapshotV1
{
    bool bound;
    bool lifetimeStarted;
    unsigned long long boundEpoch;
    uint32_t boundGeneration;
    bool ringAdmission;
    unsigned long long ringEpoch;
    bool admissionClosed;
    bool closing;
    bool teardownRequested;
    bool sourceFinalized;
    bool finalizedOnce;
    bool workerDrained;
    bool finalizeBusy;
    int sourceActiveCalls;
    bool workerNoContext;
    bool workerCleanupDone;
    bool quarantined;
    QaAsyncLedgerV1 ledger;
};

static bool QaBeginPollCopyNativeSnapshot(QaBeginPollNativeSnapshotV1& out) noexcept
{
    try
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        out.bound = g_sharedBound;
        out.lifetimeStarted = g_sharedLifetimeStarted;
        out.boundEpoch = g_sharedBoundEpoch;
        out.boundGeneration = g_sharedBoundGeneration;
        out.ringAdmission = g_ringAdmission;
        out.ringEpoch = g_ringEpoch;
        out.admissionClosed = g_sharedAdmissionClosed;
        out.closing = g_sharedClosing;
        out.teardownRequested = g_sharedTeardownRequested;
        out.sourceFinalized = g_sharedSourceFinalized;
        out.finalizedOnce = g_sharedFinalizedOnce;
        out.workerDrained = g_sharedWorkerDrained;
        out.finalizeBusy = g_sharedFinalizeBusy;
        out.sourceActiveCalls = g_sharedSourceActiveCalls;
        out.workerNoContext = g_sharedWorkerNoContext;
        out.workerCleanupDone = g_sharedWorkerCleanupDone;
        out.quarantined = g_sharedQuarantined;
        out.ledger = g_qaLedger;
    }
    catch (...) { return false; }
    return true;
}

static bool QaBeginPollBracketMatched(const QaBeginPollNativeSnapshotV1& a,
                                     const QaBeginPollNativeSnapshotV1& b) noexcept
{
    return a.bound == b.bound
        && a.lifetimeStarted == b.lifetimeStarted
        && a.boundEpoch == b.boundEpoch
        && a.boundGeneration == b.boundGeneration
        && a.ringAdmission == b.ringAdmission
        && a.ringEpoch == b.ringEpoch
        && a.admissionClosed == b.admissionClosed
        && a.closing == b.closing
        && a.teardownRequested == b.teardownRequested
        && a.sourceFinalized == b.sourceFinalized
        && a.finalizedOnce == b.finalizedOnce
        && a.workerDrained == b.workerDrained
        && a.finalizeBusy == b.finalizeBusy
        && a.sourceActiveCalls == b.sourceActiveCalls
        && a.workerNoContext == b.workerNoContext
        && a.workerCleanupDone == b.workerCleanupDone
        && a.quarantined == b.quarantined
        && a.ledger.occupied == b.ledger.occupied
        && a.ledger.sourceThread == b.ledger.sourceThread
        && a.ledger.slot == b.ledger.slot
        && a.ledger.claimEpoch == b.ledger.claimEpoch
        && a.ledger.hasToken == b.ledger.hasToken
        && memcmp(&a.ledger.identity, &b.ledger.identity,
                  sizeof(VRSIM::SharedCaptureIdentityV1)) == 0
        && memcmp(&a.ledger.token, &b.ledger.token,
                  sizeof(VRSIM::QaBeginTokenV1)) == 0;
}

static std::string QaBeginPollNativeJsonBody(const QaBeginPollNativeSnapshotV1& s)
{
    std::string r = "{";
    r += "\"bound\":" + std::string(s.bound ? "true" : "false");
    r += ",\"lifetimeStarted\":" + std::string(s.lifetimeStarted ? "true" : "false");
    r += ",\"boundEpoch\":" + std::to_string(s.boundEpoch);
    r += ",\"boundGeneration\":" + std::to_string(s.boundGeneration);
    r += ",\"ringAdmission\":" + std::string(s.ringAdmission ? "true" : "false");
    r += ",\"ringEpoch\":" + std::to_string(s.ringEpoch);
    r += ",\"admissionClosed\":" + std::string(s.admissionClosed ? "true" : "false");
    r += ",\"closing\":" + std::string(s.closing ? "true" : "false");
    r += ",\"teardownRequested\":" + std::string(s.teardownRequested ? "true" : "false");
    r += ",\"sourceFinalized\":" + std::string(s.sourceFinalized ? "true" : "false");
    r += ",\"finalizedOnce\":" + std::string(s.finalizedOnce ? "true" : "false");
    r += ",\"workerDrained\":" + std::string(s.workerDrained ? "true" : "false");
    r += ",\"finalizeBusy\":" + std::string(s.finalizeBusy ? "true" : "false");
    r += ",\"sourceActiveCalls\":" + std::to_string(s.sourceActiveCalls);
    r += ",\"workerNoContext\":" + std::string(s.workerNoContext ? "true" : "false");
    r += ",\"workerCleanupDone\":" + std::string(s.workerCleanupDone ? "true" : "false");
    r += ",\"quarantined\":" + std::string(s.quarantined ? "true" : "false");
    r += ",\"occupied\":" + std::string(s.ledger.occupied ? "true" : "false");
    r += ",\"sourceThread\":" + std::to_string(s.ledger.sourceThread);
    r += ",\"slot\":" + std::to_string(s.ledger.slot);
    r += ",\"claimEpoch\":" + std::to_string(s.ledger.claimEpoch);
    r += ",\"hasToken\":" + std::string(s.ledger.hasToken ? "true" : "false");
    r += ",\"identity\":{";
    r += "\"structBytes\":" + std::to_string(s.ledger.identity.structBytes);
    r += ",\"version\":" + std::to_string(s.ledger.identity.version);
    r += ",\"frameSerial\":" + std::to_string(s.ledger.identity.frameSerial);
    r += ",\"poseSerial\":" + std::to_string(s.ledger.identity.poseSerial);
    r += ",\"epoch\":" + std::to_string(s.ledger.identity.epoch);
    r += ",\"generation\":" + std::to_string(s.ledger.identity.generation);
    r += ",\"reserved0\":" + std::to_string(s.ledger.identity.reserved);
    r += ",\"poseSampleBeginQpc\":" + std::to_string(s.ledger.identity.poseSampleBeginQpc);
    r += ",\"poseSampleEndQpc\":" + std::to_string(s.ledger.identity.poseSampleEndQpc);
    r += ",\"captureBeginQpc\":" + std::to_string(s.ledger.identity.captureBeginQpc);
    r += ",\"captureEndQpc\":" + std::to_string(s.ledger.identity.captureEndQpc);
    r += "}";
    r += ",\"token\":{";
    r += "\"version\":" + std::to_string(s.ledger.token.version);
    r += ",\"structBytes\":" + std::to_string(s.ledger.token.structBytes);
    r += ",\"slotIndex\":" + std::to_string(s.ledger.token.slotIndex);
    r += ",\"generation\":" + std::to_string(s.ledger.token.generation);
    r += ",\"epoch\":" + std::to_string(s.ledger.token.epoch);
    r += ",\"nonce\":" + std::to_string(s.ledger.token.nonce);
    r += "}";
    r += "}";
    return r;
}

static std::string QaBeginPollBridgeJsonBody(const VRSIM::QaBridgeSnapshotV1& s)
{
    std::string r = "{";
    r += "\"version\":" + std::to_string(s.version);
    r += ",\"structBytes\":" + std::to_string(s.structBytes);
    r += ",\"ledgerState\":" + std::to_string(s.ledgerState);
    r += ",\"slotValid\":" + std::to_string(s.slotValid);
    r += ",\"slotIndex\":" + std::to_string(s.slotIndex);
    r += ",\"epoch\":" + std::to_string(s.epoch);
    r += ",\"generation\":" + std::to_string(s.generation);
    r += ",\"nonce\":" + std::to_string(s.nonce);
    r += ",\"stampFrame\":" + std::to_string(s.stampFrame);
    r += ",\"stampPose\":" + std::to_string(s.stampPose);
    r += ",\"stampCaptureBeginQpc\":" + std::to_string(s.stampCaptureBeginQpc);
    r += ",\"initializedNow\":" + std::to_string(s.initializedNow);
    r += ",\"admissionClosed\":" + std::to_string(s.admissionClosed);
    r += ",\"finalizing\":" + std::to_string(s.finalizing);
    r += ",\"finalized\":" + std::to_string(s.finalized);
    r += ",\"refsHeldMask\":" + std::to_string(s.refsHeldMask);
    r += ",\"queryState\":" + std::to_string(s.queryState);
    r += ",\"sourceBusy\":" + std::to_string(s.sourceBusy);
    r += ",\"workerBusy\":" + std::to_string(s.workerBusy);
    r += ",\"activeEntries\":" + std::to_string(s.activeEntries);
    r += ",\"quarantined\":" + std::to_string(s.quarantined);
    r += ",\"quarantineCode\":" + std::to_string(s.quarantineCode);
    r += ",\"slotStates\":[" + std::to_string(s.slotState[0])
       + "," + std::to_string(s.slotState[1])
       + "," + std::to_string(s.slotState[2]) + "]";
    r += ",\"actualCopyCalls\":" + std::to_string(s.actualCopyCalls);
    r += ",\"actualIssueCalls\":" + std::to_string(s.actualIssueCalls);
    r += ",\"actualGetDataCalls\":" + std::to_string(s.actualGetDataCalls);
    r += ",\"firstFailureHr\":" + std::to_string(s.firstFailureHr);
    r += ",\"lastIssueHr\":" + std::to_string(s.lastIssueHr);
    r += ",\"lastGetDataHr\":" + std::to_string(s.lastGetDataHr);
    r += ",\"ticketGranted\":" + std::to_string(s.ticketGranted);
    r += ",\"grantedTicket\":" + std::to_string(s.grantedTicket);
    r += ",\"initCount\":" + std::to_string(s.initCount);
    r += "}";
    return r;
}
#endif // VRSIM_QA_D9_BEGIN_POLL
// CARD2B_QA_DIAGNOSTICS_END

#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING)
// D9 GPU timing observer COMMENT METADATA ONLY (QA-internal: no C export,
// no PInvoke, no ABI/wire change). Reads the bridge's pure cached accessor
// QaD9GpuTimingCache: zero COM/GPU/SDK calls, safe at export time (after the
// original Destroy/worker-join; the Pimpl is never deleted). Emitted before
// the ORIGINAL CSV header; the 63 columns and rows are untouched.
// Truthful scope: the observer times ONLY the real right-eye D3D9
// StretchRect stage on the actual source device. It NEVER claims the opaque
// SDK rendering, the left copy, the whole stereo pair or a pure completion
// delay; missing/unsupported/incomplete timing stays unavailable (a validity
// of 0 / counter, never a synthetic 0ms). GPU fields are same-source-device
// raw ticks; readyQpc is observation-ready, NOT GPU completion; QPC, GPU and
// cross-device clocks are NEVER subtracted here.
static std::string QaD9GpuTimingMetaLines()
{
    try
    {
        VRSIM::QaD9GpuObsCacheV1 c{};
        g_sharedBridge.QaD9GpuTimingCache(&c);
        auto u = [](unsigned long long v) { return std::to_string(v); };
        std::string m;
        m += "#d9GpuTimingSchema=" + u(c.version) + "\n";
        m += "#d9GpuTimingStage=D9_RIGHT_COPY_ONLY\n";
        m += "#d9GpuTimingCoverage=observed:real-right-eye-D3D9-StretchRect-only;"
             "notOpaqueSdkRendering;notLeftCopy;notWholeStereoPair;notPureCompletionDelay\n";
        m += "#d9GpuTimingClocks=gpuTicks=same-source-device-TIMESTAMP-raw-values;"
             "readyQpc=observation-ready-NOT-GPU-completion;"
             "QPC/GPU/cross-device-clocks-NEVER-subtracted\n";
        m += "#d9GpuTiming=enabled=" + u(c.enabled) +
             ",disabledReason=" + u(c.disabledReason) +
             ",eligibleRightCopyTxns=" + u(c.eligibleRightCopyTxns) +
             ",bracketsSampled=" + u(c.bracketsSampled) +
             ",skippedBusy=" + u(c.skippedBusy) +
             ",issueFailed=" + u(c.issueFailed) +
             ",resolveFailed=" + u(c.resolveFailed) +
             ",incomplete=" + u(c.incomplete) +
             ",validRecords=" + u(c.validRecords) +
             ",overflowRecords=" + u(c.overflowRecords) +
             ",resolveGetDataCalls=" + u(c.resolveGetDataCalls) +
             ",recordCount=" + u(c.recordCount) +
             ",gpuFreqTicks=" + u(c.gpuFreqTicks) +
             ",qpcFreqTicks=" + u(c.qpcFreqTicks) +
             ",deltaTicksSum=" + u(c.deltaTicksSum) +
             ",deltaTicksMin=" + u(c.deltaTicksMin) +
             ",deltaTicksMax=" + u(c.deltaTicksMax) +
             ",finalizedCached=" + u(c.finalizedCached) +
             ",units=rawSameSourceDeviceTicks;deltaTicksArePerValidSampleOnly;"
             "gpuFreqTicks=lastValidObservedFrequencyOnly;notAllSampleMilliseconds\n";
        // Full per-record body as a comment tail: raw per-sample values kept
        // verbatim; no fabricated millisecond conversions.
        auto rec = [](const VRSIM::QaD9GpuObsRecordV1& r) {
            const char* disj = r.disjointValue < 0 ? "unknown" :
                               (r.disjointValue == 0 ? "false" : "true");
            std::string s = "stage=" + std::to_string((unsigned long long)r.stage);
            s += ",structBytes=" + std::to_string((unsigned long long)r.structBytes);
            s += ",obsSlot=" + std::to_string((unsigned long long)r.obsSlot);
            s += ",leaseSlot=" + std::to_string((unsigned long long)r.leaseSlot);
            s += ",threadId=" + std::to_string((unsigned long long)r.sourceThreadId);
            s += ",epoch=" + std::to_string(r.epoch);
            s += ",generation=" + std::to_string((unsigned long long)r.generation);
            s += ",frameSerial=" + std::to_string(r.frameSerial);
            s += ",poseSerial=" + std::to_string(r.poseSerial);
            s += ",captureBeginQpc=" + std::to_string(r.captureBeginQpc);
            s += ",cpuBeginQpc=" + std::to_string((unsigned long long)r.cpuBeginQpc);
            s += ",cpuEndQpc=" + std::to_string((unsigned long long)r.cpuEndQpc);
            s += ",readyQpc=" + std::to_string((unsigned long long)r.readyQpc);
            s += ",qpcMask=" + std::to_string((unsigned long long)r.qpcMask);
            s += ",gpuBeginTicks=" + std::to_string(r.gpuBeginTicks);
            s += ",gpuEndTicks=" + std::to_string(r.gpuEndTicks);
            s += ",gpuFreqTicks=" + std::to_string(r.gpuFreqTicks);
            s += ",gpuDeltaTicks=" + std::to_string(r.gpuDeltaTicks);
            s += ",disjoint=" + std::string(disj);
            s += ",issueCmdMask=" + std::to_string((unsigned long long)r.issueCmdMask);
            s += ",firstIssueHr=" + std::to_string((long long)r.firstIssueHr);
            s += ",rightCopyHr=" + std::to_string((long long)r.rightCopyHr);
            s += ",rightCopyHrIsReal=" + std::to_string((unsigned long long)r.rightCopyHrIsReal);
            s += ",gdHrFreq=" + std::to_string((long long)r.gdHrFreq);
            s += ",gdHrBegin=" + std::to_string((long long)r.gdHrBegin);
            s += ",gdHrEnd=" + std::to_string((long long)r.gdHrEnd);
            s += ",gdHrDisjoint=" + std::to_string((long long)r.gdHrDisjoint);
            s += ",dataMask=" + std::to_string((unsigned long long)r.dataMask);
            s += ",validity=" + std::to_string((unsigned long long)r.validity);
            s += ",resolved=" + std::to_string((unsigned long long)r.resolved);
            return s;
        };
        // EVERY retained record is emitted exactly once here (once-only
        // serialization after the original successful Destroy/OFF/join; never
        // a per-frame IO/alloc path). The indexed accessor returns rows ONLY
        // after the actual finalizedCached flag; a refused copy stops nothing
        // else and the completeness line makes any gap explicit.
        unsigned long long emitted = 0ull;
        const unsigned long long cap = (unsigned long long)VRSIM::kQaD9GpuObsCapacityV1;
        const unsigned long long rc = c.recordCount < cap ? c.recordCount : cap;
        for (unsigned long long i = 0ull; i < rc; ++i)
        {
            VRSIM::QaD9GpuObsRecordV1 r{};
            if (g_sharedBridge.QaD9GpuTimingRecord((uint32_t)i, &r))
            {
                m += "#d9GpuTimingRecord=index=" + u(i) + "," + rec(r) + "\n";
                ++emitted;
            }
        }
        // Aggregate + last-record lines stay for continuity, explicitly raw
        // same-device ticks (never all-sample milliseconds).
        if (c.lastRecordValid != 0u) m += "#d9GpuTimingLast=" + rec(c.lastRecord) + "\n";
        else m += "#d9GpuTimingLast=unavailable\n";
        m += "#d9GpuTimingCompleteness=recordsRequested=" + u(c.recordCount) +
             ",recordsEmitted=" + u(emitted) +
             ",overflowRecords=" + u(c.overflowRecords) +
             ",incomplete=" + u(c.incomplete) +
             ",status=" + (c.overflowRecords > 0ull || emitted != rc
                               ? std::string("INCOMPLETE")
                               : (emitted > 0ull ? std::string("FULL") : std::string("NO_RECORDS"))) + "\n";
        return m;
    }
    catch (...) { return std::string("#d9GpuTiming=unavailable\n"); }
}
#endif
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
// D11 LEASE-OWNED OUTPUT GPU TIMING observer (QA-internal diagnostic; the
// ONLY gate is the existing four-macro combination above; no new macro, no
// C export, no PInvoke, no ABI/wire change; comment metadata only).
// Truthful stage: D11_RENDER_SHARED_PAIR_CALL measures ONLY the GPU
// timestamp span between the ordered TIMESTAMP commands bracketing the
// original RenderSharedPair dispatch on the existing worker/context (for a
// normal S_OK sRGB91 pass: the two 88->92 staging CopyResource calls and
// the two projection Draw calls, plus the call's validation/CB/binding
// dispatch gaps). It EXCLUDES D9 source copies, the opaque SDK SurfaceRender,
// the final swapchain CopyResource/Wait/Release, compositor/OVR work and
// whole stereo-frame latency. GPU timestamps are elapsed between ordered
// commands and may include scheduling/preemption; NOT isolated shader busy
// time, NOT machine utilization, NOT a no-profiler baseline, NOT 120 FPS.
// QPC, GPU and cross-device clocks are NEVER subtracted here.
// Private constants ONLY (R3): observer state values and mask bits — no new
// macro is introduced anywhere in this diagnostic.
enum QaD11GpuObsConstV1 : uint32_t
{
    kD11GpuStageRenderSharedPair = 1u,
    kD11GpuObsFree = 0u,      // free, no owned bracket
    kD11GpuObsBegun = 1u,     // begin commands issued, awaiting the single close
    kD11GpuObsOpenX = 2u,     // bracket open with UNKNOWN issue ownership (throw)
    kD11GpuObsPending = 3u,   // closed, awaiting asynchronous resolve
    kD11GpuObsResolved = 4u,  // GetData fully resolved (valid OR invalid)
    kD11GpuObsRetired = 5u,   // GetData FAILED: permanently retired
    kD11GpuIssueDisjBegun = 0x1u, // DISJOINT Begin returned normally (void)
    kD11GpuIssueStartEnd = 0x2u,  // start TIMESTAMP End returned normally
    kD11GpuIssueEndEnd = 0x4u,    // end TIMESTAMP End returned normally
    kD11GpuIssueDisjEnd = 0x8u,   // DISJOINT End returned normally
    kD11GpuDataFreq = 0x1u,   // frequency/disjoint obtained
    kD11GpuDataStart = 0x2u,  // start UINT64 timestamp obtained
    kD11GpuDataEnd = 0x4u,    // end UINT64 timestamp obtained
    kD11GpuQpcBegin = 0x1u,
    kD11GpuQpcEnd = 0x2u,
    kD11GpuQpcReady = 0x4u
};
static const uint32_t kD11GpuIssueFull = 0x1u | 0x2u | 0x4u | 0x8u; // all four issued normally
static const uint32_t kD11GpuDataFull = 0x1u | 0x2u | 0x4u;         // freq/start/end obtained
static const uint64_t kD11GpuUnknownTicks = 0x8000000000000000ull; // explicit unknown sentinel
static const uint32_t kD11GpuObsSlots = 2u;        // two fixed query slots
static const uint32_t kD11GpuObsCapacity = 4096u;  // one fixed POD record array
static const uint32_t kD11GpuSampleEvery = 16u;    // every 16th eligible attempt
// reason codes: 0 none/valid 1 identity 2 disjoint 3 freq 4 tick-order
// 5 render-not-SOK-or-threw 6 endread-not-SOK 7 data-incomplete 8 unresolved
struct QaD11GpuObsRecordV1
{
    uint32_t recordIndex, stage, obsSlot;
    uint32_t leaseSlot, obsState;
    unsigned long long ticket, epoch, generation, frameSerial, poseSerial;
    long long captureBeginQpc;
    unsigned long long workerThreadId;
    uint32_t srcPlaneFormat, outFormat, planeWidth, planeHeight;
    long long cpuBeginQpc, cpuEndQpc, readyQpc; uint32_t qpcMask;
    uint64_t gpuStartTicks, gpuEndTicks, gpuFreqTicks;
    long long disjointValue; // -1 unknown, 0 FALSE, 1 TRUE
    uint32_t issueMask, dataMask, hrMask;
    long long renderHr, endReadHr, hrRender, hrEndStart, hrDisjoint, hrEndEnd;
    uint32_t renderThrew, endReadThrew, renderReturned, endReadReturned;
    uint32_t validity, resolved, reason;
    unsigned long long deviceKey; // frozen g_dev identity (pointer value); NEVER subtracted against any clock
};
// R3B explicit association handle: Start fills it ONLY when it reserved the
// record and issued at least the DISJOINT Begin; invalid = no sample, no
// command. Close consumes this exact handle — never a bool, never a scan of
// all slots.
struct QaD11GpuObsHandleV1
{
    uint32_t slot;
    uint32_t record;
};
static inline void QaD11GpuObsInvalidateHandle(QaD11GpuObsHandleV1& h)
{
    h.slot = 0xffffffffu; h.record = 0xffffffffu;
}
static QaD11GpuObsRecordV1 g_d11GpuRecords[kD11GpuObsCapacity];
static uint32_t g_d11GpuRecordCount = 0u;
static uint32_t g_d11GpuOverflow = 0u;
static bool   g_d11GpuObsEnabled = false;   // exact-env "1" AND g_qaTraceEnabled
static bool   g_d11GpuInitDone = false;     // worker init ran once
static bool   g_d11GpuInitFailed = false;   // query creation failed: diagnostic OFF
static bool   g_d11GpuInitThrew = false;    // CreateQuery THREW (distinct from a real HRESULT)
static long long g_d11GpuInitHr = 0;        // REAL CreateQuery HRESULT when it returned
static bool   g_d11GpuObsReady = false;     // six refs owned, sampling allowed
static bool   g_d11GpuRefsOut = false;      // one or more refs not relinquished
static bool   g_d11GpuCleanupDone = false;  // all refs relinquished on proven drain
static uint64_t g_d11GpuReleaseEx = 0ull;   // Release THROW count (category; Release returns a refcount, never an HRESULT)
static uint32_t g_d11GpuResolveCursor = 0u;
static uint64_t g_d11GpuSampleOrdinal = 0ull;
static uint64_t g_d11GpuEligible = 0ull, g_d11GpuSampled = 0ull;
static uint64_t g_d11GpuSkipBusy = 0ull, g_d11GpuSkipLimit = 0ull, g_d11GpuUnsupported = 0ull;
static uint64_t g_d11GpuIssueEx = 0ull, g_d11GpuResolveFail = 0ull, g_d11GpuDisjoint = 0ull;
static uint64_t g_d11GpuValid = 0ull, g_d11GpuIncomplete = 0ull;
static uint64_t g_d11GpuGetDataCalls = 0ull, g_d11GpuRefsReleased = 0ull;
static bool     g_d11GpuFinalized = false;
static ID3D11Query* g_d11GpuStart[kD11GpuObsSlots] = { nullptr, nullptr };
static ID3D11Query* g_d11GpuEnd[kD11GpuObsSlots] = { nullptr, nullptr };
static ID3D11Query* g_d11GpuDisjointQ[kD11GpuObsSlots] = { nullptr, nullptr };
static uint32_t g_d11GpuSlotState[kD11GpuObsSlots] = { kD11GpuObsFree, kD11GpuObsFree };
static uint32_t g_d11GpuSlotRec[kD11GpuObsSlots]; // record index currently held by each slot
static bool g_d11GpuAnySlotHeld = false; // frozen at Create: prior-session refs retained
static uint32_t g_d11GpuFieldCursor[kD11GpuObsSlots] = { 0u, 0u }; // R4 per-slot DISJOINT/start/end field cursor
static uint64_t g_d11GpuMetaEx = 0ull; // PRE-Issue metadata/lock exception category (NEVER an uncertain GPU Issue)
static inline void QaD11GpuSatAdd(uint64_t& c) { if (c != 0xffffffffffffffffull) ++c; } // saturating: no wrap, no MAX+1
static void QaD11GpuTimingResolveOne() noexcept; // R4B DEFINITION (the forward decl above is the same entity)
// Worker-written; destroyed-path READ ONLY (atomic: the joint Destroy gate
// reads it under g_sharedMetaMutex while the worker writes it lock-free).
static std::atomic<bool> g_d11GpuObsUncertain{ false }; // unknown issue/release ownership latched

void QaD11GpuTimingReleaseAllRefsLockedStyle()
{
    // R3: per slot, each of the THREE refs is handled independently: at most
    // ONE Release attempt per ref; the pointer is cleared ONLY when that
    // Release returned normally (its return value is a COM refcount, NOT an
    // HRESULT, and need not be zero). A throwing Release is recorded as a
    // category count (g_d11GpuReleaseEx) — never an invented E_FAIL "HRESULT";
    // the still-held pointer is retained, uncertain ownership is latched and
    // nothing is ever retried or reset.
    for (uint32_t i = 0; i < kD11GpuObsSlots; ++i)
    {
        ID3D11Query* refs[3] = { g_d11GpuStart[i], g_d11GpuEnd[i], g_d11GpuDisjointQ[i] };
        for (uint32_t k = 0; k < 3u; ++k)
        {
            ID3D11Query* q = refs[k];
            if (!q) continue;
            bool released = false;
            try { q->Release(); released = true; }
            catch (...) { ++g_d11GpuReleaseEx; g_d11GpuObsUncertain = true; }
            if (released)
            {
                ++g_d11GpuRefsReleased;
                if (k == 0u) g_d11GpuStart[i] = nullptr;
                else if (k == 1u) g_d11GpuEnd[i] = nullptr;
                else g_d11GpuDisjointQ[i] = nullptr;
            }
            else
            {
                OutputDebugStringA("VRSIM D11 GPU timing: query ref retained (throwing release; never retried)\n");
            }
        }
    }
    bool anyHeld = false;
    for (uint32_t i = 0; i < kD11GpuObsSlots; ++i)
        if (g_d11GpuStart[i] || g_d11GpuEnd[i] || g_d11GpuDisjointQ[i]) anyHeld = true;
    g_d11GpuRefsOut = anyHeld;
    if (!anyHeld) g_d11GpuObsReady = false;
}

void QaD11GpuTimingWorkerInitBody(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    // Allocates the six owned refs ONCE on the existing worker device BEFORE
    // sampling and BEFORE any observer can hold a bracket. Any failure leaves
    // every successfully created ref retained (cleanup releases them exactly
    // once later) and disables the diagnostic; the original init result is
    // NEVER touched by this path.
    g_d11GpuInitDone = true;
    if (!g_d11GpuObsEnabled || g_d11GpuObsReady || g_d11GpuRefsOut) return;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_TIMESTAMP;
    qd.MiscFlags = 0;
    // CreateQuery writes its outparam BEFORE any possible throw: the target is
    // the persistent owned slot pointer itself, so a call exception can never
    // orphan a ref the callee already stored. The local view aliases the slot.
    ID3D11Query** q = nullptr;
    HRESULT hr = E_FAIL;
    const bool wasRefsOut = g_d11GpuRefsOut;
    for (uint32_t i = 0; i < kD11GpuObsSlots; ++i)
    {
        q = &g_d11GpuStart[i]; *q = nullptr;
        hr = dev->CreateQuery(&qd, q);
        if (*q) g_d11GpuRefsOut = true; // non-null outparam is RETAINED even on a failed/throwing call
        if (hr != S_OK || !*q)
        {
            g_d11GpuInitHr = (long long)hr; // REAL returned HRESULT
            g_d11GpuInitFailed = true; g_d11GpuObsEnabled = false; return;
        }
        q = &g_d11GpuEnd[i]; *q = nullptr;
        hr = dev->CreateQuery(&qd, q);
        if (*q) g_d11GpuRefsOut = true;
        if (hr != S_OK || !*q)
        {
            g_d11GpuInitHr = (long long)hr;
            g_d11GpuInitFailed = true; g_d11GpuObsEnabled = false; return;
        }
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        q = &g_d11GpuDisjointQ[i]; *q = nullptr;
        hr = dev->CreateQuery(&qd, q);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (*q) g_d11GpuRefsOut = true;
        if (hr != S_OK || !*q)
        {
            g_d11GpuInitHr = (long long)hr;
            g_d11GpuInitFailed = true; g_d11GpuObsEnabled = false; return;
        }
    }
    (void)q; (void)hr; (void)wasRefsOut;
    g_d11GpuObsReady = true;
    g_d11GpuInitHr = 0;
    g_d11GpuResolveCursor = 0u;
    for (uint32_t i = 0; i < kD11GpuObsSlots; ++i) g_d11GpuSlotState[i] = kD11GpuObsFree;
}

void QaD11GpuTimingWorkerInit(ID3D11Device* dev, ID3D11DeviceContext* ctx) noexcept
{
    // No-throw boundary. A CreateQuery CALL EXCEPTION is classified separately
    // (g_d11GpuInitThrew) from a real returned HRESULT (g_d11GpuInitHr): the
    // two are never conflated. Refs already created are retained for the
    // exactly-once cleanup release; nothing here touches the init result.
    try { QaD11GpuTimingWorkerInitBody(dev, ctx); }
    catch (...)
    {
        g_d11GpuInitThrew = true;
        g_d11GpuInitFailed = true; g_d11GpuObsEnabled = false;
        bool anyHeld = false;
        for (uint32_t i = 0; i < kD11GpuObsSlots; ++i)
            if (g_d11GpuStart[i] || g_d11GpuEnd[i] || g_d11GpuDisjointQ[i]) anyHeld = true;
        if (anyHeld) g_d11GpuRefsOut = true;
        // an initializer exception leaves ownership UNKNOWN: latch global
        // uncertainty, retain every persistent pointer, disable observation;
        // NO Release attempt here
        g_d11GpuObsReady = false;
        g_d11GpuObsUncertain = true;
    }
}
void QaD11GpuTimingFreezeAndResetBody()
{
    // Frozen EXACTLY ONCE on the original legal Create (worker not yet
    // created). R3 ORDER: FIRST probe any retained ref / uncertain ownership
    // / open-or-retired slot from a prior owner. If anything is held the
    // freeze changes NOTHING except forcing OFF — records, masks, slot
    // states and the uncertain latch are NEVER cleared, and no pointer is
    // reset. Only a genuinely safe-idle legal Create may reset session state.
    bool notIdle = g_d11GpuObsUncertain.load();
    for (uint32_t i = 0; i < kD11GpuObsSlots; ++i)
    {
        if (g_d11GpuStart[i] || g_d11GpuEnd[i] || g_d11GpuDisjointQ[i]) notIdle = true;
        if (g_d11GpuSlotState[i] != kD11GpuObsFree) notIdle = true;
    }
    if (g_d11GpuRefsOut) notIdle = true;
    g_d11GpuAnySlotHeld = notIdle;
    if (notIdle)
    {
        g_d11GpuObsEnabled = false;
        g_d11GpuObsReady = false;
        OutputDebugStringA("VRSIM D11 GPU timing: prior owner not idle; OFF, state retained\n");
        return;
    }
    g_d11GpuObsEnabled = false;
    g_d11GpuInitDone = false; g_d11GpuInitFailed = false; g_d11GpuInitThrew = false; g_d11GpuInitHr = 0;
    g_d11GpuObsReady = false; g_d11GpuCleanupDone = false; g_d11GpuFinalized = false;
    g_d11GpuResolveCursor = 0u;
    g_d11GpuSampleOrdinal = 0ull;
    g_d11GpuRecordCount = 0u; g_d11GpuOverflow = 0u;
    g_d11GpuEligible = 0ull; g_d11GpuSampled = 0ull; g_d11GpuSkipBusy = 0ull;
    g_d11GpuSkipLimit = 0ull; g_d11GpuUnsupported = 0ull; g_d11GpuIssueEx = 0ull;
    g_d11GpuResolveFail = 0ull; g_d11GpuDisjoint = 0ull; g_d11GpuValid = 0ull;
    g_d11GpuIncomplete = 0ull; g_d11GpuGetDataCalls = 0ull; g_d11GpuRefsReleased = 0ull;
    g_d11GpuReleaseEx = 0ull;
    for (uint32_t i = 0; i < kD11GpuObsSlots; ++i) g_d11GpuSlotState[i] = kD11GpuObsFree;
    g_d11GpuObsUncertain = false;
    g_d11GpuRefsOut = false;
    if (!g_qaTraceEnabled) return; // sampling only when the original trace is on
    wchar_t buf[16];
    const DWORD n = GetEnvironmentVariableW(L"VRSIM_QA_D11_OUTPUT_GPU_TIMING", buf,
                                            (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n != 1 || buf[0] != L'1') return; // absent/overlong/other: OFF
    g_d11GpuObsEnabled = true;
    LogLine("D11GT", "armed once for this session; stage=D11_RENDER_SHARED_PAIR_CALL; every16th eligible attempt; not a no-profiler baseline; not 120 FPS");
}

void QaD11GpuTimingFreezeAndReset() noexcept
{
    try { QaD11GpuTimingFreezeAndResetBody(); }
    catch (...)
    {
        g_d11GpuObsEnabled = false;
        try { LogLine("D11GT", "freeze threw: D11 output GPU timing disabled for this session"); }
        catch (...) { OutputDebugStringA("VRSIM D11 GPU timing: freeze failed; disabled\n"); }
    }
}

// Bracket helpers run ONLY on the shared consumer worker thread between the
// granted BeginRead and the mandatory EndRead. All are no-throw at their
// boundary: a diagnostic failure can NEVER make the original render catch
// report a RenderSharedPair throw and can NEVER skip the EndRead.
bool QaD11GpuTimingBracketStart(const VRSIM::SharedPreparedPairV1& ticket,
                                const VRSIM::SharedCaptureIdentityV1& id,
                                int slot, bool valid, ID3D11ShaderResourceView* srvL,
                                ID3D11ShaderResourceView* srvR,
                                QaD11GpuObsHandleV1& outH)
{
    // Eligibility (ALL of): env+trace enabled, observer ready, original
    // BeginRead GRANTED (this path only runs after S_OK), original valid
    // identity, nonnull borrowed SRVs, no observed stop/closing, current
    // mixed 4096x4096 output with the swapchain format actually
    // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB (91; the actual source shared plane is
    // 88, staging 92, its SRGB SRV 93 — 91 is required of the OUTPUT ONLY;
    // requiring it of the source would silently skip every intended sample),
    // and a free observer slot. Busy/limit/unsupported issue ZERO query
    // commands and count explicitly. On success fills outH with the reserved
    // record + slot; any false return leaves the handle invalid (no sample,
    // no command). The ENTIRE body is wrapped: a lock/POD/clock/metadata
    // failure can never escape into the original render catch, can never
    // make it report a RenderSharedPair throw, and can never skip EndRead.
    QaD11GpuObsInvalidateHandle(outH);
    bool started = false;
    bool reserved = false;   // the record/slot reservation completed
    bool issueStarted = false; // from here on a throw means UNKNOWN GPU issue ownership
    bool metaThrew = false;
    bool chkIncomplete = false;
    uint32_t recIdx = 0u;
    uint32_t obsIdx = 0u;
    uint32_t maskBefore = 0u;
    uint64_t ordBefore = 0ull;
    try
    {
        do
        {
            if (!g_d11GpuObsEnabled || !g_d11GpuObsReady) break;
            if (g_stopRequested.load() || g_d11GpuFinalized) break;
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                if (g_sharedClosing || g_sharedTeardownRequested) break;
            }
            if (!valid || !srvL || !srvR) break;
            if (g_swapW != 4096u || g_swapH != 4096u ||
                (uint64_t)g_swapFormat != (uint64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            { QaD11GpuSatAdd(g_d11GpuUnsupported); break; }
            // Checked preflight on EVERY counter this path may advance: a
            // UINT64_MAX ceiling latches incomplete and disables new samples
            // instead of wrapping.
            bool stopCnt = false;
            auto chkAdd = [&](uint64_t& c) -> bool {
                if (c == 0xffffffffffffffffull)
                { QaD11GpuSatAdd(g_d11GpuIncomplete); g_d11GpuObsEnabled = false; stopCnt = true; return false; }
                return true;
            };
            if (!chkAdd(g_d11GpuEligible)) break;
            ++g_d11GpuEligible;
            if (g_d11GpuEligible % kD11GpuSampleEvery != 0ull) break; // every 16th
            if (g_d11GpuRecordCount >= kD11GpuObsCapacity || g_d11GpuOverflow > 0u)
            {
                if (chkAdd(g_d11GpuSkipLimit)) ++g_d11GpuSkipLimit;
                if (!stopCnt)
                {
                    if (chkAdd(g_d11GpuIncomplete)) ++g_d11GpuIncomplete;
                    if (!stopCnt) g_d11GpuObsEnabled = false; // capacity: no new samples; pending may still resolve
                }
                break;
            }
            int obs = -1;
            for (uint32_t i = 0; i < kD11GpuObsSlots; ++i)
                if (g_d11GpuSlotState[i] == kD11GpuObsFree) { obs = (int)i; break; }
            if (obs < 0) { if (chkAdd(g_d11GpuSkipBusy)) ++g_d11GpuSkipBusy; break; } // pending NEVER reused
            ID3D11DeviceContext* ctx = g_ctx;
            ID3D11Query* djq = g_d11GpuDisjointQ[obs];
            ID3D11Query* stq = g_d11GpuStart[obs];
            ID3D11Query* enq = g_d11GpuEnd[obs];
            if (!ctx || !djq || !stq || !enq) { QaD11GpuSatAdd(g_d11GpuUnsupported); break; }
            if (!chkAdd(g_d11GpuSampleOrdinal)) break;
            chkIncomplete = true; // incomplete-counter ceiling checked via chkAdd from here on
            // RESERVE the record BEFORE the first Issue: identity frozen
            // (ticket/id/worker thread/device key), real output format read
            // here (no GetDesc after any Issue), masks initialized, and
            // recordCount advanced EXACTLY ONCE (close never advances it).
            recIdx = g_d11GpuRecordCount; obsIdx = (uint32_t)obs;
            QaD11GpuObsRecordV1& r = g_d11GpuRecords[recIdx];
            memset(&r, 0, sizeof(r));
            r.recordIndex = recIdx; r.stage = kD11GpuStageRenderSharedPair;
            r.obsSlot = obsIdx; r.leaseSlot = (uint32_t)slot;
            r.ticket = (unsigned long long)ticket.ticket;
            r.epoch = (unsigned long long)ticket.epoch;
            r.generation = (unsigned long long)ticket.generation;
            r.frameSerial = (unsigned long long)id.frameSerial; // joins CSV activeFrameSerial
            r.poseSerial = (unsigned long long)id.poseSerial;   // joins CSV layerPoseSerial
            r.captureBeginQpc = (long long)id.captureBeginQpc;  // joins CSV idCaptureBeginQpc
            r.workerThreadId = (unsigned long long)GetCurrentThreadId();
            r.deviceKey = (unsigned long long)(uintptr_t)g_dev;
            r.srcPlaneFormat = ticket.format88;                 // 88 SOURCE plane
            uint32_t outFmt = 0u;
            try
            {
                D3D11_TEXTURE2D_DESC od{};
                ID3D11Texture2D* ot = g_gpuPass.Output(0);
                if (ot) { ot->GetDesc(&od); outFmt = (uint32_t)od.Format; }
            }
            catch (...) { outFmt = 0u; QaD11GpuSatAdd(g_d11GpuMetaEx); } // unreadable real-output metadata is a PRE-Issue category, never an Issue exception
            r.outFormat = outFmt;
            r.planeWidth = ticket.width; r.planeHeight = ticket.height;
            r.gpuStartTicks = kD11GpuUnknownTicks; r.gpuEndTicks = kD11GpuUnknownTicks;
            r.gpuFreqTicks = kD11GpuUnknownTicks; r.disjointValue = -1; // explicit unknown
            r.obsState = kD11GpuObsBegun;
            LARGE_INTEGER t;
            if (QueryPerformanceCounter(&t)) { r.cpuBeginQpc = (long long)t.QuadPart; r.qpcMask |= kD11GpuQpcBegin; }
            g_d11GpuSlotRec[obsIdx] = recIdx;
            g_d11GpuSlotState[obsIdx] = kD11GpuObsBegun;
            ++g_d11GpuRecordCount; // once and only once per reserved sample
            reserved = true;
            // Commands: each void Begin/End counts as issued ONLY after it
            // returned normally; the mask is written to the record right away
            // so a partial issue retains the exact truth. TIMESTAMP uses End
            // ONLY, never Begin.
            issueStarted = true; // any throw from here is UNKNOWN GPU issue ownership
            ctx->Begin(djq);
            maskBefore = kD11GpuIssueDisjBegun;
            r.issueMask = maskBefore;
            ordBefore = g_d11GpuSampleOrdinal;
            ctx->End(stq);
            maskBefore = kD11GpuIssueDisjBegun | kD11GpuIssueStartEnd;
            r.issueMask = maskBefore;
            if (chkAdd(g_d11GpuSampled)) ++g_d11GpuSampled;
            g_d11GpuSampleOrdinal = g_d11GpuSampleOrdinal + 1ull;
            outH.slot = obsIdx; outH.record = recIdx;
            started = true;
        } while (false);
    }
    catch (...)
    {
        if (!reserved)
        {
            // PRE-Issue metadata/lock exception: slot0/record0 are NEVER
            // written, no GPU issue ownership is claimed uncertain; this is
            // a category-counted metadata failure that only disables new
            // samples. No command ran; nothing is pending or retained.
            ++g_d11GpuMetaEx;
            g_d11GpuObsEnabled = false;
            metaThrew = true;
        }
        else if (!issueStarted)
        {
            // reserved but NOT yet issue-started: metadata failure between
            // reservation and the first command. The record stays reserved
            // and invalid; the observer slot is permanently RETIRED (never
            // back to the free pool, never re-issued) because its pairing
            // can no longer be trusted. No GPU command ran; no uncertain
            // issue ownership is fabricated.
            ++g_d11GpuMetaEx;
            g_d11GpuObsEnabled = false;
            g_d11GpuSlotState[obsIdx] = kD11GpuObsRetired;
            g_d11GpuRecords[recIdx].obsState = kD11GpuObsRetired;
            g_d11GpuRecords[recIdx].reason = 5u;
            QaD11GpuSatAdd(g_d11GpuIncomplete);
            metaThrew = true;
        }
        else
        {
            // UNKNOWN issue ownership mid-Issue: the reserved record and a
            // VALID handle stay RETAINED; the single Close point still runs
            // on exactly the known-issued commands; refs/context retained;
            // new samples disabled.
            QaD11GpuSatAdd(g_d11GpuIssueEx);
            g_d11GpuObsUncertain = true;
            g_d11GpuObsEnabled = false;
            g_d11GpuSlotState[obsIdx] = kD11GpuObsOpenX;
            g_d11GpuSlotRec[obsIdx] = recIdx;
            QaD11GpuObsRecordV1& r2 = g_d11GpuRecords[recIdx];
            r2.issueMask = maskBefore; // exact partial truth retained
            r2.obsState = kD11GpuObsOpenX;
            r2.reason = 5u;            // partial/unknown bracket: permanently invalid
            outH.slot = obsIdx; outH.record = recIdx;
            started = true; // Close MUST run on this exact handle before EndRead
        }
    }
    if (!started)
    {
        if (metaThrew && !issueStarted && chkIncomplete)
        {
            // pre-Issue/retired metadata failure pre-Close: counted incomplete
            // here; the retained issueStarted path is counted once by Close.
            QaD11GpuSatAdd(g_d11GpuIncomplete);
        }
        return false;
    }
    return true;
}

void QaD11GpuTimingClose(const QaD11GpuObsHandleV1& h, long long renderHr,
                         bool renderReturned, bool renderThrew)
{
    // R3B SINGLE close point: runs on normal return, HRESULT failure AND the
    // original catch, BEFORE the original EndRead. Consumes the EXACT handle
    // Start produced (never a bool, never a scan guessing the association).
    // Closes ONLY commands known issued (mask-derived), each exactly once;
    // never retries an unknown Issue and never invents success for a void
    // call that may have thrown. The real RenderSharedPair HRESULT is stored
    // only when it actually returned; a throw sets only the threw flag (no
    // fabricated E_FAIL). QPC end counts only when the call succeeded. The
    // EndRead stamp is R5 scope. Whole body no-throw: nothing escapes into
    // the original render catch and EndRead is never skipped.
    bool chkIncomplete = false;
    try
    {
        auto chkAdd = [&](uint64_t& c) -> bool {
            if (c == 0xffffffffffffffffull)
            { g_d11GpuObsEnabled = false; return false; }
            return true;
        };
        chkIncomplete = chkAdd(g_d11GpuIncomplete);
        if (h.slot >= kD11GpuObsSlots || h.record >= kD11GpuObsCapacity) return;
        const uint32_t obs = h.slot;
        if (g_d11GpuSlotState[obs] != kD11GpuObsBegun &&
            g_d11GpuSlotState[obs] != kD11GpuObsOpenX) return;
        if (g_d11GpuSlotRec[obs] != h.record) return; // handle must match the reservation
        QaD11GpuObsRecordV1* rp = &g_d11GpuRecords[h.record];
        if (rp->recordIndex != h.record || rp->obsSlot != obs) return;
        uint32_t mask = rp->issueMask; // the EXACT partial mask Start left
        const bool priorOpenX = (g_d11GpuSlotState[obs] == kD11GpuObsOpenX);
        ID3D11DeviceContext* ctx = g_ctx;
        bool endThrew = false;
        if (ctx && g_d11GpuEnd[obs] && (mask & kD11GpuIssueStartEnd) != 0u &&
            (mask & kD11GpuIssueEndEnd) == 0u)
        {
            // end TIMESTAMP: End ONLY; counts as issued only on normal return
            try { ctx->End(g_d11GpuEnd[obs]); mask |= kD11GpuIssueEndEnd; }
            catch (...) { endThrew = true; }
        }
        bool disjThrew = false;
        if (ctx && g_d11GpuDisjointQ[obs] && (mask & kD11GpuIssueDisjBegun) != 0u &&
            (mask & kD11GpuIssueDisjEnd) == 0u)
        {
            try { ctx->End(g_d11GpuDisjointQ[obs]); mask |= kD11GpuIssueDisjEnd; }
            catch (...) { disjThrew = true; }
        }
        LARGE_INTEGER t;
        const bool qpcOk = QueryPerformanceCounter(&t) ? true : false;
        rp->issueMask = mask; // partial/throw truth recorded immediately
        // the ACTUAL HRESULT only when the render call returned; a non-return
        // is explicit UNKNOWN, never the caller's synthetic E_FAIL
        rp->renderHr = renderReturned ? renderHr : (long long)kD11GpuUnknownTicks;
        rp->renderReturned = renderReturned ? 1u : 0u;
        rp->renderThrew = renderThrew ? 1u : 0u;
        rp->validity = 0u; // nothing is valid before the (R4) resolve
        if (qpcOk) { rp->cpuEndQpc = (long long)t.QuadPart; rp->qpcMask |= kD11GpuQpcEnd; }
        bool stateSet = false;
        if (endThrew || disjThrew)
        {
            // UNKNOWN issue ownership at close: refs/context retained, slot
            // stays an open-unknown bracket (NEVER reusable, never a
            // cleanly-closed state), new samples disabled.
            QaD11GpuSatAdd(g_d11GpuIssueEx);
            g_d11GpuObsUncertain = true;
            g_d11GpuObsEnabled = false;
            if (rp->reason == 0u) rp->reason = 5u;
            rp->obsState = kD11GpuObsOpenX;
            g_d11GpuSlotState[obs] = kD11GpuObsOpenX;
            stateSet = true;
        }
        else if (priorOpenX)
        {
            // uncertain-issue bracket: OpenX/uncertain stays STICKY even if
            // every known-open command closed cleanly here. Never handed to
            // the resolve path, never reused, never a closed-success state;
            // the refs/context remain until the original failure/process-exit
            // path (R5 cleanup).
            if (rp->reason == 0u) rp->reason = 5u;
            rp->obsState = kD11GpuObsOpenX;
            g_d11GpuSlotState[obs] = kD11GpuObsOpenX;
            stateSet = true;
        }
        else if ((mask & kD11GpuIssueStartEnd) != 0u && (mask & kD11GpuIssueEndEnd) != 0u)
        {
            // both timestamps issued: the bracket may await the one async
            // DONOTFLUSH resolve (R4). Partial-known issue (start never
            // returned) has NO pending data: the invalid record stays
            // retained, the slot is never re-issued, and new samples are
            // disabled for the rest of the owner.
            rp->obsState = kD11GpuObsPending;
            g_d11GpuSlotState[obs] = kD11GpuObsPending;
            stateSet = true;
        }
        else
        {
            if (rp->reason == 0u) rp->reason = 5u;
            g_d11GpuObsEnabled = false;
        }
        if (!stateSet)
        {
            rp->obsState = kD11GpuObsOpenX;
            g_d11GpuSlotState[obs] = kD11GpuObsOpenX;
        }
        // invalid/partial brackets count once; success start kept its masks.
        const bool fullIssue = (mask == kD11GpuIssueFull);
        const bool okRender = renderReturned && !renderThrew && renderHr == (long long)S_OK;
        if ((!fullIssue || !okRender || rp->reason != 0u || g_d11GpuSlotState[obs] != kD11GpuObsPending) &&
            chkIncomplete) ++g_d11GpuIncomplete;
        // recordCount is NEVER advanced here: Start advanced it exactly once.
    }
    catch (...)
    {
        // the close itself faulted: unknown ownership — latch, disable new
        // samples, retain everything; the mandatory original EndRead still
        // runs (this helper never throws outward).
        QaD11GpuSatAdd(g_d11GpuIssueEx);
        g_d11GpuObsUncertain = true;
        g_d11GpuObsEnabled = false;
    }
}
// R4B ResolveOne: at most ONE GetData TOTAL per call, on the ORIGINAL worker
// thread/context only, ALWAYS D3D11_ASYNC_GETDATA_DONOTFLUSH. Fixed pending-
// slot round-robin + per-slot field cursor (DISJOINT -> start TIMESTAMP ->
// end TIMESTAMP); S_FALSE simply retains the slot for a later normal
// opportunity. A FAILED or THROWING GetData permanently RETIRES that
// observer slot until proven worker cleanup: never re-issued, never reused,
// never overwritten while unresolved. Open/uncertain (Begun/OpenX/Retired)
// and stopping/finalized states are skipped. No wait, no Flush, no
// allocation, no logging in the D3D path. A GetData failure never proves or
// disproves the original bridge EVENT. The normal-loop CALLSITE is R5 scope.
// no-throw no-IO boundary. Bounds + exact slot/record association only: pure
// POD, no COM, no lock, no allocation, never frees the observer slot. A
// non-returning EndRead is UNKNOWN — the synthetic E_FAIL the original catch
// assigns is NEVER stored as a real HRESULT.
void QaD11GpuTimingStamp(const QaD11GpuObsHandleV1& h, long long ehr,
                         bool endReadReturned, bool endReadThrew) noexcept
{
    try
    {
        if (g_d11GpuFinalized) return;
        if (h.slot >= kD11GpuObsSlots || h.record >= kD11GpuObsCapacity) return;
        QaD11GpuObsRecordV1* rp = &g_d11GpuRecords[h.record];
        if (rp->recordIndex != h.record || rp->obsSlot != h.slot) return;
        if (rp->obsState != kD11GpuObsPending && rp->obsState != kD11GpuObsOpenX) return;
        rp->endReadReturned = endReadReturned ? 1u : 0u;
        rp->endReadThrew = endReadThrew ? 1u : 0u;
        if (endReadReturned)
        {
            rp->endReadHr = ehr;         // REAL returned HRESULT
            if (ehr != (long long)S_OK && rp->reason == 0u) rp->reason = 6u;
        }
        else
        {
            rp->endReadHr = kD11GpuUnknownTicks; // UNKNOWN: returned=false
            if (rp->reason == 0u) rp->reason = 6u;
            QaD11GpuSatAdd(g_d11GpuIncomplete);
        }
    }
    catch (...) { QaD11GpuSatAdd(g_d11GpuIncomplete); }
}

void QaD11GpuTimingResolveOne() noexcept
{
    uint32_t chosen = 0xffffffffu;
    try
    {
        if (g_d11GpuFinalized) return;
        if (g_stopRequested.load()) return; // no resolve during an observed stop/closing
        if (g_d11GpuObsUncertain.load()) return; // R5: global uncertain latches refuse resolve
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            if (g_sharedClosing || g_sharedTeardownRequested) return;
        }
        for (uint32_t n = 0; n < kD11GpuObsSlots; ++n)
        {
            const uint32_t i = (g_d11GpuResolveCursor + n) % kD11GpuObsSlots;
            if (g_d11GpuSlotState[i] == kD11GpuObsPending)
            {
                chosen = i;
                g_d11GpuResolveCursor = (i + 1u) % kD11GpuObsSlots;
                break;
            }
        }
        if (chosen >= kD11GpuObsSlots) return;
        const uint32_t rec = g_d11GpuSlotRec[chosen];
        bool slotUsable = (rec < kD11GpuObsCapacity);
        QaD11GpuObsRecordV1* rp = slotUsable ? &g_d11GpuRecords[rec] : nullptr;
        if (rp && (rp->recordIndex != rec || rp->obsSlot != chosen ||
                   rp->obsState != kD11GpuObsPending)) slotUsable = false, rp = nullptr;
        if (!slotUsable)
        {
            // broken pairing: retire, never guess-recover
            g_d11GpuSlotState[chosen] = kD11GpuObsRetired;
            QaD11GpuSatAdd(g_d11GpuResolveFail);
            QaD11GpuSatAdd(g_d11GpuIncomplete);
            return;
        }
        ID3D11DeviceContext* ctx = g_ctx;
        if (!ctx) return; // no context: retain the pending pairing untouched
        ID3D11Query* dq = g_d11GpuDisjointQ[chosen];
        ID3D11Query* stq = g_d11GpuStart[chosen];
        ID3D11Query* enq = g_d11GpuEnd[chosen];
        if (!dq || !stq || !enq) return; // refs retained; nothing issued here
        const uint32_t f = g_d11GpuFieldCursor[chosen] % 3u;
        const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        HRESULT hr = E_UNEXPECTED;
        const bool* pGot = nullptr; uint32_t needBit = 0u;
        if (f == 0u)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{}; // FULL struct size
            hr = ctx->GetData(dq, &dd, sizeof(dd), flags);
            QaD11GpuSatAdd(g_d11GpuGetDataCalls);
            rp->hrDisjoint = (long long)hr; rp->hrMask |= 0x2u; // hr actually obtained
            if (hr == S_FALSE) return; // pending: retain slot/field cursor
            if (hr != S_OK) goto retire;
            rp->gpuFreqTicks = (uint64_t)dd.Frequency;
            rp->disjointValue = dd.Disjoint ? 1ll : 0ll;
            rp->dataMask |= kD11GpuDataFreq;
            g_d11GpuFieldCursor[chosen] = 1u;
            return;
        }
        if (f == 1u)
        {
            UINT64 ts = 0ull; // exact timestamp size
            hr = ctx->GetData(stq, &ts, sizeof(ts), flags);
            QaD11GpuSatAdd(g_d11GpuGetDataCalls);
            rp->hrEndStart = (long long)hr; rp->hrMask |= 0x1u;
            if (hr == S_FALSE) return;
            if (hr != S_OK) goto retire;
            rp->gpuStartTicks = ts; rp->dataMask |= kD11GpuDataStart;
            g_d11GpuFieldCursor[chosen] = 2u;
            return;
        }
        {
            UINT64 te = 0ull;
            hr = ctx->GetData(enq, &te, sizeof(te), flags);
            QaD11GpuSatAdd(g_d11GpuGetDataCalls);
            rp->hrEndEnd = (long long)hr; rp->hrMask |= 0x4u;
            if (hr == S_FALSE) return;
            if (hr != S_OK) goto retire;
            rp->gpuEndTicks = te; rp->dataMask |= kD11GpuDataEnd;
        }
        // all three fields obtained: validate the FROZEN record only — the
        // pending sample is NEVER matched against a newer active frame.
        {
            LARGE_INTEGER t;
            if (QueryPerformanceCounter(&t)) { rp->readyQpc = (long long)t.QuadPart; rp->qpcMask |= kD11GpuQpcReady; }
        }
        {
            bool okId = (rp->issueMask == kD11GpuIssueFull) &&
                        (rp->dataMask == kD11GpuDataFull) &&
                        rp->hrMask == 0x7u && // query-HRESULT bits ONLY (start/disjoint/end)
                        rp->ticket != 0ull && rp->epoch != 0ull && rp->generation != 0ull &&
                        rp->frameSerial != 0ull && rp->poseSerial != 0ull &&
                        rp->deviceKey == (unsigned long long)(uintptr_t)g_dev &&
                        rp->workerThreadId == (unsigned long long)GetCurrentThreadId();
            uint32_t why = 1u; // 1 identity/issue/data
            if (okId)
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                if (rp->epoch != (unsigned long long)g_sharedBoundEpoch ||
                    rp->generation != (unsigned long long)g_sharedBoundGeneration) okId = false;
            }
            // R5: frozen configuration + REAL per-query HRESULTs, checked while
            // the window is still open so a mismatch is classified why=1
            if (okId && (rp->stage != kD11GpuStageRenderSharedPair || rp->reason != 0u ||
                         rp->leaseSlot >= (uint32_t)RING_SLOTS ||
                         rp->srcPlaneFormat != 88u ||
                         rp->outFormat != 91u ||
                         rp->planeWidth != 4096u || rp->planeHeight != 4096u ||
                         rp->hrDisjoint != (long long)S_OK ||
                         rp->hrEndStart != (long long)S_OK ||
                         rp->hrEndEnd != (long long)S_OK))
                okId = false, why = 1u;
            const bool okRender = rp->renderReturned == 1u && rp->renderThrew == 0u &&
                                  (rp->issueMask & kD11GpuIssueFull) == kD11GpuIssueFull &&
                                  rp->renderHr == (long long)S_OK; // REAL S_OK, no throw
            if (okId && !okRender) okId = false, why = 5u;
            const bool okEnd = rp->endReadReturned == 1u && rp->endReadThrew == 0u &&
                               rp->endReadHr == (long long)S_OK; // R5 stamp sets these; unset = INVALID
            if (okId && !okEnd) okId = false, why = 6u;
            if (okId && rp->disjointValue != 0ll) { QaD11GpuSatAdd(g_d11GpuDisjoint); okId = false, why = 2u; }
            if (okId && (rp->gpuFreqTicks == 0ull || rp->gpuFreqTicks == kD11GpuUnknownTicks ||
                         rp->gpuFreqTicks > 0x7fffffffffffffffull)) okId = false, why = 3u;
            if (okId && rp->gpuEndTicks < rp->gpuStartTicks) okId = false, why = 4u; // unordered raw ticks
            // clock evidence: only already-retained facts are classified here;
            // unknown/failing clock evidence stays raw and invalid, never 0ms
            if (okId && (rp->gpuStartTicks == kD11GpuUnknownTicks ||
                         rp->gpuEndTicks == kD11GpuUnknownTicks ||
                         rp->captureBeginQpc <= 0ll || rp->qpcMask != 0x7u ||
                         rp->cpuBeginQpc <= 0ll ||
                         rp->cpuEndQpc < rp->cpuBeginQpc ||
                         rp->readyQpc < rp->cpuEndQpc))
                okId = false, why = 7u;
            rp->resolved = 1u;
            rp->validity = okId ? 1u : 0u;
            rp->reason = okId ? 0u : why; // invalid stays raw/never 0ms; delta uses THIS record's own frequency
            if (okId) QaD11GpuSatAdd(g_d11GpuValid); else QaD11GpuSatAdd(g_d11GpuIncomplete);
            (void)pGot; (void)needBit;
        }
        rp->obsState = kD11GpuObsResolved; // POD now immutable; retained for export
        g_d11GpuSlotState[chosen] = kD11GpuObsFree; // frees ONLY the observer slot, NEVER the lease
        g_d11GpuSlotRec[chosen] = 0xffffffffu;
        g_d11GpuFieldCursor[chosen] = 0u;
        return;
      retire:
        QaD11GpuSatAdd(g_d11GpuResolveFail);
        rp->resolved = 1u; rp->validity = 0u;
        if (rp->reason == 0u) rp->reason = 7u; // data-incomplete/failed GetData
        rp->obsState = kD11GpuObsRetired;
        g_d11GpuSlotState[chosen] = kD11GpuObsRetired;
        QaD11GpuSatAdd(g_d11GpuIncomplete);
        return;
    }
    catch (...)
    {
        // a throwing GetData retires the chosen slot permanently; it never
        // proves/disproves the original EVENT and never fabricates completion
        QaD11GpuSatAdd(g_d11GpuResolveFail);
        if (chosen < kD11GpuObsSlots)
        {
            g_d11GpuSlotState[chosen] = kD11GpuObsRetired;
            const uint32_t rec2 = g_d11GpuSlotRec[chosen];
            if (rec2 < kD11GpuObsCapacity)
            {
                QaD11GpuObsRecordV1& r2 = g_d11GpuRecords[rec2];
                if (r2.obsState == kD11GpuObsPending)
                {
                    r2.obsState = kD11GpuObsRetired; r2.resolved = 1u; r2.validity = 0u;
                    if (r2.reason == 0u) r2.reason = 7u;
                }
            }
        }
        QaD11GpuSatAdd(g_d11GpuIncomplete);
    }
}
// R4B serializer header/meta/counters; record lines + footer + definition
// close are appended at the R4B MARKER below.
static std::string QaD11GpuTimingResolveMetaLines()
{
    std::string m;
    auto u = [](unsigned long long v) { return std::to_string(v); };
    auto s = [](long long v) { return std::to_string(v); };
    m += "#d11GpuTimingSchema=1\n";
    m += "#d11GpuTimingStage=D11_RENDER_SHARED_PAIR_CALL\n";
    m += "#d11GpuTimingScope=OWNER_LIFETIME_ELIGIBLE_SAMPLED_OUTPUT_ATTEMPTS;"
         "owner-lifetime;strict120-subset-requires-explicit-frame/QPC-join-after-run\n";
    m += "#d11GpuTimingCoverage=brackets-original-RenderSharedPair-dispatch-only;"
         "normal-srgb91-pass=two-88to92-staging-CopyResource+two-projection-Draw+CB/bind-dispatch-gaps;"
         "excludes-D9-source-copies;excludes-opaque-SDK-SurfaceRender;"
         "excludes-final-swapchain-CopyResource/Wait/Release;excludes-compositor/OVR;"
         "excludes-whole-stereo-frame-latency;not-isolated-shader-busy-time;not-machine-utilization;"
         "not-no-profiler-baseline;not-120FPS-proof\n";
    m += "#d11GpuTimingClocks=gpuTicks=raw-same-output-device-TIMESTAMP-values;"
         "per-record-own-frequency-ONLY;validDelta=poststop-reader-computed-from-THAT-record-frequency;"
         "QPC/GPU/cross-device-clocks-NEVER-subtracted\n";
    m += "#d11GpuTimingJoin=recordFrameSerial->csv.activeFrameSerial(NOT-csv-frameSerial);"
         "recordPoseSerial->csv.layerPoseSerial;recordEpoch->csv.idEpoch;"
         "recordGeneration->csv.idGeneration;recordCaptureBeginQpc->csv.idCaptureBeginQpc\n";
    m += "#d11GpuTiming=enabled=" + u(g_d11GpuObsEnabled ? 1ull : 0ull) +
         ",initDone=" + u(g_d11GpuInitDone ? 1ull : 0ull) +
         ",initFailed=" + u(g_d11GpuInitFailed ? 1ull : 0ull) +
         ",initThrew=" + u(g_d11GpuInitThrew ? 1ull : 0ull) +
         ",realInitHr=" + s(g_d11GpuInitHr) +
         ",ready=" + u(g_d11GpuObsReady ? 1ull : 0ull) +
         ",uncertain=" + u(g_d11GpuObsUncertain.load() ? 1ull : 0ull) +
         ",eligible=" + u(g_d11GpuEligible) +
         ",sampled=" + u(g_d11GpuSampled) +
         ",sampleOrdinal=" + u(g_d11GpuSampleOrdinal) +
         ",skippedBusy=" + u(g_d11GpuSkipBusy) +
         ",skippedLimit=" + u(g_d11GpuSkipLimit) +
         ",unsupported=" + u(g_d11GpuUnsupported) +
         ",issueException=" + u(g_d11GpuIssueEx) +
         ",metaException=" + u(g_d11GpuMetaEx) +
         ",resolveFailure=" + u(g_d11GpuResolveFail) +
         ",disjoint=" + u(g_d11GpuDisjoint) +
         ",valid=" + u(g_d11GpuValid) +
         ",incomplete=" + u(g_d11GpuIncomplete) +
         ",overflow=" + u(g_d11GpuOverflow) +
         ",getDataCalls=" + u(g_d11GpuGetDataCalls) +
         ",capacity=" + u(kD11GpuObsCapacity) + "\n";
    m += "#d11GpuTimingRefs=refsOut=" + u(g_d11GpuRefsOut ? 1ull : 0ull) +
         ",refsReleased=" + u(g_d11GpuRefsReleased) +
         ",releaseException=" + u(g_d11GpuReleaseEx) +
         ",cleanupDone=" + u(g_d11GpuCleanupDone ? 1ull : 0ull) +
         ",finalized=" + u(g_d11GpuFinalized ? 1ull : 0ull) + "\n";
    // R4B MARKER R4B2
    return m;
}
// R4B per-record serializer: one #d11GpuTimingRecord line for EVERY retained
// POD record (once-only cached serialization during the existing export;
// zero COM/D3D/SDK calls). Raw GPU values are emitted verbatim; the
// 0x8000000000000000 sentinel / disjoint=-1 stay explicit UNKNOWN — never a
// fabricated 0ms. Completeness footer: emitted/expected/overflow/incomplete
// + reference/finalization state.
static std::string QaD11GpuTimingRecordLines()
{
    const std::string off = "#d11GpuTiming=unavailable:not-armed\n";
    if (!g_d11GpuObsEnabled && g_d11GpuRecordCount == 0u && !g_d11GpuRefsOut &&
        !g_d11GpuInitDone && !g_d11GpuInitFailed && g_d11GpuMetaEx == 0ull)
        return off; // observer genuinely never armed
    auto u = [](unsigned long long v) { return std::to_string(v); };
    auto s = [](long long v) { return std::to_string(v); };
    std::string m;
    const uint32_t rc = g_d11GpuRecordCount;
    for (uint32_t i = 0; i < rc; ++i)
    {
        const QaD11GpuObsRecordV1& r = g_d11GpuRecords[i];
        std::string t = "stage=" + u(r.stage);
        t += ",obsSlot=" + u(r.obsSlot) + ",leaseSlot=" + u(r.leaseSlot) + ",obsState=" + u(r.obsState);
        t += ",ticket=" + u(r.ticket) + ",epoch=" + s((long long)r.epoch) + ",generation=" + u(r.generation);
        t += ",frameSerial=" + u(r.frameSerial) + ",poseSerial=" + u(r.poseSerial) + ",captureBeginQpc=" + s(r.captureBeginQpc);
        t += ",workerThreadId=" + u(r.workerThreadId) + ",deviceKey=" + u(r.deviceKey);
        t += ",srcPlaneFormat=" + u(r.srcPlaneFormat) + ",outFormat=" + u(r.outFormat);
        t += ",plane=" + u(r.planeWidth) + "x" + u(r.planeHeight);
        t += ",cpuBeginQpc=" + s(r.cpuBeginQpc) + ",cpuEndQpc=" + s(r.cpuEndQpc) + ",readyQpc=" + s(r.readyQpc);
        t += ",qpcMask=" + u(r.qpcMask) + ",issueMask=" + u(r.issueMask) + ",dataMask=" + u(r.dataMask) + ",hrMask=" + u(r.hrMask);
        t += ",gpuStartTicks=" + u(r.gpuStartTicks) + ",gpuEndTicks=" + u(r.gpuEndTicks) + ",gpuFreqTicks=" + u(r.gpuFreqTicks);
        t += ",disjoint=" + s(r.disjointValue); // -1 unknown / 0 FALSE / 1 TRUE
        t += ",renderHr=" + s(r.renderHr) + ",endReadHr=" + s(r.endReadHr);
        t += ",hrEndStart=" + s(r.hrEndStart) + ",hrDisjoint=" + s(r.hrDisjoint) + ",hrEndEnd=" + s(r.hrEndEnd);
        t += ",renderReturned=" + u(r.renderReturned) + ",renderThrew=" + u(r.renderThrew);
        t += ",endReadReturned=" + u(r.endReadReturned) + ",endReadThrew=" + u(r.endReadThrew);
        t += ",validity=" + u(r.validity) + ",resolved=" + u(r.resolved) + ",reason=" + u(r.reason);
        m += "#d11GpuTimingRecord=index=" + u(i) + "," + t + "\n";
    }
    const uint64_t expected = (uint64_t)rc + (uint64_t)g_d11GpuOverflow;
    const std::string status = (g_d11GpuOverflow > 0u || g_d11GpuIncomplete > 0ull)
        ? std::string("INCOMPLETE") : (rc > 0u ? std::string("FULL") : std::string("NO_RECORDS"));
    m += "#d11GpuTimingCompleteness=recordsExpected=" + u(expected) + ",recordsEmitted=" + u(rc) +
         ",overflow=" + u(g_d11GpuOverflow) + ",incomplete=" + u(g_d11GpuIncomplete) +
         ",refsOut=" + u(g_d11GpuRefsOut ? 1ull : 0ull) +
         ",refsReleased=" + u(g_d11GpuRefsReleased) +
         ",cleanupDone=" + u(g_d11GpuCleanupDone ? 1ull : 0ull) +
         ",finalized=" + u(g_d11GpuFinalized ? 1ull : 0ull) +
         ",status=" + status + "\n";
    return m;
}
// R4B cached CSV serializer DEFINITION. The forward declaration above (same
// unnamed-namespace scope, same no-noexcept signature) is the same entity.
// Pure static POD reads: zero COM/D3D/SDK calls at export; allocation or
// serialization failure returns an explicit unavailable line, and the
// existing export-failure path stays unchanged. The ORIGINAL 63-column
// header/rows and the D9 metadata block remain byte-exact; this block sits
// beside them before the header. Destroy's result is never changed.
std::string QaD11GpuTimingMetaLines()
{
    try
    {
        std::string m = QaD11GpuTimingResolveMetaLines();
        m += QaD11GpuTimingRecordLines();
        return m;
    }
    catch (...) { return std::string("#d11GpuTiming=unavailable\n"); }
}
// R5 Finalize: no-throw boundary. Runs ONLY after the ORIGINAL drainProven,
// before GpuWorkerCleanup/proof publication. ZERO Issue/GetData/wait here.
// Unresolved records are marked incomplete as-is. Uncertain Issue ownership
// retains ALL refs and latches the existing quarantine without any proof;
// otherwise each still-held ref gets at most ONE Release attempt (never
// retried). Returned POD evidence is NEVER rewritten.
void QaD11GpuTimingFinalize() noexcept
{
    try
    {
        if (g_d11GpuFinalized) return;
        const bool armed = g_d11GpuInitDone || g_d11GpuAnySlotHeld ||
                           g_d11GpuRefsOut || g_d11GpuObsUncertain.load();
        if (!armed) return; // observer never created/initialized: original path
        g_d11GpuFinalized = true;
        g_d11GpuObsEnabled = false;
        g_d11GpuObsReady = false;
        bool uncertain = g_d11GpuObsUncertain.load();
        for (uint32_t r = 0u; r < kD11GpuObsCapacity; ++r)
        {
            QaD11GpuObsRecordV1* rp = &g_d11GpuRecords[r];
            if (rp->obsState == kD11GpuObsPending || rp->obsState == kD11GpuObsOpenX)
            {
                if (rp->resolved == 0u)
                {
                    rp->resolved = 1u; rp->validity = 0u;
                    if (rp->reason == 0u) rp->reason = 8u; // unresolved at finalize
                }
                QaD11GpuSatAdd(g_d11GpuIncomplete);
            }
        }
        if (!uncertain)
        {
            // known ownership: at most ONE attempt per ref; a throwing Release
            // retains that pointer and leaves refs out — never retried
            QaD11GpuTimingReleaseAllRefsLockedStyle();
        }
        bool allClear = true;
        for (uint32_t i = 0u; i < kD11GpuObsSlots; ++i)
            if (g_d11GpuStart[i] || g_d11GpuEnd[i] || g_d11GpuDisjointQ[i]) allClear = false;
        bool refsUncertain = g_d11GpuObsUncertain.load() || !allClear;
        if (refsUncertain)
        {
            // unknown Issue, throwing Release or failed cleanup: original slot
            // states, record association and every held ref stay exactly as
            // they are — no reuse, no reset, no retry; no pointer is released
            for (uint32_t i = 0u; i < kD11GpuObsSlots; ++i)
                if (g_d11GpuStart[i] || g_d11GpuEnd[i] || g_d11GpuDisjointQ[i])
                    g_d11GpuRefsOut = true;
        }
        else
        {
            // proven normal Release of ALL refs and !uncertain: ONLY now may
            // slot markers/associations/field cursors clear, including a
            // retired slot; retained record evidence is never rewritten
            for (uint32_t i = 0u; i < kD11GpuObsSlots; ++i)
            {
                g_d11GpuSlotState[i] = kD11GpuObsFree;
                g_d11GpuSlotRec[i] = 0xffffffffu;
                g_d11GpuFieldCursor[i] = 0u;
            }
        }
        g_d11GpuCleanupDone = !refsUncertain;
        if (refsUncertain)
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedQuarantined = true;
        }
    }
    catch (...)
    {
        // Finalize's own failure: latch GLOBAL uncertainty and disable
        // sampling alongside the existing cleanup=false / quarantine
        g_d11GpuCleanupDone = false;
        g_d11GpuObsUncertain = true;
        g_d11GpuObsEnabled = false;
        g_d11GpuObsReady = false;
        QaD11GpuSatAdd(g_d11GpuIncomplete);
        try { std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
              g_sharedQuarantined = true; } catch (...) {}
    }
}
#endif
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
#if defined(VRSIM_QA_D9_BEGIN_POLL)
    if (g_qaLedger.occupied) return; // durable QA async lease still PREPARING: never drained
#endif
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
#if defined(VRSIM_QA_D9_BEGIN_POLL)
    if (g_qaLedger.occupied) return true; // second invariant: durable QA async lease
#endif
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
#if defined(VRSIM_QA_D9_BEGIN_POLL)
    if (g_qaLedger.occupied) return true; // never wipe a durable QA async lease on a legacy reset
#endif
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
void RingSetAdmissionLocked(bool open); // XR-FRAME-REBUILD-01: defined with the ring helpers below

// Worker-thread only. ONE joint gate performs: two-dimension pending check,
// admission/lease decision, the pure capability admission on the worker-owned
// cache (a refusal drops only the invalid queue and preserves readiness),
// then — for an admitted request ONLY — epoch record, busy arm, readiness
// invalidation and the two-dimension take + pending clear. ALL real work (the
// original rebuild body, every SetErr/LogLine, any GPU/XR/COM call) runs
// OUTSIDE the gates.
void WorkerApplyPendingSceneMode()
{
    SceneChangeBusyGuard busy;
    int w = 0, h = 0;
    bool take = false;
    const char* why = nullptr;
    int capsRefusal = 0;
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
            // DEVICE-CAPS-01 REPAIR: the shared pure policy decision runs on the
            // worker-owned capability cache HERE, inside the same ring -> meta
            // joint gate and BEFORE the take/Arm/readiness-invalidation. The
            // export never reads g_deviceCaps (worker-owned; no cross-thread
            // race is introduced). A capability refusal consumes ONLY the
            // invalid queue: the old Ready flag, ready epoch, GPU resources and
            // identity all stay exactly as they were, busy is never armed and
            // no success is published. A pure decision only — no GPU/COM/XR or
            // logging call happens under this gate.
            const int decision = VRSIMCaps::CapsAdmitSceneRequest(g_deviceCaps, w, h);
            if (decision != VRSIMCaps::kCapsOk)
            {
                g_pendingSceneW.store(0);
                g_pendingSceneH.store(0);
                if (!g_sceneMode.load()) g_vcSceneRequested = false;
                capsRefusal = decision;
                why = "scene capability gate refused queued request: old readiness and resources preserved";
            }
            else
            {
                g_pendingSceneW.store(0);
                g_pendingSceneH.store(0);
                g_sharedScene1536Ready = false; // invalidate first; republished only below
                g_sceneReadyEpoch = 0;
                busy.ArmLocked();
                take = true;
            }
        }
    }
    if (!take)
    {
        if (capsRefusal)
        {
            // outside every gate: honest refusal report, no fake success
            SetErr("scene capability gate refused queued " + std::to_string(w) + "x" +
                   std::to_string(h) + " decision=" + std::to_string(capsRefusal) +
                   " capsValid=" + std::to_string(g_deviceCaps.valid ? 1 : 0) +
                   " (request dropped; no reset/destroy performed; current resources preserved)");
        }
        if (why) LogLine("XR", std::string("scene request not applied: ") + why);
        return; // guard unarmed, or nothing taken: no rebuild, no fake success
    }
    // SF2: the actual body, the readiness publication and the result log are ONE
    // try/catch while busy is still armed. An exception may never leave
    // busy=true (the guard still owns it) nor keep a just-published readiness.
    // No GPU/COM/XR call and no real logging happens under a gate.
    // XR-FRAME-REBUILD-01: an ADMITTED rebuild that returns false (or throws) is
    // a real session fault — the body may have left mixed/new/null swapchains
    // and stale published extents, which must never keep drawing. Fail the
    // session honestly and follow the normal worker exit path: keep the body's
    // own detailed SetErr (fallback only when none was recorded), clear
    // readiness, close native ring admission and request stop atomically under
    // the existing ring -> meta order, publish ERROR outside the gates, and
    // close a real shared lease with the existing helper OUTSIDE held gates.
    // Ordering fix: the MANDATORY steps (gate transaction, ERROR publication,
    // lease-close attempt) each run in their own guarded boundary and all run
    // BEFORE any optional allocating error/log work — a failed optional report
    // can never skip them, and no failure in one mandatory step skips the
    // others or rethrows into WorkerMain's outer catch. No worker Destroy/join
    // and no OFF/cleanup flag fabrication; a pure pre-take capability refusal
    // never reaches this path.
    auto failSessionAfterRebuildFault = [&](const char* what)
    {
        // mandatory 1: the atomic ring -> meta close transaction (own boundary)
        try
        {
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
                g_sharedScene1536Ready = false;
                g_sceneReadyEpoch = 0;
                if (g_ringAdmission) RingSetAdmissionLocked(false);
                g_stopRequested = true;
            }
        }
        catch (...)
        {
            // The stop request lives INSIDE the gate transaction, so a throw on
            // lock acquisition or in the admission close can leave it false while
            // ERROR is still published below — the callsites test only stop, and
            // the worker could keep drawing partially rebuilt resources. Retry
            // the mandatory stop unconditionally (atomic, no allocation).
            // Ready/epoch/admission changes stay ONLY in the gated transaction.
            g_stopRequested = true;
        }
        // mandatory 2: ERROR publication — plain atomic store, cannot throw
        SetState(VRSIM_ERROR);
        // mandatory 3: real shared lease close attempt (own boundary). A throw
        // here is NOT silently treated as a successful close.
        try { SharedCloseLifecycleIfLease(false); }
        catch (...) { /* close attempt failed; never reported as success */ }
        // optional best-effort reporting: fallback error only when the body
        // recorded no detailed one, then the fault log. Guarded separately so
        // neither can skip the mandatory work above (already done) or each
        // other's remaining attempt.
        try
        {
            bool haveErr = false;
            {
                std::lock_guard<std::mutex> lk(g_errMutex);
                haveErr = !g_lastError.empty();
            }
            if (!haveErr)
                SetErr(std::string(what) + " " + std::to_string(w) + "x" + std::to_string(h) +
                       " (no detailed error recorded; readiness cleared, admission closed)");
        }
        catch (...) { /* fallback report is optional; never rethrow */ }
        try
        {
            LogLine("XR", std::string(what) + " " + std::to_string(w) + "x" + std::to_string(h) +
                    "; session fault: ERROR published, stop requested, shared close attempted");
        }
        catch (...) { /* log is optional; never rethrow */ }
    };
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
#if defined(VRSIM_QA_MIXED_SOURCE) && defined(VRSIM_QA_OUTPUT_4096)
                // mixed-source QA OUTPUT_4096: the XR OUTPUT swap scene is a
                // fixed 4096; kStereoBridgeWidthV1/HeightV1 remain the 4096
                // SOURCE ticket dimensions.
                w == 4096 && h == 4096)
#elif defined(VRSIM_QA_MIXED_SOURCE)
                // mixed-source QA: the XR OUTPUT swap scene is fixed 3072, while
                // kStereoBridgeWidthV1/HeightV1 now denote the 4096 SOURCE.
                w == 3072 && h == 3072)
#else
                w == (int)VRSIM::kStereoBridgeWidthV1 && h == (int)VRSIM::kStereoBridgeHeightV1)
#endif
            {
                // the ONLY place this card sets readiness true: this round's
                // verified native bridge-profile rebuild, under this round's epoch
                g_sharedScene1536Ready = true;
                g_sceneReadyEpoch = busy.epoch;
                published = true;
            }
        }
        if (!verified)
            failSessionAfterRebuildFault("scene rebuild failed verification");
        LogLine("XR", std::string("scene rebuild returned ") + std::to_string(w) + "x" +
                std::to_string(h) + " epoch=" + std::to_string(busy.epoch) +
                " verified=" + std::to_string(verified ? 1 : 0) +
                " ready=" + std::to_string(published ? 1 : 0));
    }
    catch (...)
    {
        // XR-FRAME-REBUILD-01: a throw inside the admitted rebuild body is the
        // same honest fatal fault; the best-effort fatal path owns the readiness
        // clear, admission close, stop request, ERROR publication and the
        // shared-lease close. Nothing here rethrows into WorkerMain's outer catch.
        failSessionAfterRebuildFault("scene rebuild threw");
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
    // XR-FRAME-REBUILD-01: a missing device/context before an ADMITTED rebuild
    // is a concrete fault, never a silent false — the caller's fatal path keeps
    // exactly this detailed error.
    if (!g_dev || !g_ctx)
    {
        SetErr("scene rebuild refused: D3D device/context missing before rebuild (dev=" +
               std::to_string(g_dev ? 1 : 0) + " ctx=" + std::to_string(g_ctx ? 1 : 0) + ")");
        return false;
    }
    LogLine("XR", "ApplySceneMode " + std::to_string(w) + "x" + std::to_string(h));
    // DEVICE-CAPS-01 REPAIR: DEFENSIVE second run of the same fail-closed
    // pure policy (the primary decision now happens in WorkerApplyPendingScene-
    // Mode inside the joint gate, before any readiness invalidation). Here it
    // still gates BEFORE any Reset/Destroy or active-state invalidation: a
    // nonpositive request, an unavailable current-session capability cache, a
    // request above INT32_MAX (narrowing refusal), or a request exceeding
    // EITHER eye's maxImageRect or the actual system max refuses with the
    // current resources fully preserved (no g_sceneMode/texture/swapchain/
    // identity change).
#if defined(VRSIM_QA_MIXED_SOURCE) && defined(VRSIM_QA_OUTPUT_4096)
    // The explicit common 4096 output profile passes only
    // when BOTH eyes and the system allow it.
#else
    // The explicit common 3072 output profile passes only
    // when BOTH eyes and the system allow it.
#endif
    const int capsDecision = VRSIMCaps::CapsAdmitSceneRequest(g_deviceCaps, w, h);
    if (capsDecision != VRSIMCaps::kCapsOk)
    {
        SetErr("scene capability gate refused " + std::to_string(w) + "x" + std::to_string(h) +
               " decision=" + std::to_string(capsDecision) +
               " capsValid=" + std::to_string(g_deviceCaps.valid ? 1 : 0) +
               " (no reset/destroy performed; current resources preserved)");
        return false;
    }
    // HIGHRES-H2-GPU: scene rebuild invalidates the worker-owned helper and the
    // active GPU identity/layer pose (helper re-initializes lazily at new dims).
    g_gpuPass.Reset();
    g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
#if defined(VRSIM_QA_MIXED_SOURCE)
    g_gpuPassMixedSourceQa = false; // mixed pass identity is invalidated with readiness
#endif
    g_activeSourceGpu = false;
    g_haveLayerPose = false; g_lastLayerPoseSerial = 0;
    GpuStatsResetActive();
#if defined(VRSIM_QA_FRAME_TRACE)
    g_qaTraceSharedValid = false; // scene rebuild invalidated the real active/layer
#endif
    for (int e = 0; e < 2; ++e)
    {
        if (g_swap[e] != XR_NULL_HANDLE)
        {
            // XR-FRAME-REBUILD-01: clear the owned handle ONLY when the destroy
            // actually returned XR_SUCCEEDED. On failure the handle stays (the
            // real teardown owner can still reclaim it) and this rebuild fails
            // honestly with the actual eye/result — never a silently lost handle.
            const XrResult dr = xrDestroySwapchain(g_swap[e]);
            if (XR_SUCCEEDED(dr))
            {
                g_swap[e] = XR_NULL_HANDLE;
            }
            else
            {
                SetErr("scene xrDestroySwapchain[" + std::to_string(e) + "] failed " +
                       XrResultName(dr) + " (handle preserved for teardown)");
                g_sceneMode = false;
                return false;
            }
        }
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
    // DEVICE-CAPS-01: a successful full rebuild updates BOTH actual per-eye
    // output extents consistently, only AFTER both eyes were verified from the
    // real descriptors above.
    g_eyeOutW[0] = g_eyeOutW[1] = (uint32_t)w;
    g_eyeOutH[0] = g_eyeOutH[1] = (uint32_t)h;
    LogLine("XR", "scene per-eye output extents " + std::to_string((unsigned)g_eyeOutW[0]) + "x" +
            std::to_string((unsigned)g_eyeOutH[0]) + " published for both eyes after descriptor verification");
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

#if defined(VRSIM_QA_MIXED_SOURCE)
#if defined(VRSIM_QA_OUTPUT_4096)
// QA-only worker entry for the mixed 4096-source -> 4096-output profile. SAME
// g_gpuPass helper and the SAME single XR worker as the raw path: no new pass,
// context or thread. Validates device/context and the frozen session color
// contract exactly like EnsureGpuPassReady (expected plane format selected from
// g_sessionColorSrgb, and required to agree with the selected swapchain format).
// Reuses the cached pass ONLY when it is already the mixed pass at 4096 with the
// expected format; otherwise (re)initializes through the verified QA split entry
// InitializeSharedSourceForQA(4096x4096 source -> 4096x4096 output). Failure
// resets readiness/cache and clears the mixed flag; success caches the mixed
// identity/dims/format. Logical profile log only (source 4096 / output 4096 /
// color): never a fabricated GPU time or FPS; no per-frame allocation and no
// API fallback.
bool EnsureMixedSharedGpuPassReadyForQA()
{
    if (!g_dev || !g_ctx) return false;
    const DXGI_FORMAT expectedFormat = g_sessionColorSrgb
        ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (g_swapFormat != (int64_t)expectedFormat) return false;
    if (g_gpuPassReady && g_gpuPassMixedSourceQa &&
        g_gpuPassW == 4096u && g_gpuPassH == 4096u && g_gpuPassFmt == expectedFormat) return true;
    const HRESULT hr = g_gpuPass.InitializeSharedSourceForQA(
        g_dev, g_ctx, 4096u, 4096u, 4096u, 4096u, expectedFormat);
    if (FAILED(hr))
    {
        g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
        g_gpuPassFmt = DXGI_FORMAT_B8G8R8A8_UNORM; g_gpuPassMixedSourceQa = false;
        return false;
    }
    const bool firstInit = !g_gpuPassReady;
    g_gpuPassReady = true; g_gpuPassW = 4096u; g_gpuPassH = 4096u; g_gpuPassFmt = expectedFormat;
    g_gpuPassMixedSourceQa = true;
    if (firstInit || expectedFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
        LogLine("XR", "QA mixed gpu pass init source=4096x4096 output=4096x4096 " +
                std::string(g_sessionColorSrgb ? "srgb" : "legacy") +
                " planeFormat=" + std::to_string((int64_t)expectedFormat) +
                " swapFormat=" + std::to_string(g_swapFormat) +
                " (mixed profile log only; no GPU time or FPS claim)");
    return true;
}
#else
// QA-only worker entry for the mixed 4096-source -> 3072-output profile. SAME
// g_gpuPass helper and the SAME single XR worker as the raw path: no new pass,
// context or thread. Validates device/context and the frozen session color
// contract exactly like EnsureGpuPassReady (expected plane format selected from
// g_sessionColorSrgb, and required to agree with the selected swapchain format).
// Reuses the cached pass ONLY when it is already the mixed pass at 3072 with the
// expected format; otherwise (re)initializes through the verified QA split entry
// InitializeSharedSourceForQA(4096x4096 source -> 3072x3072 output). Failure
// resets readiness/cache and clears the mixed flag; success caches the mixed
// identity/dims/format. Logical profile log only (source 4096 / output 3072 /
// color): never a fabricated GPU time or FPS; no per-frame allocation and no
// API fallback.
bool EnsureMixedSharedGpuPassReadyForQA()
{
    if (!g_dev || !g_ctx) return false;
    const DXGI_FORMAT expectedFormat = g_sessionColorSrgb
        ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (g_swapFormat != (int64_t)expectedFormat) return false;
    if (g_gpuPassReady && g_gpuPassMixedSourceQa &&
        g_gpuPassW == 3072u && g_gpuPassH == 3072u && g_gpuPassFmt == expectedFormat) return true;
    const HRESULT hr = g_gpuPass.InitializeSharedSourceForQA(
        g_dev, g_ctx, 4096u, 4096u, 3072u, 3072u, expectedFormat);
    if (FAILED(hr))
    {
        g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
        g_gpuPassFmt = DXGI_FORMAT_B8G8R8A8_UNORM; g_gpuPassMixedSourceQa = false;
        return false;
    }
    const bool firstInit = !g_gpuPassReady;
    g_gpuPassReady = true; g_gpuPassW = 3072u; g_gpuPassH = 3072u; g_gpuPassFmt = expectedFormat;
    g_gpuPassMixedSourceQa = true;
    if (firstInit || expectedFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
        LogLine("XR", "QA mixed gpu pass init source=4096x4096 output=3072x3072 " +
                std::string(g_sessionColorSrgb ? "srgb" : "legacy") +
                " planeFormat=" + std::to_string((int64_t)expectedFormat) +
                " swapFormat=" + std::to_string(g_swapFormat) +
                " (mixed profile log only; no GPU time or FPS claim)");
    return true;
}
#endif // VRSIM_QA_OUTPUT_4096
#endif // VRSIM_QA_MIXED_SOURCE

#if defined(VRSIM_QA_MIXED_SOURCE) && defined(VRSIM_QA_AREA_SHADER)
// ---- HIGHRES-H2 mixed4k->3k QA SAMPLING-CONTRACT ONCE diagnostic ---------
// The EXISTING mixed-source QA helpers immediately above (EnsureMixedSharedGpu
// PassReadyForQA, the g_gpuPassMixedSourceQa key, and the frozen PoseSample plus
// the worker-owned GPU-pass cache globals) are the ONLY thing this diagnostic
// observes. Every declaration below is compiled only under the SECOND,
// AREA-SHADER macro on top of VRSIM_QA_MIXED_SOURCE: if EITHER macro is
// undefined NONE of it compiles, and the mixed helper's own body is untouched.
//
// Runtime default OFF: only the exact value "1" of VRSIM_QA_SAMPLING_ONCE
// enables it for a session (no trim, no case fallback; absent / empty / any
// other token / overlong = OFF). A legal VRSIM_Create (after the eligibility
// gates and the existing QA freezes, before the worker exists) freezes and
// resets this once-flag exactly once; refused Creates never reset it, and
// Stop/Destroy/Finalize NEVER rearm it. The worker checks only the two frozen
// booleans at the FIRST successful shared GPU metadata commit and emits at most
// ONE bounded diagnostic line, then takes the original return. It reads NOTHING
// else from Native or the ring: the evidence is the by-value ticket/identity and
// the exact matched PoseSample already in that function's scope. OFF produces no
// per-frame string allocation or formatting. It is noexcept: a failure only
// makes the line unavailable and can NEVER change VRSIM_Create's or the
// worker's unchanged behavior.
//
// SCOPE (this is metadata-commit provenance, NOT GPU evidence):
//   SHARED_GPU_ENQUEUED_METADATA_COMMITTED_NOT_GPU_COMPLETED_NOT_XR_DISPLAYED
// The CB32 is a CPU mirror DERIVED FROM the accepted double plan (the double
// plan is NOT written back) and the VERIFIED mixed dims -- it is never a GPU
// readback and never an "actually uploaded CB". The projected branch describes
// the shader's INTENDED input classification; it is never a GPU branch readback.
// sourceFovYDerivedDeg is DERIVED (2*atan(plan.ty)); the shared wire carries NO
// SDK camera-FOV field and none is read here. This line is never a substitute
// for uniqueLiveLayerOk / xrEndFrame / any display proof. No COM/descriptor/
// GPU/pose call, no new file IO, no color change, no ABI/wire/CSV/export change.

struct QaSamplingOnceCache {
    bool enabled;    // exact env "1" ONLY
    bool attempted;  // exactly one attempt made for this session (never rearmed)
};
static QaSamplingOnceCache g_samplingOnce{};

// Raw bit views: memcpy only, never a numeric cast that could invent bits.
static inline unsigned long long QaRaw64(double v) { unsigned long long b; std::memcpy(&b, &v, 8); return b; }
static inline unsigned int       QaRaw32(float  v) { unsigned int b; std::memcpy(&b, &v, 4); return b; }
static std::string QaH64(unsigned long long v) { char b[19]; std::snprintf(b, sizeof b, "0x%016llx", v); return std::string(b); }
static std::string QaH32(unsigned int v) { char b[11]; std::snprintf(b, sizeof b, "0x%08x", v); return std::string(b); }
static std::string QaDF(double v) { char b[40]; std::snprintf(b, sizeof b, "%.17g", v); return std::string(b); }
static std::string QaFf(float v) { char b[32]; std::snprintf(b, sizeof b, "%.9g", (double)v); return std::string(b); }
static std::string QaU(unsigned long long v) { return std::to_string(v); }
static std::string QaI(long long v) { return std::to_string(v); }

// Per-eye block. cb32 is EXACTLY the twelve float32 that GpuProjectionPass.cpp
// assigns to the EyeCB ([lrud4,meta4,src4]) with the SAME static_cast<float>,
// emitted as raw32 hex; the plan values are the double raw64 hex actually
// accepted. rho and the sampling widths reproduce the CPU-mirror / shader float
// operation order (a / b * c / d, left-to-right) so the raw32 bit views match.
// Classification is emitted only when the mirrored range is finite/positive/<=2
// (exactly the condition under which that CB was accepted for the draw);
// otherwise mode=UNKNOWN and no width/branch claim is fabricated.
static std::string QaSamplingOnceEye(const char* eye, const VRSIM::ProjectionPlanV1& p, const XrFovf& fov)
{
    const float ftanL = static_cast<float>(p.tanL);
    const float ftanR = static_cast<float>(p.tanR);
    const float ftanU = static_cast<float>(p.tanU);
    const float ftanD = static_cast<float>(p.tanD);
    const float ftx   = static_cast<float>(p.tx);
    const float fty   = static_cast<float>(p.ty);
    const float sW = static_cast<float>(VRSIM::kStereoBridgeWidthV1);   // mixed SOURCE edge (4096)
    const float sH = static_cast<float>(VRSIM::kStereoBridgeHeightV1);  // mixed SOURCE edge (4096)
#if defined(VRSIM_QA_OUTPUT_4096)
    const float oW = static_cast<float>(4096u);   // mixed OUTPUT (4096)
    const float oH = static_cast<float>(4096u);   // mixed OUTPUT (4096)
#else
    const float oW = static_cast<float>(3072u);   // mixed OUTPUT (3072)
    const float oH = static_cast<float>(3072u);  // mixed OUTPUT (3072)
#endif
    const float c6[6] = { ftanL, ftanR, ftanU, ftanD, ftx, fty };       // cb.lrud[0..3], cb.meta[0..1]
    const float m4[4] = { ftx, fty, oW, oH };                            // cb.meta[0..3]
    const float sr4[4] = { sW, sH, 0.0f, 0.0f };                         // cb.src[0..3]
    const float rhoX = (sW / oW) * (ftanR - ftanL) / (2.0f * ftx);
    const float rhoY = (sH / oH) * (ftanU - ftanD) / (2.0f * fty);
    bool range = (ftx > 0.0f) && (fty > 0.0f);
    for (int i = 0; i < 6; ++i) if (!std::isfinite(c6[i])) range = false;
    if (!std::isfinite(rhoX) || !std::isfinite(rhoY) ||
        !(rhoX > 0.0f) || !(rhoY > 0.0f) || rhoX > 2.0f || rhoY > 2.0f) range = false;
    const float wX = rhoX > 1.0f ? rhoX : 1.0f; // shader widths = max(1.0, rhoX)
    const float wY = rhoY > 1.0f ? rhoY : 1.0f; // shader widths = max(1.0, rhoY)

    std::string s = " eye="; s += eye;
    s += " plan_d64=[tanL,tanR,tanU,tanD,tx,ty]{";
    s += QaH64(QaRaw64(p.tanL)) + "," + QaH64(QaRaw64(p.tanR)) + "," + QaH64(QaRaw64(p.tanU)) + ",";
    s += QaH64(QaRaw64(p.tanD)) + "," + QaH64(QaRaw64(p.tx)) + "," + QaH64(QaRaw64(p.ty)) + "}";
    s += " cb32_f32=[lrud{L,R,U,D},meta{tx,ty,oW,oH},src{sW,sH,0,0}]{";
    s += QaH32(QaRaw32(c6[0])) + "," + QaH32(QaRaw32(c6[1])) + "," + QaH32(QaRaw32(c6[2])) + ",";
    s += QaH32(QaRaw32(c6[3])) + "," + QaH32(QaRaw32(m4[0])) + "," + QaH32(QaRaw32(m4[1])) + ",";
    s += QaH32(QaRaw32(m4[2])) + "," + QaH32(QaRaw32(m4[3])) + "," + QaH32(QaRaw32(sr4[0])) + ",";
    s += QaH32(QaRaw32(sr4[1])) + "," + QaH32(QaRaw32(sr4[2])) + "," + QaH32(QaRaw32(sr4[3])) + "}";
    s += " poseFov_f32=[angleL,R,U,D]{";
    s += QaH32(QaRaw32(fov.angleLeft)) + "," + QaH32(QaRaw32(fov.angleRight)) + ",";
    s += QaH32(QaRaw32(fov.angleUp)) + "," + QaH32(QaRaw32(fov.angleDown)) + "}";
    s += " rho_f32=" + QaH32(QaRaw32(rhoX)) + "/" + QaH32(QaRaw32(rhoY));
    s += " rho_dec=" + QaFf(rhoX) + "/" + QaFf(rhoY);
    s += " modeExpectedFromCb32=";
    if (range) s += (rhoX <= 1.0f && rhoY <= 1.0f) ? "bilinear_original" : "area_box";
    else s += "UNKNOWN";
    s += " widths=";
    if (range) s += QaH32(QaRaw32(wX)) + "/" + QaH32(QaRaw32(wY)) + "(" + QaFf(wX) + "/" + QaFf(wY) + ")";
    else s += "UNKNOWN";
    // DERIVED from THIS eye's accepted plan.ty only (2*atan). The shared wire has
    // no SDK camera-FOV field and none is read; this is never an SDK read value.
    s += " sourceFovYDerivedDeg=" +
         QaDF(2.0 * std::atan(p.ty) * (180.0 / 3.14159265358979323846)) +
         " sourceFovYDerivedLabel=DERIVED_NOT_SDK_READ";
    return s;
}

// The single, bounded once log. Called ONLY from the first successful shared GPU
// metadata commit; every argument is already in the caller's scope (this
// ticket, this capture identity, the exact matched PoseSample). noexcept; any
// allocation/format failure makes only this diagnostic unavailable and NEVER
// affects the caller's already-taken control flow.
static void QaSamplingOnceLog(const VRSIM::SharedPreparedPairV1& ticket,
                              const VRSIM::SharedCaptureIdentityV1& id,
                              const PoseSample& pose) noexcept
{
    try
    {
        const bool verifiedMixedDims =
            g_gpuPassReady && g_gpuPassMixedSourceQa &&
#if defined(VRSIM_QA_OUTPUT_4096)
            g_gpuPassW == 4096u &&
            g_gpuPassH == 4096u &&
#else
            g_gpuPassW == 3072u &&
            g_gpuPassH == 3072u &&
#endif
            ticket.width == VRSIM::kStereoBridgeWidthV1 &&
            ticket.height == VRSIM::kStereoBridgeHeightV1 &&
            ticket.format88 == VRSIM::kStereoBridgeFormat88V1;
        std::string m = "scope=SHARED_GPU_ENQUEUED_METADATA_COMMITTED_NOT_GPU_COMPLETED_NOT_XR_DISPLAYED";
        m += " tuple{frameSerial,poseSerial,epoch,generation,ticket,slotIndex}=" +
             QaU(id.frameSerial) + "/" + QaU(id.poseSerial) + "/" + QaU(id.epoch) + "/" +
             QaU(id.generation) + "/" + QaU(ticket.ticket) + "/" + QaU(ticket.slotIndex);
        m += " idQpc4{poseBegin,poseEnd,captureBegin,captureEnd}=" +
             QaI(id.poseSampleBeginQpc) + "/" + QaI(id.poseSampleEndQpc) + "/" +
             QaI(id.captureBeginQpc) + "/" + QaI(id.captureEndQpc);
        if (!verifiedMixedDims)
        {
            // Verified-mixed dims absent: stay a pure diagnostic and NEVER fabricate
            // CB32 / branch / dims / a default. The first successful commit still
            // takes its original return; only this contract line degrades to UNKNOWN
            // (no eye block, no VERIFIED_MIXED_DIMS).
            m += " dims=UNKNOWN";
            m += " modeExpectedFromCb32=UNKNOWN";
            m += " provenance=UNKNOWN";
            m += " notDisplayProof=yes";
            LogLine("QA_SHARED_SAMPLING_ONCE", m);
            return;
        }
        m += " dims{source,output}=" +
#if defined(VRSIM_QA_OUTPUT_4096)
             QaU(VRSIM::kStereoBridgeWidthV1) + "/" + QaU(g_gpuPassW); // 4096 / 4096 (verified mixed cache)
#else
             QaU(VRSIM::kStereoBridgeWidthV1) + "/" + QaU(g_gpuPassW); // 4096 / 3072 (verified mixed cache)
#endif
        m += " fmt{ticketSourceSRV,configuredSamplingSRV,configuredOutput,actualSelectedSwap}=" +
             QaU(ticket.format88) + "/" + QaU(g_gpuPassFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? 93 : 88) +
             "/" + QaU((unsigned long long)g_gpuPassFmt) + "/" + QaU((unsigned long long)g_swapFormat);
        m += QaSamplingOnceEye("L", id.leftPlan, pose.fl);
        m += QaSamplingOnceEye("R", id.rightPlan, pose.fr);
        m += " provenance=CB32_DERIVED_FROM_ACCEPTED_PLAN_AND_VERIFIED_MIXED_DIMS_NOT_GPU_READBACK_NOT_UPLOADED_CB branch=PROJECTED_FROM_CB_NOT_GPU_READBACK notDisplayProof=yes";
        LogLine("QA_SHARED_SAMPLING_ONCE", m);
    }
    catch (...)
    {
        // Diagnostic only: a failure here NEVER enters SetErr/quarantine, never
        // retries, and never alters VRSIM_Create or the worker's unchanged path.
        try { OutputDebugStringA("VRSIM QA shared sampling once: log unavailable (diagnostic only)\n"); }
        catch (...) {}
    }
}

// Legal-Create env freeze/reset (called exactly once, worker not yet created):
// read VRSIM_QA_SAMPLING_ONCE into a bounded buffer; ONLY the exact "1" enables
// the session (overlong n>=8 is OFF, never truncated); reset attempted=false so a
// fresh legal Create may emit at most one line. no-throw: any failure leaves the
// diagnostic OFF/unavailable and NEVER fails VRSIM_Create.
static void QaSamplingOnceFreezeAndReset() noexcept
{
    try
    {
        QaSamplingOnceCache fresh{};
        wchar_t buf[8];
        const DWORD n = GetEnvironmentVariableW(L"VRSIM_QA_SAMPLING_ONCE", buf,
                                                (DWORD)(sizeof(buf) / sizeof(buf[0])));
        if (n == 1u && buf[0] == L'1') fresh.enabled = true; // exact "1" only
        g_samplingOnce = fresh; // enabled + attempted=false in one by-value write
    }
    catch (...) { /* freeze failure: the diagnostic merely stays unavailable; Create return untouched */ }
}
#endif // VRSIM_QA_MIXED_SOURCE && VRSIM_QA_AREA_SHADER

// worker-only: lazily (re)initialize the helper at the current scene size.
// HIGHRES-H2-COLOR: plane format follows the frozen session color contract
// (91 when sRGB, else the legacy 87) and must match the selected swapchain
// format exactly — no silent mismatch. Rebuilds on dimension or format change.
bool EnsureGpuPassReady(uint32_t w, uint32_t h)
{
    if (!g_dev || !g_ctx) return false;
#if defined(VRSIM_QA_MIXED_SOURCE)
    // QA mixed-source guard: a mixed (4096-source) pass must never be reused by
    // the raw CPU-byte path (its mixed CPU-side source is not a raw match), so
    // reset the shared helper and clear the mixed identity before the original
    // raw Initialize below starts a clean pass. The raw signature and the raw
    // Initialize call itself stay unchanged; original raw callsites are unaltered.
    if (g_gpuPassMixedSourceQa)
    {
        g_gpuPass.Reset();
        g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
        g_gpuPassFmt = DXGI_FORMAT_B8G8R8A8_UNORM; g_gpuPassMixedSourceQa = false;
    }
#endif
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
#if defined(VRSIM_QA_MIXED_SOURCE)
    g_gpuPassMixedSourceQa = false; // mixed pass identity is invalidated with readiness
#endif
    g_activeSourceGpu = false;
    g_haveLayerPose = false; g_lastLayerPoseSerial = 0;
    GpuStatsResetActive();
#if defined(VRSIM_QA_FRAME_TRACE)
    g_qaTraceSharedValid = false; // trace cache follows the real active invalidation
#endif
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
    // DEVICE-CAPS-01: re-init clear — an old runtime's capability cache and
    // per-eye output extents must never admit or feed a new session.
    g_deviceCaps = VRSIMCaps::DeviceCapsV1{};
    g_eyeOutW[0] = g_eyeOutW[1] = 0;
    g_eyeOutH[0] = g_eyeOutH[1] = 0;
    // active GPU identity + wire identity (helper holds no live resources here:
    // the prior worker already ran GpuWorkerCleanup on exit)
    g_activeSourceGpu = false;
    g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
#if defined(VRSIM_QA_MIXED_SOURCE)
    g_gpuPassMixedSourceQa = false; // mixed pass identity is invalidated with readiness
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
                g_qaTraceSharedValid = false;
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
                // raw-live legacy pair replaced any shared identity: the shared
                // trace cache must never label this submission 'shared'.
                g_qaTraceSharedValid = false;
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
    g_qaTraceSharedValid = false; // CPU-live commit: never report stale shared identity
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
    // D11 observer: the handle is invalid unless BracketStart reserves a
    // bracket; Close runs AFTER the original render try/catch (never inside
    // it) for every VALID HANDLE — a partial handle from a throwing Start is
    // still closed — and BEFORE the mandatory EndRead.
    QaD11GpuObsHandleV1 d11h;
    d11h.slot = 0xffffffffu; d11h.record = 0xffffffffu;
    bool d11RenderReturned = false;
#endif
    if (valid && srvL && srvR)
    {
        // P1-1: EnsureGpuPassReady/RenderSharedPair can throw from std::string/
        // vector work (log concatenation). Catch here so the mandatory
        // EndReadOnWorker below ALWAYS runs and no exception escapes the
        // worker. A throw mid-dispatch leaves the frozen helper's active pair
        // INDETERMINATE: treat it as a new-image attempt (invalidate below).
        try
        {
#if defined(VRSIM_QA_MIXED_SOURCE) && defined(VRSIM_QA_OUTPUT_4096)
            // mixed-source QA OUTPUT_4096: 4096 SOURCE into the 4096 OUTPUT pass
            // through the QA-only mixed ensure (same g_gpuPass, same worker, no
            // new pass).
            if (EnsureMixedSharedGpuPassReadyForQA())
#elif defined(VRSIM_QA_MIXED_SOURCE)
            // mixed-source QA: 4096 SOURCE into the 3072 OUTPUT pass through the
            // QA-only mixed ensure (same g_gpuPass, same worker, no new pass).
            if (EnsureMixedSharedGpuPassReadyForQA())
#else
            if (EnsureGpuPassReady(VRSIM::kStereoBridgeWidthV1, VRSIM::kStereoBridgeHeightV1))
#endif
            {
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
                QaD11GpuTimingBracketStart(ticket, id, slot, valid, srvL, srvR, d11h);
#endif
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                // frozen actual signature: exactly four arguments, no width/height
                rhr = g_gpuPass.RenderSharedPair(srvL, srvR, id.leftPlan, id.rightPlan);
                QueryPerformanceCounter(&t1);
                dispatchTicks = (uint64_t)(t1.QuadPart - t0.QuadPart);
                rendered = (rhr == S_OK);
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
                d11RenderReturned = true;
#endif
            }
        }
        catch (...)
        {
            renderThrew = true;
            rendered = false;
        }
    }
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
    // ONE Close per valid handle — also for an HRESULT-failed or throwing
    // render: known-open commands are closed; renderHr is real only when the
    // render actually returned. The call itself is a no-throw boundary.
    if (d11h.slot != 0xffffffffu)
        QaD11GpuTimingClose(d11h, (long long)rhr, d11RenderReturned, renderThrew);
#endif

    // End AFTER the real RenderSharedPair commands — also for a failed
    // projection, a pure drain-without-projection read and a render path that
    // threw. P1-1: End's OWN exception is caught here — never retried on the
    // same lease, the ticket is retained as unproven/quarantined (never a
    // generic FREE) and nothing escapes the worker. No logging inside either
    // catch: std::string work could throw again.
    HRESULT ehr = E_FAIL;
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
    bool endReadReturned = false;
    bool endReadThrew = false;
#endif
    try
    {
        ehr = g_sharedBridge.EndReadOnWorker(ticket);
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        endReadReturned = true;
#endif
    }
    catch (...)
    {
        ehr = E_FAIL;
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        endReadThrew = true;
#endif
    }
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
    // exactly one stamp per granted EndRead, for every valid handle (partial
    // handles included); the EndRead HRESULT is stored only when returned
    if (d11h.slot != 0xffffffffu)
        QaD11GpuTimingStamp(d11h, (long long)ehr, endReadReturned, endReadThrew);
    bool d11ObsUncertain = g_d11GpuObsUncertain.load();
#endif
    {
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        g_sharedLastBridgeHresult = (long long)ehr;
        if (ehr == S_OK) g_sharedPhase[slot] = kSharedPhaseInflight; // only End success -> INFLIGHT
        else { g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined; }
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        // observer uncertainty after the mandatory EndRead latches the
        // EXISTING quarantine; the original phase/HRESULT handling is intact
        if (d11ObsUncertain) g_sharedQuarantined = true;
#endif
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
#if defined(VRSIM_QA_MIXED_SOURCE) && defined(VRSIM_QA_OUTPUT_4096)
                              4096u, 4096u, // mixed OUTPUT profile; SOURCE stays 4096
#elif defined(VRSIM_QA_MIXED_SOURCE)
                              3072u, 3072u, // mixed OUTPUT profile; SOURCE stays 4096
#else
                              VRSIM::kStereoBridgeWidthV1, VRSIM::kStereoBridgeHeightV1,
#endif
                              dispatchTicks);
#if defined(VRSIM_QA_FRAME_TRACE)
            if (g_qaTraceEnabled)
            {
                // trace-only worker cache: the COMPLETE shared capture identity plus
                // the EXACT matching PoseSample, copied BY VALUE at the successful
                // commit (never a ring pointer, never a later Stats re-snapshot).
                g_qaTraceSharedId = id;
                g_qaTraceSharedPose = *pose;
                g_qaTraceSharedValid = true;
            }
#endif
            committed = true;
        }
    }
    if (committed)
    {
#if defined(VRSIM_QA_MIXED_SOURCE) && defined(VRSIM_QA_AREA_SHADER)
        // First successful shared GPU metadata commit only: check the two frozen
        // booleans (no string work when OFF), then arm-and-log exactly once per
        // session. The flag is set BEFORE the noexcept logger so a log failure
        // is never retried on a later frame. This is OUTSIDE the ring/meta locks
        // (already released at the commit gate above) and reads NOTHING else from
        // Native or the ring -- only this ticket, this identity and the exact
        // matched PoseSample in this scope. The original committed return is
        // preserved and nothing else in this path changes.
        if (g_samplingOnce.enabled && !g_samplingOnce.attempted)
        {
            g_samplingOnce.attempted = true;
            QaSamplingOnceLog(ticket, id, *pose);
        }
#endif
        return;
    }
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
#if defined(VRSIM_QA_FRAME_TRACE)
        g_qaTraceSharedValid = false;
#endif
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
            return; // unproven drain releases NOTHING, observer refs included
        }
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        // D11 observer cleanup: runs ONLY after the FULL original drainProven
        // and BEFORE GpuWorkerCleanup/proof publication. Zero Issue/GetData/
        // wait here; refs relinquished exactly once or truthfully retained.
        QaD11GpuTimingFinalize();
        // R5: an armed observer that still owns refs, latched uncertainty or
        // unproven cleanup refuses GpuWorkerCleanup and the drain-proof
        // publication; the existing quarantine stays latched. A never created/
        // initialized observer keeps the original path byte-identical.
        if ((g_d11GpuInitDone || g_d11GpuAnySlotHeld || g_d11GpuRefsOut) &&
            (g_d11GpuRefsOut || g_d11GpuObsUncertain.load() || !g_d11GpuCleanupDone))
        {
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                g_sharedQuarantined = true;
            }
            LogLine("D11GT", "refs retained/uncertain: GpuWorkerCleanup refused; quarantine latched; no drain proof");
            return;
        }
#endif

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
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
    // R5: the six owned query refs are allocated ONCE here, after the original
    // rc==0 init and before any lease; the original rc is never touched
    QaD11GpuTimingWorkerInit(g_dev, g_ctx);
    if (g_d11GpuObsUncertain.load() && !g_d11GpuObsReady)
    {
        // a swallowed initializer exception must NOT continue normal worker
        // work: enter the EXISTING retained-failure cleanup even prelease;
        // the original rc and Init promise outcome are preserved, no fake
        // HRESULT is introduced
        LogLine("D11GT", "worker init uncertain: entering retained-failure cleanup");
        exitCleanup();
        return;
    }
#endif

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
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        // R5: the ONE normal-loop resolve opportunity — after the top-of-loop
        // completion poll and after the sharedStopNow rejection, before event
        // pumping. At most one GetData per call; never runs in drain/export.
        if (!g_d11GpuObsUncertain.load())
            QaD11GpuTimingResolveOne();
#endif
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
        // XR-FRAME-REBUILD-01: a genuine admitted-rebuild fault requests stop in
        // the helper; leave the loop BEFORE any Dump/Wait/consume work so mixed/
        // new/null swapchains and stale extents never keep drawing. (A pure
        // capability refusal never takes the queue and cannot request this.)
        if (g_stopRequested) break;
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
#if defined(VRSIM_QA_FRAME_TRACE)
        const long long qaWaitQ0 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif
        XrResult r = xrWaitFrame(g_sess, &fwi, &fs);
#if defined(VRSIM_QA_FRAME_TRACE)
        const long long qaWaitQ1 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
            {
                // ACTUAL WaitFrame failure on the SAME buffer (kind wait_failure):
                // no fake EndFrame. frameSerial is the last BeginFrame-successful
                // serial (this frame's serial is only taken after BeginFrame).
                QaFrameRowV1 rw{};
                rw.kind = 1;
                rw.result = (int)r;
                rw.frameSerial = (unsigned long long)serial;
                rw.callBeginQpc = qaWaitQ0;
                rw.callEndQpc = qaWaitQ1;
                QaTraceAddRow(rw);
            }
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
        const long long qaBeginQ0 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif
        r = xrBeginFrame(g_sess, &fbi);
#if defined(VRSIM_QA_FRAME_TRACE)
        const long long qaBeginQ1 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif
        // XR-FRAME-REBUILD-01: XR_FAILED(r) is the ONLY failure test. OpenXR
        // positive-success codes — XR_FRAME_DISCARDED (the PREVIOUS frame was
        // discarded; THIS call still begins the current frame) and
        // XR_SESSION_LOSS_PENDING — proceed through the begun-frame path below
        // and MUST reach a real xrEndFrame. The old r != XR_SUCCESS test read
        // them as failures, skipped EndFrame and miscounted them as beginFailed.
        if (XR_FAILED(r))
        {
            ++g_beginFrameFailed;
            // Genuine negative Begin results only. The ring must still drain so
            // scene textures stay current for when VISIBLE arrives — otherwise
            // every content frame committed during this period is stolen.
            if (g_sceneMode)
            {
                WorkerApplyPendingSceneMode(); // same transaction as the between-frames site
                // XR-FRAME-REBUILD-01: a rebuild fault requests stop; never
                // consume/draw on a failed session. Safe only because the Begin
                // gate above tests XR_FAILED: no frame was begun this iteration,
                // so breaking here keeps Begin/End balanced.
                if (g_stopRequested) break;
                WorkerConsumeSceneFrame(); // CARD2: shared-bound sessions take the shared consumer
            }
            if (g_beginFrameFailed <= 3 || g_beginFrameFailed % 500 == 0)
                LogLine("XR", "BeginFrame " + XrResultName(r) + " count=" + std::to_string(g_beginFrameFailed.load()));
#if defined(VRSIM_QA_FRAME_TRACE)
            {
                // ACTUAL BeginFrame failure with the real result and NO invented
                // layer: layerCount/contentClass stay 0. Records the current
                // runtime serial (pre-increment), the ACTUAL WaitFrame target
                // and the currently valid cached pose/consumed state.
                QaFrameRowV1 rw{};
                rw.kind = 2;
                rw.result = (int)r;
                rw.frameSerial = (unsigned long long)serial;
                rw.shouldRender = (fs.shouldRender == XR_TRUE) ? 1 : 0;
                rw.targetDisplayTimeNs = (long long)fs.predictedDisplayTime;   // signed XrTime/XrDuration ns
                rw.targetDisplayPeriodNs = (long long)fs.predictedDisplayPeriod;
                rw.haveLayerPose = g_haveLayerPose ? 1 : 0;
                rw.activeSourceGpu = g_activeSourceGpu ? 1 : 0;
                rw.layerPoseSerial = g_lastLayerPoseSerial.load();
                rw.activeFrameSerial = g_lastConsumedSerial.load();
                rw.callBeginQpc = qaBeginQ0;
                rw.callEndQpc = qaBeginQ1;
                QaTraceAddRow(rw);
            }
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
            const long long qaLocQ0 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif
            r = xrLocateViews(g_sess, &vli, &vs, 2, &n, views);
#if defined(VRSIM_QA_FRAME_TRACE)
            const long long qaLocQ1 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
                if (g_qaTraceEnabled)
                {
                    // exact successful sample keeps its ORIGINAL (signed) target +
                    // real locate window (used later only via the by-value commit copy)
                    ps.qaDisplayTime = (long long)fs.predictedDisplayTime;
                    ps.qaDisplayPeriod = (long long)fs.predictedDisplayPeriod;
                    ps.qaLocateBeginQpc = qaLocQ0;
                    ps.qaLocateEndQpc = qaLocQ1;
                }
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE)
        const long long qaConsumeQ0 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif
        if (g_sceneMode) WorkerConsumeSceneFrame();
#if defined(VRSIM_QA_FRAME_TRACE)
        const long long qaConsumeQ1 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
#endif

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
                    // DEVICE-CAPS-01: each eye submits its OWN actual output
                    // extent; the legacy left g_swapW/H is never the right-eye
                    // subImage authority (asymmetric recommendations).
                    pv[e].subImage.imageRect.extent.width = (int32_t)g_eyeOutW[e];
                    pv[e].subImage.imageRect.extent.height = (int32_t)g_eyeOutH[e];
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
#if defined(VRSIM_QA_FRAME_TRACE)
        // Freeze the row of the tuple that IS ABOUT TO BE SUBMITTED immediately
        // BEFORE xrEndFrame: the ACTUAL pv[e].pose, sourceKind, cached shared
        // identity, serials and current state are snapshotted at THIS site and
        // are never reconstructed from later globals. After the call ONLY the
        // actual result, the actual end QPC and the successful-delivery
        // classification are filled. Zero per-frame work while runtime-OFF.
        QaFrameRowV1 qaRow;
        unsigned long long qaKeyFrame = 0, qaKeyPose = 0;
        const long long qaEndQ0 = g_qaTraceEnabled ? QaTraceQpcNow() : 0;
        if (g_qaTraceEnabled)
        {
            memset(&qaRow, 0, sizeof(qaRow)); // POD row; every unfilled field stays 0
            qaRow.kind = 0;
            qaRow.layerCount = (int)layerCount;
            qaRow.shouldRender = (fs.shouldRender == XR_TRUE) ? 1 : 0;
            qaRow.trackingValid = viewsValid ? 1 : 0;
            qaRow.haveLayerPose = g_haveLayerPose ? 1 : 0;
            qaRow.activeSourceGpu = g_activeSourceGpu ? 1 : 0;
            qaRow.frameSerial = (unsigned long long)serial;
            qaRow.layerPoseSerial = g_lastLayerPoseSerial.load();
            qaRow.activeFrameSerial = g_lastConsumedSerial.load();
            // ACTUAL current located-pose serial for THIS frame's views (0 when
            // tracking invalid — never a fabricated value).
            qaRow.currentLocatedPoseSerial = viewsValid ? g_poseSerial.load() : 0ull;
            qaRow.targetDisplayTimeNs = (long long)fs.predictedDisplayTime;      // signed XrTime ns
            qaRow.targetDisplayPeriodNs = (long long)fs.predictedDisplayPeriod;  // signed XrDuration ns
            qaRow.callBeginQpc = qaEndQ0;
            qaRow.consumeBeginQpc = qaConsumeQ0;
            qaRow.consumeEndQpc = qaConsumeQ1;
            qaRow.waitBeginQpc = qaWaitQ0;  // ACTUAL successful WaitFrame window
            qaRow.waitEndQpc = qaWaitQ1;    // for this loop iteration
            qaRow.beginBeginQpc = qaBeginQ0; // ACTUAL successful BeginFrame window
            qaRow.beginEndQpc = qaBeginQ1;
            // 'shared' is labeled ONLY when the submitted scene genuinely
            // carries the coherent cached commit (exact frame+pose serial
            // agreement with the by-value identity AND the layer pose). Legacy
            // or synthetic commits invalidated the cache first, so a stale
            // shared identity can never be reported over real content.
            const bool sharedCoherent =
                g_qaTraceSharedValid && (g_activeSourceGpu ? true : false) && g_haveLayerPose &&
                qaRow.layerPoseSerial == (unsigned long long)g_qaTraceSharedId.poseSerial &&
                qaRow.activeFrameSerial == (unsigned long long)g_qaTraceSharedId.frameSerial;
            int kindSel;
            if (layerCount == 0) kindSel = 0;                                   // none submitted
            else if (!g_sceneMode) kindSel = 3;                                  // synthetic quad
            else if (sharedCoherent) kindSel = 1;                                // shared commit
            else kindSel = 2;                                                    // cpu-live/raw-live/static-preview legacy pair
            qaRow.sourceKind = kindSel;
            qaRow.sharedIdentityValid = (kindSel == 1) ? 1 : 0;
            if (kindSel == 1)
            {
                qaRow.idEpoch = (unsigned long long)g_qaTraceSharedId.epoch;
                qaRow.idGeneration = (unsigned long long)g_qaTraceSharedId.generation;
                qaRow.idPoseBeginQpc = g_qaTraceSharedId.poseSampleBeginQpc;
                qaRow.idPoseEndQpc = g_qaTraceSharedId.poseSampleEndQpc;
                qaRow.idCaptureBeginQpc = g_qaTraceSharedId.captureBeginQpc;
                qaRow.idCaptureEndQpc = g_qaTraceSharedId.captureEndQpc;
                qaRow.sampleDisplayTimeNs = g_qaTraceSharedPose.qaDisplayTime;   // signed XrTime ns
                qaRow.sampleDisplayPeriodNs = g_qaTraceSharedPose.qaDisplayPeriod;
                qaRow.sampleLocateBeginQpc = g_qaTraceSharedPose.qaLocateBeginQpc;
                qaRow.sampleLocateEndQpc = g_qaTraceSharedPose.qaLocateEndQpc;
            }
            for (int e = 0; e < 2; ++e)
            {
                const XrPosef& p = views[e].pose;
                double* d = (e == 0) ? qaRow.curL : qaRow.curR;
                d[0] = p.position.x; d[1] = p.position.y; d[2] = p.position.z;
                d[3] = p.orientation.x; d[4] = p.orientation.y; d[5] = p.orientation.z; d[6] = p.orientation.w;
                // The ACTUAL submitted tuple: pv[e].pose (the product's own
                // layer-vs-fresh selection is already resolved in pv). Zero
                // when layerCount == 0 — nothing was submitted.
                if (layerCount > 0)
                {
                    const XrPosef& sp = pv[e].pose;
                    double* dl = (e == 0) ? qaRow.layL : qaRow.layR;
                    dl[0] = sp.position.x; dl[1] = sp.position.y; dl[2] = sp.position.z;
                    dl[3] = sp.orientation.x; dl[4] = sp.orientation.y;
                    dl[5] = sp.orientation.z; dl[6] = sp.orientation.w;
                }
            }
            // Content key of THIS submission (compared post-call against the
            // last SUCCESSFUL delivery only).
            if (kindSel == 1 || kindSel == 2) { qaKeyFrame = qaRow.activeFrameSerial; qaKeyPose = qaRow.layerPoseSerial; }
            else if (kindSel == 3) { qaKeyFrame = qaRow.frameSerial; qaKeyPose = qaRow.currentLocatedPoseSerial; }
            // qaRow.contentClass stays 0 here: new/reused is decided ONLY after
            // the call, and ONLY for a successful delivery.
        }
#endif
        r = xrEndFrame(g_sess, &fei);
#if defined(VRSIM_QA_FRAME_TRACE)
        if (g_qaTraceEnabled)
        {
            // Fill ONLY the ACTUAL post-call facts: result, closed call window
            // and success-only classification. Failure / zero-layer rows keep
            // contentClass 0 and never move the last-successful baseline.
            qaRow.callEndQpc = QaTraceQpcNow();
            qaRow.result = (int)r;
            const bool delivered = (r == XR_SUCCESS && layerCount > 0);
            if (delivered)
                qaRow.contentClass = (qaRow.sourceKind == g_qaTraceLastOkKind &&
                                      qaKeyFrame == g_qaTraceLastOkFrame &&
                                      qaKeyPose == g_qaTraceLastOkPose) ? 2 : 1;
            QaTraceAddRow(qaRow);
            if (delivered)
            {
                g_qaTraceLastOkKind = qaRow.sourceKind;
                g_qaTraceLastOkFrame = qaKeyFrame;
                g_qaTraceLastOkPose = qaKeyPose;
            }
        }
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        // R5: an armed observer that owns refs, latched uncertainty (including
        // a throwing WorkerInit/CreateQuery) or unproven cleanup quarantines
        // EVEN PRE-LEASE; all refs/context and the false proof flags stay
        g_d11GpuAnySlotHeld = g_d11GpuAnySlotHeld || g_d11GpuRefsOut ||
                              g_d11GpuObsUncertain.load();
        if ((g_d11GpuInitDone || g_d11GpuAnySlotHeld || g_d11GpuRefsOut) &&
            (g_d11GpuRefsOut || g_d11GpuObsUncertain.load() || !g_d11GpuCleanupDone))
            g_sharedQuarantined = true;
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
        // R5 joint gate: only once the original joinable() test has refused a
        // live worker may the non-atomic observer state be read here; owned or
        // uncertain refs block ANY device/context release. No cleanup bypass,
        // no false OFF; the existing quarantine already blocks new Creates.
        if (!blocked &&
            (g_d11GpuInitDone || g_d11GpuAnySlotHeld || g_d11GpuRefsOut ||
             g_d11GpuObsUncertain.load()) &&
            (g_d11GpuRefsOut || !g_d11GpuCleanupDone || g_d11GpuObsUncertain.load()))
            blocked = true;
#endif
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
    // DEVICE-CAPS-01: the per-session capability cache and per-eye output
    // extents clear ONLY on this actual (non-refused) cleanup path — same rule
    // as the frozen color contract; a refused cleanup retains everything.
    g_deviceCaps = VRSIMCaps::DeviceCapsV1{};
    g_eyeOutW[0] = g_eyeOutW[1] = 0;
    g_eyeOutH[0] = g_eyeOutH[1] = 0;
    // HIGHRES-H2-DIAG-FB: the FB diagnostics cache + optional PFN reset only on
    // this actual (non-refused) cleanup path; refused cleanups return earlier
    // and retain everything.
    ResetFbColorSpaceDiagnostics();
    g_gpuPass.Reset();
    g_gpuPassReady = false; g_gpuPassW = 0; g_gpuPassH = 0;
#if defined(VRSIM_QA_MIXED_SOURCE)
    g_gpuPassMixedSourceQa = false; // mixed pass identity is invalidated with readiness
#endif
    g_activeSourceGpu = false;
#if defined(VRSIM_QA_FRAME_TRACE)
    g_qaTraceSharedValid = false; // central teardown invalidated the real active identity
#endif
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
#if defined(VRSIM_QA_D9_BEGIN_POLL)
            // SOURCE-UI-BEGIN-POLL-01: refuse an occupied async QA ledger BEFORE
            // any new reservation. Ordinary synchronous backpressure (Pending=1);
            // NEVER a fake async token and it never disturbs the durable lease.
            if (g_qaLedger.occupied) return VRSIM::kSharedExportPending;
#endif
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
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_PREPARE_WAIT)
        // Optional NUMERIC observation of the ACTUAL returned call only.
        // Runtime OFF => pwOn false => nullptr passed (bridge performs zero
        // added sampling) and the guard stays unarmed (no publish, no lock).
        // ON => the stack POD is zeroed and filled by the bridge itself. The
        // deferred guard publishes at scope exit, strictly AFTER the original
        // save-first/classification work below on every early return; it
        // changes no branch, no granted ticket64, no prh and no return value.
        VRSIM::PrepareWaitSample pwSample;
        const bool pwOn = QaPrepareWaitActive();
        if (pwOn) memset(&pwSample, 0, sizeof(pwSample));
        const HRESULT prh = g_sharedBridge.PrepareOnSourceUI((uint32_t)slot, leftSurface, rightSurface,
                                                             &granted, pwOn ? &pwSample : nullptr);
        QaPrepareWaitGuard pwGuard;
        if (pwOn) pwGuard.Arm(pwSample, prh, granted.ticket, id);
#else
        const HRESULT prh = g_sharedBridge.PrepareOnSourceUI((uint32_t)slot, leftSurface, rightSurface, &granted);
#endif

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
#if defined(VRSIM_QA_D9_BEGIN_POLL)
            if (g_qaLedger.occupied)
                return VRSIM::kSharedExportPending; // durable QA async lease unresolved
#endif

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

#if defined(VRSIM_QA_D9_BEGIN_POLL)
// =============================================================================
// SOURCE-UI-BEGIN-POLL-01: opt-in QA-only async Begin/Poll/Cancel facade.
// These three C exports exist ONLY under VRSIM_QA_D9_BEGIN_POLL and reuse the
// independently proved internal bridge QaBegin/QaPoll/QaRequestCancel surface.
// They add NO new D3D/COM/driver logic of their own: a single fixed native
// ledger retains the ORIGINAL reservation, identity and token across UI
// callbacks so the source UI is never occupied while the original GPU copy is
// pending. The default OFF body and the old ABI are untouched. C ABI never
// throws. Return codes reuse the shared export set: 0 completed | 1 pending |
// -1 bad argument | -2 unavailable | -3 stale | -4 quarantined | -5 error.
// No GPU/driver/COM call is ever made while a ring/meta gate is held.
// =============================================================================
static_assert(sizeof(VRSIM::QaBeginTokenV1) == 32, "token32 layout");
static_assert(sizeof(VRSIM::QaBeginResultV1) == 80, "result80 layout");
static_assert(sizeof(VRSIM::QaPollResultV1) == 56, "result56 layout");

// Honest preflight "None" image: a rejection made WITHOUT a bridge call must
// never present a fictitious real HRESULT observation. version/structBytes are
// set; status stays None and every real count/HRESULT stays zero.
static void QaBeginResultNoneV1(VRSIM::QaBeginResultV1* r)
{
    std::memset(r, 0, sizeof(*r));
    r->version = VRSIM::kStereoBridgeQaStructVersionV1;
    r->structBytes = (uint32_t)sizeof(VRSIM::QaBeginResultV1);
    r->status = (uint32_t)VRSIM::kQaStatusNone;
}
static void QaPollResultNoneV1(VRSIM::QaPollResultV1* r)
{
    std::memset(r, 0, sizeof(*r));
    r->version = VRSIM::kStereoBridgeQaStructVersionV1;
    r->structBytes = (uint32_t)sizeof(VRSIM::QaPollResultV1);
    r->status = (uint32_t)VRSIM::kQaStatusNone;
}

// QA-only no-throw per-call active-count guard. Mirrors SharedActiveCallGuard's
// counting (ArmLocked runs inside the ring -> meta grant gate) but its
// destructor NEVER throws and NEVER fabricates a clean drop: it attempts the
// decrement under the meta gate inside its own try/catch, and if that drop
// cannot be proved (the state lock itself failed) it leaves the count RETAINED
// and requests an atomic stop, so the session fails safe instead of reporting a
// count it cannot prove. The old SharedActiveCallGuard and the default OFF
// behaviour are untouched.
struct QaActiveCallGuard
{
    bool armed = false;
    void ArmLocked() { armed = true; ++g_sharedSourceActiveCalls; }
    ~QaActiveCallGuard() noexcept
    {
        if (!armed) return;
        try
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            --g_sharedSourceActiveCalls;
        }
        catch (...)
        {
            // Unproven drop: retain the count (never fabricate 0) + atomic stop.
            g_stopRequested.store(true);
        }
    }
};

// Optional error-string reporting is BEST EFFORT and must never skip mandatory
// state protection nor escape the C ABI. The allocating std::string build AND
// SetErr both run inside this try/catch, and every call site is OUTSIDE a native
// gate. A reporting failure is swallowed; it can never veto a retain/close.
// This QA-private helper is a C++ function template and so needs C++ linkage:
// it is given a narrow nested extern "C++" scope. The surrounding broad
// extern "C" export block (existing exports + the 3 new QA exports) keeps its
// C linkage; only this template is C++-linked.
extern "C++" {
template <class QaFn>
static void QaReport(QaFn&& fn) noexcept
{
    try { SetErr(fn()); } catch (...) { /* optional reporting only */ }
}
}

// Mandatory retain/quarantine of a genuinely reserved QA claim. The state
// mutation runs under ring -> meta and NEVER depends on the optional error
// string (callers report separately, outside the gate). Returns false ONLY if
// the state lock itself could not be taken: the caller must then request an
// atomic stop and return nonzero, never faking success/cleanup.
static bool QaQuarantineSlot(int slot) noexcept
{
    if (slot < 0) return true; // nothing was reserved
    try
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta
        g_sharedQuarantined = true;
        if (slot < RING_SLOTS) g_sharedPhase[slot] = kSharedPhaseQuarantined;
        return true;
    }
    catch (...) { return false; }
}

// Real pure-cached bridge evidence gathered OUTSIDE the native ring/meta gates:
// QaSnapshot + Quarantined + Stats are pure cached Interlocked reads (no GPU,
// COM, wait or allocation). This is the REAL durable-ledger proof; a clear is
// never inferred from the 72B Stats FREE alone. `gathered` stays false if any
// read failed, and an un-gathered snapshot then proves NOTHING (a zeroed image
// must never read as an empty-lease proof).
struct QaBridgeEvidenceV1
{
    VRSIM::QaBridgeSnapshotV1 snap{};
    VRSIM::SharedBridgeStatsV1 stats{};
    bool bridgeQuarantined = false;
    bool gathered = false;
};
static QaBridgeEvidenceV1 QaGatherEvidence() noexcept
{
    QaBridgeEvidenceV1 e{};
    try
    {
        g_sharedBridge.QaSnapshot(&e.snap);
        g_sharedBridge.Stats(&e.stats);
        e.bridgeQuarantined = g_sharedBridge.Quarantined();
        e.gathered = true;
    }
    catch (...) { e.gathered = false; }
    return e;
}

// Durable-ledger EMPTY proof. Requires the real cached snapshot to show the QA
// ledger EMPTY, no slot valid, no refs held, not quarantined and not finalizing,
// AND the bridge's cached slot state FREE (paired). After a real QaReleaseQaClaim
// the snapshot epoch/generation/nonce are genuinely ZEROED, so they are NOT
// required to match here. `requireQueryProven` adds the real GetData completion
// proof needed for a completedNoGrant/failedDrained resolution; a first-Init or
// proven-prework rollback issues no QA query, so it needs only the empty proof.
static bool QaBridgeEmptyProven(const QaBridgeEvidenceV1& e, int slot, bool requireQueryProven)
{
    if (!e.gathered) return false;
    if (slot < 0 || slot >= (int)VRSIM::kStereoBridgeSlotCountV1) return false;
    const bool ledgerEmpty   = e.snap.ledgerState == (uint32_t)VRSIM::kQaLedgerEmpty;
    const bool slotValidZero = e.snap.slotValid == 0u;
    const bool refsClean     = e.snap.refsHeldMask == 0u;
    const bool notQuarantined= (e.snap.quarantined == 0u) && !e.bridgeQuarantined;
    const bool notFinalizing = e.snap.finalizing == 0u;
    const bool pairedFree    = e.stats.slotState[slot] == (uint32_t)VRSIM::kSharedStereoFree;
    const bool queryProven   = e.snap.queryState == (uint32_t)VRSIM::kQaQueryCompleteProven;
    return ledgerEmpty && slotValidZero && refsClean && notQuarantined &&
           notFinalizing && pairedFree && (!requireQueryProven || queryProven);
}

// Real-pending proof for a Begin/Poll S_FALSE. Requires the actual call return
// AND the result HRESULT to be S_FALSE, a correct result header, the exact
// original token key (version/32/slot/generation/epoch/nonzero nonce), and the
// real cached snapshot to show the lease PREPARING on the original slot with the
// original key/stamp/captureBegin and an ARMED-PENDING query, with no quarantine.
// copyOkMask is deliberately NOT required to be 3: a real right-copy failure may
// legitimately need a pending drain of the already-submitted left copy (later
// resolved as failedDrained). A nonzero granted ticket is NOT a pending and is
// routed through the save-first grant path instead.
static bool QaPendingProven(const QaBridgeEvidenceV1& ev, HRESULT callHr, LONG resultHr,
                            uint32_t resVersion, uint32_t resStructBytes, uint32_t expectStructBytes,
                            const VRSIM::QaBeginTokenV1& token, int slot,
                            const VRSIM::SharedCaptureIdentityV1& id)
{
    if (callHr != S_FALSE || resultHr != (LONG)S_FALSE) return false;
    if (resVersion != VRSIM::kStereoBridgeQaStructVersionV1) return false;
    if (resStructBytes != expectStructBytes) return false;
    if (token.version != VRSIM::kStereoBridgeQaStructVersionV1) return false;
    if (token.structBytes != (uint32_t)sizeof(VRSIM::QaBeginTokenV1)) return false;
    if (token.nonce == 0) return false;
    if ((int)token.slotIndex != slot) return false;
    if (token.generation != id.generation) return false;
    if (token.epoch != id.epoch) return false;
    if (!ev.gathered) return false;
    if (ev.snap.ledgerState != (uint32_t)VRSIM::kQaLedgerPreparing) return false;
    if (ev.snap.slotValid == 0u) return false;
    if ((int)ev.snap.slotIndex != slot) return false;
    if (ev.snap.nonce != token.nonce) return false;
    if (ev.snap.epoch != token.epoch) return false;
    if (ev.snap.generation != token.generation) return false;
    if (ev.snap.stampFrame != id.frameSerial) return false;
    if (ev.snap.stampPose != id.poseSerial) return false;
    if (ev.snap.stampCaptureBeginQpc != id.captureBeginQpc) return false;
    if (ev.snap.queryState != (uint32_t)VRSIM::kQaQueryArmedPending) return false;
    if (ev.snap.quarantined != 0u || ev.bridgeQuarantined) return false;
    // Corroborate the REAL own-ref/lifecycle/timing state of the pending lease
    // from actual cached snapshot fields only (no extra QPC/SDK/GPU work). The
    // four source-side refs are held together during a live PREPARING lease
    // (Left1|Right2|Device4|Parent8 == 15), initialized with no finalize in
    // progress and a proven monotonic QPC within the unexpired drain budget.
    // copyOkMask/admissionClosed/cancelRequested are deliberately NOT asserted:
    // a right-copy failure or a legitimately closed old token may still pend.
    if (ev.snap.refsHeldMask !=
        (uint32_t)(VRSIM::kQaRefLeftSurface | VRSIM::kQaRefRightSurface |
                   VRSIM::kQaRefDevice | VRSIM::kQaRefParent))
        return false;
    if (ev.snap.initializedNow == 0u) return false;
    if (ev.snap.finalizing != 0u || ev.snap.finalized != 0u) return false;
    if (ev.snap.timingOk == 0u) return false;
    if (ev.snap.qpcFrequency == 0ull || ev.snap.qpcFrequency > (uint64_t)INT64_MAX / 1000ull)
        return false;
    if (ev.snap.startQpc == 0ull) return false;
    if (ev.snap.lastPollElapsedMs >= 1000ull) return false;
    return true;
}

// Real ACCEPTED-cancel proof. The bridge cancel returns CloseAdmissionLocked's
// actual HRESULT (S_OK first close, S_FALSE idempotent) and publishes the CACHED
// qaLastStatus_ into result.status — that status is NOT a dedicated accepted-
// cancel enum, so acceptance is never inferred from status alone. It is proven
// ONLY by: an actual S_OK/S_FALSE return, a well-formed result header whose
// bridgeHresult matches the actual return, the UNCHANGED original token32, and a
// real gathered snapshot corroborating the ORIGINAL key/stamp on the ORIGINAL
// slot in ledger PREPARING with cancelRequested=1, admissionClosed=1,
// initialized, not finalizing/finalized and not quarantined. A wrong-key/thread/
// busy rejection returns E_ACCESSDENIED/E_FAIL and is excluded here (no lease was
// mutated), never a fabricated acceptance.
static bool QaCancelAcceptedProven(const QaBridgeEvidenceV1& ev, HRESULT callHr,
                                   const VRSIM::QaBeginResultV1& cres,
                                   const VRSIM::QaBeginTokenV1& tok, int slot,
                                   const VRSIM::SharedCaptureIdentityV1& id)
{
    if (callHr != S_OK && callHr != S_FALSE) return false;
    if (cres.version != (uint32_t)VRSIM::kStereoBridgeQaStructVersionV1) return false;
    if (cres.structBytes != (uint32_t)sizeof(VRSIM::QaBeginResultV1)) return false;
    if (cres.bridgeHresult != (LONG)callHr) return false;
    if (memcmp(&cres.token, &tok, sizeof(tok)) != 0) return false;
    if (!ev.gathered) return false;
    if (ev.snap.ledgerState != (uint32_t)VRSIM::kQaLedgerPreparing) return false;
    if (ev.snap.slotValid == 0u) return false;
    if ((int)ev.snap.slotIndex != slot) return false;
    if (ev.snap.nonce != tok.nonce || ev.snap.epoch != tok.epoch || ev.snap.generation != tok.generation)
        return false;
    if (ev.snap.stampFrame != id.frameSerial) return false;
    if (ev.snap.stampPose != id.poseSerial) return false;
    if (ev.snap.stampCaptureBeginQpc != id.captureBeginQpc) return false;
    if (ev.snap.initializedNow == 0u) return false;
    if (ev.snap.finalizing != 0u || ev.snap.finalized != 0u) return false;
    if (ev.snap.cancelRequested == 0u || ev.snap.admissionClosed == 0u) return false;
    if (ev.snap.quarantined != 0u || ev.bridgeQuarantined) return false;
    return true;
}

// SAVE-FIRST + classify for a QA grant attempt, shared by Begin (immediate
// grant) and Poll (completed grant). `expectGrant` is true ONLY when the actual
// bridge result is the expected valid grant status. ANY real nonzero ticket is
// saved onto the exact original native slot FIRST — including on an unexpected/
// failed status — so a live ticket can never vanish from tracking. Publication
// happens ONLY on: a real bridge S_OK + expectGrant + the exact grant shape + an
// actual non-quarantine + a proven bridge QA ledger EMPTY / refs-clean / real
// PREPARED slot with the SAME granted ticket + the exact original native claim,
// full identity, lifetime bound key and every post-guard. A failed/contradictory/
// quarantined live ticket stays RECORDED and RETAINED; only a proven-known non-
// quarantine case (a real PREPARED grant native cannot publish because close won,
// or a bad-shape grant) follows old Prepare's exact-ticket discard + paired-FREE
// rollback. Returns 0 ONLY when really published (ticket64 written).
// `grantStatus` is the ACTUAL bres/pres status number; `expectedGrantStatus` is
// the status that alone makes a grant eligible to publish (Immediate for Begin,
// Completed for Poll). `resVersion`/`resStructBytes` are the ACTUAL result
// header and `expectStructBytes` its size; `resultBridgeHr` is the ACTUAL
// result.bridgeHresult. Publication requires the WHOLE well-formed grant
// envelope (real S_OK return AND result HRESULT + expected grant status +
// correct header), the exact shape, non-quarantine, and a real PREPARED ledger
// proof whose queryState is CompleteProven; a malformed envelope never
// normalizes into a clean success or discard.
static int QaSaveAndPublishGrant(int slot, unsigned long long claimEpoch,
                                 const VRSIM::SharedCaptureIdentityV1& id,
                                 const VRSIM::SharedPreparedPairV1& granted,
                                 HRESULT bridgeHr, uint32_t grantStatus,
                                 uint32_t resVersion, uint32_t resStructBytes,
                                 uint32_t expectStructBytes, LONG resultBridgeHr,
                                 uint32_t expectedGrantStatus, void* ticket64)
{
    const bool hadTicket = (granted.ticket != 0);
    const bool expectGrant = (grantStatus == expectedGrantStatus);
    // Grant envelope: real S_OK call return, result.bridgeHresult==S_OK, the
    // expected grant status, and the correct version/structBytes header.
    const bool envelopeOk =
        expectGrant && bridgeHr == S_OK && resultBridgeHr == (LONG)S_OK &&
        resVersion == (uint32_t)VRSIM::kStereoBridgeQaStructVersionV1 &&
        resStructBytes == expectStructBytes;

    // ---- SAVE FIRST: any real nonzero ticket, before ANY classification ----
    bool ticketCaptured = false;
    if (hadTicket)
    {
        try
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedLastBridgeHresult = (long long)bridgeHr; // recorded in the FIRST save gate
            StereoSlot& cs = g_ring[slot];
            const bool stillOriginalClaim =
                g_qaLedger.occupied && g_qaLedger.slot == slot && g_qaLedger.claimEpoch == claimEpoch &&
                g_sharedLifetimeStarted &&
                g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                cs.state.load() == 1 && cs.mode == kGpuSlotModeSharedV1 && cs.epoch == claimEpoch &&
                g_sharedPhase[slot] == kSharedPhasePreparing &&
                VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                VRSIM::SharedIdentityFieldsEqual(cs.sharedIdentity, id);
            if (stillOriginalClaim) { cs.sharedTicket = granted; ticketCaptured = true; }
        }
        catch (...) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
    }
    if (hadTicket && !ticketCaptured)
    {
        // LIVE ticket but the original claim was lost between gates: fail closed,
        // retain + quarantine, never discard/rollback an unknown, never overwrite
        // a foreign slot identity.
        if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
        QaReport([&] { return std::string("qa grant live ticket lost original slot hr=") + SharedHrName(bridgeHr); });
        return VRSIM::kSharedExportQuarantined;
    }

    const QaBridgeEvidenceV1 ev = QaGatherEvidence(); // pure cached, outside native gates
    const bool notQuarantined = ev.gathered && !ev.bridgeQuarantined && ev.snap.quarantined == 0u;
    const bool grantShapeOk =
        granted.version == VRSIM::kStereoBridgeVersionV1 &&
        granted.structBytes == (uint32_t)sizeof(VRSIM::SharedPreparedPairV1) &&
        granted.slotIndex == (uint32_t)slot &&
        granted.generation == id.generation && granted.epoch == id.epoch && granted.ticket != 0 &&
        granted.width == VRSIM::kStereoBridgeWidthV1 &&
        granted.height == VRSIM::kStereoBridgeHeightV1 &&
        granted.format88 == VRSIM::kStereoBridgeFormat88V1;
    // Real PREPARED proof: after a bridge grant the QA async ledger is EMPTY and
    // refs are clean, while the ordinary slot is genuinely PREPARED carrying the
    // SAME granted ticket number the snapshot recorded.
    const bool preparedProven =
        ev.gathered && slot >= 0 && slot < (int)VRSIM::kStereoBridgeSlotCountV1 &&
        ev.snap.ledgerState == (uint32_t)VRSIM::kQaLedgerEmpty &&
        ev.snap.slotValid == 0u &&
        ev.snap.refsHeldMask == 0u && ev.snap.finalizing == 0u &&
        ev.snap.queryState == (uint32_t)VRSIM::kQaQueryCompleteProven &&
        ev.stats.slotState[slot] == (uint32_t)VRSIM::kSharedStereoPrepared &&
        ev.snap.ticketGranted != 0u && ev.snap.grantedTicket == granted.ticket;

    // Exact original-ticket source-UI Discard + proven paired-FREE rollback (old
    // Prepare discipline), used only on a proven-known non-quarantine case.
    auto discardAndRollback = [&](void) -> int
    {
        const HRESULT drh = g_sharedBridge.DiscardPreparedOnSourceUI(granted); // OUTSIDE native gates
        const QaBridgeEvidenceV1 ev2 = QaGatherEvidence();
        const bool freeProven =
            ev2.gathered && drh == S_OK &&
            slot >= 0 && slot < (int)VRSIM::kStereoBridgeSlotCountV1 &&
            ev2.stats.slotState[slot] == (uint32_t)VRSIM::kSharedStereoFree &&
            ev2.snap.ledgerState == (uint32_t)VRSIM::kQaLedgerEmpty &&
            ev2.snap.slotValid == 0u && ev2.snap.refsHeldMask == 0u && ev2.snap.finalizing == 0u &&
            !ev2.bridgeQuarantined && ev2.snap.quarantined == 0u;
        bool rolledBack = false;
        try
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedLastBridgeHresult = (long long)drh;
            StereoSlot& rs = g_ring[slot];
            const bool exact =
                g_qaLedger.occupied && g_qaLedger.slot == slot && g_qaLedger.claimEpoch == claimEpoch &&
                rs.state.load() == 1 && rs.mode == kGpuSlotModeSharedV1 && rs.epoch == claimEpoch &&
                g_sharedLifetimeStarted &&
                g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                !g_sharedQuarantined &&
                VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                VRSIM::SharedIdentityFieldsEqual(rs.sharedIdentity, id) &&
                VRSIM::SharedTicketFieldsEqual(granted, rs.sharedTicket);
            if (freeProven && exact)
            {
                rs.state.store(0); rs.epoch = 0; rs.mode = (int)VRSIM::kGpuSlotModeCpu;
                rs.sharedTicket = VRSIM::SharedPreparedPairV1{};
                rs.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
                g_sharedPhase[slot] = kSharedPhaseNone;
                g_qaLedger = QaAsyncLedgerV1{};
                rolledBack = true;
            }
            else { g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined; }
        }
        catch (...) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
        QaReport([&] { return std::string("qa grant rollback (discard hr=") + SharedHrName(drh) + ")"; });
        if (!rolledBack) return VRSIM::kSharedExportQuarantined;
        return grantShapeOk ? VRSIM::kSharedExportStale : VRSIM::kSharedExportError;
    };

    // Publish path: only a WELL-FORMED grant envelope (expected grant status +
    // real S_OK call return + result.bridgeHresult==S_OK + correct
    // version/structBytes), the exact shape, an actual non-quarantine and the
    // real PREPARED proof (incl. queryState CompleteProven) are eligible.
    if (envelopeOk && grantShapeOk && notQuarantined && preparedProven)
    {
        bool published = false; int lostRc = 0; bool closedMidway = false;
        try
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            StereoSlot& ws = g_ring[slot];
            const bool stillOurs =
                g_qaLedger.occupied && g_qaLedger.slot == slot && g_qaLedger.claimEpoch == claimEpoch &&
                g_sharedLifetimeStarted && g_sharedBound && !g_sharedQuarantined && !g_sharedFinalizeBusy &&
                g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                ws.state.load() == 1 && ws.mode == kGpuSlotModeSharedV1 && ws.epoch == claimEpoch &&
                g_sharedPhase[slot] == kSharedPhasePreparing &&
                VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                VRSIM::SharedIdentityFieldsEqual(ws.sharedIdentity, id);
            g_sharedLastBridgeHresult = (long long)bridgeHr;
            if (!stillOurs || !VRSIM::SharedTicketFieldsEqual(granted, ws.sharedTicket))
            {
                g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined;
                lostRc = VRSIM::kSharedExportQuarantined;
            }
            else
            {
                g_sharedPhase[slot] = kSharedPhasePrepared;
                closedMidway =
                    g_sharedClosing || g_sharedAdmissionClosed || g_sharedTeardownRequested ||
                    g_sharedFinalizeBusy || !g_ringAdmission || claimEpoch != g_ringEpoch ||
                    g_stopRequested.load();
                published = !closedMidway;
            }
        }
        catch (...) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
        if (lostRc)
        {
            QaReport([&] { return std::string("qa grant lost its identity mid-flight hr=") + SharedHrName(bridgeHr); });
            return lostRc;
        }
        if (published)
        {
            // Real granted, saved and native-published PREPARED ticket: the durable
            // pending-ledger role is complete and the ordinary Commit/Discard
            // lifecycle now owns the slot (the PREPARED phase keeps it visible to
            // drain/finalize). Clear occupancy; write the real ticket64.
            try
            {
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                g_qaLedger = QaAsyncLedgerV1{};
            }
            catch (...) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
            unsigned char tbuf[64];
            VRSIM::SharedTicketSerializeZeroedV1(tbuf, granted); // tail 60..63 serialized zero
            memcpy(ticket64, tbuf, 64);
            return VRSIM::kSharedExportCompleted;
        }
        // A real proven PREPARED grant that native cannot publish (close won):
        // proven-known exact-ticket discard + paired-FREE rollback.
        return discardAndRollback();
    }

    // A real ticket that is NOT a proven-expected valid grant (unexpected/malformed
    // status/header/hr, quarantine, unproven PREPARED, or a bad shape on a
    // non-corroborated slot).
    if (hadTicket)
    {
        // Bad shape on an otherwise WELL-FORMED (envelopeOk) proven real PREPARED,
        // non-quarantine S_OK grant is a proven-known case old Prepare discards
        // cleanly (-> Error). A malformed status/header/resultHR/callHR is NOT
        // this case: it falls through to retain + quarantine below.
        if (envelopeOk && notQuarantined && preparedProven && !grantShapeOk)
            return discardAndRollback();
        // Everything else with a live ticket is contradictory/unproven: it is
        // already RECORDED on the slot; retain + quarantine, never publish, never
        // discard an unknown.
        if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
        QaReport([&] { return std::string("qa grant unexpected/unproven live ticket retained status-hr=") +
                              SharedHrName(bridgeHr); });
        return VRSIM::kSharedExportQuarantined;
    }

    // No ticket at all (defensive: callers route non-grant statuses elsewhere).
    if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
    QaReport([&] { return std::string("qa grant path with zero ticket hr=") + SharedHrName(bridgeHr); });
    return VRSIM::kSharedExportQuarantined;
}

// Resolve a real completedNoGrant(6)/failedDrained(7): the bridge really drained
// the submitted work (real GetData S_OK) but granted NO ticket. The native
// reservation is cleared ONLY on the real durable-ledger EMPTY proof (ledger
// EMPTY + slotValid0 + refsHeldMask0 + !quarantined/finalizing + paired bridge
// FREE + real GetData completion) AND the exact original native claim, full
// identity and lifetime bound key — never on FAILED(hr) alone. Anything
// contradictory/unproven retains + quarantines. Returns -3 for completedNoGrant,
// -5 for failedDrained on a proven clear, else -4 (or -5 if the state lock
// failed, after an atomic-stop request). The lifetime lease flag is NEVER
// cleared here; it is preserved for a true source finalize.
static int QaClearProvenDrained(int slot, unsigned long long claimEpoch,
                                const VRSIM::SharedCaptureIdentityV1& id,
                                HRESULT bridgeHr, bool noGrant)
{
    const int provenCode = noGrant ? VRSIM::kSharedExportStale : VRSIM::kSharedExportError;
    const QaBridgeEvidenceV1 ev = QaGatherEvidence(); // outside native gates
    const bool emptyProven = QaBridgeEmptyProven(ev, slot, /*requireQueryProven=*/true);
    try
    {
        std::lock_guard<std::mutex> rlk(g_ringMutex);
        std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
        g_sharedLastBridgeHresult = (long long)bridgeHr;
        StereoSlot& rs = g_ring[slot];
        const bool exactNativeClaim =
            g_qaLedger.occupied && g_qaLedger.slot == slot && g_qaLedger.claimEpoch == claimEpoch &&
            g_sharedLifetimeStarted &&
            g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
            rs.state.load() == 1 && rs.mode == kGpuSlotModeSharedV1 && rs.epoch == claimEpoch &&
            g_sharedPhase[slot] == kSharedPhasePreparing &&
            VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
            VRSIM::SharedIdentityFieldsEqual(rs.sharedIdentity, id);
        if (emptyProven && exactNativeClaim && !g_sharedQuarantined)
        {
            rs.state.store(0); rs.epoch = 0; rs.mode = (int)VRSIM::kGpuSlotModeCpu;
            rs.sharedTicket = VRSIM::SharedPreparedPairV1{};
            rs.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
            g_sharedPhase[slot] = kSharedPhaseNone;
            g_qaLedger = QaAsyncLedgerV1{}; // reservation resolved; lifetime lease preserved
            SharedUpdateDrainLocked(ev.stats);
            return provenCode;
        }
        g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined;
        return VRSIM::kSharedExportQuarantined;
    }
    catch (...)
    {
        // State lock failed: do NOT fake a clear/cleanup; request an atomic stop.
        g_stopRequested.store(true);
        return VRSIM::kSharedExportError;
    }
}

// Source UI thread ONLY, inside the typed owner's D3DImage double-lock callback.
// leftSurface/rightSurface are borrowed IDirect3DSurface9 handle NUMBERS valid
// only during this call — passed straight to the bridge, NEVER cached. Reserves
// the ORIGINAL ring slot under the existing ring -> meta short gate (reusing
// every original structural / scene-ready / paired-FREE / first-lease / LUID /
// Init gate), then runs the bridge QaBegin OUTSIDE both gates. A real S_FALSE
// retains the exact token and PREPARING (zero ticket, Pending); an immediate
// real S_OK saves+publishes a PREPARED ticket (Completed). No GPU under a gate.
__declspec(dllexport) int VRSIM_QaBeginSharedStereoV1(
    unsigned long long leftSurface, unsigned long long rightSurface,
    const void* identity192, unsigned int identityBytes,
    void* result80, unsigned int resultBytes,
    void* ticket64, unsigned int ticketBytes)
{
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportUnavailable;
    if (!identity192 || !result80 || !ticket64) return VRSIM::kSharedExportBadArgument;
    if (identityBytes != VRSIM::kSharedCaptureIdentityV1Bytes) return VRSIM::kSharedExportBadArgument;
    if (resultBytes != sizeof(VRSIM::QaBeginResultV1)) return VRSIM::kSharedExportBadArgument;
    if (ticketBytes != VRSIM::kSharedTicketExportBytesV1) return VRSIM::kSharedExportBadArgument;
    if (leftSurface == 0 || rightSurface == 0) return VRSIM::kSharedExportBadArgument;

    QaBeginResultNoneV1((VRSIM::QaBeginResultV1*)result80); // honest None until the bridge really runs
    std::memset(ticket64, 0, VRSIM::kSharedTicketExportBytesV1); // only a real grant writes it

    int slot = -1;
    unsigned long long claimEpoch = 0;
    QaActiveCallGuard activeCall;
    try
    {
        VRSIM::SharedCaptureIdentityV1 id{};
        memcpy(&id, identity192, sizeof(id)); // 192 B fully named explicit layout
        if (!SharedWireValidatePrepareIdentity(id)) return VRSIM::kSharedExportBadArgument;

        bool bound = false;
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
            if (g_qaLedger.occupied) return VRSIM::kSharedExportPending; // single fixed ledger
            if (g_sharedQuarantined) return VRSIM::kSharedExportQuarantined;
            if (g_sharedSourceFinalized || g_sharedFinalizedOnce) return VRSIM::kSharedExportUnavailable;
            if (g_sharedClosing || g_sharedAdmissionClosed || g_sharedTeardownRequested ||
                g_sharedFinalizeBusy) return VRSIM::kSharedExportStale;
            if (g_sharedSourceActiveCalls > 0) return VRSIM::kSharedExportPending;
            if (g_stopRequested.load()) return VRSIM::kSharedExportStale;
            if (g_state.load() == VRSIM_ERROR) return VRSIM::kSharedExportStale;
            if (g_state.load() != VRSIM_RUNNING) return VRSIM::kSharedExportPending;
            if (!g_ringAdmission) return VRSIM::kSharedExportStale;
            if (id.epoch != g_ringEpoch) return VRSIM::kSharedExportStale;
            bound = g_sharedBound;
            if (g_sharedLifetimeStarted &&
                (id.epoch != g_sharedBoundEpoch || id.generation != g_sharedBoundGeneration))
                return VRSIM::kSharedExportStale; // one frozen epoch/generation per lease
            if (!g_dev || !g_ctx || !g_runtimeRequiredLuidValid) return VRSIM::kSharedExportUnavailable;
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
                bool allFree = true;
                for (int i = 0; i < RING_SLOTS; ++i)
                {
                    if (g_ring[i].state.load() != 0 || g_sharedPhase[i] != kSharedPhaseNone)
                    { allFree = false; break; }
                    if (i < (int)VRSIM::kStereoBridgeSlotCountV1 &&
                        bs.slotState[i] != (uint32_t)VRSIM::kSharedStereoFree)
                    { allFree = false; break; }
                }
                if (!allFree) return VRSIM::kSharedExportPending;
            }
            slot = RingClaimSharedPairedFreeLocked(bs, nullptr);
            if (slot < 0) return VRSIM::kSharedExportPending; // no paired FREE; NEVER steal READY
            claimEpoch = g_ringEpoch;
            if (!g_sharedLifetimeStarted)
            {
                g_sharedBoundEpoch = id.epoch;
                g_sharedBoundGeneration = id.generation;
                g_sharedLifetimeStarted = true; // lease from the FIRST grant; an Init failure never clears it
            }
            RingRecordSharedPreparingLocked(slot, claimEpoch, id); // caller holds ring + meta
            // Record the FIXED durable ledger: original source thread, slot,
            // frozen claim epoch and full immutable identity. No token yet: a
            // zero token during Begin is an OCCUPIED reservation, not an empty
            // lease, and hasToken stays false until a real Pending is observed.
            g_qaLedger.occupied = true;
            g_qaLedger.sourceThread = (unsigned long)GetCurrentThreadId();
            g_qaLedger.slot = slot;
            g_qaLedger.claimEpoch = claimEpoch;
            g_qaLedger.identity = id;
            g_qaLedger.token = VRSIM::QaBeginTokenV1{};
            g_qaLedger.hasToken = false;
            activeCall.ArmLocked();
        }

        // Rollback of the still-PREPARING reservation on the REAL durable-ledger
        // EMPTY proof (pure cached snapshot + Stats + Quarantined, gathered
        // OUTSIDE the native gates) plus the exact original native claim, full
        // identity and lifetime bound key. `requireQueryProven` is false for a
        // first-Init / proven-prework rollback (no QA query was issued). The
        // lifetime lease flag is NEVER cleared here — it is preserved for a true
        // source finalize; only the per-call reservation + QA ledger are released.
        // An empty snapshot while the bridge was never Initialized is corroborated
        // by the paired bridge slot FREE, so it can release a reservation holding
        // no bridge resources, but never the lifetime lease.
        auto rollbackOrQuarantine = [&](bool requireQueryProven) -> bool
        {
            const QaBridgeEvidenceV1 ev = QaGatherEvidence(); // outside native gates
            const bool emptyProven = QaBridgeEmptyProven(ev, slot, requireQueryProven);
            try
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                StereoSlot& rs = g_ring[slot];
                const bool exact =
                    rs.state.load() == 1 && rs.mode == kGpuSlotModeSharedV1 && rs.epoch == claimEpoch &&
                    g_sharedPhase[slot] == kSharedPhasePreparing &&
                    g_qaLedger.occupied && g_qaLedger.slot == slot && g_qaLedger.claimEpoch == claimEpoch &&
                    g_sharedLifetimeStarted &&
                    g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                    VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                    VRSIM::SharedIdentityFieldsEqual(rs.sharedIdentity, id);
                if (emptyProven && exact && !g_sharedQuarantined)
                {
                    rs.state.store(0); rs.epoch = 0; rs.mode = (int)VRSIM::kGpuSlotModeCpu;
                    rs.sharedTicket = VRSIM::SharedPreparedPairV1{};
                    rs.sharedIdentity = VRSIM::SharedCaptureIdentityV1{};
                    g_sharedPhase[slot] = kSharedPhaseNone;
                    g_qaLedger = QaAsyncLedgerV1{}; // reservation gone; lifetime lease preserved
                    return true;
                }
                g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined;
                return false;
            }
            catch (...) { g_stopRequested.store(true); return false; }
        };

        // First valid Begin initializes the bridge exactly once, OUTSIDE both
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
                    QaReport([&] { return std::string("qa begin: shared bridge init quarantined hr=") + SharedHrName(ihr); });
                    return VRSIM::kSharedExportQuarantined; // unproven: slot + lease + ledger retained
                }
                const bool rb = rollbackOrQuarantine(false);
                QaReport([&] { return std::string("qa begin: shared bridge init failed hr=") + SharedHrName(ihr) +
                                      (rb ? " (reservation rolled back on proven empty; lease retained)"
                                          : " (reservation retained/quarantined)"); });
                return rb ? VRSIM::kSharedExportError : VRSIM::kSharedExportQuarantined;
            }
            bool goPrepare = false, closedMidway = false; int lostRc = 0;
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                g_sharedLastBridgeHresult = (long long)ihr;
                const bool stillOurs =
                    g_sharedLifetimeStarted && !g_sharedQuarantined && !g_sharedFinalizeBusy &&
                    g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                    g_ring[slot].state.load() == 1 && g_ring[slot].mode == kGpuSlotModeSharedV1 &&
                    g_ring[slot].epoch == claimEpoch && g_sharedPhase[slot] == kSharedPhasePreparing &&
                    g_qaLedger.occupied && g_qaLedger.slot == slot && g_qaLedger.claimEpoch == claimEpoch &&
                    VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                    VRSIM::SharedIdentityFieldsEqual(g_ring[slot].sharedIdentity, id);
                if (!stillOurs)
                {
                    g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined;
                    lostRc = VRSIM::kSharedExportQuarantined;
                }
                else
                {
                    g_sharedBound = true; // Init created REAL owned resources
                    closedMidway = g_sharedClosing || g_sharedAdmissionClosed ||
                                   g_sharedTeardownRequested || g_sharedFinalizeBusy ||
                                   !g_ringAdmission || claimEpoch != g_ringEpoch ||
                                   g_stopRequested.load();
                    goPrepare = !closedMidway;
                }
            }
            if (lostRc) { QaReport([] { return std::string("qa begin: init grant identity lost mid-flight"); }); return lostRc; }
            if (closedMidway)
            {
                // No ticket exists yet; the reservation may roll back ONLY on the
                // proven durable-ledger EMPTY + exact original claim. An unproven
                // rollback retains/quarantines and must NEVER be reported as a
                // clean lease cleanup or returned as Stale.
                const bool rb = rollbackOrQuarantine(false);
                QaReport([&] { return std::string("qa begin: lifecycle closed during init: begin refused; ") +
                                      (rb ? "reservation rolled back on proven empty (lease pending finalize)"
                                          : "reservation retained/quarantined (rollback unproven)"); });
                return rb ? VRSIM::kSharedExportStale : VRSIM::kSharedExportQuarantined;
            }
            if (!goPrepare) return VRSIM::kSharedExportStale; // unreachable; keep compiler honest
        }

        // Real GPU transaction: OUTSIDE g_ringMutex AND g_sharedMetaMutex. Feed
        // the bridge stamp from the ORIGINAL identity, never a new pose.
        VRSIM::QaSourceStampV1 stamp{};
        stamp.version = VRSIM::kStereoBridgeQaStructVersionV1;
        stamp.structBytes = (uint32_t)sizeof(VRSIM::QaSourceStampV1);
        stamp.frameSerial = id.frameSerial;
        stamp.poseSerial = id.poseSerial;
        stamp.captureBeginQpc = id.captureBeginQpc;

        VRSIM::QaBeginResultV1 bres{};
        VRSIM::SharedPreparedPairV1 granted{};
        const HRESULT brh = g_sharedBridge.QaBeginOnSourceUI(
            (uint32_t)slot, leftSurface, rightSurface, stamp, &bres, &granted);
        // Preserve the bridge's ACTUAL status/HRESULT/call counts after it ran.
        memcpy(result80, &bres, sizeof(bres));

        // SAVE FIRST: route any real nonzero ticket into the grant helper BEFORE
        // any extra lock / lastHR update / status switch, so the FIRST required
        // ring->meta save transaction records the actual ticket before lastHR is
        // assigned. The helper takes the REAL bres.status/version/structBytes/
        // bridgeHresult; a status BOOL is not part of the signature and no header
        // is fabricated from constants.
        if (granted.ticket != 0)
            return QaSaveAndPublishGrant(slot, claimEpoch, id, granted, brh,
                                         bres.status, bres.version, bres.structBytes,
                                         (uint32_t)sizeof(VRSIM::QaBeginResultV1),
                                         bres.bridgeHresult,
                                         (uint32_t)VRSIM::kQaStatusImmediateGranted, ticket64);

        // No-ticket path only: now record the actual lastHR.
        try
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedLastBridgeHresult = (long long)brh;
        }
        catch (...) { if (!QaQuarantineSlot(slot)) g_stopRequested.store(true); return VRSIM::kSharedExportError; }

        // A TRUE pending requires the real S_FALSE + the full snapshot
        // / token / claim proof BEFORE the token is saved and 1 is returned. A
        // real right-copy failure may legitimately pend (copyOkMask NOT required
        // 3); a contradictory pending is never fabricated into completion.
        if (bres.status == (uint32_t)VRSIM::kQaStatusPending)
        {
            const QaBridgeEvidenceV1 ev = QaGatherEvidence();
            if (QaPendingProven(ev, brh, bres.bridgeHresult, bres.version, bres.structBytes,
                                (uint32_t)sizeof(VRSIM::QaBeginResultV1), bres.token, slot, id))
            {
                bool saved = false;
                {
                    std::lock_guard<std::mutex> rlk(g_ringMutex);
                    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                    StereoSlot& cs = g_ring[slot];
                    const bool stillOurs =
                        g_qaLedger.occupied && g_qaLedger.slot == slot && g_qaLedger.claimEpoch == claimEpoch &&
                        g_sharedLifetimeStarted &&
                        g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                        cs.state.load() == 1 && cs.mode == kGpuSlotModeSharedV1 && cs.epoch == claimEpoch &&
                        g_sharedPhase[slot] == kSharedPhasePreparing && !g_sharedQuarantined &&
                        VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                        VRSIM::SharedIdentityFieldsEqual(cs.sharedIdentity, id);
                    if (stillOurs) { g_qaLedger.token = bres.token; g_qaLedger.hasToken = true; saved = true; }
                }
                if (saved) return VRSIM::kSharedExportPending;
                if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
                QaReport([] { return std::string("qa begin: pending claim lost before token save"); });
                return VRSIM::kSharedExportQuarantined;
            }
            if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
            QaReport([] { return std::string("qa begin: pending not proven by real S_FALSE/snapshot"); });
            return VRSIM::kSharedExportQuarantined;
        }

        // Real drain with NO ticket: resolve only on the durable-ledger EMPTY +
        // real GetData completion proof (never FAILED(hr) alone).
        if (bres.status == (uint32_t)VRSIM::kQaStatusCompletedNoGrant)
            return QaClearProvenDrained(slot, claimEpoch, id, brh, /*noGrant=*/true);
        if (bres.status == (uint32_t)VRSIM::kQaStatusFailedDrained)
            return QaClearProvenDrained(slot, claimEpoch, id, brh, /*noGrant=*/false);

        if (bres.status == (uint32_t)VRSIM::kQaStatusQuarantinedRetained)
        {
            if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
            QaReport([] { return std::string("qa begin: bridge quarantined/retained"); });
            return VRSIM::kSharedExportQuarantined;
        }

        // Pre-work rolled back (nothing submitted): the bridge already released
        // its claim/refs, so the applicable empty proof (no QA query) governs the
        // native reservation rollback; the lifetime lease is preserved.
        if (bres.status == (uint32_t)VRSIM::kQaStatusPreWorkRolledBack)
        {
            const bool rb = rollbackOrQuarantine(false);
            QaReport([&] { return std::string("qa begin: pre-work rolled back hr=") + SharedHrName(brh) +
                                  (rb ? "" : " (retained/quarantined)"); });
            return rb ? VRSIM::kSharedExportError : VRSIM::kSharedExportQuarantined;
        }

        // Rejected / None / unknown reaching here is contradictory with a native
        // reservation that got as far as a real bridge call: retain + quarantine,
        // never fabricate completion and never claim FREE.
        if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
        QaReport([&] { return std::string("qa begin: unexpected status ") + std::to_string(bres.status) +
                              " hr=" + SharedHrName(brh); });
        return VRSIM::kSharedExportQuarantined;
    }
    catch (...)
    {
        // C ABI never throws. Fail closed: retain the durable ledger and
        // quarantine a genuinely reserved claim. The mandatory state protection
        // runs FIRST and is never skipped by the optional error string; if the
        // state lock itself failed, request an atomic stop and still return
        // nonzero (never fabricate success/cleanup).
        if (slot >= 0 && !QaQuarantineSlot(slot)) g_stopRequested.store(true);
        QaReport([] { return std::string("qa begin threw"); });
        return VRSIM::kSharedExportError;
    }
}

// Source UI thread ONLY, on the ORIGINAL token. No new surfaces or identity
// argument; cannot sample a pose or render/copy/re-Issue/re-acquire. Validates
// full token shape/key and the original source thread BEFORE any true-lease
// mutation, and depends on the STORED claim (not the current RUNNING/live ring
// epoch), so the token stays legal for the old lease drain after Stop closed
// admission or advanced the epoch. Wrong/duplicate keys or a wrong thread leave
// the lease unchanged and never claim FREE.
__declspec(dllexport) int VRSIM_QaPollSharedStereoV1(
    const void* token32, unsigned int tokenBytes,
    void* result56, unsigned int resultBytes,
    void* ticket64, unsigned int ticketBytes)
{
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportUnavailable;
    if (!token32 || !result56 || !ticket64) return VRSIM::kSharedExportBadArgument;
    if (tokenBytes != sizeof(VRSIM::QaBeginTokenV1)) return VRSIM::kSharedExportBadArgument;
    if (resultBytes != sizeof(VRSIM::QaPollResultV1)) return VRSIM::kSharedExportBadArgument;
    if (ticketBytes != VRSIM::kSharedTicketExportBytesV1) return VRSIM::kSharedExportBadArgument;

    QaPollResultNoneV1((VRSIM::QaPollResultV1*)result56);
    std::memset(ticket64, 0, VRSIM::kSharedTicketExportBytesV1);

    VRSIM::QaBeginTokenV1 tok{};
    memcpy(&tok, token32, sizeof(tok));
    if (tok.version != VRSIM::kStereoBridgeQaStructVersionV1 ||
        tok.structBytes != (uint32_t)sizeof(VRSIM::QaBeginTokenV1) || tok.nonce == 0)
        return VRSIM::kSharedExportBadArgument;

    int slot = -1;
    unsigned long long claimEpoch = 0;
    VRSIM::SharedCaptureIdentityV1 id{};
    QaActiveCallGuard activeCall;
    try
    {
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
            // Wrong/duplicate key or wrong thread: reject, leave the lease as-is.
            if (!g_qaLedger.occupied || !g_qaLedger.hasToken) return VRSIM::kSharedExportStale;
            if (g_qaLedger.sourceThread != (unsigned long)GetCurrentThreadId()) return VRSIM::kSharedExportStale;
            if (memcmp(&g_qaLedger.token, &tok, sizeof(tok)) != 0) return VRSIM::kSharedExportStale;
            slot = g_qaLedger.slot; claimEpoch = g_qaLedger.claimEpoch; id = g_qaLedger.identity;
            if (slot < 0 || slot >= RING_SLOTS) return VRSIM::kSharedExportStale;
            // Drain keys off the STORED claim, never the current live ring epoch.
            if (g_ring[slot].state.load() != 1 || g_ring[slot].mode != kGpuSlotModeSharedV1 ||
                g_ring[slot].epoch != claimEpoch || g_sharedPhase[slot] != kSharedPhasePreparing)
                return VRSIM::kSharedExportStale;
            // Full immutable identity must match on BOTH the durable ledger and the
            // ring slot's stored PREPARE image (captureEnd still 0), against the
            // original local id — never a zeroed bridge snapshot against the key.
            if (!VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) ||
                !VRSIM::SharedIdentityFieldsEqual(g_ring[slot].sharedIdentity, id))
                return VRSIM::kSharedExportStale;
            // Per-call busy protection AFTER exact key/thread validation, BEFORE
            // arming: another in-flight source call => honest backpressure with
            // NO true-lease mutation.
            if (g_sharedSourceActiveCalls > 0) return VRSIM::kSharedExportPending;
            activeCall.ArmLocked();
        }

        VRSIM::QaPollResultV1 pres{};
        VRSIM::SharedPreparedPairV1 granted{};
        const HRESULT prh = g_sharedBridge.QaPollOnSourceUI(tok, &pres, &granted);
        memcpy(result56, &pres, sizeof(pres)); // preserve the ACTUAL bridge observation

        // SAVE FIRST: route any real nonzero ticket into the grant helper BEFORE
        // any extra lock / lastHR update / status switch, so the FIRST required
        // ring->meta save transaction records the actual ticket before lastHR is
        // assigned. The helper takes the REAL pres.status/version/structBytes/
        // bridgeHresult; a status BOOL is not part of the signature and no header
        // is fabricated from constants.
        if (granted.ticket != 0)
            return QaSaveAndPublishGrant(slot, claimEpoch, id, granted, prh,
                                         pres.status, pres.version, pres.structBytes,
                                         (uint32_t)sizeof(VRSIM::QaPollResultV1),
                                         pres.bridgeHresult,
                                         (uint32_t)VRSIM::kQaStatusCompletedGranted, ticket64);

        // No-ticket path only: now record the actual lastHR.
        try
        {
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            g_sharedLastBridgeHresult = (long long)prh;
        }
        catch (...) { if (!QaQuarantineSlot(slot)) g_stopRequested.store(true); return VRSIM::kSharedExportError; }

        // A TRUE pending requires the real S_FALSE + the full snapshot
        // / token / claim proof before returning 1 (the token is already stored).
        if (pres.status == (uint32_t)VRSIM::kQaStatusPending)
        {
            const QaBridgeEvidenceV1 ev = QaGatherEvidence();
            if (QaPendingProven(ev, prh, pres.bridgeHresult, pres.version, pres.structBytes,
                                (uint32_t)sizeof(VRSIM::QaPollResultV1), tok, slot, id))
            {
                bool stillOurs = false;
                {
                    std::lock_guard<std::mutex> rlk(g_ringMutex);
                    std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                    stillOurs =
                        g_qaLedger.occupied && g_qaLedger.hasToken && g_qaLedger.slot == slot &&
                        g_qaLedger.claimEpoch == claimEpoch &&
                        memcmp(&g_qaLedger.token, &tok, sizeof(tok)) == 0 &&
                        g_ring[slot].state.load() == 1 && g_ring[slot].mode == kGpuSlotModeSharedV1 &&
                        g_ring[slot].epoch == claimEpoch && g_sharedPhase[slot] == kSharedPhasePreparing &&
                        !g_sharedQuarantined &&
                        VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                        VRSIM::SharedIdentityFieldsEqual(g_ring[slot].sharedIdentity, id);
                }
                if (stillOurs) return VRSIM::kSharedExportPending;
            }
            if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
            QaReport([] { return std::string("qa poll: pending not proven by real S_FALSE/snapshot/claim"); });
            return VRSIM::kSharedExportQuarantined;
        }

        // Real drain with NO ticket: resolve only on the durable-ledger EMPTY +
        // real GetData completion proof (never FAILED(hr) alone).
        if (pres.status == (uint32_t)VRSIM::kQaStatusCompletedNoGrant)
            return QaClearProvenDrained(slot, claimEpoch, id, prh, /*noGrant=*/true);
        if (pres.status == (uint32_t)VRSIM::kQaStatusFailedDrained)
            return QaClearProvenDrained(slot, claimEpoch, id, prh, /*noGrant=*/false);

        if (pres.status == (uint32_t)VRSIM::kQaStatusQuarantinedRetained)
        {
            if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
            QaReport([] { return std::string("qa poll: bridge quarantined/retained"); });
            return VRSIM::kSharedExportQuarantined;
        }

        // Rejected / None / PreWorkRolledBack / unknown after a native-validated
        // poll with no ticket: contradictory. Retain + quarantine; never claim
        // FREE, publish no readiness, fabricate no completion.
        if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
        QaReport([&] { return std::string("qa poll: unexpected status ") + std::to_string(pres.status) +
                              " hr=" + SharedHrName(prh); });
        return VRSIM::kSharedExportQuarantined;
    }
    catch (...)
    {
        if (slot >= 0 && !QaQuarantineSlot(slot)) g_stopRequested.store(true);
        QaReport([] { return std::string("qa poll threw"); });
        return VRSIM::kSharedExportError;
    }
}

// Source UI thread ONLY, on the ORIGINAL exact token. Validates full token
// shape/key, the original source thread, the occupied original claim, the full
// identity and the lifetime bound epoch/generation BEFORE any true-lease
// mutation, plus the native WRITING/shared/PREPARING ring claim. An accepted
// cancel closes NATIVE admission too (via the existing metadata-only
// SharedCloseLocked under ring->meta) so new Begins are linearized out, while
// PRESERVING the original token/claim for a later honest closed-Poll drain: it
// returns Pending (1) as CLOSURE ONLY, never a drain/free/finalize signal. A
// real rejected cancel (wrong key/thread/busy) mutated NOTHING and closes no
// foreign lease. A contradictory/unproven accepted state retains/quarantines.
// All optional reporting runs through QaReport OUTSIDE gates; the catch is
// no-throw and requests an atomic stop if the state lock cannot be proved.
__declspec(dllexport) int VRSIM_QaCancelSharedStereoV1(
    const void* token32, unsigned int tokenBytes,
    void* result80, unsigned int resultBytes)
{
    if (!kVRSIMSharedActivationEnabled) return VRSIM::kSharedExportUnavailable;
    if (!token32 || !result80) return VRSIM::kSharedExportBadArgument;
    if (tokenBytes != sizeof(VRSIM::QaBeginTokenV1)) return VRSIM::kSharedExportBadArgument;
    if (resultBytes != sizeof(VRSIM::QaBeginResultV1)) return VRSIM::kSharedExportBadArgument;

    QaBeginResultNoneV1((VRSIM::QaBeginResultV1*)result80);

    VRSIM::QaBeginTokenV1 tok{};
    memcpy(&tok, token32, sizeof(tok));
    if (tok.version != (uint32_t)VRSIM::kStereoBridgeQaStructVersionV1 ||
        tok.structBytes != (uint32_t)sizeof(VRSIM::QaBeginTokenV1) || tok.nonce == 0)
        return VRSIM::kSharedExportBadArgument;

    int slot = -1;
    unsigned long long claimEpoch = 0;
    VRSIM::SharedCaptureIdentityV1 id{};
    QaActiveCallGuard activeCall;
    try
    {
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex); // ring -> meta order
            // Wrong/duplicate key or wrong thread: reject, leave the lease as-is.
            if (!g_qaLedger.occupied || !g_qaLedger.hasToken) return VRSIM::kSharedExportStale;
            if (g_qaLedger.sourceThread != (unsigned long)GetCurrentThreadId()) return VRSIM::kSharedExportStale;
            if (memcmp(&g_qaLedger.token, &tok, sizeof(tok)) != 0) return VRSIM::kSharedExportStale;
            slot = g_qaLedger.slot; claimEpoch = g_qaLedger.claimEpoch; id = g_qaLedger.identity;
            if (slot < 0 || slot >= RING_SLOTS) return VRSIM::kSharedExportStale;
            if (!g_sharedLifetimeStarted ||
                g_sharedBoundEpoch != id.epoch || g_sharedBoundGeneration != id.generation)
                return VRSIM::kSharedExportStale;
            // Drain/cancel keys off the STORED claim, never the current live ring epoch.
            if (g_ring[slot].state.load() != 1 || g_ring[slot].mode != kGpuSlotModeSharedV1 ||
                g_ring[slot].epoch != claimEpoch || g_sharedPhase[slot] != kSharedPhasePreparing)
                return VRSIM::kSharedExportStale;
            if (!VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) ||
                !VRSIM::SharedIdentityFieldsEqual(g_ring[slot].sharedIdentity, id))
                return VRSIM::kSharedExportStale;
            // Per-call busy AFTER exact key/thread validation, BEFORE arming:
            // another in-flight source call => honest backpressure, NO mutation.
            if (g_sharedSourceActiveCalls > 0) return VRSIM::kSharedExportPending;
            activeCall.ArmLocked();
        }

        // Real bridge cancel OUTSIDE the native gates.
        VRSIM::QaBeginResultV1 cres{};
        const HRESULT crh = g_sharedBridge.QaRequestCancelOnSourceUI(tok, &cres);
        memcpy(result80, &cres, sizeof(cres)); // preserve the ACTUAL bridge observation

        const QaBridgeEvidenceV1 ev = QaGatherEvidence(); // pure cached, outside gates

        // Ordinary bridge REJECTION (wrong key/thread/busy) makes NO mutation and
        // must NOT close or quarantine a foreign lease. It is honored ONLY with the
        // ACTUAL gathered snapshot proving a coherent, UN-QUARANTINED original-key
        // PREPARING lease AND an explicit Rejected status, a permitted rejection
        // HRESULT (E_ACCESSDENIED / busy E_FAIL) and a coherent result header/HR/
        // token. The public QaRequestCancelOnSourceUI wrapper ALSO returns E_FAIL
        // after a THROWN cancel (MarkQuarantined closes bridge admission), so a
        // failed REAL call is NOT a proved rejection: any missing/contradictory
        // evidence falls through to the non-accepted quarantine path below and
        // mirrors NATIVE quarantine. Open admission / cancelRequested==0 are NOT
        // required: a prior accepted cancel may already have closed this same
        // original lease, and an ordinary busy rejection must not reopen it.
        const bool honestReject =
            (crh == E_ACCESSDENIED || crh == E_FAIL) &&
            cres.status == (uint32_t)VRSIM::kQaStatusRejected &&
            cres.version == (uint32_t)VRSIM::kStereoBridgeQaStructVersionV1 &&
            cres.structBytes == (uint32_t)sizeof(VRSIM::QaBeginResultV1) &&
            cres.bridgeHresult == (LONG)crh &&
            memcmp(&cres.token, &tok, sizeof(tok)) == 0 &&
            ev.gathered &&
            ev.snap.ledgerState == (uint32_t)VRSIM::kQaLedgerPreparing &&
            ev.snap.slotValid != 0u && (int)ev.snap.slotIndex == slot &&
            ev.snap.nonce == tok.nonce && ev.snap.epoch == tok.epoch &&
            ev.snap.generation == tok.generation &&
            ev.snap.stampFrame == id.frameSerial &&
            ev.snap.stampPose == id.poseSerial &&
            ev.snap.stampCaptureBeginQpc == id.captureBeginQpc &&
            ev.snap.initializedNow != 0u &&
            ev.snap.finalizing == 0u && ev.snap.finalized == 0u &&
            ev.snap.quarantined == 0u && !ev.bridgeQuarantined;
        if (honestReject)
        {
            // Preserve the exact original full native claim/key while recording
            // lastHR under ring->meta; a contradictory lost claim quarantines,
            // never silently overwriting a foreign lease's lastHR.
            bool ours = false;
            try
            {
                std::lock_guard<std::mutex> rlk(g_ringMutex);
                std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
                StereoSlot& cs = g_ring[slot];
                ours =
                    g_qaLedger.occupied && g_qaLedger.hasToken && g_qaLedger.slot == slot &&
                    g_qaLedger.claimEpoch == claimEpoch &&
                    memcmp(&g_qaLedger.token, &tok, sizeof(tok)) == 0 &&
                    g_sharedLifetimeStarted &&
                    g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                    cs.state.load() == 1 && cs.mode == kGpuSlotModeSharedV1 && cs.epoch == claimEpoch &&
                    g_sharedPhase[slot] == kSharedPhasePreparing && !g_sharedQuarantined &&
                    VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                    VRSIM::SharedIdentityFieldsEqual(cs.sharedIdentity, id);
                if (ours) g_sharedLastBridgeHresult = (long long)crh;
            }
            catch (...) { if (!QaQuarantineSlot(slot)) g_stopRequested.store(true); return VRSIM::kSharedExportError; }
            if (!ours)
            {
                if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
                QaReport([&] { return std::string("qa cancel honest rejection but native claim lost: retained/quarantined hr=") +
                                      SharedHrName(crh); });
                return VRSIM::kSharedExportQuarantined;
            }
            QaReport([&] { return std::string("qa cancel honestly rejected by bridge (no mutation) hr=") + SharedHrName(crh); });
            return (crh == E_ACCESSDENIED) ? VRSIM::kSharedExportStale : VRSIM::kSharedExportError;
        }

        // Attempted acceptance (S_OK/S_FALSE), or a real failed/thrown cancel the
        // snapshot does NOT corroborate as an honest rejection: only a proven
        // accepted closure may proceed; anything unproven retains + quarantines.
        if (!QaCancelAcceptedProven(ev, crh, cres, tok, slot, id))
        {
            // A real cancel return that the durable-ledger snapshot does NOT
            // corroborate: contradictory/unproven, retain + quarantine, never a
            // clean accepted-closure 1.
            if (!QaQuarantineSlot(slot)) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
            QaReport([&] { return std::string("qa cancel non-accepted/unproven: retained/quarantined hr=") +
                                  SharedHrName(crh) + " status=" + std::to_string(cres.status); });
            return VRSIM::kSharedExportQuarantined;
        }

        // ACCEPTED closure: revalidate the exact original native claim, then run
        // the existing metadata-only SharedCloseLocked(false) under ring->meta to
        // close native admission / advance the live epoch while PRESERVING the
        // original claim/token/full identity (no drain, no free, no finalize).
        bool closed = false;
        try
        {
            std::lock_guard<std::mutex> rlk(g_ringMutex);
            std::lock_guard<std::mutex> mlk(g_sharedMetaMutex);
            StereoSlot& cs = g_ring[slot];
            const bool stillOurs =
                g_qaLedger.occupied && g_qaLedger.hasToken && g_qaLedger.slot == slot &&
                g_qaLedger.claimEpoch == claimEpoch &&
                memcmp(&g_qaLedger.token, &tok, sizeof(tok)) == 0 &&
                g_sharedLifetimeStarted &&
                g_sharedBoundEpoch == id.epoch && g_sharedBoundGeneration == id.generation &&
                cs.state.load() == 1 && cs.mode == kGpuSlotModeSharedV1 && cs.epoch == claimEpoch &&
                g_sharedPhase[slot] == kSharedPhasePreparing && !g_sharedQuarantined &&
                VRSIM::SharedIdentityFieldsEqual(g_qaLedger.identity, id) &&
                VRSIM::SharedIdentityFieldsEqual(cs.sharedIdentity, id);
            if (stillOurs)
            {
                SharedCloseLocked(false); // pure metadata: close native admission + ring epoch
                // SharedCloseLocked may overwrite lastBridgeHresult with its own
                // CloseAdmission value; restore the ACTUAL cancel HR.
                g_sharedLastBridgeHresult = (long long)crh;
                closed = true;
            }
            else { g_sharedQuarantined = true; g_sharedPhase[slot] = kSharedPhaseQuarantined; }
        }
        catch (...) { g_stopRequested.store(true); return VRSIM::kSharedExportError; }
        if (!closed)
        {
            QaReport([] { return std::string("qa cancel accepted but native claim lost: retained/quarantined"); });
            return VRSIM::kSharedExportQuarantined;
        }
        // Accepted closure returns Pending (1): admission closed + lease marked,
        // GPU drain NEVER proven here; the PREPARING token stays for closed Poll.
        QaReport([&] { return std::string("qa cancel accepted: native+bridge admission closed, drain pending hr=") +
                              SharedHrName(crh); });
        return VRSIM::kSharedExportPending;
    }
    catch (...)
    {
        // C ABI never throws: mandatory retain/quarantine FIRST (never skipped by
        // the optional error string); if the state lock itself failed, request an
        // atomic stop and still return nonzero.
        if (slot >= 0 && !QaQuarantineSlot(slot)) g_stopRequested.store(true);
        QaReport([] { return std::string("qa cancel threw"); });
        return VRSIM::kSharedExportError;
    }
}
#endif // VRSIM_QA_D9_BEGIN_POLL

__declspec(dllexport) int VRSIM_EnableSceneMode(int width, int height)
{
#if defined(VRSIM_SHARED_3072) && defined(VRSIM_QA_OUTPUT_4096)
    // 3072 compile variant + QA OUTPUT_4096: the legacy 64..2048 contract is
    // UNCHANGED and the only additional accepted size is the exact 4096x4096
    // mixed OUTPUT square. Every other larger dimension stays refused; the
    // real BOTH-eyes/system capability gate (queue side + worker side) still
    // decides admission before any reset/destroy — a 4K request is never
    // forced past the runtime caps here.
    if (width < 64 || height < 64 ||
        !((width <= 2048 && height <= 2048) || (width == 4096 && height == 4096))) return -1; // size contract only
#elif defined(VRSIM_SHARED_3072)
    // 3072 compile variant: the legacy 64..2048 contract is UNCHANGED and the
    // only additional accepted size is the exact 3072x3072 obs-shared square.
    // Every other larger dimension stays refused.
    if (width < 64 || height < 64 ||
        !((width <= 2048 && height <= 2048) || (width == 3072 && height == 3072))) return -1; // size contract only
#elif defined(VRSIM_SHARED_2736)
    // 2736 compile variant: the legacy 64..2048 contract is UNCHANGED and the
    // only additional accepted size is the exact 2736x2736 obs-shared square.
    // Every other larger dimension stays refused.
    if (width < 64 || height < 64 ||
        !((width <= 2048 && height <= 2048) || (width == 2736 && height == 2736))) return -1; // size contract only
#elif defined(VRSIM_SHARED_2560)
    // 2560 compile variant: the legacy 64..2048 contract is UNCHANGED and the
    // only additional accepted size is the exact 2560x2560 obs-shared square.
    // Every other larger dimension stays refused.
    if (width < 64 || height < 64 ||
        !((width <= 2048 && height <= 2048) || (width == 2560 && height == 2560))) return -1; // size contract only
#else
    if (width < 64 || height < 64 || width > 2048 || height > 2048) return -1; // size contract only
#endif
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
            // latest request wins the queue. DEVICE-CAPS-01 REPAIR: the old
            // Ready flag/epoch are NOT invalidated here — the worker runs the
            // same pure capability policy inside this joint ring -> meta gate
            // BEFORE its take/Arm/invalidation, so an over-cap request only
            // consumes its own queue entry and the currently verified
            // pipeline's readiness survives. Retaining readiness while a valid
            // rebuild is queued opens no binding window: the first shared
            // Prepare (SCENE-FREEZE-2) refuses ANY pending dimension or armed
            // busy under these same locks (ring -> meta order), so it returns
            // Pending until the worker's take and rebuild have completed.
            // This export reads no worker-owned caps cache.
            g_pendingSceneW = width;
            g_pendingSceneH = height;
            g_vcSceneRequested = true;
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
    // HIGHRES-H2-DIAG-FB: the fresh-Create half of the safe reset points (the
    // other half is the actual non-refused DestroyXRResources path). A refused
    // retained cleanup never reaches either.
    ResetFbColorSpaceDiagnostics();
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
    // HIGHRES-H2-DIAG-FB: read VRSIM_READBACK_PIPELINE exactly once, beside the
    // RawColorspace freeze, into its own bounded buffer (read-once; no Trim, no
    // casing fallback). ONLY the exact "obs-shared" selects FB diagnostics;
    // absent/empty/exact "legacy" => unselected (0); every other token including
    // an overlong value => unselected invalid (2). Unselected diagnostics must
    // NEVER fail a default/legacy VR start and legacy never enables FB. The
    // fresh-Create reset already ran above, so this freeze is the only writer.
    {
        wchar_t rbBuf[64];
        const DWORD rn = GetEnvironmentVariableW(L"VRSIM_READBACK_PIPELINE", rbBuf, (DWORD)(sizeof(rbBuf) / sizeof(rbBuf[0])));
        int sel = 0;
        if (rn == 0) sel = 0; // absent or empty
        else if (rn < (DWORD)(sizeof(rbBuf) / sizeof(rbBuf[0])))
        {
            std::wstring rv(rbBuf, rn);
            if (rv == L"obs-shared") sel = 1;
            else if (rv == L"legacy") sel = 0;
            else sel = 2;
        }
        else sel = 2; // overlong: never silently truncated; diagnostics only
        g_diagFbPipelineSelected = sel;
        LogLine("DIAG-FB", "VRSIM_READBACK_PIPELINE FB diagnostics selected=" + std::to_string(sel) +
                " (0 unselected 1 exact obs-shared 2 invalid token; unselected never fails VR start)");
    }
    // HIGHRES-H2-REC709 OPT-IN: read VRSIM_XR_COLORSPACE exactly once, beside the
    // pipeline freeze, in its own bounded buffer (no Trim, no casing fallback).
    // ONLY the exact lowercase 'rec709' selects the opt-in (1); absent/empty => 0;
    // every other token including an overlong value => 2 (invalid). The request is
    // honored only on the exact obs-shared selection AND an actually enabled FB
    // extension; otherwise the worker performs an honest skip. Absent/invalid
    // tokens and legacy paths never resolve or call xrSetColorSpaceFB, and nothing
    // here can fail a default/legacy VR start.
    {
        wchar_t csBuf[64];
        const DWORD cn = GetEnvironmentVariableW(L"VRSIM_XR_COLORSPACE", csBuf, (DWORD)(sizeof(csBuf) / sizeof(csBuf[0])));
        int req = 0;
        if (cn == 0) req = 0; // absent or empty
        else if (cn < (DWORD)(sizeof(csBuf) / sizeof(csBuf[0])))
        {
            std::wstring cv(csBuf, cn);
            if (cv == L"rec709") req = 1;
            else req = 2;
        }
        else req = 2; // overlong: never silently truncated
        g_diagSetRequested = req;
        // honest pre-worker skip: absent/invalid tokens explain themselves at
        // freeze time; a valid request stays 0 until the worker's real decision
        g_diagSetSkipReason = (req == 0 ? 1 : (req == 2 ? 7 : 0));
        LogLine("DIAG-FB", "VRSIM_XR_COLORSPACE opt-in request=" + std::to_string(req) +
                " (0 none 1 exact rec709 2 invalid token; honored only on obs-shared+enabled FB; never fails VR start)");
    }
#if defined(VRSIM_QA_FRAME_TRACE)
    // QA frame trace: path frozen and buffers reset EXACTLY ONCE here — after
    // VRSIM_Create's existing eligibility gates and session token freezes have
    // passed, before the worker exists. Refused Creates returned above and
    // never reset; a refused Destroy never exports. Ordinary behavior untouched.
    QaTraceFreezeAndReset();
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING) && defined(VRSIM_QA_OUTPUT_4096)
    // D11 lease-owned output GPU timing: exact-env "1" freeze + session reset,
    // once here on the original legal Create (worker not yet created), AFTER
    // and gated on the just-frozen g_qaTraceEnabled (sampling only when the
    // original frame CSV trace is enabled). Absent/overlong/other values and
    // any failure leave it OFF; never fails VRSIM_Create and never touches
    // the trace's own rules.
    QaD11GpuTimingFreezeAndReset();
#endif
#if defined(VRSIM_QA_PREPARE_WAIT)
    // Prepare/wait NUMERIC telemetry: a separate exact-env "1" freeze and
    // session reset, independent of the trace path rules and g_qaTraceEnabled.
    // Same legal-Create point (eligibility passed, worker not yet created);
    // refused Creates returned above and never reset. no-throw: a failure here
    // only leaves the telemetry OFF/unavailable and never fails VRSIM_Create.
    QaPrepareWaitFreezeAndReset();
#endif
#endif
#if defined(VRSIM_QA_MIXED_SOURCE) && defined(VRSIM_QA_AREA_SHADER)
    // Sampling-contract ONCE telemetry: a separate exact-env "1" freeze and
    // session reset, independent of the trace / prepare-wait paths. Same legal-
    // Create point (eligibility passed, worker not yet created); refused Creates
    // returned above and never reset, and Stop/Destroy never rearm. no-throw: a
    // failure here only leaves the once-diagnostic OFF/unavailable and never
    // fails VRSIM_Create.
    QaSamplingOnceFreezeAndReset();
#endif
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

    // ---- instance (requests XR_KHR_D3D11_enable + XR_EXT_user_presence when
    // available; XR_FB_color_space is DIAGNOSTICS-ONLY and is enabled exactly
    // once only when the frozen exact obs-shared selection AND the name is in
    // the actually successful extension list) ----
    const char* exts[3] = { "XR_KHR_D3D11_enable", nullptr, nullptr };
    uint32_t extCount = 1;
    {
        uint32_t n = 0;
        PFN_xrEnumerateInstanceExtensionProperties enumProps = nullptr;
        g_gpa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumProps);
        if (enumProps)
        {
            const XrResult cr = enumProps(nullptr, 0, &n, nullptr);
            g_diagFbExtEnumRc = (int)cr;      // actual count-call rc, always preserved
            g_diagFbExtEnumRcValid = 1;
            if (cr == XR_SUCCESS)
            {
                if (n > 0)                    // original data-enumeration condition restored (no new total cap)
                {
                    std::vector<XrExtensionProperties> props(n);
                    for (auto& eprop : props) { eprop.type = XR_TYPE_EXTENSION_PROPERTIES; eprop.next = nullptr; }
                    uint32_t got = n;
                    const XrResult dr = enumProps(nullptr, n, &got, props.data());
                    g_diagFbExtEnumDataRc = (int)dr;   // actual data-call rc, preserved separately
                    g_diagFbExtEnumDataRcValid = 1;
                    if (dr == XR_SUCCESS && got <= n)
                    {
                        // availability is proven only by a successful data
                        // enumeration whose returned count fits the actual capacity
                        g_diagFbAvailableValid = 1;
                        bool presenceAdded = false, fbAdded = false;
                        for (uint32_t i = 0; i < got; ++i)    // FULL scan: no presence early-break
                        {
                            if (strcmp(props[i].extensionName, XR_EXT_USER_PRESENCE_EXTENSION_NAME) == 0)
                            {
                                g_userPresenceAvail = 1; // original presence behavior preserved
                                if (!presenceAdded && extCount < 3)
                                {
                                    exts[extCount++] = XR_EXT_USER_PRESENCE_EXTENSION_NAME;
                                    presenceAdded = true;
                                }
                            }
                            else if (strcmp(props[i].extensionName, XR_FB_COLOR_SPACE_EXTENSION_NAME) == 0)
                            {
                                g_diagFbAvailable = 1;
                                if (!fbAdded && g_diagFbPipelineSelected.load() == 1 && extCount < 3)
                                {
                                    exts[extCount++] = XR_FB_COLOR_SPACE_EXTENSION_NAME;
                                    g_diagFbEnabled = 1; // enabled once; observation only
                                    fbAdded = true;
                                }
                            }
                        }
                    }
                    // data rc failure or got > count: unknown, never proved unsupported
                }
                else if (n == 0)
                {
                    g_diagFbAvailableValid = 1; // a successful empty list is a real absence
                }
            }
        }
        LogLine("XR", "userPresence extension available=" + std::to_string(g_userPresenceAvail.load()));
        LogLine("DIAG-FB", "FB color space diagnostics: extCountRc=" + std::to_string((int)g_diagFbExtEnumRc.load()) +
                " extDataRcRecorded=" + std::to_string(g_diagFbExtEnumDataRcValid.load()) +
                " extDataRc=" + std::to_string((int)g_diagFbExtEnumDataRc.load()) +
                " available=" + std::to_string(g_diagFbAvailable.load()) +
                " availableValid=" + std::to_string(g_diagFbAvailableValid.load()) +
                " enabled=" + std::to_string(g_diagFbEnabled.load()) +
                " (observation only; absence/failure never fails VR and proves no app colorspace)");
        PublishFbDiagnosticsSnapshot(); // stage boundary: full extension scan + decisions complete
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

    // HIGHRES-H2-DIAG-FB: resolve the official PFN_xrEnumerateColorSpacesFB via
    // xrGetInstanceProcAddr ONLY when FB was actually enabled. No new mandatory
    // RES entry (every other runtime must keep working), the real rc and pointer
    // presence are cached, a failed GPA leaves the pointer NULL (never called).
    // HIGHRES-H2-REC709 OPT-IN: the official PFN_xrSetColorSpaceFB is resolved
    // in this same enabled block ONLY when the exact frozen 'rec709' token was
    // selected. Its real GPA rc and pointer presence are cached; a failed GPA
    // leaves it NULL so the worker makes an honest skip — optional resolution
    // failure NEVER fails Create or aborts normal rendering.
    if (g_diagFbEnabled.load() != 0)
    {
        const XrResult gr = g_gpa(g_inst, "xrEnumerateColorSpacesFB", (PFN_xrVoidFunction*)&xrEnumerateColorSpacesFB);
        if (gr != XR_SUCCESS) xrEnumerateColorSpacesFB = nullptr;
        g_diagFbGetProcRc = (int)gr;
        g_diagFbGetProcRcValid = 1;
        g_diagFbFunctionPresent = (gr == XR_SUCCESS && xrEnumerateColorSpacesFB != nullptr) ? 1 : 0;
        LogLine("DIAG-FB", "FB GetProcAddr rc=" + std::to_string((int)gr) +
                " functionPresent=" + std::to_string(g_diagFbFunctionPresent.load()) +
                " (missing function stays unknown/unusable, never fatal)");
        if (g_diagSetRequested.load() == 1 && g_sessionColorSrgb)
        {
            const XrResult sqr = g_gpa(g_inst, "xrSetColorSpaceFB", (PFN_xrVoidFunction*)&xrSetColorSpaceFB);
            if (sqr != XR_SUCCESS) xrSetColorSpaceFB = nullptr;
            g_diagSetGetProcRc = (int)sqr;
            g_diagSetGetProcRcValid = 1;
            g_diagSetFunctionPresent = (sqr == XR_SUCCESS && xrSetColorSpaceFB != nullptr) ? 1 : 0;
            LogLine("DIAG-FB", "FB SetGetProcAddr rc=" + std::to_string((int)sqr) +
                    " setFunctionPresent=" + std::to_string(g_diagSetFunctionPresent.load()) +
                    " (optional: missing stays NULL and honestly skips the Set)");
        }
        PublishFbDiagnosticsSnapshot(); // stage boundary: GPA results complete (enumerate + opt-in Set)
    }

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
    XrSystemColorSpacePropertiesFB fbColorSpaceProps{ XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB };
    fbColorSpaceProps.next = nullptr; // local chain tail: nothing extends this
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    if (g_userPresenceAvail)
    {
        sp.next = &presenceProps;
        if (g_diagFbEnabled.load() != 0) presenceProps.next = &fbColorSpaceProps;
    }
    else if (g_diagFbEnabled.load() != 0)
    {
        sp.next = &fbColorSpaceProps;
    }
    const XrResult spr = xrGetSystemProperties(g_inst, g_sysId, &sp);
    if (spr == XR_SUCCESS)
    {
        memcpy(g_systemName, sp.systemName, XR_MAX_SYSTEM_NAME_SIZE);
        g_systemName[XR_MAX_SYSTEM_NAME_SIZE] = 0;
        // DEVICE-CAPS-01: cache the ACTUAL system swapchain maxima only from
        // this successful query; zero maxima are not a usable capability.
        g_deviceCaps.systemMaxWidth = sp.graphicsProperties.maxSwapchainImageWidth;
        g_deviceCaps.systemMaxHeight = sp.graphicsProperties.maxSwapchainImageHeight;
        g_deviceCaps.systemMaxValid = g_deviceCaps.systemMaxWidth > 0 &&
                                      g_deviceCaps.systemMaxHeight > 0;
        LogLine("XR", "system maxSwapchainImage " +
                std::to_string(g_deviceCaps.systemMaxWidth) + "x" +
                std::to_string(g_deviceCaps.systemMaxHeight) +
                " valid=" + std::to_string(g_deviceCaps.systemMaxValid ? 1 : 0));
        if (g_userPresenceAvail)
        {
            g_userPresenceSupported = presenceProps.supportsUserPresence ? 1 : 0;
            LogLine("XR", "userPresence system support=" + std::to_string(g_userPresenceSupported.load()));
        }
        // HIGHRES-H2-DIAG-FB: marked valid ONLY on the actual xrGetSystemProperties
        // XR_SUCCESS. This is the DEVICE NATIVE colorspace — never the current
        // app/session colorspace, and never proof that any Set was applied.
        // The original error path (no else branch, names stay "?") is unchanged.
        if (g_diagFbEnabled.load() != 0)
        {
            g_diagNativeColorSpace = (int)fbColorSpaceProps.colorSpace;
            g_diagNativeColorSpaceValid = 1;
            LogLine("DIAG-FB", "FB nativeDeviceColorSpace=" + std::to_string((int)fbColorSpaceProps.colorSpace) +
                    " (" + FbColorSpaceName((int)fbColorSpaceProps.colorSpace) + ") deviceNativeNotAppColorSpace");
        }
    }
    PublishFbDiagnosticsSnapshot(); // stage boundary: system-property chain result complete (success OR failure; failure now fails closed below)
    // DEVICE-CAPS-01: a failed query or zero maxima leave the capability cache
    // unavailable — never continue to session creation with unknown maxima.
    // Same joined-worker/resource cleanup path as the other Create failures.
    if (spr != XR_SUCCESS || !g_deviceCaps.systemMaxValid)
    {
        SetErr(spr != XR_SUCCESS
            ? "xrGetSystemProperties " + XrResultName(spr)
            : "xrGetSystemProperties returned zero maxSwapchainImage extents (" +
              std::to_string(g_deviceCaps.systemMaxWidth) + "x" +
              std::to_string(g_deviceCaps.systemMaxHeight) + ")");
        const bool cleaned = centralCleanup();
        SetState(VRSIM_ERROR);
        return cleaned ? -2 : -4;
    }

    // ---- blend mode ----
    // DEVICE-CAPS-01: FULL two-call enumeration with actual result/count checks.
    // This desktop PCVR line requires the ACTUAL advertised OPAQUE even when it
    // is not the first mode; a failed/empty enumeration, a count that does not
    // fit the returned capacity, or an advertised list without OPAQUE rejects
    // Create through the existing error/cleanup path — never a guessed or
    // default mode, never the old capacity-1 unchecked call.
    {
        uint32_t bn = 0;
        const XrResult bcr = xrEnumerateEnvironmentBlendModes(g_inst, g_sysId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bn, nullptr);
        std::vector<XrEnvironmentBlendMode> modes;
        uint32_t got = 0;
        XrResult bdr = XR_SUCCESS;
        bool dataCallMade = false;
        // SANITY POLICY (not a claimed device capability): a successful count
        // call advertising more than 64 blend modes is unreasonable data and is
        // refused BEFORE the vector allocation, through the same honest
        // cleanup/ERROR path. The actual two-call result/count checks below are
        // unchanged; no clamp and no guessed mode.
        if (bcr == XR_SUCCESS && bn > 64u)
        {
            SetErr("environment blend: countOutput=" + std::to_string(bn) +
                   " exceeds sanity limit 64 (sanity policy, not a device capability claim)");
            const bool cleaned = centralCleanup();
            SetState(VRSIM_ERROR);
            return cleaned ? -2 : -4;
        }
        if (bcr == XR_SUCCESS && bn > 0)
        {
            try
            {
                modes.resize(bn);
            }
            catch (...)
            {
                // An allocation failure must never cross the extern C Create ABI.
                SetErr("environment blend: allocation refused for countOutput=" + std::to_string(bn));
                const bool cleaned = centralCleanup();
                SetState(VRSIM_ERROR);
                return cleaned ? -2 : -4;
            }
            bdr = xrEnumerateEnvironmentBlendModes(g_inst, g_sysId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bn, &got, modes.data());
            dataCallMade = true;
        }
        XrEnvironmentBlendMode selected = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        const bool chosen = VRSIMCaps::SelectRequiredBlendMode(
            modes.empty() ? nullptr : modes.data(),
            dataCallMade ? got : 0u, bn,
            dataCallMade && bdr == XR_SUCCESS,
            XR_ENVIRONMENT_BLEND_MODE_OPAQUE, &selected);
        if (!chosen)
        {
            std::string advertised;
            for (uint32_t i = 0; dataCallMade && bdr == XR_SUCCESS && i < got && i < bn; ++i)
                advertised += (advertised.empty() ? "" : " ") + std::to_string((int)modes[i]);
            SetErr("environment blend: no advertised OPAQUE (countRc=" + XrResultName(bcr) +
                   " countOutput=" + std::to_string(bn) +
                   " dataRc=" + (dataCallMade ? XrResultName(bdr) : std::string("not-called")) +
                   " returned=" + std::to_string(dataCallMade ? got : 0u) +
                   " list=" + (advertised.empty() ? std::string("none") : advertised) + ")");
            const bool cleaned = centralCleanup();
            SetState(VRSIM_ERROR);
            return cleaned ? -2 : -4;
        }
        g_blend = selected; // the actual advertised OPAQUE, even if not list[0]
        LogLine("XR", "blend modes advertised=" + std::to_string(got) +
                " selected=OPAQUE(" + std::to_string((int)g_blend) +
                ") (exact advertised choice; no list[0]/default fallback)");
    }

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
#if defined(VRSIM_QA_FRAME_TRACE)
    // The ONLY export site: worker joined above, DestroyXRResources() returned
    // true and state OFF is set. Export is exception-safe, cannot alter the
    // unchanged return value, and a failed/partial export is diagnosticFAIL.
    QaTraceExportCsv();
#endif
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
    // ---- HIGHRES-H2-DIAG-FB: additive FB color space facts from the ONE
    // committed snapshot. The whole POD image is copied inside a single short
    // dedicated gate; every JSON/string/allocation runs OUTSIDE it. No per-atomic
    // working field is read here, so payload+valid can never mix across stages,
    // and a concurrent cleanup reset yields either the complete old image or the
    // complete default/unknown image — never a mix. There is NO getter for the
    // current app/session colorspace: appColorSpace is REC709 only when THIS
    // build's single opt-in xrSetColorSpaceFB returned exact XR_SUCCESS (the
    // last explicit declaration we ourselves made), otherwise the honest
    // literal RUNTIME_SELECTED_UNKNOWN. Absent/invalid opt-in never attempted
    // a Set; nativeDeviceColorSpace remains the DEVICE native gamut, never an
    // applied app color.
    {
        FbColorSpaceDiagSnapshot ds{};
        {
            std::lock_guard<std::mutex> dlk(g_diagSnapMutex);
            ds = g_diagCommitted; // one coherent full copy inside the gate
        }
        std::string csj = "{";
        csj += "\"pipelineSelected\":" + std::to_string(ds.pipelineSelected);
        csj += ",\"extensionCountRc\":" + std::to_string(ds.extCountRc) +
               ",\"extensionCountRcValid\":" + std::string(ds.extCountRcValid ? "true" : "false");
        csj += ",\"extensionDataRc\":" + std::to_string(ds.extDataRc) +
               ",\"extensionDataRcValid\":" + std::string(ds.extDataRcValid ? "true" : "false");
        csj += ",\"extensionAvailable\":" + std::string(ds.available ? "true" : "false") +
               ",\"extensionAvailableValid\":" + std::string(ds.availableValid ? "true" : "false") +
               ",\"extensionEnabled\":" + std::string(ds.enabled ? "true" : "false");
        csj += ",\"getProcRc\":" + std::to_string(ds.getProcRc) +
               ",\"getProcRcValid\":" + std::string(ds.getProcRcValid ? "true" : "false") +
               ",\"functionPresent\":" + std::string(ds.functionPresent ? "true" : "false");
        csj += ",\"nativeDeviceColorSpace\":" + std::to_string(ds.nativeColorSpace) +
               ",\"nativeDeviceColorSpaceName\":\"" + FbColorSpaceName(ds.nativeColorSpace) + "\"" +
               ",\"nativeDeviceColorSpaceValid\":" + std::string(ds.nativeColorSpaceValid ? "true" : "false");
        csj += ",\"supportedEnumerationCountRc\":" + std::to_string(ds.supportedCountRc) +
               ",\"supportedEnumerationCountRcValid\":" + std::string(ds.supportedCountRcValid ? "true" : "false");
        csj += ",\"supportedEnumerationDataRc\":" + std::to_string(ds.supportedDataRc) +
               ",\"supportedEnumerationDataRcValid\":" + std::string(ds.supportedDataRcValid ? "true" : "false");
        csj += ",\"supportedCountOutput\":" + std::to_string((unsigned long long)ds.supportedCountOutput);
        csj += ",\"supportedColorSpaceCount\":" + std::to_string((unsigned long long)ds.supportedCount) +
               ",\"supportedColorSpacesValid\":" + std::string(ds.supportedListValid ? "true" : "false");
        csj += ",\"supportedColorSpaces\":[";
        if (ds.supportedListValid)
        {
            // bounded by the actual capacity, real numeric values only
            const unsigned int lim = ds.supportedCount < (unsigned int)kDiagMaxColorSpaces
                ? ds.supportedCount : (unsigned int)kDiagMaxColorSpaces;
            for (unsigned int i = 0; i < lim; ++i)
            {
                if (i) csj += ",";
                csj += std::to_string(ds.supportedColorSpaces[i]);
            }
        }
        csj += "]";
        // ---- REC709 opt-in facts (same committed image; no atomic re-read) ----
        csj += ",\"rec709Request\":" + std::to_string(ds.setRequested); // 0 none 1 exact rec709 2 invalid
        csj += ",\"setGetProcRc\":" + std::to_string(ds.setGetProcRc) +
               ",\"setGetProcRcValid\":" + std::string(ds.setGetProcRcValid ? "true" : "false") +
               ",\"setFunctionPresent\":" + std::string(ds.setFunctionPresent ? "true" : "false");
        csj += ",\"setAttempted\":" + std::string(ds.setAttempted ? "true" : "false");
        csj += ",\"setResult\":" + (ds.setResultValid ? std::to_string(ds.setResult) : std::string("null")) +
               ",\"setResultValid\":" + std::string(ds.setResultValid ? "true" : "false");
        csj += ",\"appliedColorSpace\":" + std::to_string(ds.appliedColorSpace) +
               ",\"appliedColorSpaceName\":\"" + FbColorSpaceName(ds.appliedColorSpace) + "\"" +
               ",\"appliedValid\":" + std::string(ds.appliedValid ? "true" : "false");
        csj += ",\"setSkipReasonCode\":" + std::to_string(ds.setSkipReason) +
               ",\"setSkipReason\":\"" + FbSetSkipReasonName(ds.setSkipReason) + "\"";
        csj += ",\"appColorSpace\":" + (ds.appliedValid
            ? std::string("\"REC709\"") : std::string("\"RUNTIME_SELECTED_UNKNOWN\""));
        csj += "}";
        j += ",\"colorSpaceDiagnostics\":" + csj;
    }
#if defined(VRSIM_QA_FRAME_TRACE) && defined(VRSIM_QA_PREPARE_WAIT)
    // ---- ADDITIVE optional child: prepareWaitDiagnosticsV1. The ENTIRE cache
    // is copied by value under the short numeric gate, the gate is released,
    // and ALL serialization/allocation happens outside it. No-throw into the
    // C ABI: a snapshot or serialization failure only labels the child
    // unavailable (or omits it); Native state, Stop/Destroy, every pre-existing
    // key, and the cap/-need rule below stay EXACTLY as before. Present only
    // under both QA compile macros; the ordinary build compiles none of this.
    try
    {
        QaPwCache pwSnap{};
        if (QaPrepareWaitSnapshot(pwSnap))
            j += ",\"prepareWaitDiagnosticsV1\":" + QaPrepareWaitJsonBody(pwSnap);
        else
            j += ",\"prepareWaitDiagnosticsV1\":{\"schemaVersion\":1,\"enabled\":false,"
                 "\"available\":false,\"failure\":\"snapshot unavailable\"}";
    }
    catch (...)
    {
        // allocation failure during the optional child: emit nothing extra;
        // nested fixed-literal append is itself best-effort, never throwing
        try { j += ",\"prepareWaitDiagnosticsV1\":\"unavailable\""; } catch (...) {}
    }
#endif
// CARD2B_QA_DIAGNOSTICS_BEGIN
#if defined(VRSIM_QA_D9_BEGIN_POLL)
    try
    {
        LARGE_INTEGER qpf{};
        const BOOL freqOk = QueryPerformanceFrequency(&qpf);
        LARGE_INTEGER tSnapBegin{}, tBridgeBegin{}, tBridgeEnd{}, tSnapEnd{};
        const BOOL qpc1 = QueryPerformanceCounter(&tSnapBegin);

        QaBeginPollNativeSnapshotV1 nativeA{};
        const bool copyAOk = QaBeginPollCopyNativeSnapshot(nativeA);

        const BOOL qpc2 = QueryPerformanceCounter(&tBridgeBegin);

        VRSIM::QaBridgeSnapshotV1 bridgeSnap{};
        g_sharedBridge.QaSnapshot(&bridgeSnap);

        const BOOL qpc3 = QueryPerformanceCounter(&tBridgeEnd);

        QaBeginPollNativeSnapshotV1 nativeB{};
        const bool copyBOk = QaBeginPollCopyNativeSnapshot(nativeB);

        const BOOL qpc4 = QueryPerformanceCounter(&tSnapEnd);

        const bool timingOk = freqOk
            && qpc1 && qpc2 && qpc3 && qpc4
            && qpf.QuadPart > 0
            && tSnapBegin.QuadPart > 0
            && tSnapBegin.QuadPart <= tBridgeBegin.QuadPart
            && tBridgeBegin.QuadPart <= tBridgeEnd.QuadPart
            && tBridgeEnd.QuadPart <= tSnapEnd.QuadPart;

        const bool bridgeSnapOk = (bridgeSnap.version == 1
            && bridgeSnap.structBytes == (uint32_t)sizeof(VRSIM::QaBridgeSnapshotV1)
            && bridgeSnap.initCount > 0);
        const bool bracketMatched = copyAOk && copyBOk
            && QaBeginPollBracketMatched(nativeA, nativeB);
        const bool avail = timingOk && copyAOk && copyBOk && bridgeSnapOk;

        uint32_t failureCode = 0;
        if (!timingOk) failureCode = 1;
        else if (!copyAOk || !copyBOk) failureCode = 2;
        else if (!bridgeSnapOk) failureCode = 3;

        std::string child = "{";
        child += "\"schemaVersion\":1";
        child += ",\"enabled\":true";
        child += ",\"available\":" + std::string(avail ? "true" : "false");
        child += ",\"bridgeSnapshotAvailable\":" + std::string(bridgeSnapOk ? "true" : "false");
        child += ",\"qpcFrequency\":" + ((freqOk && qpf.QuadPart > 0) ? std::to_string((long long)qpf.QuadPart) : std::string("0"));
        child += ",\"snapshotBeginQpc\":" + (qpc1 ? std::to_string((long long)tSnapBegin.QuadPart) : std::string("0"));
        child += ",\"bridgeSnapshotBeginQpc\":" + (qpc2 ? std::to_string((long long)tBridgeBegin.QuadPart) : std::string("0"));
        child += ",\"bridgeSnapshotEndQpc\":" + (qpc3 ? std::to_string((long long)tBridgeEnd.QuadPart) : std::string("0"));
        child += ",\"snapshotEndQpc\":" + (qpc4 ? std::to_string((long long)tSnapEnd.QuadPart) : std::string("0"));
        child += ",\"bracketMatched\":" + std::string(bracketMatched ? "true" : "false");
        child += ",\"failureCode\":" + std::to_string(failureCode);
        child += ",\"nativeBefore\":" + (copyAOk ? QaBeginPollNativeJsonBody(nativeA) : std::string("null"));
        child += ",\"bridge\":" + (bridgeSnapOk ? QaBeginPollBridgeJsonBody(bridgeSnap) : std::string("null"));
        child += ",\"nativeAfter\":" + (copyBOk ? QaBeginPollNativeJsonBody(nativeB) : std::string("null"));
        child += "}";
        j += ",\"qaSharedBeginPollDiagnosticsV1\":" + child;
    }
    catch (...)
    {
        try { j += ",\"qaSharedBeginPollDiagnosticsV1\":{\"schemaVersion\":1,\"enabled\":true,\"available\":false,\"bridgeSnapshotAvailable\":false,\"qpcFrequency\":0,\"snapshotBeginQpc\":0,\"bridgeSnapshotBeginQpc\":0,\"bridgeSnapshotEndQpc\":0,\"snapshotEndQpc\":0,\"bracketMatched\":false,\"failureCode\":4,\"nativeBefore\":null,\"bridge\":null,\"nativeAfter\":null}"; } catch (...) {}
    }
#endif // VRSIM_QA_D9_BEGIN_POLL
// CARD2B_QA_DIAGNOSTICS_END
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
