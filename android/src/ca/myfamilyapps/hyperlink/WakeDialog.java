package ca.myfamilyapps.hyperlink;

import android.app.Activity;
import android.app.AlertDialog;
import android.widget.*;
import java.net.*;
import java.util.*;

final class WakeDialog {
    static final class Network {
        final String name; final InetAddress local, broadcast;
        Network(String name, InetAddress local, InetAddress broadcast) { this.name = name; this.local = local; this.broadcast = broadcast; }
        public String toString() { return name + " — " + local.getHostAddress(); }
    }
    static void show(Activity activity) {
        LinearLayout body = new LinearLayout(activity); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(24, 12, 24, 12);
        TextView help = new TextView(activity); help.setText("Enable Wake-on-LAN in the computer's firmware and network adapter settings. Your phone must be on the same home network. Sending a request does not confirm the computer woke up."); body.addView(help);
        List<Network> networks = new ArrayList<>();
        try {
            Enumeration<NetworkInterface> adapters = NetworkInterface.getNetworkInterfaces();
            while (adapters != null && adapters.hasMoreElements()) {
                NetworkInterface adapter = adapters.nextElement();
                if (!adapter.isUp() || adapter.isLoopback() || adapter.isPointToPoint()) continue;
                for (InterfaceAddress ip : adapter.getInterfaceAddresses())
                    if (ip.getAddress() instanceof Inet4Address && ip.getBroadcast() != null && ip.getNetworkPrefixLength() > 0 && ip.getNetworkPrefixLength() < 31)
                        networks.add(new Network(adapter.getDisplayName(), ip.getAddress(), ip.getBroadcast()));
            }
        } catch (Exception ignored) { }
        Spinner network = new Spinner(activity); network.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item, networks)); body.addView(network);
        EditText mac = new EditText(activity); mac.setSingleLine(true); mac.setHint("Target adapter address (MAC)"); body.addView(mac);
        TextView status = new TextView(activity); body.addView(status);
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle("Wake a computer").setView(body).setPositiveButton("Send request", null).setNegativeButton("Close", null).create();
        dialog.setOnShowListener(shown -> {
            Button send = dialog.getButton(AlertDialog.BUTTON_POSITIVE); send.setEnabled(!networks.isEmpty());
            if (networks.isEmpty()) status.setText("No active IPv4 broadcast network is available.");
            send.setOnClickListener(view -> {
                final byte[] packet;
                try { packet = WakePacket.create(mac.getText().toString()); } catch (IllegalArgumentException ex) { status.setText(ex.getMessage()); return; }
                Network selected = (Network)network.getSelectedItem(); send.setEnabled(false);
                new Thread(() -> {
                    String message;
                    try (DatagramSocket socket = new DatagramSocket(new InetSocketAddress(selected.local, 0))) {
                        socket.setBroadcast(true);
                        for (int i = 0; i < 3; i++) socket.send(new DatagramPacket(packet, packet.length, selected.broadcast, 9));
                        message = "Wake request sent. Allow a moment, then try connecting.";
                    } catch (Exception ex) { message = "Could not send the wake request: " + ex.getMessage(); }
                    final String result = message;
                    activity.runOnUiThread(() -> { if (!activity.isFinishing() && !activity.isDestroyed() && dialog.isShowing()) { status.setText(result); send.setEnabled(true); } });
                }, "Hyperlink-wake").start();
            });
        });
        dialog.show();
    }
}
