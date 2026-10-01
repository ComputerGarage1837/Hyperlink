using System;
using System.Net;

namespace Hyperlink
{
    static class WakeTests
    {
        internal static string Run()
        {
            byte[] packet = WakeOnLan.Packet("00:11:22:33:44:55");
            if (packet.Length != 102) throw new Exception("Wake packet length.");
            for (int i = 0; i < 6; i++) if (packet[i] != 255) throw new Exception("Wake synchronization prefix.");
            for (int i = 0; i < 16; i++) for (int j = 0; j < 6; j++) if (packet[6 + i * 6 + j] != j * 17) throw new Exception("Wake target repetition.");
            if (Convert.ToBase64String(packet) != Convert.ToBase64String(WakeOnLan.Packet("00-11-22-33-44-55"))) throw new Exception("Wake address formats.");
            foreach (string invalid in new[] { "", "00112233445", "00:11:22:33:44:GG", "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "01:11:22:33:44:55" })
            {
                bool denied = false; try { WakeOnLan.Packet(invalid); } catch (ArgumentException) { denied = true; }
                if (!denied) throw new Exception("Unsafe wake address accepted.");
            }
            if (!WakeOnLan.Broadcast(IPAddress.Parse("192.168.10.22"), IPAddress.Parse("255.255.255.0")).Equals(IPAddress.Parse("192.168.10.255"))) throw new Exception("Wake /24 subnet.");
            if (!WakeOnLan.Broadcast(IPAddress.Parse("10.2.3.4"), IPAddress.Parse("255.255.0.0")).Equals(IPAddress.Parse("10.2.255.255"))) throw new Exception("Wake /16 subnet.");
            foreach (string mask in new[] { "0.0.0.0", "255.255.255.255", "255.255.255.254", "255.0.255.0" })
            {
                bool denied = false; try { WakeOnLan.Broadcast(IPAddress.Parse("10.2.3.4"), IPAddress.Parse(mask)); } catch (ArgumentException) { denied = true; }
                if (!denied) throw new Exception("Unsafe wake subnet accepted.");
            }
            return "Wake-on-LAN packet, target validation and subnet checks passed; no wake requests were sent.";
        }
    }
}
