package com.hyperlink.app

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.DisplayMetrics
import android.view.Surface
import android.view.WindowManager
import java.nio.ByteBuffer
import kotlin.math.min
import kotlin.math.roundToInt

/** JNI side of sharing this phone (hl::HostCore). Callbacks come from native threads. */
class NativeHost(private val service: HostService) {
    private var handle = 0L

    fun start(name: String, pin: String, hostId: String, codecMask: Int): Boolean {
        handle = nativeCreate(name, pin, hostId, BuildConfig.VERSION_NAME, codecMask)
        return nativeStart(handle)
    }
    fun release() { if (handle != 0L) { nativeDestroy(handle); handle = 0 } }
    fun setPin(pin: String) = nativeSetPin(handle, pin)
    fun streamReady(w: Int, h: Int, fps: Int, codec: Int, name: String) = nativeStreamReady(handle, w, h, fps, codec, name)
    fun sendFrame(b: ByteBuffer, off: Int, size: Int, key: Boolean, captureUs: Long) = nativeSendFrame(handle, b, off, size, key, captureUs)
    fun monitorsChanged() = nativeMonitorsChanged(handle)
    fun counts(): IntArray = if (handle != 0L) nativeCounts(handle) else intArrayOf(0, 0)

    @Suppress("unused") private fun monitorInfo(): IntArray = service.screenInfo()
    @Suppress("unused") private fun startEncoder(maxW: Int, maxH: Int, fps: Int, kbps: Int, codec: Int) = service.startEncoder(maxW, maxH, fps, kbps, codec)
    @Suppress("unused") private fun stopEncoder() = service.stopEncoder()
    @Suppress("unused") private fun requestKeyframe() = service.requestKeyframe()
    @Suppress("unused") private fun onInput(type: Int, a: Int, b: Int, c: Int, text: String?) = RemoteInput.handle(service, type, a, b, text)
    @Suppress("unused") private fun onStatus() = service.statusChanged()

    private external fun nativeCreate(name: String, pin: String, hostId: String, version: String, codecMask: Int): Long
    private external fun nativeStart(h: Long): Boolean
    private external fun nativeDestroy(h: Long)
    private external fun nativeSetPin(h: Long, pin: String)
    private external fun nativeStreamReady(h: Long, w: Int, hh: Int, fps: Int, codec: Int, name: String)
    private external fun nativeSendFrame(h: Long, b: ByteBuffer, off: Int, size: Int, key: Boolean, captureUs: Long)
    private external fun nativeMonitorsChanged(h: Long)
    private external fun nativeCounts(h: Long): IntArray

    companion object {
        init { System.loadLibrary("hyperlink") }
        @JvmStatic external fun nativeNowUs(): Long
    }
}

/**
 * Shares this phone's screen with other Hyperlink devices while it runs: screen capture
 * (MediaProjection) into a hardware encoder, sent through the same protocol PCs use.
 * Touch control needs the Hyperlink accessibility service switched on.
 */
class HostService : Service() {
    companion object {
        const val EXTRA_RESULT_CODE = "code"
        const val EXTRA_RESULT_DATA = "data"
        private const val CHANNEL = "sharing"
        @Volatile var running = false
            private set
        @Volatile var instance: HostService? = null
            private set
        var onStatus: (() -> Unit)? = null

        fun start(ctx: Context, resultCode: Int, data: Intent) {
            ctx.startForegroundService(Intent(ctx, HostService::class.java)
                .putExtra(EXTRA_RESULT_CODE, resultCode).putExtra(EXTRA_RESULT_DATA, data))
        }
        fun stop(ctx: Context) = ctx.stopService(Intent(ctx, HostService::class.java))
    }

    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var host: NativeHost? = null
    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private val thread = HandlerThread("hyperlink-host").apply { start() }
    private val handler = Handler(thread.looper)
    private var drain: Thread? = null
    @Volatile private var lastRequest: IntArray? = null   // maxW, maxH, fps, kbps, codec
    private var lastRotation = -1

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundNow()
        if (projection != null) return START_NOT_STICKY
        val code = intent?.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED) ?: Activity.RESULT_CANCELED
        @Suppress("DEPRECATION")
        val data = intent?.getParcelableExtra<Intent>(EXTRA_RESULT_DATA)
        if (code != Activity.RESULT_OK || data == null) { stopSelf(); return START_NOT_STICKY }
        val mpm = getSystemService(MediaProjectionManager::class.java)
        val p = mpm.getMediaProjection(code, data) ?: run { stopSelf(); return START_NOT_STICKY }
        p.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() { handler.post { stopSelf() } }
        }, handler)
        projection = p
        val s = Settings(this)
        val h = NativeHost(this)
        if (!h.start(s.clientName, s.hostPin, s.clientId, encodableCodecs())) {
            h.release()
            android.widget.Toast.makeText(this, "Couldn't start sharing (ports in use?)", android.widget.Toast.LENGTH_LONG).show()
            stopSelf()
            return START_NOT_STICKY
        }
        host = h
        running = true
        instance = this
        watchRotation()
        onStatus?.invoke()
        return START_NOT_STICKY
    }

    private fun startForegroundNow() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Sharing this phone", NotificationManager.IMPORTANCE_LOW))
        val pi = PendingIntent.getActivity(this, 2, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val counts = host?.counts() ?: intArrayOf(0, 0)
        val n = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle("Sharing this phone")
            .setContentText(if (counts[0] > 0) "${counts[0]} device(s) connected" else "Other Hyperlink devices can connect (PIN ${Settings(this).hostPin})")
            .setOngoing(true)
            .setContentIntent(pi)
            .build()
        if (Build.VERSION.SDK_INT >= 29) startForeground(2, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        else startForeground(2, n)
    }

    fun statusChanged() {
        handler.post { startForegroundNow() }
        onStatus?.let { cb -> android.os.Handler(mainLooper).post { cb() } }
    }

    fun counts(): IntArray = host?.counts() ?: intArrayOf(0, 0)
    fun setPin(pin: String) = host?.setPin(pin)

    private fun metrics(): DisplayMetrics {
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION")
        (getSystemService(WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(dm)
        return dm
    }

    /** [width, height, refreshHz] of the screen as it is now. */
    fun screenInfo(): IntArray {
        val dm = metrics()
        @Suppress("DEPRECATION")
        val hz = (getSystemService(WINDOW_SERVICE) as WindowManager).defaultDisplay.refreshRate.roundToInt()
        return intArrayOf(dm.widthPixels, dm.heightPixels, hz)
    }

    private fun encodableCodecs(): Int {
        var mask = 0
        val types = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { it.isEncoder }
            .flatMap { it.supportedTypes.toList() }.map { it.lowercase() }.toSet()
        if ("video/avc" in types) mask = mask or (1 shl NativeClient.CODEC_H264)
        if ("video/hevc" in types) mask = mask or (1 shl NativeClient.CODEC_HEVC)
        return if (mask == 0) 1 else mask
    }

    fun startEncoder(maxW: Int, maxH: Int, fps: Int, kbps: Int, codecId: Int) {
        lastRequest = intArrayOf(maxW, maxH, fps, kbps, codecId)
        handler.post { configure() }
    }

    fun stopEncoder() {
        lastRequest = null
        handler.post { releaseEncoder(); display?.surface = null }
    }

    fun requestKeyframe() {
        handler.post {
            runCatching { codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
        }
    }

    private fun releaseEncoder() {
        val c = codec
        codec = null
        drain?.interrupt()
        drain = null
        runCatching { c?.stop() }
        runCatching { c?.release() }
        inputSurface?.release()
        inputSurface = null
    }

    private fun configure() {
        val req = lastRequest ?: return
        val p = projection ?: return
        releaseEncoder()
        val dm = metrics()
        var w = dm.widthPixels
        var h = dm.heightPixels
        val (maxW, maxH) = req[0] to req[1]
        if (maxW > 0 && maxH > 0 && (w > maxW || h > maxH)) {
            val s = min(maxW.toFloat() / w, maxH.toFloat() / h)
            w = (w * s).toInt(); h = (h * s).toInt()
        }
        w = w and 7.inv()
        h = h and 7.inv()
        val fps = req[2].coerceIn(15, 120)
        val useHevc = req[4] == NativeClient.CODEC_HEVC && encodableCodecs() and (1 shl NativeClient.CODEC_HEVC) != 0
        val mime = if (useHevc) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        val fmt = MediaFormat.createVideoFormat(mime, w, h).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, (req[3].coerceIn(2000, 60000)) * 1000)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 60)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 200_000)  // keyframes still arrive on a still screen
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
        }
        val c = runCatching { MediaCodec.createEncoderByType(mime) }.getOrNull() ?: return
        runCatching { c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE) }.onFailure { c.release(); return }
        val surface = c.createInputSurface()
        c.start()
        codec = c
        inputSurface = surface
        val d = display
        if (d == null) {
            display = p.createVirtualDisplay("Hyperlink", w, h, dm.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, handler)
        } else {
            d.resize(w, h, dm.densityDpi)
            d.surface = surface
        }
        host?.streamReady(w, h, fps, if (useHevc) NativeClient.CODEC_HEVC else NativeClient.CODEC_H264, c.name)
        drain = Thread({ drainLoop(c) }, "hyperlink-encoder").apply { start() }
    }

    private fun drainLoop(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        var config: ByteArray? = null
        var joined: ByteBuffer? = null
        while (!Thread.currentThread().isInterrupted) {
            val idx = try { c.dequeueOutputBuffer(info, 20_000) } catch (_: Exception) { break }
            if (idx < 0) continue
            val buf = try { c.getOutputBuffer(idx) } catch (_: Exception) { null }
            if (buf != null && info.size > 0) {
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                    config = ByteArray(info.size).also { buf.position(info.offset); buf.get(it) }
                } else {
                    val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                    val now = NativeHost.nativeNowUs()
                    val cfg = config
                    if (key && cfg != null) {
                        // Viewers need the parameter sets in front of every keyframe.
                        val total = cfg.size + info.size
                        var jb = joined
                        if (jb == null || jb.capacity() < total) { jb = ByteBuffer.allocateDirect(total * 2); joined = jb }
                        jb!!.clear()
                        jb.put(cfg)
                        buf.position(info.offset); buf.limit(info.offset + info.size)
                        jb.put(buf)
                        host?.sendFrame(jb, 0, total, true, now)
                    } else {
                        host?.sendFrame(buf, info.offset, info.size, key, now)
                    }
                }
            }
            try { c.releaseOutputBuffer(idx, false) } catch (_: Exception) { break }
        }
    }

    private fun watchRotation() {
        val dmgr = getSystemService(DisplayManager::class.java)
        lastRotation = dmgr.getDisplay(android.view.Display.DEFAULT_DISPLAY)?.rotation ?: 0
        dmgr.registerDisplayListener(object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(id: Int) {}
            override fun onDisplayRemoved(id: Int) {}
            override fun onDisplayChanged(id: Int) {
                if (id != android.view.Display.DEFAULT_DISPLAY) return
                val r = dmgr.getDisplay(id)?.rotation ?: return
                if (r == lastRotation) return
                lastRotation = r
                // Turned sideways or back: tell viewers the new shape and restart the video.
                host?.monitorsChanged()
                if (lastRequest != null) configure()
            }
        }, handler)
    }

    override fun onDestroy() {
        running = false
        instance = null
        handler.post {
            releaseEncoder()
            display?.release()
            display = null
            projection?.stop()
            projection = null
        }
        host?.release()
        host = null
        thread.quitSafely()
        onStatus?.invoke()
        super.onDestroy()
    }
}
