// ObsD3D9ActualSourceQA.h -- CARD2A native synchronous QA DLL export (x64 MSVC,
// C++17, cdecl, Unicode).
//
// leftSurface / rightSurface are the caller's real, live IDirect3DSurface9
// IUnknown pointers held inside the same managed UI-transaction double
// D3DImage lock. The native side cannot validate an arbitrary numeric
// pointer; it QueryInterfaces own references immediately, uses only those,
// and never stores the borrowed caller sources across calls.
//
// resultUtf8 receives one complete UTF-8, NUL-terminated JSON document.
// capacity must be in [4096, 65536]; pairId must be > 0. There is no silent
// truncation: if the serialized JSON does not fit, nothing is claimed as
// published and the return is non-zero.
//
// Return codes:
//   0  all real gates passed (per-pixel parity, both-event drains, flag
//      restore, same-GPU proof, stereo content distinct, cleanup) AND the
//      complete JSON was published into resultUtf8.
//   1  executed but at least one gate failed, or JSON did not fit capacity.
//   2  invalid ABI parameters (null buffer, capacity out of range, pairId 0).
//   3  another call is already in flight (atomic single-call guard).
//   4  this process is quarantined by a previous call (flag restore or GPU
//      drain unproven; COM refs intentionally retained until process exit).
//
// This records only THIS static pair's parity. Frame freshness can only be
// proven by two separate managed invocations on two distinct frames. No
// claim here proves XR submission, encoding or FPS. Native performs no file
// IO; the root-owned managed adapter saves the returned JSON with CREATE_NEW.
#ifndef OBS_D3D9_ACTUAL_SOURCE_QA_H
#define OBS_D3D9_ACTUAL_SOURCE_QA_H

#include <cstdint>

#if !defined(VRSIM_PRODUCTION_SHARED_HELPERS)
extern "C" {

__declspec(dllexport) int ObsSourcePairGate2V1(uint64_t leftSurface, uint64_t rightSurface,
                                               uint64_t pairId, char* resultUtf8, uint32_t capacity);

}
#endif // !VRSIM_PRODUCTION_SHARED_HELPERS

#endif // OBS_D3D9_ACTUAL_SOURCE_QA_H
