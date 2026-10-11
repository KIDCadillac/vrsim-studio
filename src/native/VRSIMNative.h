// VRSIMNative.h — VRSIM Native XR Backend (C ABI for P/Invoke from C# plugin)
// Runs inside iiQWorks.Sim process. OpenXR session belongs to the SIM process lifecycle.
#ifndef VRSIM_NATIVE_H
#define VRSIM_NATIVE_H

#include <windows.h>

// VR state machine
enum VRSIMState {
    VRSIM_OFF = 0,
    VRSIM_STARTING = 1,
    VRSIM_WAITING_FOR_HMD = 2,
    VRSIM_INITIALIZING_OPENXR = 3,
    VRSIM_SYNCHRONIZED = 4,
    VRSIM_VISIBLE = 5,
    VRSIM_RUNNING = 6,
    VRSIM_STOPPING = 7,
    VRSIM_ERROR = 8
};

struct VRSIMFrameState {
    unsigned long long frameSerial;
    long long displayTime;
    long long displayPeriod; // nanoseconds
    int shouldRender;
    int trackingValid;
};

struct VRSIMEyeView {
    double posMm[3];       // SIM world position (mm, after CARD16 rig)
    double quatXyzw[4];    // SIM world orientation
    double fovLeft, fovRight, fovUp, fovDown; // radians (raw XR)
};

extern "C" {
    // Optional: override loader path before VRSIM_Create. Empty = auto-discover
    // (registry ActiveRuntime dir, then Oculus runtime, then system).
    __declspec(dllexport) int VRSIM_SetLoaderPathW(const wchar_t* absolutePath);

    // Initialize OpenXR instance + system + session + swapchains, spawn XR worker.
    // Returns 0 on success (worker then drives READY->SYNCHRONIZED->VISIBLE).
    __declspec(dllexport) int VRSIM_Create();

    // Kept for API compat; worker already runs after Create. Returns current state code.
    __declspec(dllexport) int VRSIM_Start();

    // Non-blocking poll. Returns current VRSIMState.
    __declspec(dllexport) int VRSIM_Poll();

    // Get current state.
    __declspec(dllexport) int VRSIM_GetState();

    // Get current frame state. Returns 0 if valid.
    __declspec(dllexport) int VRSIM_GetFrameState(VRSIMFrameState* out);

    // Get current L/R eye views (SIM mm after CARD16 rig). Returns 0 if valid.
    __declspec(dllexport) int VRSIM_GetViews(VRSIMEyeView* outLeft, VRSIMEyeView* outRight);

    // Request session exit (xrRequestExitSession). Worker ends session on STOPPING.
    __declspec(dllexport) int VRSIM_Stop();

    // Stop worker and destroy all resources. Blocks until clean.
    __declspec(dllexport) int VRSIM_Destroy();

    // Get last error string (process-global).
    __declspec(dllexport) const char* VRSIM_GetLastError();

    // Compact diagnostics JSON (key evidence: inProcessSessionReachedVisible).
    // Returns bytes written (excl. NUL); negative on truncation.
    __declspec(dllexport) int VRSIM_GetDiagnostics(char* buf, int cap);
}

// ---- HIGHRES-H2SH-CARD1: additive shared-stereo C ABI (activation DISABLED) --
// Wire structs (native x64 Pack=8, mirrors frozen in SharedStereoWire.h):
//   ticket64  = VRSIM::SharedPreparedPairV1 (existing bridge ticket; tail
//               padding bytes 60..63 always serialized zero, never identity)
//   identity192 = VRSIM::SharedCaptureIdentityV1 (captureEndQpc MUST be 0 at
//               PREPARE and is supplied exactly once at COMMIT; all other
//               fields/bit patterns immutable between PREPARE and COMMIT)
//   stop32    = VRSIM::SharedStopStateV1 | stats72 = VRSIM::SharedBridgeStatsV1
//               (padding 20..23 serialized zero). Existing Plan56/Stats120
//               layouts are unchanged.
// Stable return codes for EVERY entry below:
//   0 = completed | 1 = pending / not completed
//  -1 = bad argument | -2 = unavailable (activation gate disabled / not bound)
//  -3 = stale (admission closed, epoch/generation moved, ticket/identity not
//      current) | -4 = quarantined (unproven GPU work; resources retained)
//  -5 = error (exact bridge HRESULT preserved internally; bridge S_FALSE is
//      never reported as completion)
// Shared activation stays disabled through the intermediate cards: Prepare/
// Commit/Discard/Drain-when-bound/Finalize refuse with -2 BEFORE any GPU call;
// state/stats queries are pure cached reads and never initialize the GPU. No
// shared capability is advertised via GetBuildInfo/Plan56/Stats120.
extern "C" {
    // Source UI thread ONLY, inside the typed owner's D3DImage double-lock
    // callback. leftSurface/rightSurface are borrowed IDirect3DSurface9 handle
    // NUMBERS valid only during this call — never cached past the callback.
    // On 0, writes the granted zero-padded ticket64 into outTicket64.
    __declspec(dllexport) int VRSIM_PrepareSharedStereoV1(
        unsigned long long leftSurface, unsigned long long rightSurface,
        const void* identity192, unsigned int identityCapacityBytes,
        void* outTicket64, unsigned int ticketCapacityBytes);

    // Source UI thread ONLY, after all managed post guards (never inside the
    // locked callback). Grants bridge COMMITTED + ring READY atomically against
    // the drain close. Requires the identical ticket/identity with the single
    // completion captureEndQpc set: 0 < poseBegin <= poseEnd <= captureBegin < captureEnd.
    __declspec(dllexport) int VRSIM_CommitSharedStereoV1(
        const void* ticket64, unsigned int ticketCapacityBytes,
        const void* identity192, unsigned int identityCapacityBytes);

    // Source UI thread ONLY. Exact-ticket checked; unknown tickets are never
    // dropped. READING/INFLIGHT stay for the worker event drain; a ring FREE
    // follows only a proven paired bridge FREE.
    __declspec(dllexport) int VRSIM_DiscardSharedStereoV1(
        const void* ticket64, unsigned int ticketCapacityBytes);

    // Closes shared admissions and reports honest state. Executes NO D3D or
    // context calls from this caller.
    __declspec(dllexport) int VRSIM_RequestSharedDrainV1();

    // Pure cached reads (capacities: stop32 / stats72). Never touch the GPU.
    __declspec(dllexport) int VRSIM_GetSharedStopStateV1(void* out32, unsigned int capacityBytes);
    __declspec(dllexport) int VRSIM_GetSharedBridgeStatsV1(void* out72, unsigned int capacityBytes);

    // Same source UI thread, after worker-drained proof and source capture
    // busy = 0. While nothing was ever bound reports unavailable (-2), NEVER a
    // fabricated drained success. A finalized owner is not reusable.
    __declspec(dllexport) int VRSIM_FinalizeSharedSourceUIV1();
}

#if defined(VRSIM_QA_D9_BEGIN_POLL)
// ---- SOURCE-UI-BEGIN-POLL-01: opt-in QA-only async Begin/Poll/Cancel facade --
// Present ONLY when VRSIM_QA_D9_BEGIN_POLL is compiled in; absent from the OFF
// build. These reuse the proved internal bridge Begin/Poll as a QA facade so the
// source UI is not occupied while the original GPU copy is pending. They add no
// new wire struct: identity192 is the unchanged SharedCaptureIdentityV1
// (captureEndQpc=0), and token32/result80/result56 are exactly the copied bridge
// QaBeginTokenV1(32)/QaBeginResultV1(80)/QaPollResultV1(56) numeric layouts;
// ticket64 uses the existing zero-padding serializer. Return codes match the
// shared set: 0 completed | 1 pending | -1 bad argument | -2 unavailable |
// -3 stale | -4 quarantined | -5 error. This is a QA-only facade: it makes no
// product frame-rate, comfort, multi-brand or compatibility claim.
extern "C" {
    // Source UI thread ONLY, inside the owner's D3DImage double-lock callback.
    // leftSurface/rightSurface are borrowed handle NUMBERS valid only this call.
    // 0 = granted/saved/published PREPARED ticket (ticket64 written);
    // 1 = real pending (result80 carries the exact token, ticket64 stays zero).
    __declspec(dllexport) int VRSIM_QaBeginSharedStereoV1(
        unsigned long long leftSurface, unsigned long long rightSurface,
        const void* identity192, unsigned int identityBytes,
        void* result80, unsigned int resultBytes,
        void* ticket64, unsigned int ticketBytes);

    // Source UI thread ONLY, on the ORIGINAL token. No new surfaces/identity; at
    // most one real GetData per call. 0 = granted PREPARED ticket (ticket64);
    // 1 = still pending; -3 = CompletedNoGrant drained; -5 = FailedDrained.
    __declspec(dllexport) int VRSIM_QaPollSharedStereoV1(
        const void* token32, unsigned int tokenBytes,
        void* result56, unsigned int resultBytes,
        void* ticket64, unsigned int ticketBytes);

    // Source UI thread ONLY, on the ORIGINAL exact token. Accepted cancel returns
    // 1 (never a completion signal): it only closes admission and marks the
    // original lease; it never drains/frees/finalizes. Wrong key/thread => -3.
    __declspec(dllexport) int VRSIM_QaCancelSharedStereoV1(
        const void* token32, unsigned int tokenBytes,
        void* result80, unsigned int resultBytes);
}
#endif // VRSIM_QA_D9_BEGIN_POLL

#endif // VRSIM_NATIVE_H
