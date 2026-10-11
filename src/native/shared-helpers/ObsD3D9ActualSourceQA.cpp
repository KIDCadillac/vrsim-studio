// ObsD3D9ActualSourceQA.cpp -- CARD2A: real SIM D3D9 surface -> D3D11 shared
// texture native synchronous QA, built as an x64 DLL. Direction/review/
// compile/root-owned real verification by Codex; this file is the code.
//
// Reuses the audited Gate1 library (which itself includes frozen Gate0) via
// absolute-path #include. Only definitions are pulled in; we NEVER call
// g1::Run, g1::InitDevicesAndAdapter, g1::FillCopy, g1::WaitEvent9, either
// included wmain, or g1::EmitJson file IO. We DO reuse g1::Ctx,
// g1::LoadDlls, g1::DiscoverLayout, the safe ReadUxx/ReadBytesSafe scans,
// g1::g1_writable, g1::MakeHiddenWindow/Show and g1::Cleanup.
// No hardcoded historical off1/off2, no byte patches, no NVENC, no XR,
// no FPS, not a production per-frame path.
//
// Suggested compile (by Codex, DLL):
//   cl /std:c++17 /EHsc /LD /DUNICODE /D_UNICODE ObsD3D9ActualSourceQA.cpp
//   (user32.lib version.lib dbghelp.lib bcrypt.lib are pragma-attached;
//    d3d9/d3d11/dxgi are runtime-loaded from System32 absolute paths only)
#ifndef _M_X64
#error CARD2A ObsD3D9ActualSourceQA requires x64
#endif
#include "ObsD3D9ActualSourceQA.h"

#ifndef UNICODE
#define UNICODE
#endif
#include "ObsD3D9SharedColorQA.cpp"

#include <bcrypt.h>
#include <cfenv>
#include <intrin.h>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace g2 {

// ---- process state: atomic single-call guard + sticky quarantine ----------
static volatile LONG g_inCall = 0;
static volatile LONG g_quarantined = 0;

// Allocation-free RAII: guarantees g_inCall is released on every return AND
// every unwind path out of the export.
struct InCallGuard {
    ~InCallGuard() noexcept { InterlockedExchange(&g_inCall, 0); }
};

// Fixed literal published when a C++ exception escapes: never rebuilt,
// never allocated, so the catch path performs no std::string/format/vector work.
static const char kExceptionJson[] =
    "{\"schemaVersion\":2,"
    "\"scope\":\"ACTUAL_SIM_SURFACE_SHARED_PARITY_ONLY_NOT_XR_NOT_FPS\","
    "\"gatePass\":false,\"quarantine\":true,\"cleanupPass\":false,"
    "\"reason\":\"C++ exception escaped; state unknown; own refs retained until process exit; "
    "subsequent calls refused\","
    "\"errors\":[\"exception escaped run body\"]}";

// Fixed literal for calls refused while the process is quarantined.
static const char kQuarantineJson[] =
    "{\"schemaVersion\":2,"
    "\"scope\":\"ACTUAL_SIM_SURFACE_SHARED_PARITY_ONLY_NOT_XR_NOT_FPS\","
    "\"gatePass\":false,\"quarantine\":true,\"cleanupPass\":false,"
    "\"reason\":\"process quarantined after unproven flag restore, GPU drain, unlock or exception; "
    "call refused; refs retained until process exit\","
    "\"errors\":[\"call refused\"]}";

// ---- FPU preservation: actual UCRT fenv save/restore (fegetenv/fesetenv,
// field-verified against _Fe_ctl/_Fe_stat like the audited Nv9 FenvGuard)
// plus MXCSR save/set/readback. NEVER _control87 with _MCW_PC/_MCW_IC on x64
// (invalid-parameter/assert risk). Every rc is recorded for real; success is
// never hardcoded. Destructor is a fallback for unwinds only; it restores
// only when no explicit restore was attempted and verifies nothing.
class FpuPreserve {
public:
    FpuPreserve() : captured_(false), restoreAttempted_(false), restored_(false),
                    captureRc_(-1), restoreRc_(-1), mxcsr_(0), mxcsrCaptured_(false),
                    mxcsrVerified_(false), postReadRc_(-1), postIdentical_(false),
                    postCtl_(0), postStat_(0)
    {
        std::memset(&env_, 0, sizeof(env_)); // zero-init per contract
    }
    FpuPreserve(const FpuPreserve&) = delete;
    FpuPreserve& operator=(const FpuPreserve&) = delete;
    ~FpuPreserve() noexcept
    {
        if (captured_ && !restoreAttempted_) { std::fesetenv(&env_); }
        if (mxcsrCaptured_ && !restoreAttempted_) { _mm_setcsr(mxcsr_); }
    }

    void Capture()
    {
        if (captured_ || mxcsrCaptured_) { return; }
        captureRc_ = std::fegetenv(&env_);
        captured_ = (captureRc_ == 0);
        mxcsr_ = _mm_getcsr();
        mxcsrCaptured_ = true;
    }

    // Real restore + real verification; idempotent (called once before
    // serialization and again at the final exit after publication).
    bool RestoreAndVerify()
    {
        restoreAttempted_ = true;
        bool ok = true;
        if (captured_) { restoreRc_ = std::fesetenv(&env_); restored_ = (restoreRc_ == 0); }
        else { restored_ = false; }
        ok = ok && restored_;
        if (mxcsrCaptured_) { _mm_setcsr(mxcsr_); mxcsrVerified_ = (_mm_getcsr() == mxcsr_); }
        else { mxcsrVerified_ = false; }
        ok = ok && mxcsrVerified_;
        std::fenv_t cur;
        std::memset(&cur, 0, sizeof(cur));
        postReadRc_ = std::fegetenv(&cur);
        if (postReadRc_ == 0) {
            postCtl_ = cur._Fe_ctl;
            postStat_ = cur._Fe_stat;
            postIdentical_ = (postCtl_ == env_._Fe_ctl) && (postStat_ == env_._Fe_stat);
        } else {
            postIdentical_ = false;
        }
        ok = ok && postIdentical_;
        return ok;
    }

    bool Captured() const { return captured_; }
    bool RestoreAttempted() const { return restoreAttempted_; }
    int CaptureRc() const { return captureRc_; }
    int RestoreRc() const { return restoreRc_; }
    bool MxcsrVerified() const { return mxcsrVerified_; }
    unsigned long SavedCtl() const { return env_._Fe_ctl; }
    unsigned long SavedStat() const { return env_._Fe_stat; }
    unsigned SavedMxcsr() const { return mxcsr_; }
    int PostReadRc() const { return postReadRc_; }
    bool PostIdentical() const { return postIdentical_; }

private:
    std::fenv_t env_;
    bool captured_;
    bool restoreAttempted_;
    bool restored_;
    int captureRc_;
    int restoreRc_;
    unsigned mxcsr_;
    bool mxcsrCaptured_;
    bool mxcsrVerified_;
    int postReadRc_;
    bool postIdentical_;
    unsigned long postCtl_;
    unsigned long postStat_;
};

// ---- QPC helpers (Win32 BOOL returns already handled by g1::Timer*) -------
struct Tm {
    LARGE_INTEGER f, n0;
    bool ok;
    Tm() : ok(g1::TimerStart(f, n0)) {}
};
static long long TmMs(const Tm& t)
{
    LARGE_INTEGER n;
    if (!t.ok || !QueryPerformanceCounter(&n) || n.QuadPart < t.n0.QuadPart) return -1;
    return (long long)((n.QuadPart - t.n0.QuadPart) * 1000 / t.f.QuadPart);
}

// ---- per-eye evidence ------------------------------------------------------
struct Eye {
    const char* name;
    UINT w = 0, h = 0;

    IDirect3DSurface9* surf = nullptr;   // own QI ref of borrowed caller source
    IUnknown* canon = nullptr;           // own QI(IID_IUnknown) ref, released early
    D3DSURFACE_DESC d{};
    bool haveDesc = false, sizeOk = false, fmtOk = false, typeOk = false,
         usageRt = false, poolDef = false, msaaNone = false;

    ID3D11Texture2D* shared11 = nullptr;
    D3D11_TEXTURE2D_DESC sharedActual{};
    bool sharedDescOk = false;
    HANDLE legacy = nullptr;             // legacy non-NT number: NEVER CloseHandle/Duplicate

    bool flagWritableFalseProven = false, setTrueVerified = false, restoreVerified = false, flagSeh = false;
    HRESULT createRc = E_FAIL;
    bool createRcKnown = false;
    IDirect3DTexture9* shared9 = nullptr;
    IDirect3DSurface9* sharedSurf9 = nullptr;

    IDirect3DQuery9* ev9 = nullptr;
    ID3D11Texture2D* stage11 = nullptr;
    ID3D11Query* ev11 = nullptr;
    bool stretched = false;              // marks GPU submission for quarantine logic
    int d9Polls = -1;
    bool d9Ready = false;
    bool copyIssued = false, flushDone = false, endDone = false;
    int d11Polls = -1;
    bool d11Ready = false;               // S_OK && TRUE (4-byte BOOL) only

    IDirect3DSurface9* plain = nullptr;  // QA oracle SYSTEMMEM
    bool lockOk = false, unlockOk = false, mapOk = false, mapStillDrawing = false;
    UINT srcPitch = 0, shPitch = 0;
    long long mismatch = -1, srcMinX = -1, srcMaxX = -1, shMinX = -1, shMaxX = -1;
    bool shaSrcOk = false, shaShOk = false;
    std::string shaSrc, shaSh;
    bool oracleOk = false;

    // CPU-side QPC segment ms; -1 == segment never executed. NOT GPU exec time.
    long long tBoot = -1, tStretch = -1, tD9Wait = -1, tCopy = -1, tD11Wait = -1, tQ9 = -1, tQ11 = -1;

    bool released = false;

    Eye(const char* n) : name(n) {}
    bool FullPass() const
    {
        return sharedDescOk && legacy != nullptr && flagWritableFalseProven && setTrueVerified &&
               restoreVerified && !flagSeh &&
               createRcKnown && createRc == S_OK && shared9 && sharedSurf9 && stretched && d9Ready &&
               copyIssued && flushDone && endDone && d11Ready && lockOk && unlockOk && mapOk && oracleOk &&
               mismatch == 0 && shaSrcOk && shaShOk;
    }
};

static void ReleaseEye(Eye& e)
{
    auto rel = [](IUnknown** p) { if (*p) { (*p)->Release(); *p = nullptr; } };
    rel((IUnknown**)&e.plain);
    rel((IUnknown**)&e.ev9);
    rel((IUnknown**)&e.ev11);
    rel((IUnknown**)&e.stage11);
    rel((IUnknown**)&e.sharedSurf9);
    rel((IUnknown**)&e.shared9);
    rel((IUnknown**)&e.shared11);
    rel((IUnknown**)&e.surf);
    e.legacy = nullptr; // opaque number dropped; it was never closed, only its owning ref goes away
    e.released = true;
}

// ---- optional internal wait counters (prepare-wait telemetry, Stage A) -----
// NOT a wire/export struct: pure fixed-width numerics, zero-initialized by
// the caller. nullptr (the default, e.g. the QA RunEye caller) leaves the
// original wait policy and observable work byte-for-byte unchanged. When
// present, the wait ONLY observes real execution: counters increment before
// the real call they describe; classification never rewrites the original
// branches; QPC timing failures clear only the affected timing mask and
// never change any HRESULT, wait branch, FPU state or quarantine outcome.
struct Wait9Counters {
    uint64_t getDataCalls;        // real GetData(NULL,0,D3DGETDATA_FLUSH) invocations
    uint64_t flushGetDataCalls;   // observed FLUSH-flagged calls (this policy: same real calls)
    uint64_t sFalseReturns;       // exact S_FALSE returns only
    uint64_t otherPendingReturns; // other non-failure, non-S_OK returns the ORIGINAL code kept waiting on
    uint64_t sleep1Calls;         // ONLY iterations where Sleep(1) really ran (never inferred from d9Polls)
    LONG lastGetDataHr;
    uint32_t lastGetDataHrValid;  // 1 once at least one real GetData returned
    uint32_t maskWaitWall;        // waitWallTicks measured
    uint32_t maskGetDataWall;     // getDataWallTicksSum measured
    uint32_t maskSleepWall;       // sleepWallTicksSum measured
    uint64_t qpcFrequency;        // from the wait's own successful QPF (raw ticks are not ms)
    uint64_t waitWallTicks;       // whole n0..exit window, raw QPC ticks (CPU-side wall, NOT GPU time)
    uint64_t getDataWallTicksSum; // sum of real per-GetData call windows
    uint64_t sleepWallTicksSum;   // sum of real per-Sleep(1) windows
};

// ---- event waits: bounded <=128 polls / 1000 ms / Sleep(1), NO message pump
// Same params, same order, same 128/DEADLINE_MS/error/Sleep(1) policy as the
// sealed original; wc is observation only (default nullptr = original path).
static bool WaitEvent9NoPump(g1::Ctx& c, Eye& e, IDirect3DQuery9* ev, Wait9Counters* wc = nullptr)
{
    LARGE_INTEGER f, n0;
    e.d9Polls = 0;
    e.d9Ready = false;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0 || !QueryPerformanceCounter(&n0)) {
        c.err("QueryPerformance timer unavailable (event9)");
        return false; // original refusal; the wait loop never ran
    }
    if (wc != nullptr) wc->qpcFrequency = (uint64_t)f.QuadPart;
    // Fix(StageA2) sticky completeness: once ANY real GetData/Sleep call
    // lacks a successful start+end QPC pair (or end<start), the phase stays
    // invalid through every later success; a mask of 1 then means "at least
    // one real call of this phase ran AND every such call was completely
    // measured". Zero real calls leaves the caller-zeroed mask 0 (never a
    // measured zero). Observation only: no branch, HRESULT or return changes.
    bool gdIncomplete = false;
    bool slIncomplete = false;
    while (e.d9Polls < g1::MAX_POLLS) {
        LARGE_INTEGER gA, gB;
        bool gAok = false;
        if (wc != nullptr) gAok = QueryPerformanceCounter(&gA) != 0;
        if (wc != nullptr) { ++wc->getDataCalls; ++wc->flushGetDataCalls; }
        const HRESULT h = ev->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        if (wc != nullptr) {
            const bool gBok = QueryPerformanceCounter(&gB) != 0;
            wc->lastGetDataHr = (LONG)h;
            wc->lastGetDataHrValid = 1u;
            if (gAok && gBok && gB.QuadPart >= gA.QuadPart) {
                wc->getDataWallTicksSum += (uint64_t)(gB.QuadPart - gA.QuadPart);
            } else {
                gdIncomplete = true; // sticky: this real GetData was not completely measured
            }
            // Maintained after EVERY real call, so the value at any exit
            // (S_OK/FAILED/deadline/original-QPC-failure/MAX_POLLS) is final:
            wc->maskGetDataWall = gdIncomplete ? 0u : 1u;
        }
        if (h == S_OK) {
            LARGE_INTEGER n;
            if (QueryPerformanceCounter(&n) && n.QuadPart >= n0.QuadPart) {
                e.tD9Wait = (long long)((n.QuadPart - n0.QuadPart) * 1000 / f.QuadPart);
                if (wc != nullptr) {
                    wc->waitWallTicks = (uint64_t)(n.QuadPart - n0.QuadPart);
                    wc->maskWaitWall = 1u;
                }
            }
            e.d9Ready = true;
            return true;
        }
        if (FAILED(h)) {
            LARGE_INTEGER tE;
            const bool tEok = (wc != nullptr) && (QueryPerformanceCounter(&tE) != 0);
            c.hr("IDirect3DQuery9::GetData(event9,FLUSH)", h);
            if (tEok && tE.QuadPart >= n0.QuadPart) {
                wc->waitWallTicks = (uint64_t)(tE.QuadPart - n0.QuadPart);
                wc->maskWaitWall = 1u;
            }
            return false; // FAILED: original immediate stop; lastGetDataHr already recorded
        }
        if (wc != nullptr) {
            if (h == S_FALSE) ++wc->sFalseReturns;
            else ++wc->otherPendingReturns; // any other non-failure code: original path keeps waiting
        }
        ++e.d9Polls;
        LARGE_INTEGER n;
        if (!QueryPerformanceCounter(&n) || n.QuadPart < n0.QuadPart) { c.err("QPC failed (event9 wait)"); return false; }
        const long long el = (long long)((n.QuadPart - n0.QuadPart) * 1000 / f.QuadPart);
        if (el >= g1::DEADLINE_MS) {
            e.tD9Wait = el;
            if (wc != nullptr) {
                wc->waitWallTicks = (uint64_t)(n.QuadPart - n0.QuadPart);
                wc->maskWaitWall = 1u;
            }
            return false; // deadline: this final iteration had NO Sleep, never infer one
        }
        if (wc != nullptr) {
            LARGE_INTEGER sA, sB;
            const bool sAok = QueryPerformanceCounter(&sA) != 0;
            ++wc->sleep1Calls; // only now does a real Sleep(1) execute
            Sleep(1);
            bool slComplete = false;
            if (sAok) {
                const bool sBok = QueryPerformanceCounter(&sB) != 0;
                slComplete = sBok && sB.QuadPart >= sA.QuadPart;
                if (slComplete) wc->sleepWallTicksSum += (uint64_t)(sB.QuadPart - sA.QuadPart);
            }
            if (!slComplete) slIncomplete = true; // sticky; no end QPC on a failed start
            wc->maskSleepWall = slIncomplete ? 0u : 1u;
        } else {
            Sleep(1);
        }
    }
    // MAX_POLLS exhaustion (original return path): the final poll's own
    // deadline QPC already ran above; capture only the exit window here.
    if (wc != nullptr) {
        LARGE_INTEGER tE;
        if (QueryPerformanceCounter(&tE) != 0 && tE.QuadPart >= n0.QuadPart) {
            wc->waitWallTicks = (uint64_t)(tE.QuadPart - n0.QuadPart);
            wc->maskWaitWall = 1u;
        }
    }
    return false;
}

static bool WaitEvent11NoPump(g1::Ctx& c, Eye& e, ID3D11Query* ev)
{
    LARGE_INTEGER f, n0;
    e.d11Polls = 0;
    e.d11Ready = false;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0 || !QueryPerformanceCounter(&n0)) {
        c.err("QueryPerformance timer unavailable (event11)");
        return false;
    }
    while (e.d11Polls < g1::MAX_POLLS) {
        BOOL signaled = FALSE;
        const HRESULT h = c.ctx11->GetData(ev, &signaled, sizeof(BOOL), 0); // exactly 4-byte Win32 BOOL
        if (h == S_OK) {
            LARGE_INTEGER n;
            if (QueryPerformanceCounter(&n) && n.QuadPart >= n0.QuadPart)
                e.tD11Wait = (long long)((n.QuadPart - n0.QuadPart) * 1000 / f.QuadPart);
            e.d11Ready = (signaled == TRUE); // only S_OK + TRUE BOOL means complete
            return e.d11Ready;
        }
        if (FAILED(h) && h != S_FALSE) { c.hr("ID3D11DeviceContext::GetData(event11)", h); return false; }
        ++e.d11Polls;
        LARGE_INTEGER n;
        if (!QueryPerformanceCounter(&n) || n.QuadPart < n0.QuadPart) { c.err("QPC failed (event11 wait)"); return false; }
        const long long el = (long long)((n.QuadPart - n0.QuadPart) * 1000 / f.QuadPart);
        if (el >= g1::DEADLINE_MS) { e.tD11Wait = el; return false; }
        Sleep(1);
    }
    return false;
}

// ---- POD SEH leaf: writable-FALSE proof, one scoped TRUE write, exactly one
// CreateTexture at the ACTUAL dynamic eye size, finally-restore to original.
struct FlagOut2 {
    BOOL original;
    bool originalReadOk;
    HRESULT createRc;
    bool setTrueReadback;
    bool exceptionDuring;
};
static void FlagOpenLeaf2(IDirect3DDevice9* dev, void* flagAddr, UINT w, UINT h,
                          IDirect3DTexture9** ppTex, HANDLE shared, FlagOut2* out)
{
    BOOL orig = FALSE;
    out->original = FALSE;
    out->originalReadOk = false;
    out->createRc = E_FAIL;
    out->setTrueReadback = false;
    out->exceptionDuring = false;
    if (!g1::g1_writable(flagAddr) || !ReadBytesSafe(flagAddr, &orig, sizeof(BOOL)) || orig != FALSE) {
        out->original = orig;
        out->exceptionDuring = true; // refuse to enter the flag scope at all
        return;
    }
    out->original = orig;
    out->originalReadOk = true;
    __try {
        __try {
            *(volatile BOOL*)flagAddr = TRUE;
            out->setTrueReadback = (*(volatile BOOL*)flagAddr) == TRUE;
            out->createRc = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, g1::FMT22, D3DPOOL_DEFAULT,
                                               ppTex, &shared);
        } __finally {
            *(volatile BOOL*)flagAddr = orig;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->exceptionDuring = true;
    }
}

// ---- BCrypt SHA256 over packed RGB only (bytes B,G,R used as R,G,B; the X
// byte and row padding never enter the hash). Pure POD body, no C++ objects.
static bool Sha256RgbPod(const uint8_t* base, UINT pitch, UINT w, UINT h, uint8_t digest[32])
{
    uint8_t row[2048 * 3];
    if (w > 2048u) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    bool ok = true;
    for (UINT y = 0; y < h && ok; ++y) {
        const uint8_t* rp = base + (size_t)y * (size_t)pitch;
        for (UINT x = 0; x < w; ++x) {
            row[(size_t)3 * x] = rp[(size_t)4 * x + 2];     // R
            row[(size_t)3 * x + 1] = rp[(size_t)4 * x + 1]; // G
            row[(size_t)3 * x + 2] = rp[(size_t)4 * x];     // B
        }
        if (BCryptHashData(hh, row, (ULONG)((size_t)w * 3u), 0) != 0) ok = false;
    }
    if (ok && BCryptFinishHash(hh, digest, 32, 0) != 0) ok = false;
    BCryptDestroyHash(hh);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- exactly-once unlock/unmap guards (these scopes contain no SEH) -------
struct UnlockRectG {
    Eye* e;
    IDirect3DSurface9* s;
    ~UnlockRectG() { e->unlockOk = (s->UnlockRect() == S_OK); }
};
struct UnmapG {
    ID3D11DeviceContext* ctx;
    ID3D11Texture2D* res;
    ~UnmapG() { ctx->Unmap(res, 0); }
};

// ---- Stage A: QI own refs of both borrowed surfaces + real desc gates -----
static bool AcquireSources(g1::Ctx& c, uint64_t lp, uint64_t rp, Eye& eL, Eye& eR,
                           bool& surfacesDistinct, bool& sameDevice, bool& beh044, bool& halType,
                           bool& parentOk, bool& ordNotExQI)
{
    surfacesDistinct = sameDevice = beh044 = halType = parentOk = ordNotExQI = false;
    if (lp == 0 || rp == 0) { c.err("caller surface pointer is zero"); return false; }

    // Own refs only from here on; the borrowed numbers are never stored.
    IUnknown* lu = reinterpret_cast<IUnknown*>((uintptr_t)lp);
    IUnknown* ru = reinterpret_cast<IUnknown*>((uintptr_t)rp);
    if (FAILED(lu->QueryInterface(__uuidof(IDirect3DSurface9), (void**)&eL.surf)) || eL.surf == nullptr) {
        c.err("left surface QI(IDirect3DSurface9) failed");
        return false;
    }
    if (FAILED(ru->QueryInterface(__uuidof(IDirect3DSurface9), (void**)&eR.surf)) || eR.surf == nullptr) {
        c.err("right surface QI(IDirect3DSurface9) failed");
        return false;
    }

    // Canonical IUnknown identity: two DIFFERENT surfaces.
    bool lq = SUCCEEDED(eL.surf->QueryInterface(IID_IUnknown, (void**)&eL.canon)) && eL.canon;
    bool rq = SUCCEEDED(eR.surf->QueryInterface(IID_IUnknown, (void**)&eR.canon)) && eR.canon;
    surfacesDistinct = lq && rq && eL.canon != eR.canon;
    if (eL.canon) { eL.canon->Release(); eL.canon = nullptr; }
    if (eR.canon) { eR.canon->Release(); eR.canon = nullptr; }
    if (!lq || !rq) { c.err("canonical IUnknown QI failed"); return false; }
    if (!surfacesDistinct) { c.err("left and right surfaces are the same canonical object"); return false; }

    HRESULT h = eL.surf->GetDesc(&eL.d);
    if (FAILED(h)) { c.hr("left GetDesc", h); return false; }
    eL.haveDesc = true;
    h = eR.surf->GetDesc(&eR.d);
    if (FAILED(h)) { c.hr("right GetDesc", h); return false; }
    eR.haveDesc = true;

// HIGHRES-H2 mixed-source QA: the 4096 square is admitted into the inherited
// 3072-profile whitelist ONLY under VRSIM_QA_MIXED_SOURCE, which requires
// VRSIM_SHARED_3072. Mixed without 3072 is a hard compile-time error; the
// format-22 / MS0 / canonical-device / LUID / FPU / device-borrow / StretchRect
// / event-wait gates are never relaxed here.
#if defined(VRSIM_QA_MIXED_SOURCE) && !defined(VRSIM_SHARED_3072)
#error VRSIM_QA_MIXED_SOURCE requires VRSIM_SHARED_3072 in the actual source QA helper
#endif
#if defined(VRSIM_QA_MIXED_SOURCE)
    const UINT sizes[7] = { 1024, 1536, 2048, 2560, 2736, 3072, 4096 };
    auto sq = [&](UINT v) { return v == sizes[0] || v == sizes[1] || v == sizes[2] || v == sizes[3] || v == sizes[4] || v == sizes[5] || v == sizes[6]; };
#elif defined(VRSIM_SHARED_3072)
    const UINT sizes[6] = { 1024, 1536, 2048, 2560, 2736, 3072 };
    auto sq = [&](UINT v) { return v == sizes[0] || v == sizes[1] || v == sizes[2] || v == sizes[3] || v == sizes[4] || v == sizes[5]; };
#elif defined(VRSIM_SHARED_2736)
    const UINT sizes[5] = { 1024, 1536, 2048, 2560, 2736 };
    auto sq = [&](UINT v) { return v == sizes[0] || v == sizes[1] || v == sizes[2] || v == sizes[3] || v == sizes[4]; };
#elif defined(VRSIM_SHARED_2560)
    const UINT sizes[4] = { 1024, 1536, 2048, 2560 };
    auto sq = [&](UINT v) { return v == sizes[0] || v == sizes[1] || v == sizes[2] || v == sizes[3]; };
#else
    const UINT sizes[3] = { 1024, 1536, 2048 };
    auto sq = [&](UINT v) { return v == sizes[0] || v == sizes[1] || v == sizes[2]; };
#endif
    eL.sizeOk = eL.d.Width == eL.d.Height && sq(eL.d.Width);
    eR.sizeOk = eR.d.Width == eR.d.Height && sq(eR.d.Width);
    for (Eye* e : { &eL, &eR }) {
        e->fmtOk = (uint32_t)e->d.Format == 22u; // D3DFMT_X8R8G8B8
        e->typeOk = e->d.Type == D3DRTYPE_SURFACE;
        e->usageRt = (e->d.Usage & D3DUSAGE_RENDERTARGET) != 0;
        e->poolDef = e->d.Pool == D3DPOOL_DEFAULT;
        e->msaaNone = e->d.MultiSampleType == D3DMULTISAMPLE_NONE;
    }
    bool sameSquare = eL.d.Width == eR.d.Width && eL.d.Height == eR.d.Height;
#if defined(VRSIM_QA_MIXED_SOURCE)
    if (!eL.sizeOk || !eR.sizeOk || !sameSquare) { c.err("desc gate: surfaces must be same square 1024/1536/2048/2560/2736/3072/4096"); return false; }
#elif defined(VRSIM_SHARED_3072)
    if (!eL.sizeOk || !eR.sizeOk || !sameSquare) { c.err("desc gate: surfaces must be same square 1024/1536/2048/2560/2736/3072"); return false; }
#elif defined(VRSIM_SHARED_2736)
    if (!eL.sizeOk || !eR.sizeOk || !sameSquare) { c.err("desc gate: surfaces must be same square 1024/1536/2048/2560/2736"); return false; }
#elif defined(VRSIM_SHARED_2560)
    if (!eL.sizeOk || !eR.sizeOk || !sameSquare) { c.err("desc gate: surfaces must be same square 1024/1536/2048/2560"); return false; }
#else
    if (!eL.sizeOk || !eR.sizeOk || !sameSquare) { c.err("desc gate: surfaces must be same square 1024/1536/2048"); return false; }
#endif
    if (!eL.fmtOk || !eR.fmtOk) { c.err("desc gate: Format is not 22 (X8R8G8B8)"); return false; }
    if (!eL.typeOk || !eR.typeOk) { c.err("desc gate: Type is not D3DRTYPE_SURFACE"); return false; }
    if (!eL.usageRt || !eR.usageRt) { c.err("desc gate: Usage lacks D3DUSAGE_RENDERTARGET"); return false; }
    if (!eL.poolDef || !eR.poolDef) { c.err("desc gate: Pool is not D3DPOOL_DEFAULT"); return false; }
    if (!eL.msaaNone || !eR.msaaNone) { c.err("desc gate: MultiSampleType is not NONE"); return false; }
    eL.w = eL.d.Width; eL.h = eL.d.Height;
    eR.w = eR.d.Width; eR.h = eR.d.Height;

    // Devices: both GetDevice results must be ONE canonical device.
    IDirect3DDevice9* dL = nullptr;
    IDirect3DDevice9* dR = nullptr;
    h = eL.surf->GetDevice(&dL);
    if (FAILED(h)) { c.hr("left GetDevice", h); return false; }
    h = eR.surf->GetDevice(&dR);
    if (FAILED(h)) { c.hr("right GetDevice", h); if (dL) dL->Release(); return false; }
    IUnknown* ul = nullptr;
    IUnknown* ur = nullptr;
    const bool qL = SUCCEEDED(dL->QueryInterface(IID_IUnknown, (void**)&ul)) && ul;
    const bool qR = SUCCEEDED(dR->QueryInterface(IID_IUnknown, (void**)&ur)) && ur;
    sameDevice = qL && qR && ul == ur;
    if (ul) ul->Release();
    if (ur) ur->Release();
    if (!sameDevice) { c.err("left/right GetDevice canonical identity differs; pair refused"); if (dL) dL->Release(); if (dR) dR->Release(); return false; }
    dR->Release(); // our own second ref on the same device, balanced exactly once
    c.ord = dL;    // OUR owned ref taken via GetDevice; released once in g1::Cleanup

    // Actual creation parameters on the real source device.
    h = c.ord->GetCreationParameters(&c.cp);
    c.haveCp = SUCCEEDED(h);
    c.hr("IDirect3DDevice9::GetCreationParameters(actual source device)", h);
    if (!c.haveCp) return false;
    beh044 = c.cp.BehaviorFlags == g1::BEH_ORD; // actual must be 0x44
    halType = c.cp.DeviceType == D3DDEVTYPE_HAL;
    if (!beh044 || !halType) { c.err("actual source device is not 0x44/HAL; gate refused"); return false; }

    // Ordinary device must NOT QI as Ex (canonical identity of an ordinary device).
    IDirect3DDevice9Ex* exqi = nullptr;
    ordNotExQI = FAILED(c.ord->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&exqi));
    if (exqi) exqi->Release();
    if (!ordNotExQI) { c.err("actual source device QIs as Ex; expected ordinary 0x44 device"); return false; }

    // Known actual parent via public GetDirect3D (our own ref; Cleanup releases).
    h = c.ord->GetDirect3D(&c.d9);
    if (FAILED(h) || c.d9 == nullptr) { c.hr("IDirect3DDevice9::GetDirect3D(actual parent)", h); return false; }
    IUnknown* pu = nullptr;
    IUnknown* ku = nullptr;
    parentOk = SUCCEEDED(c.d9->QueryInterface(IID_IUnknown, (void**)&pu)) && pu &&
               SUCCEEDED(((IUnknown*)c.d9)->QueryInterface(IID_IUnknown, (void**)&ku)) && ku && pu == ku;
    if (pu) pu->Release();
    if (ku) ku->Release();
    if (!parentOk) { c.err("parent canonical IUnknown self-check failed"); return false; }
    return true;
}

// ---- Stage B: System32 loads, own hidden window + Ex probe on the ACTUAL
// source ordinal, LUID/identifier/DXGI 16-enum unique match, D11 on the
// matched adapter only (no default/WARP fallback), FL >= 11_0.
static bool InitProbeAndGpu(g1::Ctx& c)
{
    if (!g1::LoadDlls(c)) return false;

    c.hwnd = g1::MakeHiddenWindow();
    if (!c.hwnd) { c.err("own hidden 64x64 window creation failed"); return false; }
    g1::Show(c.hwnd);

    PFN_Direct3DCreate9Ex create9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(c.d3d9, "Direct3DCreate9Ex");
    if (!create9Ex) { c.err("Direct3DCreate9Ex export missing"); return false; }
    HRESULT h = create9Ex(D3D_SDK_VERSION, &c.d9ex);
    c.hr("Direct3DCreate9Ex(own probe parent)", h);
    if (FAILED(h) || !c.d9ex) return false;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = g1::FMT22;
    pp.BackBufferWidth = g1::W;
    pp.BackBufferHeight = g1::H;
    pp.hDeviceWindow = c.hwnd;
    h = c.d9ex->CreateDeviceEx(c.cp.AdapterOrdinal, D3DDEVTYPE_HAL, c.hwnd, g1::BEH_EX, &pp, nullptr, &c.dex);
    c.hr("IDirect3D9Ex::CreateDeviceEx(actual source ordinal, HAL, 0x46)", h);
    if (FAILED(h) || !c.dex) return false;
    c.ownExIsExQI = true;
    IDirect3DDevice9Ex* exqi = nullptr;
    if (FAILED(c.dex->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&exqi)) || !exqi) c.ownExIsExQI = false;
    if (exqi) exqi->Release();
    if (!c.ownExIsExQI) { c.err("own Ex probe does not QI as Ex"); return false; }

    h = c.d9ex->GetAdapterLUID(c.cp.AdapterOrdinal, &c.luid);
    c.hr("IDirect3D9Ex::GetAdapterLUID(actual ordinal)", h);
    if (FAILED(h)) return false;
    c.haveLuid = true;

    h = c.d9->GetAdapterIdentifier(c.cp.AdapterOrdinal, 0, &c.ai); // actual source parent, actual ordinal
    c.hr("IDirect3D9::GetAdapterIdentifier(actual ordinal on actual parent)", h);
    if (FAILED(h)) return false;

    h = c.pfnCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&c.factory);
    c.hr("CreateDXGIFactory1", h);
    if (FAILED(h)) return false;
    int matches = 0;
    for (UINT i = 0; i < 16; ++i) {
        IDXGIAdapter* a = nullptr;
        const HRESULT e = c.factory->EnumAdapters(i, &a);
        if (e == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(e)) { c.hr("EnumAdapters", e); break; }
        DXGI_ADAPTER_DESC d{};
        const HRESULT g = a->GetDesc(&d);
        const bool same = SUCCEEDED(g) && d.AdapterLuid.HighPart == c.luid.HighPart &&
                          d.AdapterLuid.LowPart == c.luid.LowPart;
        if (same) { ++matches; if (!c.adapter) { c.adapter = a; c.ad = d; } else a->Release(); if (matches > 1) break; }
        else a->Release();
    }
    c.sameAdapter = (matches == 1 && c.adapter != nullptr);
    if (!c.sameAdapter) { c.err("same-adapter LUID gate: not exactly one of <=16 DXGI adapters matches"); return false; }
    if (!(c.ai.VendorId == c.ad.VendorId && c.ai.DeviceId == c.ad.DeviceId &&
          c.ai.SubSysId == c.ad.SubSysId && c.ai.Revision == c.ad.Revision)) {
        c.err("Vendor/Device/SubSys/Revision mismatch between D3D9 identifier and matched DXGI adapter");
        return false;
    }

    D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    h = c.pfnD3D11CreateDevice(c.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               lvls, 3, D3D11_SDK_VERSION, &c.dev11, &c.fl, &c.ctx11);
    c.hr("D3D11CreateDevice(matched adapter, UNKNOWN driver, BGRA_SUPPORT)", h);
    if (FAILED(h) || !c.dev11 || !c.ctx11) return false;
    if (c.fl < D3D_FEATURE_LEVEL_11_0) { c.err("feature level below 11_0; CARD2A refuses to continue (no fallback)"); return false; }
    return true;
}

// ---- Stage C+D+E for one eye. Returns 0 pass, 1 clean fail, 2 quarantine.
static int RunEye(g1::Ctx& c, Eye& e)
{
    Tm t0;
    HRESULT h;

    // C: D11 shared texture at the actual eye size, format 88.
    D3D11_TEXTURE2D_DESC d{};
    d.Width = e.w; d.Height = e.h; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = g1::FMT88; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    h = c.dev11->CreateTexture2D(&d, nullptr, &e.shared11);
    c.hr("CreateTexture2D(shared 88, actual eye size)", h);
    if (FAILED(h)) return 1;
    e.shared11->GetDesc(&e.sharedActual);
    e.sharedDescOk = e.sharedActual.Width == e.w && e.sharedActual.Height == e.h &&
                     e.sharedActual.Format == g1::FMT88 && e.sharedActual.MipLevels == 1 &&
                     e.sharedActual.ArraySize == 1 && e.sharedActual.SampleDesc.Count == 1 &&
                     e.sharedActual.Usage == D3D11_USAGE_DEFAULT &&
                     (e.sharedActual.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) ==
                         (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
                     (e.sharedActual.MiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0;
    if (!e.sharedDescOk) { c.err("actual shared desc mismatch"); return 1; }
    IDXGIResource* xr = nullptr;
    h = e.shared11->QueryInterface(__uuidof(IDXGIResource), (void**)&xr);
    c.hr("ID3D11Texture2D::QueryInterface(IDXGIResource)", h);
    if (FAILED(h)) return 1;
    h = xr->GetSharedHandle(&e.legacy);
    xr->Release();
    c.hr("IDXGIResource::GetSharedHandle(legacy non-NT)", h);
    if (FAILED(h) || e.legacy == nullptr) { c.err("legacy shared handle invalid"); return 1; }
    // legacy handle: never CloseHandle, never DuplicateHandle anywhere.

    // Flag-scoped single CreateTexture opening the legacy handle on the
    // ACTUAL ordinary parent (address proven by g1::DiscoverLayout).
    FlagOut2 fo{};
    FlagOpenLeaf2(c.ord, (void*)(c.ordParent + c.isoff), e.w, e.h, &e.shared9, e.legacy, &fo);
    e.createRc = fo.createRc;
    e.createRcKnown = true;
    e.setTrueVerified = fo.setTrueReadback;
    e.flagWritableFalseProven = fo.originalReadOk && !fo.exceptionDuring;
    e.flagSeh = fo.exceptionDuring;
    BOOL now = TRUE;
    e.restoreVerified = !fo.exceptionDuring &&
                        ReadBytesSafe((void*)(c.ordParent + c.isoff), &now, sizeof(BOOL)) &&
                        now == FALSE && fo.original == FALSE;
    if (!e.restoreVerified) {
        c.unsafeRetained = true;
        c.err("flag restore unproven: no further GPU calls, refs retained, process quarantined");
        return 2;
    }
    if (FAILED(e.createRc)) { c.hr("IDirect3DDevice9::CreateTexture(shared open, flag scoped)", e.createRc); return 1; }
    if (e.shared9 == nullptr) { c.err("CreateTexture succeeded but shared9 null"); return 1; }
    h = e.shared9->GetSurfaceLevel(0, &e.sharedSurf9);
    c.hr("IDirect3DTexture9::GetSurfaceLevel(0)", h);
    if (FAILED(h) || !e.sharedSurf9) return 1;

    h = c.ord->CreateQuery(D3DQUERYTYPE_EVENT, &e.ev9);
    c.hr("CreateQuery(EVENT)", h);
    if (FAILED(h)) return 1;

    // D: StretchRect actual source -> shared9 level0, no ColorFill, no writes
    // to the real source, no Reset/Present/SetRenderTarget.
    {
        Tm ts;
        h = c.ord->StretchRect(e.surf, nullptr, e.sharedSurf9, nullptr, D3DTEXF_NONE);
        e.tStretch = TmMs(ts);
        c.hr("StretchRect(actual source -> shared9 level0, NONE)", h);
        if (FAILED(h)) return 1;
        e.stretched = true; // GPU submission point: drain must be proven or quarantine
        h = e.ev9->Issue(D3DISSUE_END);
        c.hr("Issue(D3DISSUE_END)", h);
        if (FAILED(h)) { c.err("D3D9 drain unproven after submission"); return 2; }
        if (!WaitEvent9NoPump(c, e, e.ev9)) { c.err("D3D9 event drain not proven (<=128 polls/1000ms)"); return 2; }
    }

    // D11 copy + drain.
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = e.w; sd.Height = e.h; sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1;
    sd.Format = g1::FMT88; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    h = c.dev11->CreateTexture2D(&sd, nullptr, &e.stage11);
    c.hr("CreateTexture2D(staging CPU_READ)", h);
    if (FAILED(h)) return 1; // nothing D11-side submitted yet; clean release is safe
    {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        h = c.dev11->CreateQuery(&qd, &e.ev11);
        c.hr("ID3D11Device::CreateQuery(EVENT)", h);
        if (FAILED(h)) return 1;
    }
    {
        Tm tc;
        c.ctx11->CopyResource(e.stage11, e.shared11);
        e.copyIssued = true;
        c.ctx11->Flush(); // exactly once
        e.flushDone = true;
        c.ctx11->End(e.ev11);
        e.endDone = true;
        e.tCopy = TmMs(tc);
        if (!WaitEvent11NoPump(c, e, e.ev11)) { c.err("D3D11 event drain not proven (S_OK+TRUE past budget)"); return 2; }
    }

    // E: QA-only CPU oracle on the same frozen actual source. No SurfaceRender
    // re-invocation, no writes to the caller source.
    Tm tq9;
    h = c.ord->CreateOffscreenPlainSurface(e.w, e.h, g1::FMT22, D3DPOOL_SYSTEMMEM, &e.plain, nullptr);
    c.hr("CreateOffscreenPlainSurface(QA oracle SYSTEMMEM)", h);
    if (FAILED(h)) return 1;
    h = c.ord->GetRenderTargetData(e.surf, e.plain);
    c.hr("GetRenderTargetData(actual source -> SYSTEMMEM)", h);
    if (FAILED(h)) { e.tQ9 = TmMs(tq9); return 1; }
    D3DLOCKED_RECT lr{};
    h = e.plain->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    e.tQ9 = TmMs(tq9);
    c.hr("LockRect(READONLY)", h);
    if (FAILED(h)) return 1;
    e.lockOk = true;
    UnlockRectG ug{ &e, e.plain }; // exactly once on every exit path

    D3D11_MAPPED_SUBRESOURCE m{};
    Tm tq11;
    {
        h = c.ctx11->Map(e.stage11, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        bool mapped = (h == S_OK);
        int polls = 0;
        LARGE_INTEGER f, n0;
        const bool timerOk = g1::TimerStart(f, n0);
        while (!mapped) {
            if (h == DXGI_ERROR_WAS_STILL_DRAWING) {
                e.mapStillDrawing = true; // the only legal pending code
            } else {
                c.hr("ID3D11DeviceContext::Map(READ|DO_NOT_WAIT)", h); // any other HRESULT: fail now
                return 1;
            }
            DWORD el = 0;
            if (!timerOk || !g1::TimerElapsedMs(f, n0, el)) { c.err("QPC failed (Map wait)"); return 1; }
            if (polls >= g1::MAX_POLLS || el >= g1::DEADLINE_MS) { c.err("Map still drawing past 128 polls/1000ms"); return 1; }
            ++polls;
            Sleep(1);
            h = c.ctx11->Map(e.stage11, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            mapped = (h == S_OK);
        }
    }
    e.mapOk = true; // Map eventually succeeded; the guard below unmaps exactly once
    UnmapG mg{ c.ctx11, e.stage11 }; // exactly once per successful Map
    e.tQ11 = TmMs(tq11);

    // pointer/pitch validation on both, positive, overflow-bounded extents
    const uint8_t* sp = (const uint8_t*)lr.pBits; // D3DLOCKED_RECT member is pBits, not pData
    const uint8_t* dp = (const uint8_t*)m.pData;  // D3D11_MAPPED_SUBRESOURCE keeps pData
    e.srcPitch = (UINT)lr.Pitch;
    e.shPitch = m.RowPitch;
    bool valid = sp != nullptr && dp != nullptr && lr.Pitch > 0 && m.RowPitch > 0 &&
                 (UINT)lr.Pitch >= e.w * 4u && m.RowPitch >= e.w * 4u;
    const size_t lastExtent = (size_t)(e.h - 1u) * (size_t)e.shPitch + (size_t)e.w * 4u;
    if (!valid || lastExtent < (size_t)e.w * 4u ||
        (size_t)(e.h - 1u) * (size_t)e.srcPitch + (size_t)e.w * 4u < (size_t)e.w * 4u) {
        c.err("oracle pData/pitch/extent validation failed");
        return 1;
    }

    long long mism = 0;
    long long smin = 1LL << 31, smax = -1, xmin = 1LL << 31, xmax = -1;
    for (UINT y = 0; y < e.h; ++y) {
        const uint8_t* srow = sp + (size_t)y * (size_t)e.srcPitch;
        const uint8_t* drow = dp + (size_t)y * (size_t)e.shPitch;
        for (UINT x = 0; x < e.w; ++x) {
            const uint8_t* s = srow + (size_t)x * 4u;
            const uint8_t* d = drow + (size_t)x * 4u;
            if (s[0] != d[0] || s[1] != d[1] || s[2] != d[2]) ++mism; // B,G,R exactly; 4th byte is X
            if ((long long)s[3] < smin) smin = s[3];
            if ((long long)s[3] > smax) smax = s[3];
            if ((long long)d[3] < xmin) xmin = d[3];
            if ((long long)d[3] > xmax) xmax = d[3];
        }
    }
    e.mismatch = mism;
    e.srcMinX = smin; e.srcMaxX = smax; e.shMinX = xmin; e.shMaxX = xmax;

    uint8_t dig[32];
    e.shaSrcOk = Sha256RgbPod(sp, e.srcPitch, e.w, e.h, dig);
    if (e.shaSrcOk) e.shaSrc = HexBytes(dig, 32);
    e.shaShOk = Sha256RgbPod(dp, e.shPitch, e.w, e.h, dig);
    if (e.shaShOk) e.shaSh = HexBytes(dig, 32);
    if (!e.shaSrcOk) c.err("BCrypt SHA256 (source) failed");
    if (!e.shaShOk) c.err("BCrypt SHA256 (shared) failed");
    e.oracleOk = e.shaSrcOk && e.shaShOk;
    e.tBoot = TmMs(t0);
    if (!e.oracleOk) return 1;
    if (mism != 0) { c.err("per-pixel BGR mismatch between actual source readback and shared readback"); return 1; }
    return 0;
}

// ---- JSON ------------------------------------------------------------------
// legacyNum/shared9Opened are pre-release evidence snapshots (pure historical
// numbers/bools captured while the objects were live); Emit never re-derives
// identity or success from pointers already nulled by cleanup.
static void EmitEye(JsonObj& j, const char* p, const Eye& e, uintptr_t legacyNum, bool shared9Opened)
{
    const std::string N(p);
    j.boolean(N + "DescCaptured", e.haveDesc);
    j.num(N + "Width", (long long)e.d.Width);
    j.num(N + "Height", (long long)e.d.Height);
    j.num(N + "Format", (long long)(uint32_t)e.d.Format);
    j.num(N + "Type", (long long)e.d.Type);
    j.num(N + "Usage", (long long)e.d.Usage);
    j.num(N + "Pool", (long long)e.d.Pool);
    j.num(N + "MultiSampleType", (long long)e.d.MultiSampleType);
    j.boolean(N + "SquareAllowedSize", e.sizeOk);
    j.boolean(N + "SharedDescMatches", e.sharedDescOk);
    j.nullableStr(N + "LegacySharedHandleNumber", legacyNum != 0,
                  legacyNum != 0 ? HexPtr(legacyNum) : std::string());
    j.boolean(N + "FlagWritableFalseProven", e.flagWritableFalseProven);
    j.boolean(N + "FlagSetTrueReadback", e.setTrueVerified);
    j.boolean(N + "FlagSeh", e.flagSeh);
    j.boolean(N + "FlagRestoredFalseVerified", e.restoreVerified);
    j.nullableStr(N + "CreateTextureRc", e.createRcKnown,
                  e.createRcKnown ? HexHresult(e.createRc) : std::string());
    j.boolean(N + "Shared9SurfaceOpened", shared9Opened);
    j.boolean(N + "StretchSubmitted", e.stretched);
    j.boolean(N + "D9DrainProven", e.d9Ready);
    j.nullableNum(N + "D9Polls", e.d9Polls >= 0, e.d9Polls);
    j.boolean(N + "D11CopyFlushEndIssued", e.copyIssued && e.flushDone && e.endDone);
    j.boolean(N + "D11DrainProvenSoKTrue", e.d11Ready);
    j.nullableNum(N + "D11Polls", e.d11Polls >= 0, e.d11Polls);
    j.boolean(N + "OracleLockRectOk", e.lockOk);
    j.boolean(N + "OracleUnlockRectOk", e.unlockOk);
    j.boolean(N + "OracleMapOk", e.mapOk);
    j.boolean(N + "MapPendingWAS_STILL_DRAWINGonly", e.mapStillDrawing);
    j.nullableNum(N + "SourceRowPitch", e.lockOk, (long long)e.srcPitch);
    j.nullableNum(N + "SharedRowPitch", e.oracleOk, (long long)e.shPitch);
    j.nullableNum(N + "RgbMismatchCount", e.mismatch >= 0, e.mismatch);
    j.nullableNum(N + "SourceXMin", e.mismatch >= 0, e.srcMinX);
    j.nullableNum(N + "SourceXMax", e.mismatch >= 0, e.srcMaxX);
    j.nullableNum(N + "SharedXMin", e.mismatch >= 0, e.shMinX);
    j.nullableNum(N + "SharedXMax", e.mismatch >= 0, e.shMaxX);
    j.nullableStr(N + "Sha256SourceRgb", e.shaSrcOk, e.shaSrc);
    j.nullableStr(N + "Sha256SharedRgb", e.shaShOk, e.shaSh);
    j.nullableNum(N + "BootstrapCpuMs", e.tBoot >= 0, e.tBoot);
    j.nullableNum(N + "StretchSubmitCpuMs", e.tStretch >= 0, e.tStretch);
    j.nullableNum(N + "D9WaitMs", e.tD9Wait >= 0, e.tD9Wait);
    j.nullableNum(N + "D11CopyCpuMs", e.tCopy >= 0, e.tCopy);
    j.nullableNum(N + "D11WaitMs", e.tD11Wait >= 0, e.tD11Wait);
    j.nullableNum(N + "Qa9ReadbackMs", e.tQ9 >= 0, e.tQ9);
    j.nullableNum(N + "Qa11ReadbackMs", e.tQ11 >= 0, e.tQ11);
}

// ---- full run --------------------------------------------------------------
// c/eL/eR live in the same function scope as the try/catch so the catch body
// can honestly refuse every Release: after a C++ throw the flag/GPU/lock state
// is unknown, so all own refs are retained (sticky quarantine) until the
// root-owned process exits. The FpuPreserve object itself lives in the export
// scope; its own destructor is only the unwind fallback.
static int Run(uint64_t lp, uint64_t rp, uint64_t pairId, FpuPreserve& fpu,
               std::string& out, bool& exceptionThrown)
{
    exceptionThrown = false;
    g1::Ctx c;
    c.releasedAll = false;
    Eye eL("left"), eR("right");

    bool surfacesDistinct = false, sameDevice = false, beh044 = false, halType = false,
         parentOk = false, ordNotExQI = false;
    bool layoutDone = false, eyesL = false, eyesR = false;
    bool quarantine = false, cleanupPass = false, fpuOk = false, stereoDistinct = false;
    bool srcOk = false, probeOk = false;

    // Pre-release evidence snapshot: pure historical numbers/bools recorded
    // while the objects were still live. Emit reads ONLY these afterwards.
    struct EvSnap {
        uintptr_t ownLSurf = 0, ownRSurf = 0, device = 0, parent = 0;
        uintptr_t legacyL = 0, legacyR = 0;
        bool shared9OpenedL = false, shared9OpenedR = false;
        bool dev11Created = false;
    } ev;

    try {
        srcOk = AcquireSources(c, lp, rp, eL, eR, surfacesDistinct, sameDevice, beh044, halType, parentOk, ordNotExQI);
        if (srcOk) probeOk = InitProbeAndGpu(c);
        srcOk = srcOk && probeOk;
        if (srcOk) {
            g1::DiscoverLayout(c); // re-prove CRR125 whole-221 scan on actual device+parent and own Ex probe
            layoutDone = c.layoutProved;
            if (!layoutDone) c.err("layout discovery failed on this actual device/parent; missing refuses, no historical offsets");
        }
        if (srcOk && layoutDone) {
            // FullPass()/unlockOk are read AFTER RunEye returns, so the
            // UnlockRectG/UnmapG destructors have already run for real.
            const int r1 = RunEye(c, eL);
            eyesL = (r1 == 0) && eL.FullPass();
            if (eL.lockOk && !eL.unlockOk) {
                // successful LockRect whose UnlockRect failed: our own surface
                // ref may still be locked; retain, stop, never fake cleanup.
                c.err("UnlockRect failed after successful LockRect (left); own refs retained, right eye stopped");
                quarantine = true;
                eyesL = false;
            } else if (r1 == 2) {
                quarantine = true;
            } else if (r1 == 0 && !eyesL) {
                c.err("left eye post-return full-pass recheck failed");
            }
            if (!quarantine && eyesL) {
                const int r2 = RunEye(c, eR);
                eyesR = (r2 == 0) && eR.FullPass();
                if (eR.lockOk && !eR.unlockOk) {
                    c.err("UnlockRect failed after successful LockRect (right); own refs retained");
                    quarantine = true;
                    eyesR = false;
                } else if (r2 == 2) {
                    quarantine = true;
                } else if (r2 == 0 && !eyesR) {
                    c.err("right eye post-return full-pass recheck failed");
                }
            }
        }
        if (eyesL && eyesR) {
            stereoDistinct = eL.shaSrc != eR.shaSrc && eL.shaSh != eR.shaSh &&
                             eL.shaSrcOk && eR.shaSrcOk && eL.shaShOk && eR.shaShOk;
            if (!stereoDistinct) c.err("left/right content hashes identical; stereoContentDistinct=false");
        }

        // Capture numeric/bool evidence BEFORE any Release so later Emit never
        // infers stale identity or success from zeroed pointers.
        ev.ownLSurf = (uintptr_t)eL.surf;
        ev.ownRSurf = (uintptr_t)eR.surf;
        ev.device = (uintptr_t)c.ord;
        ev.parent = (uintptr_t)c.d9;
        ev.legacyL = (uintptr_t)eL.legacy;
        ev.legacyR = (uintptr_t)eR.legacy;
        ev.shared9OpenedL = (eL.sharedSurf9 != nullptr);
        ev.shared9OpenedR = (eR.sharedSurf9 != nullptr);
        ev.dev11Created = (c.dev11 != nullptr) && (c.ctx11 != nullptr);

        if (quarantine || c.unsafeRetained) {
            InterlockedCompareExchange(&g_quarantined, 1, 0);
            c.unsafeRetained = true;
            // Retain every possibly-in-use COM/module/window reference until process
            // exit. No Release, no DestroyWindow, no FreeLibrary on this path.
            cleanupPass = false;
        } else {
            ReleaseEye(eL);
            ReleaseEye(eR);
            g1::Cleanup(c);
            cleanupPass = c.releasedAll && eL.released && eR.released &&
                          eL.surf == nullptr && eR.surf == nullptr;
        }

        // Real FPU restore + verification BEFORE serialization so the JSON
        // records actual results; the export performs the final post-
        // publication restore so formatting cannot leave the state altered.
        fpuOk = fpu.RestoreAndVerify();
        if (!fpuOk) {
            c.err("FPU fenv/MXCSR restore or post-read verification failed (pre-serialization)");
            InterlockedCompareExchange(&g_quarantined, 1, 0); // restore failure: sticky quarantine
        }

        bool gatePass = srcOk && layoutDone && surfacesDistinct && sameDevice && beh044 && halType &&
                        parentOk && ordNotExQI && c.sameAdapter && c.haveLuid &&
                        c.fl >= D3D_FEATURE_LEVEL_11_0 && ev.dev11Created &&
                        eyesL && eyesR && stereoDistinct && !quarantine && !c.unsafeRetained && cleanupPass &&
                        c.legacyCloseCalls == 0 && fpuOk && fpu.Captured() && c.errors.empty();

        JsonObj j;
        j.num("schemaVersion", 2);
        j.str("scope", "ACTUAL_SIM_SURFACE_SHARED_PARITY_ONLY_NOT_XR_NOT_FPS");
        j.num("selfPid", (long long)GetCurrentProcessId());
        j.num("pairId", (long long)pairId);
        j.boolean("singleCallGuardAcquired", true);
        j.boolean("priorProcessQuarantine", InterlockedCompareExchange(&g_quarantined, 0, 0) != 0);
        // provenance (snapshot numbers; only meaningful within this process/call)
        j.str("callerLeftSurface", HexPtr((uintptr_t)lp));
        j.str("callerRightSurface", HexPtr((uintptr_t)rp));
        j.str("ownLeftSurfaceRef", HexPtr(ev.ownLSurf));
        j.str("ownRightSurfaceRef", HexPtr(ev.ownRSurf));
        j.boolean("surfacesCanonicalDistinct", surfacesDistinct);
        j.boolean("sameDeviceCanonical", sameDevice);
        j.str("actualDevice", HexPtr(ev.device));
        j.str("actualParent", HexPtr(ev.parent));
        j.boolean("actualParentCanonicalOk", parentOk);
        j.boolean("actualOrdinaryNotExQI", ordNotExQI);
        j.boolean("sourceCreationParamsCaptured", c.haveCp);
        j.num("actualBehaviorFlags", c.haveCp ? (long long)c.cp.BehaviorFlags : -1);
        j.boolean("actualBehaviorIs0x44", beh044);
        j.num("actualDeviceType", c.haveCp ? (long long)c.cp.DeviceType : -1);
        j.boolean("actualDeviceIsHAL", halType);
        j.num("actualAdapterOrdinal", c.haveCp ? (long long)c.cp.AdapterOrdinal : -1);
        j.boolean("sourceDescGatesPass", srcOk);
        // same-GPU / adapter
        j.str("luid", c.haveLuid ? std::to_string(c.luid.HighPart) + ":" + std::to_string(c.luid.LowPart) : "null");
        j.num("d3d9VendorId", (long long)c.ai.VendorId);
        j.num("d3d9DeviceId", (long long)c.ai.DeviceId);
        j.num("d3d9SubSysId", (long long)c.ai.SubSysId);
        j.num("d3d9Revision", (long long)c.ai.Revision);
        j.num("dxgiVendorId", (long long)c.ad.VendorId);
        j.num("dxgiDeviceId", (long long)c.ad.DeviceId);
        j.num("dxgiSubsystemId", (long long)c.ad.SubSysId);
        j.num("dxgiRevision", (long long)c.ad.Revision);
        j.boolean("uniqueLuidMatchIn16", c.sameAdapter);
        j.boolean("adapterIdsMatch", c.sameAdapter && c.ai.VendorId == c.ad.VendorId &&
                                     c.ai.DeviceId == c.ad.DeviceId && c.ai.SubSysId == c.ad.SubSysId &&
                                     c.ai.Revision == c.ad.Revision);
        j.num("d3d11FeatureLevel", (long long)c.fl);
        j.boolean("d11OnMatchedAdapterNoFallback", ev.dev11Created && c.fl >= D3D_FEATURE_LEVEL_11_0);
        // layout
        j.boolean("layoutThisProcessProved", c.layoutProved);
        j.nullableNum("d3d9SigMatches", c.sigMatches >= 0, c.sigMatches);
        j.nullableStr("d3d9CandidatePairs", !c.candStr.empty(), c.candStr);
        j.nullableNum("clsoff", c.layoutProved, (long long)c.clsoff);
        j.nullableNum("isoff", c.layoutProved, (long long)c.isoff);
        j.boolean("knownParentsAreThisRunsObjects", c.layoutProved);
        // per eye (snapshot-backed identity fields)
        EmitEye(j, "left", eL, ev.legacyL, ev.shared9OpenedL);
        EmitEye(j, "right", eR, ev.legacyR, ev.shared9OpenedR);
        j.boolean("pairParityThisStaticFrame", eyesL && eyesR);
        j.boolean("stereoContentDistinct", stereoDistinct);
        // cleanup / quarantine
        j.boolean("cleanupPass", cleanupPass);
        // real process state: sticky quarantine (incl. the pre-serialization
        // FPU failure set above) must show true here, never a fabricated false
        j.boolean("quarantine", quarantine || c.unsafeRetained ||
                                InterlockedCompareExchange(&g_quarantined, 0, 0) != 0);
        j.boolean("unsafeRetained", c.unsafeRetained);
        j.num("legacyHandleCloseCalls", (long long)c.legacyCloseCalls);
        // FPU evidence: every value below is the real recorded rc/result
        j.boolean("fpuCaptured", fpu.Captured());
        j.num("fpuCaptureRc", fpu.CaptureRc());
        j.boolean("fpuRestoreAttempted", fpu.RestoreAttempted());
        j.nullableNum("fpuRestoreRc", fpu.RestoreAttempted(), fpu.RestoreRc());
        j.boolean("fpuMxcsrRestoreVerified", fpu.MxcsrVerified());
        j.num("fpuSavedMxcsr", (long long)fpu.SavedMxcsr());
        j.nullableNum("fpuSavedCtl", fpu.Captured(), (long long)fpu.SavedCtl());
        j.nullableNum("fpuSavedStat", fpu.Captured(), (long long)fpu.SavedStat());
        j.num("fpuPostReadRc", fpu.PostReadRc());
        j.boolean("fpuPostIdentical", fpu.PostIdentical());
        j.boolean("fpuStateRestoredVerified", fpuOk);
        // claims
        j.boolean("qaReadbackOnly", true);
        j.num("encodedFrames", 0);
        j.num("xrSubmittedFrames", 0);
        j.boolean("fpsUnknown", true);
        j.str("productXrSupport", "UNPROVEN");
        j.boolean("gatePass", gatePass);
        j.str("limits", "ACTUAL SIM surface pair only; this call proves one STATIC pair's pixel parity; freshness "
                        "needs two managed calls on two distinct frames; native cannot validate arbitrary numeric "
                        "pointers (managed side must own real D3DImage refs inside the UI transaction double lock); "
                        "QPC segments are CPU-side costs (compare/hash time is after Qa11ReadbackMs), NOT GPU "
                        "execution time; 128-poll/1000ms waits are finite budgets, not driver deadlines; 4th byte "
                        "of format 22/88 is X, not alpha; QA source readback is not a production per-frame path; "
                        "no XR, no encode, no FPS claim");
        j.add("errors", JsonObj::arrayOfStrings(c.errors));
        out = "{" + j.body + "}";
        return gatePass ? 0 : 1;
    } catch (...) {
        exceptionThrown = true;
        InterlockedCompareExchange(&g_quarantined, 1, 0);
        // Unknown flag/GPU/lock state after a throw: NO Release, NO
        // DestroyWindow, NO FreeLibrary attempted here; own refs are retained
        // until process exit and later calls are refused. The export publishes
        // the fixed literal JSON (no allocation in this path).
        out.clear(); // clear() cannot throw; content is ignored when exceptionThrown
        return 1;
    }
}

} // namespace g2

// ---- exported ABI -----------------------------------------------------------
// FpuPreserve lives at this scope: Capture() runs first and its REAL success
// gates entry (capture unproven => never reach Run/AcquireSources). The final
// explicit restore+verify runs AFTER serialization/publication and its REAL
// result is judged: failure overwrites whatever was published (including a
// gatePass:true document), forces sticky quarantine and non-zero rc. All
// failure documents are fixed literals written with sizeof-1 + memcpy + NUL;
// no allocation, no formatting, no _control87/_MCW_PC/_MCW_IC anywhere.
// InCallGuard releases the single-call CAS on every return and unwind path.
#if !defined(VRSIM_PRODUCTION_SHARED_HELPERS)
extern "C" __declspec(dllexport) int ObsSourcePairGate2V1(uint64_t leftSurface, uint64_t rightSurface,
                                                           uint64_t pairId, char* resultUtf8,
                                                           uint32_t capacity)
{
    // Fixed FPU failure document: never rebuilt, never allocated.
    static const char kFpuFailJson[] =
        "{\"schemaVersion\":2,"
        "\"scope\":\"ACTUAL_SIM_SURFACE_SHARED_PARITY_ONLY_NOT_XR_NOT_FPS\","
        "\"gatePass\":false,\"quarantine\":true,\"cleanupPass\":false,"
        "\"reason\":\"FPU capture or final restore unproven; process quarantined until exit\","
        "\"errors\":[\"FPU capture or final restore unproven\"]}";

    if (resultUtf8 == nullptr || capacity < 4096u || capacity > 65536u || pairId == 0)
        return 2; // invalid ABI parameters; buffer untouched
    if (InterlockedCompareExchange(&g2::g_inCall, 1, 0) != 0)
        return 3; // another call is in flight on this process
    g2::InCallGuard inCallGuard; // no-allocation RAII; destructor is noexcept

    g2::FpuPreserve fpu;
    fpu.Capture(); // actual fegetenv rc + MXCSR read before any D3D/C++ work
    if (!fpu.Captured()) {
        // Capture unproven: close the process to further calls, publish the
        // fixed literal, return non-zero; no SDK, no COM, no Run body.
        InterlockedExchange(&g2::g_quarantined, 1);
        const size_t n = sizeof(kFpuFailJson) - 1;
        if (n + 1u <= (size_t)capacity) {
            memcpy(resultUtf8, kFpuFailJson, n);
            resultUtf8[n] = '\0';
        }
        return 1;
    }

    int rc = 1;
    try {
        if (InterlockedCompareExchange(&g2::g_quarantined, 0, 0) != 0) {
            // Process quarantine: flag restore, GPU drain, unlock or exception
            // state was unproven earlier; refuse, refs retained.
            const size_t n = sizeof(g2::kQuarantineJson) - 1;
            if (n + 1u <= (size_t)capacity) {
                memcpy(resultUtf8, g2::kQuarantineJson, n);
                resultUtf8[n] = '\0';
            }
            rc = 4;
        } else {
            bool exceptionThrown = false;
            std::string json;
            rc = g2::Run(leftSurface, rightSurface, pairId, fpu, json, exceptionThrown);
            if (exceptionThrown) {
                // escaped C++ exception inside the run body: publish ONLY the
                // fixed literal; never trust the partially built std::string
                rc = 1; // non-zero, no fabricated full PASS
                const size_t n = sizeof(g2::kExceptionJson) - 1;
                if (n + 1u <= (size_t)capacity) {
                    memcpy(resultUtf8, g2::kExceptionJson, n);
                    resultUtf8[n] = '\0';
                }
            } else {
                const bool fits = json.size() + 1u <= (size_t)capacity;
                if (fits) {
                    memcpy(resultUtf8, json.data(), json.size());
                    resultUtf8[json.size()] = '\0';
                }
                if (rc == 0 && !fits) rc = 1; // gate passed but publication incomplete: never claim 0
            }
        }
    } catch (...) {
        // absolute C ABI backstop: no std::string, no allocation, no format
        const size_t n = sizeof(g2::kExceptionJson) - 1;
        if (n + 1u <= (size_t)capacity) {
            memcpy(resultUtf8, g2::kExceptionJson, n);
            resultUtf8[n] = '\0';
        }
        InterlockedCompareExchange(&g2::g_quarantined, 1, 0);
        rc = 1;
    }
    // Borrowed caller sources are not stored anywhere across this boundary.
    // Final restore AFTER serialization/publication on EVERY path above;
    // its real return value decides, never ignored, no read-only CAS echo.
    if (!fpu.RestoreAndVerify()) {
        InterlockedExchange(&g2::g_quarantined, 1); // sticky quarantine on unproven restore
        const size_t n = sizeof(kFpuFailJson) - 1;  // overwrite anything published, incl. gatePass:true
        if (n + 1u <= (size_t)capacity) {
            memcpy(resultUtf8, kFpuFailJson, n);
            resultUtf8[n] = '\0';
        }
        rc = 1; // restore failure is always non-zero
    }
    return rc;
}

#endif // !VRSIM_PRODUCTION_SHARED_HELPERS
