using System;
using System.Diagnostics;
using System.Security.Cryptography;

namespace Hyperlink
{
    public sealed partial class Host
    {
        readonly PinGate pinGate = new PinGate();
    }
    sealed class PinGate
    {
        readonly object sync = new object();
        readonly Stopwatch clock = Stopwatch.StartNew();
        int failures; long blockedUntil;
        internal static bool Valid(string pin)
        {
            if (pin == null || pin.Length < 6 || pin.Length > 12) return false;
            foreach (char c in pin) if (c < '0' || c > '9') return false;
            return true;
        }
        static byte[] Hash(string pin, byte[] salt)
        {
            using (var derive = new Rfc2898DeriveBytes(pin, salt, 600000, HashAlgorithmName.SHA256)) return derive.GetBytes(32);
        }
        internal static void Configure(Settings settings, string pin)
        {
            if (!Valid(pin)) throw new ArgumentException("Choose a PIN with 6–12 digits.");
            byte[] salt = new byte[16]; using (var random = RandomNumberGenerator.Create()) random.GetBytes(salt);
            settings.PinSalt = Convert.ToBase64String(salt); settings.PinHash = Convert.ToBase64String(Hash(pin, salt));
            settings.Unattended = true;
        }
        internal bool Check(Settings settings, string pin)
        {
            lock (sync)
            {
                if (!settings.Unattended || blockedUntil > clock.ElapsedMilliseconds) return false;
                bool match = false;
                if (Valid(pin))
                {
                    try
                    {
                        byte[] salt = Convert.FromBase64String(settings.PinSalt ?? ""), expected = Convert.FromBase64String(settings.PinHash ?? "");
                        if (salt.Length == 16 && expected.Length == 32)
                        {
                            byte[] actual = Hash(pin, salt); int difference = 0;
                            for (int i = 0; i < 32; i++) difference |= actual[i] ^ expected[i];
                            match = difference == 0; Array.Clear(actual, 0, actual.Length);
                        }
                    }
                    catch (FormatException) { }
                }
                if (match) { failures = 0; return true; }
                if (++failures >= 5) { failures = 0; blockedUntil = clock.ElapsedMilliseconds + 10 * 60 * 1000; }
                return false;
            }
        }
        internal void Reset() { lock (sync) { failures = 0; blockedUntil = 0; } }
    }
}
