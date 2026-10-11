// IXRProviders.cs — discovery/connection provider contracts (ARCH-DEVICE-01 PART B/C/G).
// Discovery ≠ XR session: a provider reports FACTS about existence/transport/capability;
// it never claims XR-readiness on behalf of another layer.
using System;
using System.Collections.Generic;

namespace VRSIM.XR
{
    public interface IXRDeviceDiscoveryProvider
    {
        string ProviderId { get; }
        void StartDiscovery();
        void StopDiscovery();
        IReadOnlyList<XRDeviceDescriptor> GetDevices();
        event Action<XRDeviceDescriptor> DeviceAdded;
        event Action<XRDeviceDescriptor> DeviceUpdated;
        event Action<string> DeviceRemoved;
    }

    public sealed class LinkStats
    {
        public double? LatencyMs;
        public double? NetworkRttMs;
        public double? BandwidthMbps;
        public uint? DroppedFrames;
    }

    public interface IXRConnectionProvider
    {
        string ProviderId { get; }
        bool CanConnect(XRDeviceDescriptor device);
        void Connect(XRDeviceDescriptor device);
        void Disconnect(string deviceId);
        ConnectionState GetConnectionState(string deviceId);
        LinkStats GetLinkStats(string deviceId);
    }
}
