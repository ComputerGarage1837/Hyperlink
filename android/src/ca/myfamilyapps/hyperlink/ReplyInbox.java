package ca.myfamilyapps.hyperlink;

import java.io.IOException;
import java.util.concurrent.*;

/** Bounded request correlation, independent of keyboard-control permission. */
final class ReplyInbox implements AutoCloseable {
    private final ConcurrentHashMap<String, CompletableFuture<byte[]>> pending = new ConcurrentHashMap<>();
    private boolean closed;
    synchronized CompletableFuture<byte[]> register(String id) throws IOException {
        if (closed || pending.size() >= 4 || !id.matches("[a-f0-9]{32}")) throw new IOException("Session tools unavailable or busy");
        CompletableFuture<byte[]> future = new CompletableFuture<>();
        if (pending.putIfAbsent(id, future) != null) throw new IOException("Duplicate request");
        return future;
    }
    void accept(String id, byte[] reply) {
        CompletableFuture<byte[]> future = pending.get(id);
        if (future != null) future.complete(reply);
    }
    byte[] await(String id, CompletableFuture<byte[]> future) throws IOException, InterruptedException {
        try { return future.get(10, TimeUnit.SECONDS); }
        catch (ExecutionException | TimeoutException e) { throw new IOException("Session tool request failed or timed out", e); }
        finally { pending.remove(id, future); }
    }
    void abandon(String id, CompletableFuture<byte[]> future) { pending.remove(id, future); }
    public synchronized void close() {
        closed = true;
        for (CompletableFuture<byte[]> future : pending.values()) future.completeExceptionally(new IOException("Disconnected"));
        pending.clear();
    }
}
