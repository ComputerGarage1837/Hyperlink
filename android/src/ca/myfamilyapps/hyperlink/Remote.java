package ca.myfamilyapps.hyperlink;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.*;

final class Remote implements Closeable {
    interface AudioSink { void accept(byte[] packet); void close(); }
    private final ReplyInbox replies = new ReplyInbox();
    volatile boolean extensions;
    volatile AudioSink audioSink;
    final java.util.concurrent.atomic.AtomicReference<MjpegRecording> recording = new java.util.concurrent.atomic.AtomicReference<>();
    boolean attachRecording(MjpegRecording candidate) {
        if (closed.get() || !recording.compareAndSet(null,candidate)) return false;
        if (closed.get()) { if(recording.compareAndSet(candidate,null))candidate.close(); return false; }
        return true;
    }
    interface Listener { void ready(int width, int height, boolean control, boolean text); void frame(byte[] jpeg) throws Exception; void ended(String reason); }
    private final Identity identity;
    private final Listener listener;
    private final AtomicBoolean closed = new AtomicBoolean();
    private volatile PinnedWire wire;
    private final ThreadPoolExecutor writer = new ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS,
        new ArrayBlockingQueue<Runnable>(64), task -> { Thread t = new Thread(task, "Hyperlink input"); t.setDaemon(true); return t; });
    private volatile ScheduledExecutorService heartbeat;
    volatile boolean control, textInput;
    int width, height;
    Remote(Identity identity, Listener listener) { this.identity = identity; this.listener = listener; }
    static JSONObject invitation(String text) throws Exception {
        text = text.trim(); if (!text.startsWith("hlink1:") || text.length() > 8192) throw new IOException("Paste a Hyperlink host invitation");
        JSONObject value = Json.parse(Base64.getUrlDecoder().decode(text.substring(7)), 8192);
        validatePeer(value);
        if (!value.getString("Code").matches("[A-Za-z0-9_-]{32}")) throw new IOException("Invalid invitation code");
        long expires = Math.subtractExact(value.getLong("Expires"), 621355968000000000L)/10000;
        long now = System.currentTimeMillis();
        if (expires <= now || expires > now+330000) throw new IOException("Invitation expired, or the device clock differs");
        return value;
    }
    private static void validatePeer(JSONObject value) throws Exception {
        if (!value.getString("Fingerprint").matches("[a-f0-9]{64}") || !value.getString("Id").matches("[a-f0-9]{64}")) throw new IOException("Invalid host identity");
        String address = value.getString("Address"); int port = value.getInt("Port");
        if (address.length() < 1 || address.length() > 253 || port < 1 || port > 65535) throw new IOException("Invalid host address");
    }
    private static void send(PinnedWire wire, JSONObject value) throws IOException { wire.write(1, value.toString().getBytes(StandardCharsets.UTF_8)); }
    private static JSONObject json(PinnedWire wire) throws Exception {
        PinnedWire.Packet packet = wire.read(); if (packet.kind != 1) throw new IOException("Expected host authorization"); return Json.parse(packet.bytes, 16384);
    }
    private static void authenticate(PinnedWire wire, Identity identity, JSONObject hello, String fingerprint) throws Exception {
        send(wire, hello); JSONObject challenge = json(wire);
        if (!challenge.optString("kind").equals("challenge")) throw new IOException("The host has not approved this identity or invitation");
        String nonce = challenge.getString("nonce");
        if (!nonce.matches("[A-Za-z0-9_-]{32,128}")) throw new IOException("Invalid host challenge");
        byte[] data = ("Hyperlink/1\n"+nonce+"\n"+fingerprint+"\n"+identity.id).getBytes(StandardCharsets.UTF_8);
        send(wire, Json.object("signature", Base64.getEncoder().encodeToString(Signing.sign(identity.privateKey, data))));
    }
    static JSONObject pair(Identity identity, JSONObject invite, String name) throws Exception {
        validatePeer(invite);
        try (PinnedWire wire = new PinnedWire(invite.getString("Address"), invite.getInt("Port"), invite.getString("Fingerprint"))) {
            authenticate(wire, identity, Json.object("version", 1, "operation", "pair", "id", identity.id, "publicKey", identity.xml, "name", name, "code", invite.getString("Code")), invite.getString("Fingerprint"));
            JSONObject reply = json(wire); if (!reply.optString("kind").equals("paired")) throw new IOException("The owner did not approve pairing");
            return Json.object("Address", invite.getString("Address"), "Port", invite.getInt("Port"), "Fingerprint", invite.getString("Fingerprint"),
                "Id", invite.getString("Id"), "Name", reply.optString("name", invite.optString("Name", "Computer")));
        }
    }
    void connect(JSONObject peer) {
        Thread reader = new Thread(() -> {
            try {
                validatePeer(peer);
                PinnedWire connection = new PinnedWire(peer.getString("Address"), peer.getInt("Port"), peer.getString("Fingerprint")); wire = connection;
                if (closed.get()) { connection.close(); return; }
                authenticate(connection, identity, Json.object("version", 1, "operation", "connect", "id", identity.id), peer.getString("Fingerprint"));
                JSONObject reply = json(connection);
                if (!reply.optString("kind").equals("accepted")) throw new IOException("The owner did not approve this session");
                width = reply.getInt("width"); height = reply.getInt("height"); control = reply.getBoolean("control"); textInput = reply.optBoolean("textInput", false); extensions = reply.optBoolean("extensions", false);
                if (width < 1 || width > 1600 || height < 1 || height > 1000) throw new IOException("Unsupported host display");
                connection.timeout(8000); if (closed.get()) return; listener.ready(width, height, control, textInput);
                heartbeat = Executors.newSingleThreadScheduledExecutor(task -> { Thread t = new Thread(task, "Hyperlink heartbeat"); t.setDaemon(true); return t; });
                if (closed.get()) { heartbeat.shutdownNow(); return; } heartbeat.scheduleAtFixedRate(() -> { try { enqueue(Json.object("kind", "ping")); } catch (JSONException e) { finish("Invalid keepalive"); } }, 2, 2, TimeUnit.SECONDS);
                while (!closed.get()) {
                    PinnedWire.Packet packet = connection.read();
                if (packet.kind == 10) {listener.frame(packet.bytes);MjpegRecording capture=recording.get();if(capture!=null)capture.accept(packet.bytes);}
                    else if (packet.kind == 11) { AudioSink sink = audioSink; if (sink != null) sink.accept(packet.bytes); MjpegRecording capture=recording.get();if(capture!=null)capture.acceptAudio(packet.bytes); }
                    else {
                        JSONObject event = Json.parse(packet.bytes, 16384);
                        if (event.optString("kind").equals("extension")) {
                            String request = event.getString("request");
                            if (!request.matches("[a-f0-9]{32}")) throw new IOException("Invalid session tool reply");
                            replies.accept(request, packet.bytes);
                        } else if (!event.optString("kind").equals("pong")) throw new IOException("The host ended this session");
                    }
                }
            } catch (Exception e) { finish("Connection ended. The host may be offline, access may have ended, or a secure connection could not be established."); }
        }, "Hyperlink desktop"); reader.setDaemon(true); reader.start();
    }
    private void enqueue(JSONObject packet) {
        if (closed.get() || wire == null) return;
        try { writer.execute(() -> { try { if (!closed.get()) send(wire, packet); } catch (IOException e) { finish("Connection interrupted"); } }); }
        catch (RejectedExecutionException e) { finish("Connection ended because input could not be delivered safely"); }
    }
    volatile JSONObject audioFormat;
    private final Object audioLock=new Object();
    JSONObject extension(String operation, Object... values) throws Exception {
        if(!operation.equals("audio-start")&&!operation.equals("audio-stop"))return extensionCore(operation,values);
        synchronized(audioLock){
            if(operation.equals("audio-start")&&audioFormat!=null)throw new IOException("Audio is already active for playback or recording");
            JSONObject result=extensionCore(operation,values);audioFormat=operation.equals("audio-start")?result:null;return result;
        }
    }
    private JSONObject extensionCore(String operation, Object... values) throws Exception {
        if (!extensions || closed.get()) throw new IOException("This host does not offer session tools");
        String request = java.util.UUID.randomUUID().toString().replace("-", "");
        CompletableFuture<byte[]> future = replies.register(request);
        try {
            JSONObject packet = Json.object(values);
            packet.put("kind", "extension"); packet.put("request", request); packet.put("operation", operation);
            enqueue(packet);
            JSONObject reply = Json.parse(replies.await(request, future), 16384);
            if (!reply.getBoolean("ok")) throw new IOException("The host denied this operation. Check its owner permissions.");
            return reply.getJSONObject("result");
        } finally { replies.abandon(request, future); }
    }
    void input(Object... values) {
        if (!control || closed.get()) return;
        try { JSONObject packet = Json.object(values); packet.put("kind", "input"); enqueue(packet); }
        catch (JSONException e) { finish("Invalid input"); }
    }
    void key(int key, boolean down) { input("type", "key", "key", key, "down", down ? 1 : 0); }
    void stroke(int key) { key(key, true); key(key, false); }
    void click(int button) { input("type", "button", "button", button, "down", 1); input("type", "button", "button", button, "down", 0); }
    void release() { input("type", "release"); }
    private void finish(String reason) {
        if (!closed.compareAndSet(false, true)) return;
        replies.close(); AudioSink sink = audioSink; audioSink = null; if (sink != null) sink.close();
        MjpegRecording capture=recording.getAndSet(null);if(capture!=null)capture.close();
        writer.shutdownNow(); if (heartbeat != null) heartbeat.shutdownNow();
        PinnedWire connection = wire; if (connection != null) { Thread closer = new Thread(() -> { try { connection.close(); } catch (IOException ignored) {} }, "Hyperlink disconnect"); closer.setDaemon(true); closer.start(); }
        listener.ended(reason);
    }
    public void close() { finish("Disconnected"); }
}
