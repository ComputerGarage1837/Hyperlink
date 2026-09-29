package com.hyperlink.app

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.media.MediaCodecList
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlin.concurrent.thread
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * One connection to one host, in its own window/task. Shows all of the host's monitors
 * together, or one at a time. Several of these can be open at once (other hosts, or other
 * monitors of the same host in separate windows).
 */
class SessionActivity : AppCompatActivity(), NativeClient.Listener, MonitorCanvas.SurfaceListener {

    companion object {
        const val EXTRA_DEVICE = "device"
        const val EXTRA_MONITOR = "monitor"   // -1 = all monitors / user's default

        /** Opens a session in a new window (its own task, beside this one in split screen). */
        fun open(ctx: Context, deviceId: String, monitorId: Int = -1) {
            val i = Intent(ctx, SessionActivity::class.java)
                .putExtra(EXTRA_DEVICE, deviceId)
                .putExtra(EXTRA_MONITOR, monitorId)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_DOCUMENT or Intent.FLAG_ACTIVITY_MULTIPLE_TASK or
                    Intent.FLAG_ACTIVITY_LAUNCH_ADJACENT or Intent.FLAG_ACTIVITY_NEW_TASK)
            ctx.startActivity(i)
        }
    }

    private class Slot(val monitor: Monitor, val streamId: Int) {
        var surface: Surface? = null
        var running = false
        var reqW = 0
        var reqH = 0
        var info = ""
    }

    private lateinit var settings: Settings
    private lateinit var device: SavedDevice
    private var client: NativeClient? = null
    @Volatile private var connected = false
    private var monitors = listOf<Monitor>()
    private val slots = LinkedHashMap<Int, Slot>()
    private var hostCodecs = 0
    private var hostName = ""
    private var visibleToUser = false
    private var requestedMonitor = -1

    private lateinit var root: FrameLayout
    private lateinit var canvas: MonitorCanvas
    private lateinit var cursor: CursorOverlay
    private lateinit var toolbar: View
    private lateinit var toolbarRow: LinearLayout
    private lateinit var keysBar: View
    private lateinit var statsView: TextView
    private lateinit var handleView: TextView
    private lateinit var overlay: LinearLayout
    private lateinit var overlayText: TextView
    private lateinit var overlayProgress: ProgressBar
    private lateinit var overlayButtons: LinearLayout
    private lateinit var keyInput: KeyInputView
    private lateinit var touch: TouchInput
    private val stickyMods = LinkedHashSet<Int>()
    private val modButtons = HashMap<Int, MaterialButton>()

    private val statsTick = object : Runnable {
        override fun run() {
            updateStats()
            root.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        val d = DeviceStore.get(this, intent.getStringExtra(EXTRA_DEVICE) ?: "")
        if (d == null) {
            finish()
            return
        }
        device = d
        requestedMonitor = intent.getIntExtra(EXTRA_MONITOR, -1)
        if (settings.lockLandscape) requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        setupWindow()
        buildUi()
        setTaskDescription(android.app.ActivityManager.TaskDescription(device.name))
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (toolbar.visibility == View.VISIBLE) showToolbar(false) else finish()
            }
        })
        connect()
    }

    // ------------------------------------------------------------------ window & UI

    private fun setupWindow() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        if (Build.VERSION.SDK_INT >= 28) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        // Ask for the display's fastest refresh rate at its current resolution (120 Hz etc).
        @Suppress("DEPRECATION")
        val display = windowManager.defaultDisplay
        val cur = display.mode
        val best = display.supportedModes
            .filter { it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight }
            .maxByOrNull { it.refreshRate }
        if (best != null) window.attributes = window.attributes.apply { preferredDisplayModeId = best.modeId }
        hideSystemBars()
    }

    private fun hideSystemBars() {
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    private fun maxDisplayHz(): Float {
        @Suppress("DEPRECATION")
        val display = windowManager.defaultDisplay
        return display.supportedModes.maxOfOrNull { it.refreshRate } ?: display.refreshRate
    }

    private fun streamFps() = minOf(settings.fps, maxOf(30, maxDisplayHz().roundToInt()))

    @SuppressLint("ClickableViewAccessibility")
    private fun buildUi() {
        root = FrameLayout(this).apply { setBackgroundColor(0xFF05060F.toInt()) }
        canvas = MonitorCanvas(this)
        canvas.surfaceListener = this
        canvas.onLayoutChanged = { cursor.invalidate(); syncStreams() }
        root.addView(canvas, FrameLayout.LayoutParams(-1, -1))

        cursor = CursorOverlay(this).apply { canvasView = canvas }
        root.addView(cursor, FrameLayout.LayoutParams(-1, -1))

        touch = TouchInput(canvas, { if (connected) client else null }, settings,
            onThreeFingerTap = { toggleToolbar() },
            onTwoFingerDoubleTap = { x, y ->
                if (canvas.isAll) canvas.hit(x, y)?.let { canvas.focus(it.first.id) } else canvas.showAll()
                if (toolbar.visibility == View.VISIBLE) rebuildToolbar()
            },
            onZoomEnd = { syncStreams() })
        canvas.setOnTouchListener { _, e ->
            if (e.actionMasked == MotionEvent.ACTION_DOWN && toolbar.visibility == View.VISIBLE &&
                e.getToolType(0) != MotionEvent.TOOL_TYPE_MOUSE) showToolbar(false)
            touch.onTouch(e)
        }
        canvas.setOnGenericMotionListener { _, e -> touch.onGenericMotion(e) }

        statsView = TextView(this).apply {
            setTextColor(0xFFE6FBFF.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            typeface = Typeface.MONOSPACE
            setPadding(dp(10), dp(6), dp(10), dp(6))
            background = GradientDrawable().apply { setColor(0xB3000000.toInt()); cornerRadius = dp(8).toFloat() }
            visibility = View.GONE
        }
        root.addView(statsView, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.START).apply {
            setMargins(dp(12), 0, 0, dp(12))
        })

        handleView = TextView(this).apply {
            text = "•••"
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            setPadding(dp(18), dp(2), dp(18), dp(6))
            background = GradientDrawable().apply { setColor(0x663EE6FF); cornerRadius = dp(12).toFloat() }
            setOnClickListener { showToolbar(true) }
        }
        root.addView(handleView, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
            topMargin = dp(4)
        })

        toolbarRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(8), dp(6), dp(8), dp(6))
        }
        toolbar = HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            background = GradientDrawable().apply { setColor(0xE60E1026.toInt()); cornerRadius = dp(16).toFloat() }
            addView(toolbarRow)
            visibility = View.GONE
        }
        root.addView(toolbar, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
            setMargins(dp(8), dp(8), dp(8), 0)
        })

        keysBar = buildKeysBar()
        root.addView(keysBar, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            setMargins(dp(8), 0, dp(8), dp(8))
        })

        keyInput = KeyInputView(this, onText = { sendText(it) }, onKey = { sendKeyTap(it) })
        root.addView(keyInput, FrameLayout.LayoutParams(1, 1))

        overlayText = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            gravity = Gravity.CENTER
        }
        overlayProgress = ProgressBar(this).apply { indeterminateTintList = ColorStateList.valueOf(0xFF3EE6FF.toInt()) }
        overlayButtons = LinearLayout(this).apply { gravity = Gravity.CENTER }
        overlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(0xF005060F.toInt())
            addView(overlayProgress)
            addView(overlayText, LinearLayout.LayoutParams(-2, -2).apply { topMargin = dp(16) })
            addView(overlayButtons, LinearLayout.LayoutParams(-2, -2).apply { topMargin = dp(16) })
            isClickable = true
        }
        root.addView(overlay, FrameLayout.LayoutParams(-1, -1))
        setContentView(root)
    }

    private fun button(label: String, filled: Boolean = false, onClick: (MaterialButton) -> Unit): MaterialButton {
        val b = MaterialButton(this, null,
            if (filled) com.google.android.material.R.attr.materialButtonStyle
            else com.google.android.material.R.attr.materialButtonOutlinedStyle)
        b.text = label
        b.isAllCaps = false
        b.minHeight = dp(40)
        b.minimumHeight = dp(40)
        b.insetTop = 0
        b.insetBottom = 0
        b.setTextColor(if (filled) 0xFF0E1026.toInt() else Color.WHITE)
        if (filled) b.backgroundTintList = ColorStateList.valueOf(0xFF3EE6FF.toInt())
        else b.strokeColor = ColorStateList.valueOf(0x663EE6FF)
        b.setOnClickListener { onClick(b) }
        b.layoutParams = LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(6) }
        return b
    }

    private fun rebuildToolbar() {
        toolbarRow.removeAllViews()
        toolbarRow.addView(TextView(this).apply {
            text = hostName.ifEmpty { device.name }
            setTextColor(Color.WHITE)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(6), 0, dp(12), 0)
        })
        if (monitors.size > 1) {
            toolbarRow.addView(button("All screens", filled = canvas.isAll) { canvas.showAll(); rebuildToolbar() })
            monitors.forEachIndexed { i, m ->
                toolbarRow.addView(button("${i + 1}  ${m.name}", filled = canvas.focusedId == m.id) {
                    canvas.focus(m.id); rebuildToolbar()
                })
            }
        }
        if (monitors.size > 1) toolbarRow.addView(button("Show/hide screens") { chooseScreens() })
        if (canvas.zoom > 1f) toolbarRow.addView(button("Fit to screen") { canvas.resetZoom(); rebuildToolbar() })
        toolbarRow.addView(button("New window") { openNewWindow() })
        toolbarRow.addView(button("Keyboard") { keyInput.toggleKeyboard() })
        toolbarRow.addView(button("Keys") { keysBar.visibility = if (keysBar.visibility == View.VISIBLE) View.GONE else View.VISIBLE })
        toolbarRow.addView(button(if (settings.trackpad) "Mouse: touchpad" else "Mouse: touch") {
            settings.trackpad = !settings.trackpad
            it.text = if (settings.trackpad) "Mouse: touchpad" else "Mouse: touch"
        })
        toolbarRow.addView(button("Stats") {
            settings.showStats = !settings.showStats
            statsView.visibility = if (settings.showStats) View.VISIBLE else View.GONE
            updateStats()
        })
        toolbarRow.addView(button("Disconnect") { finish() })
    }

    private fun buildKeysBar(): View {
        val row = LinearLayout(this).apply { setPadding(dp(8), dp(6), dp(8), dp(6)) }
        fun tap(label: String, vararg vks: Int) = button(label) {
            vks.forEach { sendVk(it, true) }
            vks.reversed().forEach { sendVk(it, false) }
            releaseSticky()
        }
        fun mod(label: String, vk: Int) = button(label) { b ->
            if (vk in stickyMods) {
                stickyMods.remove(vk); sendVk(vk, false); b.backgroundTintList = null
            } else {
                stickyMods.add(vk); sendVk(vk, true); b.backgroundTintList = ColorStateList.valueOf(0x553EE6FF)
            }
        }.also { modButtons[vk] = it }
        row.addView(tap("Esc", KeyMap.VK_ESCAPE))
        row.addView(tap("Tab", KeyMap.VK_TAB))
        row.addView(mod("Ctrl", KeyMap.VK_CONTROL))
        row.addView(mod("Alt", KeyMap.VK_MENU))
        row.addView(mod("Shift", KeyMap.VK_SHIFT))
        row.addView(mod("Win", KeyMap.VK_LWIN))
        row.addView(tap("←", KeyMap.VK_LEFT))
        row.addView(tap("↑", KeyMap.VK_UP))
        row.addView(tap("↓", KeyMap.VK_DOWN))
        row.addView(tap("→", KeyMap.VK_RIGHT))
        row.addView(tap("Del", KeyMap.VK_DELETE))
        row.addView(tap("Home", KeyMap.VK_HOME))
        row.addView(tap("End", KeyMap.VK_END))
        row.addView(tap("Task Manager", KeyMap.VK_CONTROL, KeyMap.VK_SHIFT, KeyMap.VK_ESCAPE))
        row.addView(tap("Alt+Tab", KeyMap.VK_MENU, KeyMap.VK_TAB))
        row.addView(tap("Win+D", KeyMap.VK_LWIN, 0x44))
        for (f in 1..12) row.addView(tap("F$f", 0x6F + f))
        return HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            background = GradientDrawable().apply { setColor(0xE60E1026.toInt()); cornerRadius = dp(16).toFloat() }
            addView(row)
            visibility = View.GONE
        }
    }

    private fun showToolbar(show: Boolean) {
        toolbar.visibility = if (show) View.VISIBLE else View.GONE
        handleView.visibility = if (show) View.GONE else View.VISIBLE
        if (show) rebuildToolbar()
    }

    private fun toggleToolbar() = showToolbar(toolbar.visibility != View.VISIBLE)

    private fun showOverlay(text: String?, busy: Boolean = false, vararg buttons: Pair<String, () -> Unit>) {
        if (text == null) {
            overlay.visibility = View.GONE
            return
        }
        overlay.visibility = View.VISIBLE
        overlayText.text = text
        overlayProgress.visibility = if (busy) View.VISIBLE else View.GONE
        overlayButtons.removeAllViews()
        buttons.forEachIndexed { i, (label, action) -> overlayButtons.addView(button(label, filled = i == 0) { action() }) }
    }

    private fun chooseScreens() {
        val names = monitors.mapIndexed { i, m -> "${i + 1}  ${m.name}  (${m.width}×${m.height})" }.toTypedArray()
        val hidden = settings.hiddenScreens(device.id).toMutableSet()
        val checked = BooleanArray(monitors.size) { monitors[it].id !in hidden }
        MaterialAlertDialogBuilder(this)
            .setTitle("Screens to show")
            .setMultiChoiceItems(names, checked) { _, which, on -> checked[which] = on }
            .setPositiveButton("Show") { _, _ ->
                val newHidden = monitors.filterIndexed { i, _ -> !checked[i] }.map { it.id }.toSet()
                if (newHidden.size == monitors.size) return@setPositiveButton  // keep at least one
                settings.setHiddenScreens(device.id, newHidden)
                canvas.hidden = newHidden
                canvas.focusedId?.let { if (it in newHidden) canvas.showAll() }
                rebuildToolbar()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun openNewWindow() {
        if (monitors.size <= 1) {
            SessionActivity.open(this, device.id, -1)
            return
        }
        val names = arrayOf("All screens") + monitors.mapIndexed { i, m -> "${i + 1}  ${m.name}" }
        MaterialAlertDialogBuilder(this)
            .setTitle("Open in a new window")
            .setItems(names) { _, which ->
                SessionActivity.open(this, device.id, if (which == 0) -1 else monitors[which - 1].id)
            }
            .show()
    }

    // ------------------------------------------------------------------ connection

    private fun deviceCodecMask(): Int {
        var mask = 1 shl NativeClient.CODEC_H264
        val types = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { !it.isEncoder }.flatMap { it.supportedTypes.toList() }.map { it.lowercase() }.toSet()
        if ("video/hevc" in types) mask = mask or (1 shl NativeClient.CODEC_HEVC)
        if ("video/av01" in types) mask = mask or (1 shl NativeClient.CODEC_AV1)
        return mask
    }

    private fun connect() {
        showOverlay("Connecting to ${device.name}…", busy = true, "Cancel" to { finish() })
        client?.release()
        val c = NativeClient(this)
        client = c
        val dm = resources.displayMetrics
        val name = settings.clientName
        val id = settings.clientId
        val mask = deviceCodecMask()
        val hz = maxDisplayHz().roundToInt()
        thread(name = "connect") {
            // Find the PC by its permanent id first; the saved address is only a fallback
            // (e.g. a Tailscale name when away from home).
            val found = Presence.locate(device.hostId)
            val address = found ?: device.address
            if (address.isEmpty()) {
                runOnUiThread {
                    showOverlay("Can't find ${device.name} on this network.\n\nIs it switched on and running Hyperlink? " +
                        "To connect from elsewhere, add a fallback address (such as its Tailscale name) in Edit.",
                        false, "Try again" to { connect() }, "Close" to { finish() })
                }
                return@thread
            }
            val err = c.connect(address, device.port, name, id, device.pin, dm.widthPixels, dm.heightPixels, hz, mask)
            runOnUiThread {
                if (isDestroyed || client !== c) return@runOnUiThread
                if (err == null) {
                    connected = true
                    ActiveSessions.add(device.id)
                    showOverlay(null)
                    if (settings.showStats) statsView.visibility = View.VISIBLE
                    root.removeCallbacks(statsTick)
                    root.post(statsTick)
                    syncStreams()
                } else if (err.contains("PIN", ignoreCase = true)) {
                    askPin(err)
                } else {
                    showOverlay("Couldn't connect to ${device.name}\n\n$err", false,
                        "Try again" to { connect() }, "Close" to { finish() })
                }
            }
        }
    }

    private fun askPin(message: String) {
        showOverlay(message, false, "Enter PIN" to { askPin(message) }, "Close" to { finish() })
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
            hint = "PIN shown on the PC"
        }
        val box = FrameLayout(this).apply { setPadding(dp(24), dp(8), dp(24), 0); addView(input) }
        MaterialAlertDialogBuilder(this)
            .setTitle("PIN for ${device.name}")
            .setMessage(message)
            .setView(box)
            .setPositiveButton("Connect") { _, _ ->
                device = device.copy(pin = input.text.toString().trim())
                DeviceStore.save(this, device)
                connect()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    override fun onWelcome(hostName: String, hostId: String, hostVersion: String, codecMask: Int) {
        this.hostCodecs = codecMask
        this.hostName = hostName
        runOnUiThread {
            device = device.copy(hostId = hostId, lastSeen = System.currentTimeMillis())
            DeviceStore.save(this, device)
        }
    }

    override fun onMonitors(monitors: List<Monitor>) = runOnUiThread {
        val c = client
        slots.values.filter { it.running }.forEach { c?.stopStream(it.streamId) }
        slots.clear()
        this.monitors = monitors
        monitors.forEachIndexed { i, m -> slots[m.id] = Slot(m, i) }
        canvas.setMonitors(monitors)
        canvas.hidden = settings.hiddenScreens(device.id).filter { id -> monitors.any { it.id == id } }.toSet()
        when {
            requestedMonitor >= 0 && monitors.any { it.id == requestedMonitor } -> canvas.focus(requestedMonitor)
            settings.startWithAllMonitors || monitors.size == 1 -> canvas.showAll()
            else -> canvas.focus((monitors.firstOrNull { it.primary } ?: monitors.first()).id)
        }
        showToolbar(true)
        root.postDelayed({ if (connected) showToolbar(false) }, 5000)
        android.widget.Toast.makeText(this, "Pinch to zoom · two-finger double-tap switches screens · three-finger tap shows the menu",
            android.widget.Toast.LENGTH_LONG).show()
    }

    override fun onStreamStarted(streamId: Int, monitorId: Int, width: Int, height: Int, fps: Int, codec: Int, encoder: String) =
        runOnUiThread {
            slots[monitorId]?.info = "${width}x$height ${NativeClient.codecName(codec)} $fps fps · $encoder"
            // Keep the surface buffer at the video's size; the display scales it, so zooming is free.
            canvas.tiles[monitorId]?.surface?.holder?.setFixedSize(width, height)
        }

    override fun onStreamError(streamId: Int, message: String) = runOnUiThread {
        val slot = slots.values.firstOrNull { it.streamId == streamId }
        slot?.running = false
        slot?.let { canvas.tiles[it.monitor.id]?.setStatus(message) }
        showOverlay(message, false, "Retry" to { showOverlay(null); syncStreams() }, "Close" to { finish() })
    }

    override fun onCursorShape(width: Int, height: Int, hotX: Int, hotY: Int, argb: IntArray) =
        runOnUiThread { cursor.setShape(width, height, hotX, hotY, argb) }

    override fun onCursorPos(monitorId: Int, x: Int, y: Int, visible: Boolean) =
        runOnUiThread {
            cursor.setPosition(monitorId, x, y, visible)
            if (visible && canvas.zoom > 1f) canvas.rectOf(monitorId)?.let { r ->
                canvas.keepVisible(r.left + x / 65535f * r.width(), r.top + y / 65535f * r.height())
            }
        }

    override fun onDisconnected(reason: String) = runOnUiThread {
        connected = false
        ActiveSessions.remove(device.id)
        slots.values.forEach { it.running = false }
        showOverlay("Disconnected from ${device.name}\n\n$reason", false,
            "Reconnect" to { connect() }, "Close" to { finish() })
    }

    // ------------------------------------------------------------------ streams

    override fun onSurfaceReady(tile: MonitorTile, holder: SurfaceHolder) {
        val slot = slots[tile.monitor.id] ?: return
        val s = holder.surface
        if (Build.VERSION.SDK_INT >= 30) runCatching {
            s.setFrameRate(streamFps().toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        }
        if (slot.surface !== s) {
            slot.surface = s
            if (slot.running && connected) client?.setSurface(slot.streamId, s, streamFps())
        }
        syncStreams()
    }

    override fun onSurfaceGone(tile: MonitorTile) {
        val slot = slots[tile.monitor.id] ?: return
        slot.surface = null
        if (slot.running && connected) client?.stopStream(slot.streamId)
        slot.running = false
    }

    private fun chooseCodec(): Int {
        val both = hostCodecs and deviceCodecMask()
        val pref = settings.codec
        if (pref >= 0 && both and (1 shl pref) != 0) return pref
        return if (both and (1 shl NativeClient.CODEC_HEVC) != 0) NativeClient.CODEC_HEVC else NativeClient.CODEC_H264
    }

    /** Starts, resizes or stops streams to match what's on screen. */
    private fun syncStreams() {
        val c = client ?: return
        if (!connected || !visibleToUser || canvas.gestureActive) return
        val showing = slots.values.filter { canvas.rectOf(it.monitor.id) != null && it.surface != null }
        val totalArea = showing.sumOf { r -> canvas.rectOf(r.monitor.id)!!.let { (it.width() * it.height()).toDouble() } }
        for (slot in slots.values) {
            val rect = canvas.rectOf(slot.monitor.id)
            val surface = slot.surface
            if (rect == null || surface == null) {
                if (slot.running) {
                    c.stopStream(slot.streamId)
                    slot.running = false
                }
                continue
            }
            var w = rect.width().roundToInt()
            var h = rect.height().roundToInt()
            when {
                canvas.zoom > 1f -> { w = 0; h = 0 }                              // zoomed in: full detail
                settings.maxHeight < 0 && !canvas.isAll -> { w = 0; h = 0 }        // native resolution
                settings.maxHeight > 0 && !canvas.isAll -> { w = 16384; h = settings.maxHeight }
            }
            val changed = !slot.running || abs(w - slot.reqW) > slot.reqW / 12 + 8 || abs(h - slot.reqH) > slot.reqH / 12 + 8
            if (!changed) continue
            val share = if (totalArea > 0) rect.width() * rect.height() / totalArea else 1.0
            val kbps = maxOf(3000, (settings.bitrateMbps * 1000 * share).roundToInt())
            c.startStream(slot.streamId, slot.monitor.id, surface, w, h, streamFps(), kbps, chooseCodec(), settings.fecPercent)
            slot.running = true
            slot.reqW = w
            slot.reqH = h
        }
    }

    private fun stopAllStreams() {
        val c = client ?: return
        for (slot in slots.values) if (slot.running) {
            if (connected) c.stopStream(slot.streamId)
            slot.running = false
        }
    }

    private fun updateStats() {
        if (!settings.showStats || !connected) return
        val c = client ?: return
        val sb = StringBuilder()
        for (slot in slots.values) {
            if (!slot.running) continue
            val s = c.stats(slot.streamId)
            if (!s.valid) continue
            if (sb.isNotEmpty()) sb.append('\n')
            sb.append("${slot.monitor.name}: ${slot.info}\n")
            sb.append("  %.0f fps shown · %.1f Mb/s · host %.1f ms · network %.1f ms · decode %.1f ms\n".format(
                s.decodedFps, s.mbps, s.hostMs, s.rttMs / 2 + s.assemblyMs, s.decodeMs))
            sb.append("  PC captured %d fps · encode %.1f ms · loss %.1f%% (fixed %d) · frames lost %d".format(
                s.captureFps, s.encodeMs, s.lossPercent, s.recovered, s.framesLost))
        }
        statsView.text = sb.toString().ifEmpty { "Waiting for video…" }
    }

    // ------------------------------------------------------------------ keyboard

    private fun sendVk(vk: Int, down: Boolean) {
        if (connected) client?.key(vk, down)
    }

    private fun releaseSticky() {
        for (vk in stickyMods) {
            sendVk(vk, false)
            modButtons[vk]?.backgroundTintList = null
        }
        stickyMods.clear()
    }

    private fun sendText(t: String) {
        if (!connected) return
        if (stickyMods.isNotEmpty()) {
            // With Ctrl/Alt/Win held, letters must go as keys (Ctrl+C, not the character "c").
            for (ch in t) {
                val vk = when (ch) {
                    in 'a'..'z' -> 0x41 + (ch - 'a')
                    in 'A'..'Z' -> 0x41 + (ch - 'A')
                    in '0'..'9' -> 0x30 + (ch - '0')
                    ' ' -> 0x20
                    else -> 0
                }
                if (vk != 0) { sendVk(vk, true); sendVk(vk, false) } else client?.text(ch.toString())
            }
            releaseSticky()
        } else {
            client?.text(t)
        }
    }

    private fun sendKeyTap(keyCode: Int) {
        val vk = KeyMap.toVk(keyCode)
        if (vk == 0) return
        sendVk(vk, true)
        sendVk(vk, false)
        releaseSticky()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val fromKeyboard = event.device?.let {
            !it.isVirtual && (it.sources and InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD &&
                it.keyboardType == InputDevice.KEYBOARD_TYPE_ALPHABETIC
        } ?: false
        val code = event.keyCode
        if (!fromKeyboard && (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_VOLUME_UP ||
                code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_VOLUME_MUTE)) {
            return super.dispatchKeyEvent(event)
        }
        if (!connected) return super.dispatchKeyEvent(event)
        val vk = KeyMap.toVk(code)
        if (vk == 0) return super.dispatchKeyEvent(event)
        when (event.action) {
            KeyEvent.ACTION_DOWN -> sendVk(vk, true)
            KeyEvent.ACTION_UP -> { sendVk(vk, false); if (vk !in stickyMods) releaseSticky() }
        }
        return true
    }

    // ------------------------------------------------------------------ lifecycle

    override fun onStart() {
        super.onStart()
        visibleToUser = true
        hideSystemBars()
        syncStreams()
    }

    override fun onStop() {
        super.onStop()
        // Not on screen: stop the video so the PC and network aren't working for nothing.
        visibleToUser = false
        stopAllStreams()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (::root.isInitialized) root.removeCallbacks(statsTick)
        if (::device.isInitialized && connected) ActiveSessions.remove(device.id)
        connected = false
        val c = client
        client = null
        if (c != null) thread { c.disconnect(); c.release() }
    }
}

/** Which saved devices have a session window open in this app right now. */
object ActiveSessions {
    private val counts = HashMap<String, Int>()
    var onChange: (() -> Unit)? = null

    @Synchronized fun add(id: String) { counts[id] = (counts[id] ?: 0) + 1; onChange?.invoke() }
    @Synchronized fun remove(id: String) {
        val n = (counts[id] ?: 0) - 1
        if (n <= 0) counts.remove(id) else counts[id] = n
        onChange?.invoke()
    }
    @Synchronized fun count(id: String) = counts[id] ?: 0
}
