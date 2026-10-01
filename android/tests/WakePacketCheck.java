package ca.myfamilyapps.hyperlink;

public final class WakePacketCheck {
    public static void main(String[] args) {
        byte[] packet = WakePacket.create("00:11:22:33:44:55");
        if (packet.length != 102) throw new AssertionError("Packet length");
        for (int i = 0; i < 6; i++) if (packet[i] != (byte)255) throw new AssertionError("Prefix");
        for (int i = 0; i < 16; i++) for (int j = 0; j < 6; j++) if (packet[6 + i * 6 + j] != j * 17) throw new AssertionError("Target repetition");
        if (!java.util.Arrays.equals(packet, WakePacket.create("00-11-22-33-44-55"))) throw new AssertionError("Address formats");
        for (String invalid : new String[] { "", "00112233445", "00:11:22:33:44:GG", "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "01:11:22:33:44:55" }) {
            boolean denied = false; try { WakePacket.create(invalid); } catch (IllegalArgumentException ex) { denied = true; }
            if (!denied) throw new AssertionError("Invalid target accepted");
        }
        System.out.println("Android wake packet and target checks passed; no wake requests were sent.");
    }
}
