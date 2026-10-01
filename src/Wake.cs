using System;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    static class WakeOnLan
    {
        internal static byte[] Packet(string address)
        {
            string compact = (address ?? "").Trim().Replace(":", "").Replace("-", "");
            if (compact.Length != 12 || compact.Any(c => !Uri.IsHexDigit(c))) throw new ArgumentException("Enter six hexadecimal pairs, such as 00:11:22:33:44:55.");
            byte[] mac = Enumerable.Range(0, 6).Select(i => Convert.ToByte(compact.Substring(i * 2, 2), 16)).ToArray();
            if (mac.All(b => b == 0) || (mac[0] & 1) != 0) throw new ArgumentException("Enter the computer's unicast network adapter address.");
            byte[] packet = new byte[102];
            for (int i = 0; i < 6; i++) packet[i] = 255;
            for (int i = 0; i < 16; i++) Buffer.BlockCopy(mac, 0, packet, 6 + i * 6, 6);
            return packet;
        }
        internal static IPAddress Broadcast(IPAddress address, IPAddress mask)
        {
            if (address.AddressFamily != AddressFamily.InterNetwork || mask.AddressFamily != AddressFamily.InterNetwork) throw new ArgumentException("An IPv4 network is required.");
            byte[] ip = address.GetAddressBytes(), subnet = mask.GetAddressBytes();
            uint bits = 0; foreach (byte b in subnet) bits = (bits << 8) | b;
            uint inverse = ~bits;
            if (bits == 0 || inverse < 3 || (inverse & (inverse + 1)) != 0) throw new ArgumentException("This interface does not have a usable broadcast subnet.");
            for (int i = 0; i < 4; i++) ip[i] = (byte)(ip[i] | ~subnet[i]);
            return new IPAddress(ip);
        }
        internal static void Send(byte[] packet, IPAddress local, IPAddress broadcast)
        {
            using (var socket = new UdpClient(new IPEndPoint(local, 0)))
            {
                socket.EnableBroadcast = true;
                for (int i = 0; i < 3; i++) socket.Send(packet, packet.Length, new IPEndPoint(broadcast, 9));
            }
        }
    }
    sealed class WakePanel : TableLayoutPanel
    {
        sealed class Network
        {
            public string Name; public IPAddress Address, Broadcast;
            public override string ToString() { return Name + " — " + Address; }
        }
        public WakePanel()
        {
            Dock = DockStyle.Top; Height = 360; Padding = new Padding(24); ColumnCount = 1;
            Controls.Add(Theme.Label("Wake a computer on your home network", 18, Theme.Text));
            var instructions = Theme.Label("Enable Wake-on-LAN in the target computer's firmware and network adapter settings. Enter that adapter's physical address. Sending a request does not confirm the computer woke up.", 11, Theme.Muted);
            instructions.AutoSize = false; instructions.Height = 75; instructions.Dock = DockStyle.Top; Controls.Add(instructions);
            var networks = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Top, BackColor = Theme.Card, ForeColor = Theme.Text, DrawMode = DrawMode.OwnerDrawFixed };
            networks.DrawItem += delegate(object sender, DrawItemEventArgs e)
            {
                e.DrawBackground();
                if (e.Index >= 0) TextRenderer.DrawText(e.Graphics, networks.Items[e.Index].ToString(), networks.Font, e.Bounds, Theme.Text, TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
                e.DrawFocusRectangle();
            };
            foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (adapter.OperationalStatus != OperationalStatus.Up || (adapter.NetworkInterfaceType != NetworkInterfaceType.Ethernet && adapter.NetworkInterfaceType != NetworkInterfaceType.Wireless80211)) continue;
                foreach (var ip in adapter.GetIPProperties().UnicastAddresses)
                {
                    if (ip.Address.AddressFamily != AddressFamily.InterNetwork || IPAddress.IsLoopback(ip.Address)) continue;
                    try { networks.Items.Add(new Network { Name = adapter.Name, Address = ip.Address, Broadcast = WakeOnLan.Broadcast(ip.Address, ip.IPv4Mask) }); } catch (ArgumentException) { }
                }
            }
            if (networks.Items.Count > 0) networks.SelectedIndex = 0;
            Controls.Add(networks);
            var address = Theme.Field(""); address.Dock = DockStyle.Top; Controls.Add(Theme.Label("Target adapter address (MAC)", 11, Theme.Muted)); Controls.Add(address);
            var status = Theme.Label("", 11, Theme.Muted); status.AutoSize = false; status.Height = 55; status.Dock = DockStyle.Top;
            var send = Theme.Button("Send wake request", true); send.Dock = DockStyle.Top; send.Enabled = networks.Items.Count > 0;
            send.Click += async delegate
            {
                try
                {
                    byte[] packet = WakeOnLan.Packet(address.Text); var network = (Network)networks.SelectedItem;
                    send.Enabled = false;
                    await Task.Run(() => WakeOnLan.Send(packet, network.Address, network.Broadcast));
                    if (!IsDisposed) status.Text = "Wake request sent. Allow a moment, then connect from Computers.";
                }
                catch (Exception ex) { if (!IsDisposed) status.Text = ex.Message; }
                finally { if (!IsDisposed) send.Enabled = networks.Items.Count > 0; }
            };
            Controls.Add(send); Controls.Add(status);
            if (networks.Items.Count == 0) status.Text = "No active IPv4 home-network adapter is available.";
        }
    }
}
