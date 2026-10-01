package ca.myfamilyapps.hyperlink;

import java.io.*;
import java.net.*;
import java.security.*;
import java.security.cert.*;
import javax.net.ssl.*;

/** Protocol 1: bounded framing over TLS 1.2 with an exact invited host pin. */
public final class PinnedWire implements Closeable {
    public static final class Packet {
        public final int kind; public final byte[] bytes;
        Packet(int kind, byte[] bytes) { this.kind = kind; this.bytes = bytes; }
    }
    private final SSLSocket socket;
    private final Socket transport;
    private final DataInputStream input;
    private final DataOutputStream output;
    private final Object writeLock = new Object();
    public PinnedWire(String address, int port, String fingerprint) throws IOException, GeneralSecurityException {
        if (address == null || address.length() < 1 || address.length() > 253 || port < 1 || port > 65535 ||
            fingerprint == null || !fingerprint.matches("[a-f0-9]{64}")) throw new IOException("Invalid host invitation");
        final byte[] expected = new byte[32];
        for (int i = 0; i < expected.length; i++) expected[i] = (byte)Integer.parseInt(fingerprint.substring(i*2, i*2+2), 16);
        X509TrustManager pinned = new X509TrustManager() {
            public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
            public void checkClientTrusted(X509Certificate[] chain, String auth) throws CertificateException { throw new CertificateException("Client certificates are not supported"); }
            public void checkServerTrusted(X509Certificate[] chain, String auth) throws CertificateException {
                if (chain == null || chain.length == 0) throw new CertificateException("Host certificate missing");
                chain[0].checkValidity();
                try {
                    if (!MessageDigest.isEqual(expected, Signing.hash(chain[0].getEncoded()))) throw new CertificateException("Host identity mismatch");
                } catch (GeneralSecurityException e) { throw new CertificateException("Host identity could not be verified", e); }
            }
        };
        SSLContext context = SSLContext.getInstance("TLS"); context.init(null, new TrustManager[] { pinned }, new SecureRandom());
        Socket raw = new Socket(); SSLSocket secure = null;
        try {
            raw.connect(new InetSocketAddress(address, port), 10000); raw.setTcpNoDelay(true);
            secure = (SSLSocket)context.getSocketFactory().createSocket(raw, address, port, true);
            secure.setEnabledProtocols(new String[] { "TLSv1.2" }); secure.setSoTimeout(10000); secure.startHandshake(); secure.setSoTimeout(120000);
            socket = secure; transport = raw; input = new DataInputStream(socket.getInputStream()); output = new DataOutputStream(socket.getOutputStream());
        } catch (IOException|RuntimeException e) {
            if (secure != null) try { secure.close(); } catch (IOException ignored) {}
            try { raw.close(); } catch (IOException ignored) {} throw e;
        }
    }
    public void timeout(int milliseconds) throws SocketException { socket.setSoTimeout(milliseconds); }
    public Packet read() throws IOException {
        int kind = input.readUnsignedByte(), size = input.readInt();
        if ((kind != 1 && kind != 10 && kind != 11) || size < 1 || size > (kind == 1 ? 16384 : kind == 11 ? 8208 : 4*1024*1024)) throw new IOException("Invalid packet size or type");
        byte[] bytes = new byte[size]; input.readFully(bytes); return new Packet(kind, bytes);
    }
    public void write(int kind, byte[] bytes) throws IOException {
        if (kind != 1 || bytes.length < 1 || bytes.length > 16384) throw new IOException("Invalid control packet");
        synchronized (writeLock) { output.writeByte(kind); output.writeInt(bytes.length); output.write(bytes); output.flush(); }
    }
    public void close() throws IOException { transport.close(); socket.close(); }
}
