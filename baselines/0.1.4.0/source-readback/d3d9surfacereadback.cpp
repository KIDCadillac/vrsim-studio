// NC-01 independent diagnostic helper: readback of a trusted, caller-owned eye
// View._scene IDirect3DSurface9 after the UI dispatcher validated the
// _window/_scene/D3DImage relationship. QA-only. Not a general pointer probe.
//
// Build: C++17, x64, Windows, d3d9. Owner (Codex) builds the actual helper
// binary and gate before any owned run. Do not run this from the repo.
// Link: d3d9.lib
//
// Contract limitations:
//  - GetRenderTargetData may block on the GPU; this API is NOT fully bounded.
//  - The source surface pointer is BORROWED. It is never Release()d and never
//    retained past return. The caller must keep the hosted view alive.
//  - No COM QueryInterface at render stage, no renderer/reprojection/XR
//    changes, no new threads, no private D3D device, no global quality or
//    resolution changes, no silent resize/resolve, no logging, no writes to
//    private files, no GPU shared texture / public resource escape /
//    executor loop.
//  - Alpha is copied raw from the source channel; never invented opaque.

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef NC01_API
#define NC01_API extern "C" __declspec(dllexport)
#endif

#pragma pack(push, 8)
struct NC01SurfaceInfoV1 {
    uint32_t structBytes;          // 0
    uint32_t version;              // 4
    uint32_t width;                // 8   (actual desc width)
    uint32_t height;               // 12  (actual desc height)
    uint32_t format;               // 16  (D3DFORMAT as captured)
    uint32_t type;                 // 20  (D3DSURFACETYPE as captured)
    uint32_t usage;                // 24
    uint32_t pool;                 // 28  (D3DPOOL as captured)
    uint32_t multiSampleType;      // 32  (D3DMULTISAMPLE_TYPE as captured)
    uint32_t multiSampleQuality;   // 36
    uint64_t deviceIdentity;       // 40  (raw GetDevice ptr, within-run identity evidence only; no ref held after return)
    uint64_t readbackQpcTicks;     // 48  (GetRenderTargetData duration)
    uint64_t copyQpcTicks;         // 56  (LockRect row-copy duration; excludes PNG/hash)
};                                   // sizeof == 64
#pragma pack(pop)

static_assert(sizeof(NC01SurfaceInfoV1) == 64, "NC01SurfaceInfoV1 must be 64 bytes");
static_assert(offsetof(NC01SurfaceInfoV1, deviceIdentity) == 40, "deviceIdentity at 40");
static_assert(offsetof(NC01SurfaceInfoV1, readbackQpcTicks) == 48, "readbackQpcTicks at 48");
static_assert(offsetof(NC01SurfaceInfoV1, copyQpcTicks) == 56, "copyQpcTicks at 56");

// Return codes (C ABI, int32):
//   0                success
//   -1               invalid argument (null surface/output, out-of-range dims,
//                    insufficient capacity, bad info capacity when info != null)
//   -2               UNSUPPORTED desc: format/pool/usage/type/multisample or
//                    actual dims != expected dims. Explicit negative; no
//                    silent resize/resolve/AA change.
//   -3               unlock-failure safety code (negative, data not claimed good)
//   -9               unexpected C++ exception swallowed at ABI boundary
//   otherwise        the actual failure HRESULT of the failing call, cast to
//                    int32. All D3D9 failure HRESULTs have bit 31 set, so they
//                    are <= -1073741825 and cannot collide with sentinels above.
//                    If a failing path ever sees a nonnegative code, the
//                    generic negative NC01_E_UNEXPECTED_HR (-4) is returned.

namespace {

constexpr int NC01_OK = 0;
constexpr int NC01_E_INVALIDARG = -1;
constexpr int NC01_E_UNSUPPORTED = -2;
constexpr int NC01_E_UNLOCK = -3;
constexpr int NC01_E_UNEXPECTED_HR = -4;
constexpr int NC01_E_CPP_EXCEPTION = -9;

constexpr uint64_t NC01_MAX_OUTPUT_BYTES = 32ull * 1024ull * 1024ull; // 32 MiB
constexpr uint32_t NC01_MIN_DIM = 2;
constexpr uint32_t NC01_MAX_DIM = 2048;

// HRESULT -> safe negative int32 for the C ABI.
inline int NC01_HrToCode(HRESULT hr) {
    const int32_t code = static_cast<int32_t>(hr);
    if (code < 0) {
        return code; // genuine failure HRESULT range; no sentinel collision
    }
    return NC01_E_UNEXPECTED_HR; // nonnegative hr on a failure path: keep ABI negative
}

// Minimal RAII for interfaces WE own (device ref from GetDevice, staging
// surface from CreateOffscreenPlainSurface). The borrowed source surface is
// NEVER passed to this and NEVER Released.
template <typename I>
class Nc01Owned {
    I* p_ = nullptr;
  public:
    Nc01Owned() = default;
    Nc01Owned(const Nc01Owned&) = delete;
    Nc01Owned& operator=(const Nc01Owned&) = delete;
    ~Nc01Owned() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    I** Receive() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
        return &p_;
    }
    I* get() const { return p_; }
    I* operator->() const { return p_; }
    operator I*() const { return p_; }
};

class Nc01QpcTimer {
    LARGE_INTEGER start_{};
  public:
    void Start() { QueryPerformanceCounter(&start_); }
    uint64_t ElapsedTicks() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (now.QuadPart < start_.QuadPart) {
            return 0;
        }
        return static_cast<uint64_t>(now.QuadPart - start_.QuadPart);
    }
};

inline bool NC01_FormatAccepted(D3DFORMAT f) {
    return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8;
}

NC01_API int NC01_ReadSurface(
    void* borrowedSurface,
    uint32_t expectedW,
    uint32_t expectedH,
    unsigned char* output,
    uint64_t capacity,
    NC01SurfaceInfoV1* info,
    uint32_t infoCapacity)
{
    try {
        // ---- 1. Cheap argument validation (no COM calls yet) ----
        if (borrowedSurface == nullptr || output == nullptr) {
            return NC01_E_INVALIDARG;
        }
        if (expectedW < NC01_MIN_DIM || expectedW > NC01_MAX_DIM ||
            expectedH < NC01_MIN_DIM || expectedH > NC01_MAX_DIM) {
            return NC01_E_INVALIDARG;
        }

        // info: zero-fill ONLY after pointer + capacity >= 64 is verified.
        if (info != nullptr) {
            if (infoCapacity < 64) {
                return NC01_E_INVALIDARG;
            }
            std::memset(info, 0, 64);
            info->structBytes = 64;
            info->version = 1;
        }

        IDirect3DSurface9* src =
            static_cast<IDirect3DSurface9*>(borrowedSurface); // borrowed; never Release

        // ---- 2. GetDesc: capture actual fields; explicit UNSUPPORTED filter ----
        D3DSURFACE_DESC desc{};
        HRESULT hr = src->GetDesc(&desc);
        if (FAILED(hr)) {
            return NC01_HrToCode(hr); // desc info not further populated; actual HR
        }

        if (info != nullptr) {
            info->width = static_cast<uint32_t>(desc.Width);
            info->height = static_cast<uint32_t>(desc.Height);
            info->format = static_cast<uint32_t>(desc.Format);
            info->type = static_cast<uint32_t>(desc.Type);
            info->usage = static_cast<uint32_t>(desc.Usage);
            info->pool = static_cast<uint32_t>(desc.Pool);
            info->multiSampleType = static_cast<uint32_t>(desc.MultiSampleType);
            info->multiSampleQuality = desc.MultiSampleQuality;
        }

        const bool descOk =
            desc.Width == expectedW && desc.Height == expectedH &&
            desc.Width >= NC01_MIN_DIM && desc.Width <= NC01_MAX_DIM &&
            desc.Height >= NC01_MIN_DIM && desc.Height <= NC01_MAX_DIM &&
            NC01_FormatAccepted(desc.Format) &&
            desc.Pool == D3DPOOL_DEFAULT &&
            desc.Usage == D3DUSAGE_RENDERTARGET &&
            desc.Type == D3DRTYPE_SURFACE &&
            desc.MultiSampleType == D3DMULTISAMPLE_NONE &&
            desc.MultiSampleQuality == 0;
        if (!descOk) {
            return NC01_E_UNSUPPORTED; // captured desc stays in info; no fixups
        }

        // ---- 3. Shape/overflow checks before any output copy ----
        // w,h <= 2048 so w*4 and w*4*h cannot overflow uint64; checked anyway.
        const uint64_t stride = static_cast<uint64_t>(desc.Width) * 4ull;
        const uint64_t need = stride * static_cast<uint64_t>(desc.Height);
        if (stride / 4ull != desc.Width || need / stride != desc.Height) {
            return NC01_E_INVALIDARG;
        }
        if (need > NC01_MAX_OUTPUT_BYTES) {
            return NC01_E_INVALIDARG;
        }
        if (capacity < need) {
            return NC01_E_INVALIDARG;
        }

        // ---- 4. GetDevice: we own the returned ref; RAII on all paths ----
        IDirect3DDevice9* devRaw = nullptr;
        hr = src->GetDevice(&devRaw);
        if (FAILED(hr) || devRaw == nullptr) {
            if (devRaw) {
                devRaw->Release();
            }
            return FAILED(hr) ? NC01_HrToCode(hr) : NC01_E_UNEXPECTED_HR;
        }
        Nc01Owned<IDirect3DDevice9> dev;
        *dev.Receive() = devRaw; // RAII owns it from here
        if (info != nullptr) {
            info->deviceIdentity = reinterpret_cast<uint64_t>(devRaw); // identity evidence only
        }

        // ---- 5. Staging: same ACTUAL W/H/format, D3DPOOL_SYSTEMMEM; owned RAII ----
        IDirect3DSurface9* stagingRaw = nullptr;
        hr = dev->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
            &stagingRaw, nullptr);
        if (FAILED(hr) || stagingRaw == nullptr) {
            if (stagingRaw) {
                stagingRaw->Release();
            }
            return FAILED(hr) ? NC01_HrToCode(hr) : NC01_E_UNEXPECTED_HR;
        }
        Nc01Owned<IDirect3DSurface9> staging;
        *staging.Receive() = stagingRaw; // owned RAII from here; source stays borrowed

        // ---- 6. GetRenderTargetData, QPC-measured (may block on GPU) ----
        Nc01QpcTimer readbackTimer;
        readbackTimer.Start();
        hr = dev->GetRenderTargetData(src, staging);
        const uint64_t readbackTicks = readbackTimer.ElapsedTicks();
        if (info != nullptr) {
            info->readbackQpcTicks = readbackTicks;
        }
        if (FAILED(hr)) {
            return NC01_HrToCode(hr); // GetDesc info retained; actual HR; RAII frees staging/dev
        }

        // ---- 7. LockRect READONLY, validate, row-copy, always unlock ----
        D3DLOCKED_RECT lr{};
        hr = staging->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            return NC01_HrToCode(hr); // lock failed: nothing was locked, no unlock needed
        }

        int result = NC01_OK;
        {
            bool shapeOk = true;
            if (lr.pBits == nullptr) {
                shapeOk = false;
            }
            // lr.Pitch is signed; reject <= 0 BEFORE widening, otherwise a
            // negative value wraps to a huge uint64 and bypasses the checks.
            const uint64_t lockPitch = lr.Pitch > 0
                ? static_cast<uint64_t>(lr.Pitch)
                : 0ull;
            if (lockPitch == 0 || lockPitch < stride) {
                shapeOk = false; // require positive pitch >= w*4
            }
            if (!shapeOk) {
                hr = staging->UnlockRect();
                if (FAILED(hr)) {
                    result = NC01_E_UNLOCK; // never claim data good
                } else {
                    result = NC01_E_INVALIDARG; // bad locked-rect shape
                }
            } else {
                Nc01QpcTimer copyTimer;
                copyTimer.Start();
                const uint32_t rowBytes = desc.Width * 4u; // safe: w <= 2048
                const uint8_t* srcBits = static_cast<const uint8_t*>(lr.pBits);
                for (uint32_t y = 0; y < desc.Height; ++y) {
                    std::memcpy(
                        output + static_cast<size_t>(y) * rowBytes,
                        srcBits + static_cast<size_t>(y) * static_cast<size_t>(lockPitch),
                        rowBytes); // raw channels incl. source alpha; never invented
                }
                if (info != nullptr) {
                    info->copyQpcTicks = copyTimer.ElapsedTicks();
                }
                hr = staging->UnlockRect();
                if (FAILED(hr)) {
                    result = NC01_HrToCode(hr); // negative; do not claim data good
                }
            }
        }

        // ---- 8. Owned refs unwind via RAII (dev, staging). The borrowed
        // source is never Released and no pointer is retained past return.
        return result;
    } catch (...) {
        return NC01_E_CPP_EXCEPTION;
    }
}

} // namespace
