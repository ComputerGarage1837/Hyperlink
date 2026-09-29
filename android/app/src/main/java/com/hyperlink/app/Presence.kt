package com.hyperlink.app

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

/** What a host said about itself in answer to a discovery query. */
data class HostPresence(
    val address: String,
    val hostId: String,
    val name: String,
    val version: String,
    val port: Int,
    val pinRequired: Boolean,
    val clients: Int,
    val streams: Int,
    val seenAt: Long,
)

/**
 * Finds hosts on the network (broadcast) and checks whether saved hosts are online (unicast,
 * which also works over Tailscale/VPN). Results are delivered on the main thread.
 */
class Presence(private val ctx: Context, private val onUpdate: (Map<String, HostPresence>) -> Unit) {
    @Volatile private var running = false
    @Volatile var targets: List<String> = emptyList()
    private val main = Handler(Looper.getMainLooper())
    private val seen = HashMap<String, HostPresence>()
    private val resolved = java.util.concurrent.ConcurrentHashMap<String, String>()

    /** Presence for a saved device: by host id, else by its address (names are resolved). */
    fun find(all: Map<String, HostPresence>, address: String, hostId: String): HostPresence? {
        if (hostId.isNotEmpty()) all.values.firstOrNull { it.hostId == hostId }?.let { return it }
        return all[address] ?: resolved[address]?.let { all[it] }
    }

    fun start() {
        if (running) return
        running = true
        thread(name = "presence", isDaemon = true) { loop() }
    }

    fun stop() {
        running = false
    }

    private fun loop() {
        val wifi = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val lock = wifi.createMulticastLock("hyperlink-discovery").apply { setReferenceCounted(false) }
        val socket = runCatching { DatagramSocket().apply { broadcast = true; soTimeout = 250 } }.getOrNull() ?: return
        val query = QUERY.toByteArray()
        try {
            lock.acquire()
            while (running) {
                val round = System.currentTimeMillis()
                runCatching {
                    socket.send(DatagramPacket(query, query.size, InetAddress.getByName("255.255.255.255"), PORT))
                }
                for (t in targets) runCatching {
                    val addr = InetSocketAddress(t, PORT)
                    addr.address?.hostAddress?.let { resolved[t] = it }
                    socket.send(DatagramPacket(query, query.size, addr))
                }
                val buf = ByteArray(1024)
                while (running && System.currentTimeMillis() - round < 2500) {
                    val pkt = DatagramPacket(buf, buf.size)
                    try {
                        socket.receive(pkt)
                    } catch (_: SocketTimeoutException) {
                        continue
                    } catch (_: Exception) {
                        break
                    }
                    val p = parse(pkt.data, pkt.length, pkt.address.hostAddress ?: continue) ?: continue
                    synchronized(seen) { seen[p.address] = p }
                }
                // Forget hosts that stopped answering.
                val now = System.currentTimeMillis()
                val snapshot = synchronized(seen) {
                    seen.values.removeAll { now - it.seenAt > 8000 }
                    HashMap(seen)
                }
                main.post { if (running) onUpdate(snapshot) }
            }
        } finally {
            socket.close()
            runCatching { lock.release() }
        }
    }

    companion object {
        const val PORT = 47802
        private const val QUERY = "HYPERLINK?1"
        private const val REPLY = "HYPERLINK!1"

        fun parse(data: ByteArray, len: Int, from: String): HostPresence? {
            val tag = REPLY.toByteArray()
            if (len < tag.size || !data.copyOfRange(0, tag.size).contentEquals(tag)) return null
            val b = ByteBuffer.wrap(data, tag.size, len - tag.size).order(ByteOrder.LITTLE_ENDIAN)
            return runCatching {
                fun str(): String {
                    val n = b.short.toInt() and 0xffff
                    val bytes = ByteArray(n)
                    b.get(bytes)
                    return String(bytes, Charsets.UTF_8)
                }
                val hostId = str()
                val name = str()
                val version = str()
                val port = b.short.toInt() and 0xffff
                val pin = b.get().toInt() != 0
                val clients = b.short.toInt() and 0xffff
                val streams = b.short.toInt() and 0xffff
                HostPresence(from, hostId, name, version, port, pin, clients, streams, System.currentTimeMillis())
            }.getOrNull()
        }
    }
}
