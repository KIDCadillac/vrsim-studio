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

// ---- QA area-filter entry: selected ONLY by Root's fxc /EPSMainAreaQA entry. ----
// PSMain above is byte-for-byte unchanged and stays the default /EPSMain build.
// Same hardware color contract: the bound g_source view (93 shared-sRGB staging
// or 88/87 plain) does all sRGB decode per texel; the 91 RTV encodes once. The
// CPU guards reject out-of-contract rho before any draw, so the invalid branch
// here (black opaque) is defensive only.
float4 PSMainAreaQA(PSInput input) : SV_Target
{
    // Per-axis source/output scale: how many source texels one output pixel
    // spans. rho<=1 means magnification (or 1:1) -> exact original bilinear.
    float rhoX = (g_src.x / g_meta.z) * (g_lrud.y - g_lrud.x) / (2.0 * g_meta.x);
    float rhoY = (g_src.y / g_meta.w) * (g_lrud.z - g_lrud.w) / (2.0 * g_meta.y);

    bool finite = rhoX > 0.0 && rhoY > 0.0 &&
                  isfinite(rhoX) && isfinite(rhoY) && isfinite(g_meta.x) &&
                  isfinite(g_meta.y) && isfinite(g_src.x) && isfinite(g_src.y) &&
                  g_meta.x > 0.0 && g_meta.y > 0.0 && g_src.x > 0.0 && g_src.y > 0.0;
    if (!finite || rhoX > 2.0 || rhoY > 2.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0); // fail closed; CPU rejected this already
    }
    if (rhoX <= 1.0 && rhoY <= 1.0)
    {
        return PSMain(input); // no extra sampling/blur when not minifying
    }

    float a = input.position.x / g_meta.z;
    float b = input.position.y / g_meta.w;
    float rx = g_lrud.x + (g_lrud.y - g_lrud.x) * a;
    float ry = g_lrud.z + (g_lrud.w - g_lrud.z) * b;
    float2 uv = float2(0.5 + rx / (2.0 * g_meta.x),
                       0.5 - ry / (2.0 * g_meta.y));
    float2 lo = float2(0.5 / g_src.x, 0.5 / g_src.y);
    float2 hi = float2(1.0, 1.0) - lo;
    uv = clamp(uv, lo, hi);

    float2 srcDims = float2(g_src.x, g_src.y);
    float2 C = uv * srcDims;                 // texel-center space (same +0.5 origin)
    float2 widths = max(float2(1.0, 1.0), float2(rhoX, rhoY));
    float2 low = C - widths * 0.5;
    float2 high = C + widths * 0.5;
    int2 base = int2(floor(low));
    int2 last = int2(g_src.x, g_src.y) - 1;

    float3 acc = float3(0.0, 0.0, 0.0);
    float totalW = 0.0;
    [unroll]
    for (int oy = 0; oy < 3; ++oy)
    {
        int iy = base.y + oy;
        float wy = max(0.0, min(high.y, (float)(iy + 1)) - max(low.y, (float)iy));
        if (wy <= 0.0)
        {
            continue;
        }
        [unroll]
        for (int ox = 0; ox < 3; ++ox)
        {
            int ix = base.x + ox;
            float wx = max(0.0, min(high.x, (float)(ix + 1)) - max(low.x, (float)ix));
            if (wx <= 0.0)
            {
                continue;
            }
            // Explicit clamp before Load so edge/clamped taps still carry their
            // box overlap weight (no missing-edge or out-of-bounds black).
            int2 ci = clamp(int2(ix, iy), int2(0, 0), last);
            float3 t = g_source.Load(int3(ci, 0)).rgb; // hardware 93/88 decode
            acc += t * (wx * wy);
            totalW += wx * wy;
        }
    }

    float3 rgb = acc / (widths.x * widths.y);
    return float4(rgb, 1.0);
}

// ---- QA cross025 entry: selected ONLY by Root's fxc /EPSMainAreaCross025QA. ----
// PSMain and PSMainAreaQA above are byte-for-byte unchanged; default /EPSMain
// and /EPSMainAreaQA bytecode are untouched by this appended entry.
// Output-domain five-tap cross: evaluates the existing PSMainAreaQA at this
// output center and the four cardinal +/-1 OUTPUT-pixel centers (output
// domain, not source-texel +/-1, which is rho-scaled after mapping). Each
// center is clamped to [0.5, g_meta.z-0.5] x [0.5, g_meta.w-0.5] BEFORE the
// existing mapper so edge taps replicate in-range centers instead of
// inventing frustum beyond the output; other position components preserved.
// Returned LINEAR RGB is summed 0.90 center + 0.025 per cardinal; the
// existing hardware view decode feeds every tap and the single final RTV
// write encodes once (no manual sRGB/gamma). Alpha always 1. Flat
// equal-neighbor RGB bypass keeps uniform areas identical to the center tap.
// Cost: minify can reach 5 x 9 = 45 logical source Loads per output pixel
// (magnification: five SampleLevel evaluations). Full-resolution cost,
// actual SIM and HMD acceptance MUST still be verified; nothing here proves
// them.
float4 PSMainAreaCross025QA(PSInput input) : SV_Target
{
    float2 lo = float2(0.5, 0.5);
    float2 hi = float2(g_meta.z - 0.5, g_meta.w - 0.5);

    PSInput tap = input;
    tap.position.xy = clamp(input.position.xy, lo, hi);
    float3 center = PSMainAreaQA(tap).rgb;

    tap.position.xy = clamp(input.position.xy + float2(0.0, -1.0), lo, hi);
    float3 up = PSMainAreaQA(tap).rgb;

    tap.position.xy = clamp(input.position.xy + float2(0.0, 1.0), lo, hi);
    float3 down = PSMainAreaQA(tap).rgb;

    tap.position.xy = clamp(input.position.xy + float2(-1.0, 0.0), lo, hi);
    float3 leftTap = PSMainAreaQA(tap).rgb;

    tap.position.xy = clamp(input.position.xy + float2(1.0, 0.0), lo, hi);
    float3 rightTap = PSMainAreaQA(tap).rgb;

    if (all(center == up) && all(center == down) &&
        all(center == leftTap) && all(center == rightTap))
    {
        return float4(center, 1.0); // flat bypass; equals the weighted sum
    }

    float3 rgb = 0.90 * center + 0.025 * (up + down + leftTap + rightTap);
    return float4(rgb, 1.0);
}
