// GpuProjection.hlsl - Asymmetric-FOV reprojection for the VRSIM GPU helper.
//
// Maps each output pixel of the XR asymmetric frustum back to the symmetric
// raw-captured source image of the same capture pose, then linearly samples.
// SV_Position.xy already carries the half-pixel offset in top-left space; no
// extra .5 is added. Alpha is always forced to 1; no brightness/tint/gamma
// constants are applied here.
//
// Color contract: ALL sRGB decode/encode is done by hardware views, never
// in this shader (g_src.z/g_src.w are reserved and always 0):
//  - CPU path: g_source is the helper's own 87 plain or 91 hardware-SRGB
//    view; the 91 SRV decodes per texel before filtering, the 91 output RTV
//    encodes once.
//  - Shared 91 path: the borrowed B8G8R8X8_UNORM (88) Gate2 texture is
//    GPU-copied by the C++ side into an owned B8G8R8X8_TYPELESS (92)
//    staging texture (same X8 bit group) whose cached B8G8R8X8_UNORM_SRGB
//    (93) view is bound here, so filtering matches the CPU 91 path exactly
//    (no typed 88->93 cast, no manual decode-then-lerp).
//  - Shared 87 path: the borrowed 88 SRV is bound directly; plain sampling.
//
// Build-time bytecode (no runtime compiler dependency), from this directory:
//   fxc /Tvs_5_0 /EVSMain /Fh GpuProjectionVS.inc /Vn g_gpuProjectionVS GpuProjection.hlsl
//   fxc /Tps_5_0 /EPSMain /Fh GpuProjectionPS.inc /Vn g_gpuProjectionPS GpuProjection.hlsl

Texture2D    g_source      : register(t0);
SamplerState g_linearClamp : register(s0);

cbuffer ProjectionCB : register(b0)
{
    float4 g_lrud; // tanL tanR tanU tanD (source symmetric frustum tangents)
    float4 g_meta; // tx ty outW outH
    float4 g_src;  // srcW srcH reserved(0) reserved(0)
};

struct PSInput
{
    float4 position : SV_Position;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
    PSInput output;
    // Explicit full-screen triangle, no vertex buffer:
    // (-1,-1) (3,-1) (-1,3) in NDC.
    float2 ndc = float2(float((vertexId << 1) & 2), float(vertexId & 2)) * 2.0
               + float2(-1.0, -1.0);
    output.position = float4(ndc, 0.0, 1.0);
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    float a = input.position.x / g_meta.z;
    float b = input.position.y / g_meta.w;

    float rx = g_lrud.x + (g_lrud.y - g_lrud.x) * a;
    float ry = g_lrud.z + (g_lrud.w - g_lrud.z) * b;

    float2 uv = float2(0.5 + rx / (2.0 * g_meta.x),
                       0.5 - ry / (2.0 * g_meta.y));

    float2 lo = float2(0.5 / g_src.x, 0.5 / g_src.y);
    float2 hi = float2(1.0, 1.0) - lo;
    uv = clamp(uv, lo, hi);

    // Hardware view selection (plain vs SRGB vs shared-SRGB staging) is the
    // whole color contract; the filter itself is identical on every path.
    float3 rgb = g_source.SampleLevel(g_linearClamp, uv, 0).rgb;
    return float4(rgb, 1.0);
}
