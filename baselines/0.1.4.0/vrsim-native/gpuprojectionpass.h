// GpuProjectionPass.h - Bounded GPU asymmetric-FOV reprojection helper for VRSIM.
//
// Projects actual SIM raw-captured BGRA images onto the same-pose OpenXR
// asymmetric frusta. Not a renderer: the D3D11 device/context are supplied by
// the real XR worker and every Initialize/RenderPair/RenderSharedPair call
// must come from the thread that owns Initialize. No XR frame loop, no own
// thread, no locks.
//
// Shader bytecode is compile-time embedded: the .cpp includes the fxc-generated
// headers GpuProjectionVS.inc / GpuProjectionPS.inc from GpuProjection.hlsl.
// No runtime shader compiler is used or shipped.
//
// Build: x64, C++17, NOMINMAX, links against the DLL's existing d3d11 usage.

#ifndef VRSIM_GPU_PROJECTION_PASS_H
#define VRSIM_GPU_PROJECTION_PASS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace VRSIM {

// Wire contract, shared with the managed side (StructLayout Sequential Pack=8).
// version 1 with structBytes == 56 is the only accepted plan.
struct ProjectionPlanV1 {
    uint32_t structBytes;
    uint32_t version;
    double   tanL;
    double   tanR;
    double   tanU;
    double   tanD;
    double   tx;
    double   ty;
};

static_assert(sizeof(ProjectionPlanV1) == 56, "ProjectionPlanV1 must be 56 bytes");
static_assert(offsetof(ProjectionPlanV1, structBytes) == 0, "structBytes at offset 0");
static_assert(offsetof(ProjectionPlanV1, version) == 4, "version at offset 4");
static_assert(offsetof(ProjectionPlanV1, tanL) == 8, "tanL at offset 8");
static_assert(offsetof(ProjectionPlanV1, tanR) == 16, "tanR at offset 16");
static_assert(offsetof(ProjectionPlanV1, tanU) == 24, "tanU at offset 24");
static_assert(offsetof(ProjectionPlanV1, tanD) == 32, "tanD at offset 32");
static_assert(offsetof(ProjectionPlanV1, tx) == 40, "tx at offset 40");
static_assert(offsetof(ProjectionPlanV1, ty) == 48, "ty at offset 48");

constexpr uint32_t kProjectionPlanV1Bytes = 56;
constexpr uint32_t kProjectionPlanV1Version = 1;

// ---- HIGHRES-H2-GPU: additive wire stats (v1), shared with the managed side
// (StructLayout Sequential Pack=8). Pure additive snapshot: the existing helper
// interface (Initialize/RenderPair/Output/Reset) is unchanged. This POD is
// produced/consumed by the DLL export VRSIM_GetGpuPipelineStatsV1 only; the
// helper never reads or writes it. FINAL wire size is 120 bytes: existing field
// offsets are unchanged; sessionEpoch (offset 112) is appended after
// lastGpuDispatchQpcTicks (offset 104). ----
struct GpuPipelineStatsV1 {
    uint32_t structBytes;
    uint32_t version;
    uint64_t rawAccepted;
    uint64_t rawRejected;
    uint64_t gpuPairCommitted;
    uint64_t gpuPairFailed;
    uint64_t exactPoseRejected;
    uint64_t uniqueLiveLayerOk;
    uint64_t lastSuccessfulLayerFrameSerial;
    uint64_t lastSuccessfulLayerPoseSerial;
    uint64_t activeFrameSerial;
    uint64_t activePoseSerial;
    uint32_t activeSource;         // 0=none 1=CPUlive 2=GPUlive 3=staticpreview
    uint32_t activeWidth;
    uint32_t activeHeight;
    uint32_t reserved;             // bit0 = raw admission open; other bits must be 0
    uint64_t lastGpuDispatchQpcTicks; // CPU enqueue (Stopwatch/QPC) duration, NOT GPU timestamp
    uint64_t sessionEpoch;         // ring admission epoch at snapshot time (0 before Create)
};

static_assert(sizeof(GpuPipelineStatsV1) == 120, "GpuPipelineStatsV1 must be 120 bytes");
static_assert(offsetof(GpuPipelineStatsV1, structBytes) == 0, "structBytes at offset 0");
static_assert(offsetof(GpuPipelineStatsV1, version) == 4, "version at offset 4");
static_assert(offsetof(GpuPipelineStatsV1, rawAccepted) == 8, "rawAccepted at offset 8");
static_assert(offsetof(GpuPipelineStatsV1, rawRejected) == 16, "rawRejected at offset 16");
static_assert(offsetof(GpuPipelineStatsV1, gpuPairCommitted) == 24, "gpuPairCommitted at offset 24");
static_assert(offsetof(GpuPipelineStatsV1, gpuPairFailed) == 32, "gpuPairFailed at offset 32");
static_assert(offsetof(GpuPipelineStatsV1, exactPoseRejected) == 40, "exactPoseRejected at offset 40");
static_assert(offsetof(GpuPipelineStatsV1, uniqueLiveLayerOk) == 48, "uniqueLiveLayerOk at offset 48");
static_assert(offsetof(GpuPipelineStatsV1, lastSuccessfulLayerFrameSerial) == 56, "lastSuccessfulLayerFrameSerial at offset 56");
static_assert(offsetof(GpuPipelineStatsV1, lastSuccessfulLayerPoseSerial) == 64, "lastSuccessfulLayerPoseSerial at offset 64");
static_assert(offsetof(GpuPipelineStatsV1, activeFrameSerial) == 72, "activeFrameSerial at offset 72");
static_assert(offsetof(GpuPipelineStatsV1, activePoseSerial) == 80, "activePoseSerial at offset 80");
static_assert(offsetof(GpuPipelineStatsV1, activeSource) == 88, "activeSource at offset 88");
static_assert(offsetof(GpuPipelineStatsV1, activeWidth) == 92, "activeWidth at offset 92");
static_assert(offsetof(GpuPipelineStatsV1, activeHeight) == 96, "activeHeight at offset 96");
static_assert(offsetof(GpuPipelineStatsV1, reserved) == 100, "reserved at offset 100");
static_assert(offsetof(GpuPipelineStatsV1, lastGpuDispatchQpcTicks) == 104, "lastGpuDispatchQpcTicks at offset 104");
static_assert(offsetof(GpuPipelineStatsV1, sessionEpoch) == 112, "sessionEpoch at offset 112");

constexpr uint32_t kGpuPipelineStatsV1Bytes = 120;
constexpr uint32_t kGpuPipelineStatsV1Version = 1;

// Active source identity codes (wire contract).
constexpr uint32_t kGpuActiveSourceNone = 0;
constexpr uint32_t kGpuActiveSourceCpuLive = 1;
constexpr uint32_t kGpuActiveSourceGpuLive = 2;
constexpr uint32_t kGpuActiveSourceStaticPreview = 3;

// slot modes carried on a ring slot's payload: CPU reprojection vs raw-GPU.
constexpr uint32_t kGpuSlotModeCpu = 0;
constexpr uint32_t kGpuSlotModeRawGpu = 1;

// Double-buffered pair: each RenderPair/RenderSharedPair writes the inactive
// pair of eye outputs, and only commits (switches active) when both eyes'
// commands were enqueued with no device-removed error. Output() borrows the
// active textures.
class GpuProjectionPass {
public:
    GpuProjectionPass();
    ~GpuProjectionPass();

    GpuProjectionPass(const GpuProjectionPass&) = delete;
    GpuProjectionPass& operator=(const GpuProjectionPass&) = delete;

    // Records the calling thread as the owning XR-worker thread and AddRefs the
    // device/context. width/height are the square-profile source and output
    // dimensions (2..2048). planeFormat selects the session color contract:
    // DXGI_FORMAT_B8G8R8A8_UNORM (87, default) or
    // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB (91); anything else is E_INVALIDARG.
    // RTV + shader-sample support are verified for the SELECTED format before
    // any resource is created. Re-Initialize from the same owning thread
    // rebuilds at a new size or a new plane format (a mode change rebuilds
    // even at unchanged dimensions); another thread is rejected.
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                       uint32_t width, uint32_t height,
                       DXGI_FORMAT planeFormat = DXGI_FORMAT_B8G8R8A8_UNORM);

    // Consumes the caller's raw byte buffers immediately (UpdateSubresource
    // deep-copies during the call); retains no caller pointer. The real native
    // ring deep-copies before calling. Returns S_OK only when the new pair is
    // committed. Any failure preserves the previous active pair.
    HRESULT RenderPair(const uint8_t* left, size_t leftBytes,
                       const uint8_t* right, size_t rightBytes,
                       uint32_t stride,
                       const ProjectionPlanV1& leftPlan,
                       const ProjectionPlanV1& rightPlan);

    // HIGHRES-H2-GPU shared-input path: projects two BORROWED Texture2D SRVs
    // from the same XR device (Gate2 pool: DXGI_FORMAT_B8G8R8X8_UNORM, the
    // shared texture holding native SIM gamma/sRGB-coded RGB with X=0)
    // directly into the inactive output pair. No CPU pixel Read/Map/Copy, no
    // UpdateSubresource of the CPU source texture, and no per-frame GPU
    // allocation: only GPU CopyResource (91 contract), the two 48-byte
    // per-eye constant-buffer updates and the two draws.
    //
    // Color contract (exact match with the CPU path filtering):
    //  - planeFormat 91 (B8G8R8A8_UNORM_SRGB): the borrowed 88 resource is
    //    GPU-copied per eye into the owned cached B8G8R8X8_TYPELESS (92)
    //    staging texture (same X8 bit group) and sampled through the cached
    //    B8G8R8X8_UNORM_SRGB (93) view, so the SAME hardware sRGB decode
    //    before filtering as the CPU 91 SRV applies. No manual in-shader
    //    decode/lerp and no blind 88->87/91 copy.
    //  - planeFormat 87: the borrowed 88 SRV is sampled directly.
    // src.z/src.w are always 0 (reserved) on both paths.
    //
    // Borrowed contract: the caller retains pool-owned references and MUST
    // guarantee that neither SRV (nor its resource) is overwritten or
    // released until the real XR GPU event for this dispatch completes.
    // Returns S_OK only when the new pair is committed (both eyes enqueued
    // with no device-removed error). S_OK does NOT claim GPU completion and
    // measures no FPS. Any failure preserves the previous active pair.
    HRESULT RenderSharedPair(ID3D11ShaderResourceView* left,
                             ID3D11ShaderResourceView* right,
                             const ProjectionPlanV1& leftPlan,
                             const ProjectionPlanV1& rightPlan);

    // Borrowed active output for eye 0/1; null before the first successful
    // RenderPair, after Reset, or for a bad eye index. The pointer is valid
    // only until the next Reset or an Initialize with different dimensions.
    ID3D11Texture2D* Output(int eye) const;

    // Releases every owned resource (including AddRefed device/context) and
    // invalidates Output(). No thread check: the caller must guarantee the
    // owning XR-worker thread is not concurrently using the helper/context.
    void Reset();

private:
    void ReleaseAll();
    bool OnOwnerThread() const;
    HRESULT CreateResources(uint32_t width, uint32_t height);
    // Cached shared-input 91 staging: two X8_TYPELESS (92) DEFAULT
    // shader-resource textures at width_x_height_ plus two cached
    // X8_UNORM_SRGB (93) TEX2D views. Allocated only when planeFormat_ is
    // 91, with a fail-closed CheckFormatSupport(93) gate before creation;
    // any failure returns and Initialize's ReleaseAll owns everything.
    HRESULT CreateSharedStage(uint32_t width, uint32_t height);
    HRESULT ValidateInputs(const uint8_t* left, size_t leftBytes,
                           const uint8_t* right, size_t rightBytes,
                           uint32_t stride,
                           const ProjectionPlanV1& leftPlan,
                           const ProjectionPlanV1& rightPlan) const;
    HRESULT UploadPair(const uint8_t* left, const uint8_t* right, uint32_t stride,
                       const ProjectionPlanV1& leftPlan, const ProjectionPlanV1& rightPlan,
                       int pair);
    // Validates one borrowed shared-input SRV: TEX2D view, non-null, backed
    // by a same-device (canonical IUnknown identity equals device_)
    // B8G8R8X8_UNORM DEFAULT shader-resource Texture2D matching width_/
    // height_ with single mip, ArraySize 1, Samples 1. Temporary AddRefs
    // from GetResource/GetDevice are released on every path.
    HRESULT ValidateSharedInput(ID3D11ShaderResourceView* srv) const;
    // Projects one eye from an explicit input SRV: owned sourceSrv_[eye] for
    // the CPU path; the borrowed 88 view (87 contract) or the owned cached
    // sRGB 93 staging view (91 contract) for RenderSharedPair.
    HRESULT ProjectEye(int eye, int pair, ID3D11ShaderResourceView* inputSrv);
    void UnbindPass();

    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    DWORD ownerThreadId_;
    uint32_t width_;
    uint32_t height_;
    DXGI_FORMAT planeFormat_;                // frozen at Initialize (87 or 91)

    ID3D11Texture2D* sourceTex_[2];          // [eye]
    ID3D11ShaderResourceView* sourceSrv_[2]; // [eye]
    ID3D11Texture2D* outputTex_[2][2];       // [pair][eye]
    ID3D11RenderTargetView* outputRtv_[2][2];// [pair][eye]

    // Owned cached staging for the shared-input 91 color contract only
    // (null when planeFormat_ is 87): CopyResource target for the borrowed
    // 88 resources, sampled through the hardware-sRGB 93 views.
    ID3D11Texture2D* sharedStageTex_[2];         // [eye], format 92
    ID3D11ShaderResourceView* sharedSrgbSrv_[2]; // [eye], format 93

    ID3D11VertexShader* vs_;
    ID3D11PixelShader* ps_;
    ID3D11Buffer* eyeCb_[2];                 // per-eye projection constants
    ID3D11SamplerState* linearClampSampler_;
    ID3D11RasterizerState* rasterizer_;
    ID3D11DepthStencilState* noDepth_;

    D3D11_VIEWPORT viewport_;
    int activePair_;                         // -1 until first committed RenderPair
};

} // namespace VRSIM

#endif // VRSIM_GPU_PROJECTION_PASS_H
