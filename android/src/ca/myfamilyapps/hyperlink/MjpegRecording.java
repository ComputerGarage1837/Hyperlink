package ca.myfamilyapps.hyperlink;

import java.io.*;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

final class MjpegRecording implements AutoCloseable {
    private static final long LIMIT = 1024L * 1024 * 1024;
    private static final class Frame {
        final byte[] bytes; final long milliseconds; boolean audio;
        Frame(byte[] bytes, long milliseconds) { this.bytes = bytes; this.milliseconds = milliseconds; }
    }
    private final ArrayBlockingQueue<Frame> frames = new ArrayBlockingQueue<>(4);
    private final AtomicBoolean stopped = new AtomicBoolean();
    private final long started = System.nanoTime();
    private final File temporary, destination;
    private final int width, height;
    final CompletableFuture<File> completion = new CompletableFuture<>();
    volatile boolean storageLimit;

    private final byte[] audioId;
    MjpegRecording(File folder, int width, int height) throws IOException { this(folder,width,height,null); }
    MjpegRecording(File folder, int width, int height, byte[] audio) throws IOException {
        if(audio!=null && audio.length!=16)throw new IOException("Invalid recording audio stream");
        audioId=audio==null?null:audio.clone();
        if (width < 1 || width > 1600 || height < 1 || height > 1000) throw new IOException("Unsupported recording dimensions");
        if (!folder.isDirectory() && !folder.mkdirs()) throw new IOException("Recording storage is unavailable");
        this.width = width; this.height = height;
        String id = UUID.randomUUID().toString().replace("-", "");
        temporary = new File(folder, ".hyperlink-" + id + ".part");
        destination = new File(folder, "Hyperlink-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + "-" + id.substring(0, 8) + ".mkv");
        if (destination.exists() || !temporary.createNewFile()) throw new IOException("Choose new recording storage");
        Thread worker = new Thread(this::write, "Hyperlink recording"); worker.setDaemon(true); worker.start();
    }
    void accept(byte[] jpeg) {
        if (stopped.get()) return;
        if (!frames.offer(new Frame(jpeg, (System.nanoTime() - started) / 1000000))) { storageLimit = true; close(); }
    }
    void acceptAudio(byte[] packet) throws IOException {
        if(audioId==null||stopped.get())return;
        if(packet==null||packet.length<20||packet.length>8208||(packet.length-16)%4!=0)throw new IOException("Invalid recording audio packet");
        for(int i=0;i<16;i++)if(packet[i]!=audioId[i])throw new IOException("Recording audio stream changed");
        Frame frame=new Frame(java.util.Arrays.copyOfRange(packet,16,packet.length),(System.nanoTime()-started)/1000000);frame.audio=true;
        if(!frames.offer(frame)){storageLimit=true;close();}
    }
    public void close() { stopped.set(true); }
    private void write() {
        File completed = null; Throwable failure = null;
        try {
            int count = 0; long first = -1;
            try (FileOutputStream file = new FileOutputStream(temporary); BufferedOutputStream output = new BufferedOutputStream(file, 65536)) {
                header(output, width, height, audioId!=null); long length = 512;
                while (!stopped.get() || !frames.isEmpty()) {
                    Frame frame = frames.poll(50, TimeUnit.MILLISECONDS); if (frame == null) continue;
                    if (length + frame.bytes.length + 64 > LIMIT) { storageLimit = true; close(); break; }
                    if (first < 0) first = frame.milliseconds;
                    if(frame.audio)audioSample(output,frame.bytes,frame.milliseconds-first);else {sample(output, frame.bytes, frame.milliseconds - first);count++;} length += frame.bytes.length + 64;
                }
                output.flush(); file.getFD().sync();
            }
            if (count == 0) throw new IOException("No frames arrived during recording");
            publish(temporary,destination); completed = destination;
        } catch (Throwable ex) { failure = ex; }
        finally {
            close(); frames.clear();
            if (completed == null && temporary.exists() && !temporary.delete()) failure = new IOException("Recording failed and its private temporary file could not be removed", failure);
        }
        if (failure == null) completion.complete(completed); else completion.completeExceptionally(failure);
    }
    static void header(OutputStream output, int width, int height) throws IOException { header(output,width,height,false); }
    static void header(OutputStream output, int width, int height, boolean audio) throws IOException {
        output.write(element(0x1A45DFA3, join(number(0x4286, 1), number(0x42F7, 1), number(0x42F2, 4), number(0x42F3, 8), text(0x4282, "matroska"), number(0x4287, 4), number(0x4285, 2))));
        output.write(new byte[] { 0x18, 0x53, (byte)0x80, 0x67, 1, -1, -1, -1, -1, -1, -1, -1 });
        output.write(element(0x1549A966, join(number(0x2AD7B1, 1000000), text(0x4D80, "Hyperlink"), text(0x5741, "Hyperlink"))));
        byte[] video=element(0xAE, join(number(0xD7, 1), number(0x73C5, 1), number(0x83, 1), text(0x86, "V_MJPEG"), element(0xE0, join(number(0xB0, width), number(0xBA, height)))));
        byte[] sound=audio?element(0xAE,join(number(0xD7,2),number(0x73C5,2),number(0x83,2),text(0x86,"A_PCM/INT/LIT"),element(0xE1,join(element(0xB5,java.nio.ByteBuffer.allocate(8).putDouble(48000.0).array()),number(0x9F,2),number(0x6264,16))))):new byte[0];
        output.write(element(0x1654AE6B,join(video,sound)));
    }
    static void publish(File temporary,File destination) throws IOException { if(destination.exists())throw new java.nio.file.FileAlreadyExistsException(destination.getName());Files.move(temporary.toPath(),destination.toPath()); }
    static void audioSample(OutputStream output,byte[] pcm,long milliseconds)throws IOException {
        if(pcm==null||pcm.length<4||pcm.length>8192||pcm.length%4!=0||milliseconds<0)throw new IOException("Invalid recording audio sample");
        output.write(element(0x1F43B675,join(number(0xE7,milliseconds),element(0xA3,join(new byte[]{(byte)0x82,0,0,(byte)0x80},pcm)))));
    }
    static void sample(OutputStream output, byte[] jpeg, long milliseconds) throws IOException {
        if (jpeg == null || jpeg.length < 4 || jpeg.length > 4 * 1024 * 1024 || jpeg[0] != (byte)255 || jpeg[1] != (byte)216 || jpeg[jpeg.length - 2] != (byte)255 || jpeg[jpeg.length - 1] != (byte)217 || milliseconds < 0) throw new IOException("Invalid recording frame");
        output.write(element(0x1F43B675, join(number(0xE7, milliseconds), element(0xA3, join(new byte[] { (byte)0x81, 0, 0, (byte)0x80 }, jpeg)))));
    }
    private static byte[] text(int id, String value) throws IOException { return element(id, value.getBytes(java.nio.charset.StandardCharsets.UTF_8)); }
    private static byte[] number(int id, long value) throws IOException {
        if (value < 0) throw new IOException("Invalid timestamp");
        int count = 1; while (count < 8 && (value >>> (count * 8)) != 0) count++;
        byte[] bytes = new byte[count]; for (int i = 0; i < count; i++) bytes[count - i - 1] = (byte)(value >>> (i * 8)); return element(id, bytes);
    }
    private static byte[] element(int id, byte[] value) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        int bytes = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 255 ? 2 : 1;
        for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) output.write(id >>> shift);
        int size = 1; while (value.length >= (1L << (size * 7)) - 1) size++;
        long encoded = value.length | (1L << (size * 7));
        for (int shift = (size - 1) * 8; shift >= 0; shift -= 8) output.write((int)(encoded >>> shift));
        output.write(value); return output.toByteArray();
    }
    private static byte[] join(byte[]... values) throws IOException { ByteArrayOutputStream output = new ByteArrayOutputStream(); for (byte[] value : values) output.write(value); return output.toByteArray(); }
}
