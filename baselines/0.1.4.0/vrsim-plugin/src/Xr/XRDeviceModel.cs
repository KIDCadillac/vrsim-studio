// XRDeviceModel.cs — VRSIM generic XR device model (VRSIM-ARCH-DEVICE-01 PART A).
// Brand-neutral by design: no Quest/Meta/PICO fields; unavailable data = null/Unknown.
// Docs: docs/VRSIM_DEVICE_MODEL.md
using System;
using System.Collections.Generic;

namespace VRSIM.XR
{
    public enum ConnectionTransport { Unknown, Usb, UsbStream, Wifi, WifiStream, Lan, LocalRuntime, RemoteClient, Virtual }

    public enum ConnectionState
    {
        Discovered, Available, Pairing, Connecting, Connected, XrReady, InVr, Disconnected, Error
    }

    public enum DeviceClass { Unknown, Headset, Controller, Tracker }

    public enum CapabilitySupport { Unknown, Unsupported, Supported }

    /// <summary>Generic XR device descriptor. Every field is filled from providers;
    /// nothing is fabricated (unavailable = null / Unknown). See docs/VRSIM_DEVICE_MODEL.md.</summary>
    public sealed class XRDeviceDescriptor
    {
        public string DeviceId;
        public string PersistentId;
        public string DisplayName;
        public string Manufacturer;
        public string Model;
        public string Serial;
        public DeviceClass DeviceClass = DeviceClass.Unknown;
        public ConnectionState Connection = ConnectionState.Discovered;
        public ConnectionTransport Transport = ConnectionTransport.Unknown;
        public string RuntimeName;
        public string RuntimePath;
        public string RuntimeVersion;
        public bool IsActiveRuntimeDevice;
        public bool IsLocal;
        public bool IsRemote;
        public string IpAddress;
        public int? UsbVid;
        public int? UsbPid;
        public double? BatteryLevel;
        public List<double> RefreshRates;
        public double? CurrentRefreshRate;
        public uint? RecommendedEyeWidth;
        public uint? RecommendedEyeHeight;
        public uint? MaxEyeWidth;
        public uint? MaxEyeHeight;
        public CapabilitySupport SupportsControllers = CapabilitySupport.Unknown;
        public CapabilitySupport SupportsHandTracking = CapabilitySupport.Unknown;
        public CapabilitySupport SupportsEyeTracking = CapabilitySupport.Unknown;
        public CapabilitySupport SupportsPassthrough = CapabilitySupport.Unknown;
        public CapabilitySupport SupportsWireless = CapabilitySupport.Unknown;
        public CapabilitySupport SupportsWired = CapabilitySupport.Unknown;
        public CapabilitySupport SupportsMirror = CapabilitySupport.Unknown;
        public CapabilitySupport SupportsNativeOpenXR = CapabilitySupport.Unknown;
        public DateTime? LastSeen;
        public List<string> Capabilities = new List<string>();
    }
}
