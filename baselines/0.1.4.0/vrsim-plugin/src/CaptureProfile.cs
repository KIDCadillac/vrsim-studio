using System;

namespace VRSIM
{
    internal static class CaptureProfile
    {
        private const string LegacyProfile = "legacy-640x480";
        private const string NativeProfile = "native-1024x1024";
        private const string Native1536Profile = "native-1536x1536";
        private const string Native2048Profile = "native-2048x2048";
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
