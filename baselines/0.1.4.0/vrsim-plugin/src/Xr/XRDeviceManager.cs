// XRDeviceManager.cs — platform hub skeleton (ARCH-DEVICE-01 PART A/U).
// Owns providers + identity merge + known registry. Product upper layers ask ONLY this
// class for devices; brand questions are forbidden by architecture.
using System;
using System.Collections.Generic;

namespace VRSIM.XR
{
    public sealed class XRDeviceManager
    {
        private readonly List<IXRDeviceDiscoveryProvider> _providers = new List<IXRDeviceDiscoveryProvider>();
        private readonly KnownDeviceRegistry _known;
        private List<DeviceIdentityResolver.MergedDevice> _merged = new List<DeviceIdentityResolver.MergedDevice>();

        public XRDeviceManager(KnownDeviceRegistry known) { _known = known; }

        public void AddProvider(IXRDeviceDiscoveryProvider p)
        {
            _providers.Add(p);
            p.DeviceAdded += _ => Refresh();
            p.DeviceUpdated += _ => Refresh();
            p.DeviceRemoved += _ => Refresh();
        }

        /// <summary>PART N: start ALL available providers in parallel; never a blocking scan.
        /// A provider that fails to start is skipped (core product keeps working).</summary>
        public void StartAllDiscoveries()
        {
            foreach (var p in _providers)
            {
                try { p.StartDiscovery(); }
                catch { /* provider unavailable — core unaffected */ }
            }
            Refresh();
        }

        public void StopAllDiscoveries()
        {
            foreach (var p in _providers) { try { p.StopDiscovery(); } catch { } }
        }

        public IReadOnlyList<XRDeviceDescriptor> GetDevices()
        {
            var list = new List<XRDeviceDescriptor>();
            foreach (var m in _merged) list.Add(m.Descriptor);
            return list;
        }

        public IReadOnlyList<DeviceIdentityResolver.MergedDevice> GetMerged() => _merged;

        private void Refresh()
        {
            var results = new List<(string, IEnumerable<XRDeviceDescriptor>)>();
            foreach (var p in _providers)
            {
                try { results.Add((p.ProviderId, p.GetDevices())); }
                catch { }
            }
            _merged = DeviceIdentityResolver.Merge(results);
            if (_known != null) _known.Annotate(_merged);
        }
    }
}
