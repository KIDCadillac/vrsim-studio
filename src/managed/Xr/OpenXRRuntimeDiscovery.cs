// OpenXRRuntimeDiscovery.cs — Provider 1 skeleton + diagnostic tool (ARCH-DEVICE-01 PART C/实验).
// Reads Windows OpenXR registry: ActiveRuntime + AvailableRuntimes. It discovers XR RUNTIMES,
// NEVER physical HMDs; AvailableRuntimes must never be reported as connected headsets (ADR-003).
// Writes runtime-discovery.json evidence next to the plugin diagnostics (read-only system access).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace VRSIM.XR
{
    public sealed class OpenXRRuntimeDiscovery : IXRDeviceDiscoveryProvider
    {
        public string ProviderId { get { return "openxr-runtime-registry"; } }

        public event Action<XRDeviceDescriptor> DeviceAdded;
        public event Action<XRDeviceDescriptor> DeviceUpdated;
        public event Action<string> DeviceRemoved;

        private readonly List<XRDeviceDescriptor> _runtimes = new List<XRDeviceDescriptor>();
        public IReadOnlyList<XRDeviceDescriptor> GetDevices() { return _runtimes; }

        public void StartDiscovery()
        {
            _runtimes.Clear();
            ReadHive("SOFTWARE\\Khronos\\OpenXR\\1", true);
            ReadHive("SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1", true);
            ReadHive("SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", false);
        }
        public void StopDiscovery() { }

        private void ReadHive(string subKey, bool readActiveValue)
        {
            try
            {
                using (var k = Microsoft.Win32.RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.LocalMachine,
                    Microsoft.Win32.RegistryView.Registry64).OpenSubKey(subKey))
                {
                    if (k == null) return;
                    if (readActiveValue)
                    {
                        var manifest = k.GetValue("ActiveRuntime") as string;
                        if (manifest != null) _runtimes.Add(FromManifest(manifest, active: true));
                    }
                    else
                    {
                        foreach (var name in k.GetValueNames())
                        {
                            // AvailableRuntimes subkey: value names are manifest paths
                            _runtimes.Add(FromManifest(name, active: false));
                        }
                        foreach (var sub in k.GetSubKeyNames())
                        {
                            using (var sk = k.OpenSubKey(sub))
                            {
                                var manifest = sk.GetValue("ActiveRuntime") as string;
                                if (manifest != null) _runtimes.Add(FromManifest(manifest, active: false));
                            }
                        }
                    }
                }
            }
            catch { /* registry shapes vary; missing data stays null */ }
        }

        /// <summary>Parse a runtime manifest JSON (file_format 1.0.0: { runtime: { name, library_path, ... } }).
        /// Missing fields stay null — never fabricated (measured: Oculus manifest has no runtime_version).</summary>
        private static XRDeviceDescriptor FromManifest(string manifestPath, bool active)
        {
            var d = new XRDeviceDescriptor
            {
                DeviceId = "runtime:" + manifestPath,
                DisplayName = null,
                RuntimePath = manifestPath,
                IsActiveRuntimeDevice = active,
                IsLocal = true,
                Connection = active ? ConnectionState.Available : ConnectionState.Discovered,
                Transport = ConnectionTransport.LocalRuntime,
                DeviceClass = DeviceClass.Unknown, // a RUNTIME is not a physical HMD
                LastSeen = DateTime.UtcNow,
            };
            try
            {
                var json = File.ReadAllText(manifestPath);
                int i = json.IndexOf("\"runtime\"");
                if (i >= 0) d.RuntimeName = Grab(json, i, "name");
                if (i >= 0) d.RuntimeVersion = Grab(json, i, "runtime_version");
            }
            catch { }
            return d;
        }

        private static string Grab(string src, int start, string field)
        {
            int i = src.IndexOf("\"" + field + "\":\"", start);
            if (i < 0) return null;
            int s = i + field.Length + 5;
            int e = src.IndexOf('"', s);
            return e < 0 ? null : src.Substring(s, e - s);
        }

        /// <summary>Diagnostic export: runtime-discovery.json (RUNTIMES, not headsets).</summary>
        public string ToJson()
        {
            var sb = new StringBuilder("{\"note\":\"XR RUNTIMES, not physical HMDs\",\"runtimes\":[");
            bool first = true;
            foreach (var r in _runtimes)
            {
                if (!first) sb.Append(',');
                first = false;
                sb.Append("{\"manifest\":\"").Append(Esc(r.RuntimePath))
                  .Append("\",\"name\":\"").Append(Esc(r.RuntimeName))
                  .Append("\",\"version\":\"").Append(Esc(r.RuntimeVersion))
                  .Append("\",\"isActive\":").Append(r.IsActiveRuntimeDevice ? "true" : "false").Append("}");
            }
            sb.Append("]}");
            return sb.ToString();
        }

        private static string Esc(string s)
        {
            if (s == null) return "";
            return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
        }
    }
}
