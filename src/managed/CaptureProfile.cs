using System;

#if VRSIM_SHARED_3072 && VRSIM_SHARED_2736
#error VRSIM_SHARED_3072 and VRSIM_SHARED_2736 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_3072 && VRSIM_SHARED_2560
#error VRSIM_SHARED_3072 and VRSIM_SHARED_2560 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_3072 && VRSIM_SHARED_2048
#error VRSIM_SHARED_3072 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_2736 && VRSIM_SHARED_2560
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2560 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_2736 && VRSIM_SHARED_2048
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_2560 && VRSIM_SHARED_2048
#error VRSIM_SHARED_2560 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if VRSIM_QA_OUTPUT_4096 && (!VRSIM_NATIVE_QA || !NC02_XR_QA || !VRSIM_SHARED_3072 || !VRSIM_SOURCE_4096_QA || !VRSIM_QA_MIXED_SOURCE || !VRSIM_QA_D9_BEGIN_POLL)
#error VRSIM_QA_OUTPUT_4096 requires VRSIM_NATIVE_QA, NC02_XR_QA, VRSIM_SHARED_3072, VRSIM_SOURCE_4096_QA, VRSIM_QA_MIXED_SOURCE and VRSIM_QA_D9_BEGIN_POLL
#endif

namespace VRSIM
{
    internal static class CaptureProfile
    {
        private const string LegacyProfile = "legacy-640x480";
        private const string NativeProfile = "native-1024x1024";
        private const string Native1536Profile = "native-1536x1536";
        private const string Native2048Profile = "native-2048x2048";
#if VRSIM_SHARED_3072
        // Accepted ONLY in the isolated 3072 compile variant; every CPU/raw
        // submit gate below (2048 dimension ceiling, 32 MiB payload) stays.
        private const string Native3072Profile = "native-3072x3072";
#endif
#if VRSIM_SHARED_2736
        // Accepted ONLY in the isolated 2736 compile variant; every CPU/raw
        // submit gate below (2048 dimension ceiling, 32 MiB payload) stays.
        private const string Native2736Profile = "native-2736x2736";
#endif
#if VRSIM_SHARED_2560
        // Accepted ONLY in the isolated 2560 compile variant; every CPU/raw
        // submit gate below (2048 dimension ceiling, 32 MiB payload) stays.
        private const string Native2560Profile = "native-2560x2560";
#endif
#if VRSIM_QA_OUTPUT_4096
        // Accepted ONLY in the isolated output-4096 QA compile variant on the existing
        // source4096 mixed Begin/Poll route; the 2048 dimension ceiling and 32 MiB
        // payload gates in ValidateInput below stay, so this token is never CPU-accepted.
        private const string Native4096Profile = "native-4096x4096";
#endif
        private const long MaxPayloadBytes = 32L * 1024L * 1024L;
        private const int MaxDimension = 2048;

        internal static int Width { get; private set; }
        internal static int Height { get; private set; }
        internal static int Stride { get; private set; }
        internal static int PayloadBytes { get; private set; }
        internal static string Requested { get; private set; }
        internal static string Selected { get; private set; }
        internal static string Reason { get; private set; }
        internal static string DiagnosticText { get; private set; }

        static CaptureProfile()
        {
            string requested = null;
            string readFailure = null;
            try
            {
                requested = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_PROFILE");
            }
            catch (Exception ex)
            {
                readFailure = "ENVIRONMENT_READ_FAILED " + ex.GetType().Name + ": " + ex.Message;
            }
            // Raw value preserved (no Trim): tokens must match exactly.
            Requested = requested ?? string.Empty;

            string selected;
            string reason;

            if (readFailure != null)
            {
                selected = LegacyProfile;
                reason = readFailure + "; environment value unreadable, forcing legacy-640x480";
            }
            else if (string.Equals(Requested, NativeProfile, StringComparison.Ordinal))
            {
                selected = NativeProfile;
                reason = "native profile requested and accepted";
            }
            else if (string.Equals(Requested, Native1536Profile, StringComparison.Ordinal))
            {
                selected = Native1536Profile;
                reason = "native 1536 profile requested and accepted";
            }
            else if (string.Equals(Requested, Native2048Profile, StringComparison.Ordinal))
            {
                selected = Native2048Profile;
                reason = "native 2048 profile requested and accepted";
            }
#if VRSIM_SHARED_3072
            else if (string.Equals(Requested, Native3072Profile, StringComparison.Ordinal))
            {
                selected = Native3072Profile;
                reason = "native 3072 profile requested and accepted (isolated 3072 compile variant)";
            }
#endif
#if VRSIM_SHARED_2736
            else if (string.Equals(Requested, Native2736Profile, StringComparison.Ordinal))
            {
                selected = Native2736Profile;
                reason = "native 2736 profile requested and accepted (isolated 2736 compile variant)";
            }
#endif
#if VRSIM_SHARED_2560
            else if (string.Equals(Requested, Native2560Profile, StringComparison.Ordinal))
            {
                selected = Native2560Profile;
                reason = "native 2560 profile requested and accepted (isolated 2560 compile variant)";
            }
#endif
#if VRSIM_QA_OUTPUT_4096
            else if (string.Equals(Requested, Native4096Profile, StringComparison.Ordinal))
            {
                selected = Native4096Profile;
                reason = "native 4096 output profile requested and accepted (isolated output-4096 QA compile variant; GPU obs-shared route only)";
            }
#endif
            else if (string.Equals(Requested, LegacyProfile, StringComparison.Ordinal)
                || string.IsNullOrEmpty(Requested))
            {
                selected = LegacyProfile;
                reason = string.IsNullOrEmpty(Requested)
                    ? "no profile requested; defaulting to legacy"
                    : "legacy profile requested and accepted";
            }
            else
            {
                selected = LegacyProfile;
                reason = "invalid requested value '" + Requested + "' (exact token required); falling back to legacy-640x480";
            }

            Selected = selected;
            Reason = reason;

            if (selected == NativeProfile)
            {
                Width = 1024;
                Height = 1024;
            }
            else if (selected == Native1536Profile)
            {
                Width = 1536;
                Height = 1536;
            }
            else if (selected == Native2048Profile)
            {
                Width = 2048;
                Height = 2048;
            }
#if VRSIM_SHARED_3072
            else if (selected == Native3072Profile)
            {
                Width = 3072;
                Height = 3072;
            }
#endif
#if VRSIM_SHARED_2736
            else if (selected == Native2736Profile)
            {
                Width = 2736;
                Height = 2736;
            }
#endif
#if VRSIM_SHARED_2560
            else if (selected == Native2560Profile)
            {
                Width = 2560;
                Height = 2560;
            }
#endif
#if VRSIM_QA_OUTPUT_4096
            else if (selected == Native4096Profile)
            {
                Width = 4096;
                Height = 4096;
            }
#endif
            else
            {
                Width = 640;
                Height = 480;
            }

            Stride = checked(Width * 4);
            PayloadBytes = checked(Stride * Height);

            DiagnosticText = "VRSIM CaptureProfile: requested='" + Requested
                + "' selected=" + selected
                + " width=" + Width + " height=" + Height
                + " stride=" + Stride + " payloadBytes=" + PayloadBytes
                + " reason=" + reason;
        }

        internal static int ValidateInput(byte[] left, byte[] right, int width, int height, int stride)
        {
            if (left == null || right == null)
            {
                return -10;
            }

            if (width <= 0 || height <= 0
                || width > MaxDimension || height > MaxDimension
                || width != Width || height != Height)
            {
                return -11;
            }

            if ((long)stride < (long)width * 4L)
            {
                return -12;
            }

            if ((long)stride * (long)height > MaxPayloadBytes)
            {
                return -13;
            }

            long required = (long)stride * (long)height;
            if (left.LongLength < required || right.LongLength < required)
            {
                return -14;
            }

            return 0;
        }
    }
}
