// DeviceIdentityResolver.cs — merge multi-provider hits into one descriptor (ARCH-DEVICE-01 PART D).
// Confidence ladder: EXACT only on serial/unique id; NEVER merge on manufacturer name alone.
using System;
using System.Collections.Generic;
using System.Linq;

namespace VRSIM.XR
{
    public enum IdentityConfidence { Unknown, Low, Medium, High, Exact }

    public static class DeviceIdentityResolver
    {
        public sealed class MergedDevice
        {
            public XRDeviceDescriptor Descriptor = new XRDeviceDescriptor();
            public IdentityConfidence Confidence = IdentityConfidence.Unknown;
            public List<string> ContributingProviders = new List<string>();
        }

        /// <summary>Merge provider outputs by identity evidence. Skeleton rule set:
        /// EXACT  = same Serial or same unique deviceId across providers
        /// HIGH   = same USB VID+PID (provider-claimed vendor device) or same runtime path + model
        /// MEDIUM = same IpAddress, or same Model + same transport
        /// LOW/UNKNOWN = name similarity only (never auto-merged into one device)</summary>
        public static List<MergedDevice> Merge(IEnumerable<(string providerId, IEnumerable<XRDeviceDescriptor> devices)> providerResults)
        {
            var merged = new List<MergedDevice>();
            foreach (var pr in providerResults)
            {
                foreach (var d in pr.devices)
                {
                    var hit = FindExisting(merged, d);
                    if (hit == null)
                    {
                        hit = new MergedDevice { Descriptor = CloneShallow(d), Confidence = IdentityConfidence.Unknown };
                        merged.Add(hit);
                    }
                    if (!hit.ContributingProviders.Contains(pr.providerId))
                        hit.ContributingProviders.Add(pr.providerId);
                    Absorb(hit, d, pr.providerId);
                }
            }
            return merged;
        }

        private static MergedDevice FindExisting(List<MergedDevice> list, XRDeviceDescriptor d)
        {
            foreach (var m in list)
            {
                var e = m.Descriptor;
                if (!string.IsNullOrEmpty(e.Serial) && e.Serial == d.Serial) { m.Confidence = IdentityConfidence.Exact; return m; }
                if (e.UsbVid.HasValue && d.UsbVid.HasValue && e.UsbVid == d.UsbVid && e.UsbPid == d.UsbPid) { m.Confidence = Max(m.Confidence, IdentityConfidence.High); return m; }
                if (!string.IsNullOrEmpty(e.IpAddress) && e.IpAddress == d.IpAddress) { m.Confidence = Max(m.Confidence, IdentityConfidence.Medium); return m; }
                if (!string.IsNullOrEmpty(e.Model) && e.Model == d.Model && e.Transport == d.Transport) { m.Confidence = Max(m.Confidence, IdentityConfidence.Medium); return m; }
            }
            return null;
        }

        private static void Absorb(MergedDevice m, XRDeviceDescriptor d, string providerId)
        {
            var e = m.Descriptor;
            e.DisplayName = e.DisplayName ?? d.DisplayName;
            e.Manufacturer = e.Manufacturer ?? d.Manufacturer;
            e.Model = e.Model ?? d.Model;
            e.Serial = e.Serial ?? d.Serial;
            e.RuntimeName = e.RuntimeName ?? d.RuntimeName;
            e.RuntimePath = e.RuntimePath ?? d.RuntimePath;
            e.RuntimeVersion = e.RuntimeVersion ?? d.RuntimeVersion;
            e.IpAddress = e.IpAddress ?? d.IpAddress;
            e.UsbVid = e.UsbVid ?? d.UsbVid;
            e.UsbPid = e.UsbPid ?? d.UsbPid;
            e.BatteryLevel = e.BatteryLevel ?? d.BatteryLevel;
            e.LastSeen = MaxTime(e.LastSeen, d.LastSeen);
            e.IsActiveRuntimeDevice |= d.IsActiveRuntimeDevice;
            e.IsLocal |= d.IsLocal;
            e.IsRemote |= d.IsRemote;
            if (e.Connection < d.Connection) e.Connection = d.Connection;
        }

        private static DateTime? MaxTime(DateTime? a, DateTime? b) => !a.HasValue ? b : !b.HasValue ? a : (a > b ? a : b);
        private static IdentityConfidence Max(IdentityConfidence a, IdentityConfidence b) => a > b ? a : b;

        private static XRDeviceDescriptor CloneShallow(XRDeviceDescriptor d)
        {
            var c = new XRDeviceDescriptor();
            foreach (var f in typeof(XRDeviceDescriptor).GetFields())
                f.SetValue(c, f.GetValue(d));
            return c;
        }
    }
}
