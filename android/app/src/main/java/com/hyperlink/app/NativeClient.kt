package com.hyperlink.app

import android.view.Surface

/** One monitor on the host, in the host's desktop coordinates (physical pixels). */
data class Monitor(
    val id: Int,
    val name: String,
    val x: Int,
    val y: Int,
    val width: Int,
    val height: Int,
    val refreshHz: Int,
    val primary: Boolean,
)

/**
 * A connection to one host, backed by the native client (networking, FEC, hardware decoding).
 * Callbacks arrive on native threads; [Listener] implementations must hop to the UI thread.
 */
class NativeClient(private val listener: Listener) {

    interface Listener {
        fun onWelcome(hostName: String, hostId: String, hostVersion: String, codecMask: Int)
        fun onMonitors(monitors: List<Monitor>)
        fun onStreamStarted(streamId: Int, monitorId: Int, width: Int, height: Int, fps: Int, codec: Int, encoder: String)
        fun onStreamError(streamId: Int, message: String)
        fun onCursorShape(width: Int, height: Int, hotX: Int, hotY: Int, argb: IntArray)
        fun onCursorPos(monitorId: Int, x: Int, y: Int, visible: Boolean)
        fun onDisconnected(reason: String)
    }

    private var handle: Long = nativeCreate()

    /** Blocking. Returns null on success, otherwise the reason it failed. */
    fun connect(host: String, port: Int, clientName: String, clientId: String, pin: String,
                width: Int, height: Int, hz: Int, codecMask: Int): String? {
        val err = nativeConnect(handle, host, port, clientName, clientId, pin, width, height, hz, codecMask)
        return err.ifEmpty { null }
    }

    fun disconnect() = nativeDisconnect(handle)

    fun release() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0
        }
    }

    fun startStream(streamId: Int, monitorId: Int, surface: Surface?, maxW: Int, maxH: Int, fps: Int,
                    kbps: Int, codec: Int, fec: Int) =
        nativeStartStream(handle, streamId, monitorId, surface, maxW, maxH, fps, kbps, codec, fec)

    fun setSurface(streamId: Int, surface: Surface?, fps: Int) = nativeSetSurface(handle, streamId, surface, fps)
    fun stopStream(streamId: Int) = nativeStopStream(handle, streamId)

    fun mouseAbs(monitorId: Int, x: Int, y: Int) = nativeMouseAbs(handle, monitorId, x, y)
    fun mouseRel(dx: Int, dy: Int) = nativeMouseRel(handle, dx, dy)
    fun mouseButton(button: Int, down: Boolean) = nativeMouseButton(handle, button, down)
    fun scroll(dy: Int, dx: Int) = nativeScroll(handle, dy, dx)
    fun key(vk: Int, down: Boolean) = nativeKey(handle, vk, down)
    fun text(t: String) = nativeText(handle, t)

    class Stats(v: DoubleArray) {
        val valid = v[0] > 0
        val width = v[1].toInt()
        val height = v[2].toInt()
        val codec = v[3].toInt()
        val fps = v[4]
        val mbps = v[5]
        val lossPercent = v[6]
        val framesLost = v[7].toInt()
        val recovered = v[8].toInt()
        val hostMs = v[9]
        val assemblyMs = v[10]
        val decodeMs = v[11]
        val decodedFps = v[12]
        val rttMs = v[13]
        val captureFps = v[14].toInt()
        val encodeMs = v[15]
        val decoderDrops = v[16].toInt()
    }

    fun stats(streamId: Int) = Stats(nativeStats(handle, streamId))

    // ---- called from native code
    @Suppress("unused")
    private fun onWelcome(hostName: String, hostId: String, version: String, codecMask: Int) =
        listener.onWelcome(hostName, hostId, version, codecMask)

    @Suppress("unused")
    private fun onMonitors(ints: IntArray, names: Array<String>) {
        val list = names.indices.map { i ->
            val b = i * 7
            Monitor(ints[b], names[i], ints[b + 1], ints[b + 2], ints[b + 3], ints[b + 4], ints[b + 5], ints[b + 6] != 0)
        }
        listener.onMonitors(list)
    }

    @Suppress("unused")
    private fun onStreamStarted(streamId: Int, monitorId: Int, w: Int, h: Int, fps: Int, codec: Int, encoder: String) =
        listener.onStreamStarted(streamId, monitorId, w, h, fps, codec, encoder)

    @Suppress("unused")
    private fun onStreamError(streamId: Int, message: String) = listener.onStreamError(streamId, message)

    @Suppress("unused")
    private fun onCursorShape(w: Int, h: Int, hx: Int, hy: Int, argb: IntArray) = listener.onCursorShape(w, h, hx, hy, argb)

    @Suppress("unused")
    private fun onCursorPos(monitorId: Int, x: Int, y: Int, visible: Boolean) = listener.onCursorPos(monitorId, x, y, visible)

    @Suppress("unused")
    private fun onDisconnected(reason: String) = listener.onDisconnected(reason)

    private external fun nativeCreate(): Long
    private external fun nativeDestroy(h: Long)
    private external fun nativeConnect(h: Long, host: String, port: Int, name: String, id: String, pin: String,
                                       w: Int, hh: Int, hz: Int, codecMask: Int): String
    private external fun nativeDisconnect(h: Long)
    private external fun nativeStartStream(h: Long, streamId: Int, monitorId: Int, surface: Surface?, maxW: Int,
                                           maxH: Int, fps: Int, kbps: Int, codec: Int, fec: Int)
    private external fun nativeSetSurface(h: Long, streamId: Int, surface: Surface?, fps: Int)
    private external fun nativeStopStream(h: Long, streamId: Int)
    private external fun nativeMouseAbs(h: Long, monitorId: Int, x: Int, y: Int)
    private external fun nativeMouseRel(h: Long, dx: Int, dy: Int)
    private external fun nativeMouseButton(h: Long, button: Int, down: Boolean)
    private external fun nativeScroll(h: Long, dy: Int, dx: Int)
    private external fun nativeKey(h: Long, vk: Int, down: Boolean)
    private external fun nativeText(h: Long, text: String)
    private external fun nativeStats(h: Long, streamId: Int): DoubleArray

    companion object {
        const val CODEC_H264 = 0
        const val CODEC_HEVC = 1
        const val CODEC_AV1 = 2
        const val MOUSE_LEFT = 1
        const val MOUSE_RIGHT = 2
        const val MOUSE_MIDDLE = 3
        const val DEFAULT_PORT = 47800

        init {
            System.loadLibrary("hyperlink")
        }

        fun codecName(c: Int) = when (c) {
            CODEC_H264 -> "H.264"
            CODEC_HEVC -> "HEVC"
            CODEC_AV1 -> "AV1"
            else -> "?"
        }
    }
}
