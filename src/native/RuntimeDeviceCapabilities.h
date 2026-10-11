// RuntimeDeviceCapabilities.h — DEVICE-CAPS-01 pure capability policy helpers.
// Header-only, no OpenXR/D3D/ABI dependency: the EXACT same policy compiles
// into VRSIMNative.cpp and into the standalone qa/RuntimeDeviceCapabilitiesQA.cpp
// fixtures, so the test logic is never duplicated. Every function is pure: it
// reads only caller-supplied ACTUAL queried values and never queries, defaults,
// clamps or guesses. Decisions are fail-closed: anything not proven by an
// actual successful query is refused with an explicit reason code.
#ifndef VRSIM_RUNTIME_DEVICE_CAPABILITIES_H
#define VRSIM_RUNTIME_DEVICE_CAPABILITIES_H

#include <cstdint>

namespace VRSIMCaps {

enum CapsDecision : int {
    kCapsOk = 0,
    kCapsUnavailable = 1,          // current-session capability cache not populated
    kCapsNonPositiveRequest = 2,   // requested dimension <= 0
    kCapsExceedsEyeMaxRect = 3,    // request exceeds EITHER eye's maxImageRect
    kCapsExceedsSystemMax = 4,     // request exceeds the actual queried system max
    kCapsInvalidRecommendation = 5,// recommended zero or above its own maxImageRect
    kCapsExceedsInt32 = 6          // extent above INT32_MAX: not representable in the
                                   // int32 imageRect/size fields; conversion would narrow
};

// int32 representability boundary. The submission path casts extents to int32
// (XrExtent2Di, the type of projection layer subImage.imageRect.extent), so
// any positive extent that is actually submitted or converted — each eye's
// initial recommendation and every scene request — must be <= INT32_MAX, or
// the cast yields a negative rect. Queried MAXIMA
// themselves may exceed INT32_MAX on a runtime; that is harmless and is never
// refused on its own — only extents that would be converted are gated.
inline constexpr uint32_t kMaxRepresentableExtent = 0x7FFFFFFFu;

struct EyeCapsV1 {
    uint32_t recommendedWidth = 0;
    uint32_t recommendedHeight = 0;
    uint32_t maxRectWidth = 0;   // XrViewConfigurationView.maxImageRectWidth
    uint32_t maxRectHeight = 0;  // XrViewConfigurationView.maxImageRectHeight
};

struct DeviceCapsV1 {
    bool valid = false;           // complete per-eye cache from an actual successful enumeration
    bool systemMaxValid = false;  // actual successful system-properties query with nonzero maxima
    uint32_t systemMaxWidth = 0;  // XrSystemGraphicsProperties.maxSwapchainImageWidth
    uint32_t systemMaxHeight = 0; // XrSystemGraphicsProperties.maxSwapchainImageHeight
    EyeCapsV1 eye[2];
};

// Blend policy: choose `required` ONLY from the actual advertised list.
// dataCallOk is the real data-enumeration success; returnedCount is its real
// countOutput and must fit capacity. Never picks list[0] and never defaults on
// a failed/empty enumeration — absence of `required` is a refusal.
template <typename T>
inline bool SelectRequiredBlendMode(const T* modes, uint32_t returnedCount, uint32_t capacity,
                                    bool dataCallOk, T required, T* outMode)
{
    if (!dataCallOk || !modes || returnedCount == 0 || returnedCount > capacity) return false;
    for (uint32_t i = 0; i < returnedCount; ++i)
        if (modes[i] == required) { if (outMode) *outMode = modes[i]; return true; }
    return false;
}

// Initial swapchain admission: every actual enumerated value must be nonzero,
// each eye's recommendation must be int32-representable (it is later converted
// to the int32 per-eye subImage extent), and each recommendation must fit its
// OWN maxImageRect and the actual queried system max. No clamp, no
// left-eye-only assumption, no fallback.
inline int CapsAdmitInitialSwapchains(const DeviceCapsV1& c)
{
    if (!c.systemMaxValid || c.systemMaxWidth == 0 || c.systemMaxHeight == 0)
        return kCapsUnavailable;
    for (int e = 0; e < 2; ++e)
    {
        const EyeCapsV1& ey = c.eye[e];
        if (ey.recommendedWidth == 0 || ey.recommendedHeight == 0 ||
            ey.maxRectWidth == 0 || ey.maxRectHeight == 0)
            return kCapsInvalidRecommendation;
        if (ey.recommendedWidth > kMaxRepresentableExtent ||
            ey.recommendedHeight > kMaxRepresentableExtent)
            return kCapsExceedsInt32;
        if (ey.recommendedWidth > ey.maxRectWidth || ey.recommendedHeight > ey.maxRectHeight)
            return kCapsInvalidRecommendation;
        if (ey.recommendedWidth > c.systemMaxWidth || ey.recommendedHeight > c.systemMaxHeight)
            return kCapsExceedsSystemMax;
    }
    return kCapsOk;
}

// Scene-rebuild request admission (fail-closed): the request positive AND
// int32-representable (it is converted to swapchain/subImage sizes; an
// above-INT32_MAX positive request must be refused here, never compared after
// a wrapping or narrowing cast), the current-session cache must be available,
// and the request must be within BOTH eyes' maxImageRect and the system max.
// Used BEFORE any Reset/Destroy or readiness invalidation so a rejection
// preserves the current resources.
inline int CapsAdmitSceneRequest(const DeviceCapsV1& c, long long w, long long h)
{
    if (w <= 0 || h <= 0) return kCapsNonPositiveRequest;
    if (w > (long long)kMaxRepresentableExtent || h > (long long)kMaxRepresentableExtent)
        return kCapsExceedsInt32;
    if (!c.valid || !c.systemMaxValid || c.systemMaxWidth == 0 || c.systemMaxHeight == 0)
        return kCapsUnavailable;
    for (int e = 0; e < 2; ++e)
    {
        const EyeCapsV1& ey = c.eye[e];
        if (ey.maxRectWidth == 0 || ey.maxRectHeight == 0) return kCapsUnavailable;
        if ((unsigned long long)w > ey.maxRectWidth || (unsigned long long)h > ey.maxRectHeight)
            return kCapsExceedsEyeMaxRect;
    }
    if ((unsigned long long)w > c.systemMaxWidth || (unsigned long long)h > c.systemMaxHeight)
        return kCapsExceedsSystemMax;
    return kCapsOk;
}

} // namespace VRSIMCaps

#endif // VRSIM_RUNTIME_DEVICE_CAPABILITIES_H
