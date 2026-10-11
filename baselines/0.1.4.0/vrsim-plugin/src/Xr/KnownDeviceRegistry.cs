// KnownDeviceRegistry.cs — "my devices" persistence (ARCH-DEVICE-01 PART E).
// Stores user-named, trusted devices across sessions; offline devices stay visible
// with lastSeen. Minimal hand-rolled JSON (no external dependency in the plugin).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace VRSIM.XR
{
    public sealed class KnownDeviceRecord
    {
        public string PersistentId;
        public string Nickname;
        public string LastKnownModel;
        public string LastConnectionType;
        public DateTime? LastSeen;
        public string PreferredConnection;
        public bool AutoConnect;
        public bool Trusted;
    }

    public sealed class KnownDeviceRegistry
    {
        private readonly string _path;
        private readonly Dictionary<string, KnownDeviceRecord> _byId = new Dictionary<string, KnownDeviceRecord>();

        public KnownDeviceRegistry(string directory)
        {
            Directory.CreateDirectory(directory);
            _path = Path.Combine(directory, "known-devices.json");
            Load();
        }

        public IReadOnlyCollection<KnownDeviceRecord> All => _byId.Values;

        public KnownDeviceRecord GetOrCreate(string persistentId)
        {
            if (string.IsNullOrEmpty(persistentId)) return null;
            if (!_byId.TryGetValue(persistentId, out var rec))
            {
                rec = new KnownDeviceRecord { PersistentId = persistentId, Trusted = false, AutoConnect = false };
                _byId[persistentId] = rec;
            }
            return rec;
        }

        /// <summary>Overlay "known" info onto merged devices (nickname etc.).</summary>
        public void Annotate(List<DeviceIdentityResolver.MergedDevice> merged)
        {
            foreach (var m in merged)
            {
                var d = m.Descriptor;
                var key = d.PersistentId ?? d.DeviceId;
                if (key != null && _byId.TryGetValue(key, out var rec))
                {
                    d.PersistentId = rec.PersistentId;
                    if (!string.IsNullOrEmpty(rec.Nickname)) d.DisplayName = rec.Nickname;
                }
            }
        }

        public void Save()
        {
            var sb = new StringBuilder("{\"devices\":[");
            bool first = true;
            foreach (var r in _byId.Values)
            {
                if (!first) sb.Append(',');
                first = false;
                sb.Append("{\"persistentId\":\"").Append(Esc(r.PersistentId))
                  .Append("\",\"nickname\":\"").Append(Esc(r.Nickname))
                  .Append("\",\"lastKnownModel\":\"").Append(Esc(r.LastKnownModel))
                  .Append("\",\"lastConnectionType\":\"").Append(Esc(r.LastConnectionType))
                  .Append("\",\"lastSeen\":").Append(r.LastSeen.HasValue ? "\"" + r.LastSeen.Value.ToString("o") + "\"" : "null")
                  .Append(",\"preferredConnection\":\"").Append(Esc(r.PreferredConnection))
                  .Append("\",\"autoConnect\":").Append(r.AutoConnect ? "true" : "false")
                  .Append(",\"trusted\":").Append(r.Trusted ? "true" : "false").Append("}");
            }
            sb.Append("]}");
            File.WriteAllText(_path, sb.ToString());
        }

        private void Load()
        {
            try
            {
                if (!File.Exists(_path)) return;
                var json = File.ReadAllText(_path);
                // minimal parse of our own fixed shape
                int i = json.IndexOf("\"devices\":[");
                if (i < 0) return;
                foreach (var part in json.Substring(i + 11).TrimEnd('}', ']').Split('{'))
                {
                    if (part.Length < 5) continue;
                    var rec = new KnownDeviceRecord
                    {
                        PersistentId = Grab(part, "persistentId"),
                        Nickname = Grab(part, "nickname"),
                        LastKnownModel = Grab(part, "lastKnownModel"),
                        LastConnectionType = Grab(part, "lastConnectionType"),
                        PreferredConnection = Grab(part, "preferredConnection"),
                        AutoConnect = part.Contains("\"autoConnect\":true"),
                        Trusted = part.Contains("\"trusted\":true"),
                    };
                    if (rec.PersistentId != null) _byId[rec.PersistentId] = rec;
                }
            }
            catch { /* corrupted registry must never break the plugin */ }
        }

        private static string Grab(string src, string field)
        {
            int i = src.IndexOf("\"" + field + "\":\"");
            if (i < 0) return null;
            int s = i + field.Length + 5;
            int e = src.IndexOf('"', s);
            return e < 0 ? null : src.Substring(s, e - s);
        }

        private static string Esc(string s)
        {
            if (s == null) return "";
            return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
        }
    }
}
