package ca.myfamilyapps.hyperlink;

final class WakePacket {
    static byte[] create(String address) {
        String compact = address == null ? "" : address.trim().replace(":", "").replace("-", "");
        if (!compact.matches("[0-9a-fA-F]{12}")) throw new IllegalArgumentException("Enter six hexadecimal pairs, such as 00:11:22:33:44:55.");
        byte[] mac = new byte[6]; boolean nonzero = false;
        for (int i = 0; i < 6; i++) { mac[i] = (byte)Integer.parseInt(compact.substring(i * 2, i * 2 + 2), 16); nonzero |= mac[i] != 0; }
        if (!nonzero || (mac[0] & 1) != 0) throw new IllegalArgumentException("Enter the computer's unicast network adapter address.");
        byte[] packet = new byte[102];
        java.util.Arrays.fill(packet, 0, 6, (byte)255);
        for (int i = 0; i < 16; i++) System.arraycopy(mac, 0, packet, 6 + i * 6, 6);
        return packet;
    }
}
