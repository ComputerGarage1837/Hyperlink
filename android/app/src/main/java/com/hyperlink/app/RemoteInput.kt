package com.hyperlink.app

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Context
import android.graphics.Path
import android.os.Bundle
import android.os.SystemClock
import android.util.DisplayMetrics
import android.view.WindowManager
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import kotlin.math.abs
import kotlin.math.hypot

/**
 * Lets a remote PC control this phone. Android only allows this through an accessibility
 * service, which the user switches on once (Settings → Accessibility → Hyperlink).
 */
class RemoteControlService : AccessibilityService() {
    override fun onServiceConnected() { RemoteInput.service = this }
    override fun onUnbind(intent: android.content.Intent?): Boolean { RemoteInput.service = null; return super.onUnbind(intent) }
    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}
}

/**
 * Turns the viewer's mouse and keyboard into phone gestures:
 * left click = tap, drag = swipe, hold = long press, right click = Back, middle click = Recents,
 * wheel = scroll, Esc = Back, Windows key = Home, typing goes into the focused text field.
 */
object RemoteInput {
    @Volatile var service: RemoteControlService? = null
    val enabled get() = service != null

    private var x = 0f
    private var y = 0f
    private var down = false
    private var downAt = 0L
    private val path = ArrayList<Pair<Float, Float>>()
    private var scrollAcc = 0
    private var lastScroll = 0L

    private fun screen(ctx: Context): DisplayMetrics {
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION")
        (ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(dm)
        return dm
    }

    @Synchronized
    fun handle(ctx: Context, type: Int, a: Int, b: Int, text: String?) {
        val s = service ?: return
        when (type) {
            10 -> { // absolute move, 0..65535
                val dm = screen(ctx)
                x = a / 65535f * (dm.widthPixels - 1)
                y = b / 65535f * (dm.heightPixels - 1)
                if (down) path.add(x to y)
            }
            11 -> { // relative move
                val dm = screen(ctx)
                x = (x + a).coerceIn(0f, dm.widthPixels - 1f)
                y = (y + b).coerceIn(0f, dm.heightPixels - 1f)
                if (down) path.add(x to y)
            }
            12 -> when (a) { // button
                NativeClient.MOUSE_LEFT -> if (b != 0) {
                    down = true; downAt = SystemClock.uptimeMillis(); path.clear(); path.add(x to y)
                } else if (down) {
                    down = false; path.add(x to y); gesture(s)
                }
                NativeClient.MOUSE_RIGHT -> if (b != 0) s.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK)
                NativeClient.MOUSE_MIDDLE -> if (b != 0) s.performGlobalAction(AccessibilityService.GLOBAL_ACTION_RECENTS)
            }
            13 -> scroll(ctx, s, a)
            14 -> if (b != 0) key(s, a)
            15 -> if (!text.isNullOrEmpty()) type(s, text)
        }
    }

    private fun gesture(s: RemoteControlService) {
        val held = (SystemClock.uptimeMillis() - downAt).coerceIn(1, 5000)
        val (sx, sy) = path.first()
        val moved = path.maxOf { hypot(it.first - sx, it.second - sy) }
        val p = Path().apply { moveTo(sx, sy) }
        if (moved < 12f) {
            p.lineTo(sx, sy)
            // Short press = tap, long press stays long.
            dispatch(s, p, if (held < 400) 40 else held.coerceAtLeast(600))
        } else {
            // Thin the path out so long drags stay within gesture limits.
            val step = maxOf(1, path.size / 40)
            for (i in step until path.size step step) p.lineTo(path[i].first, path[i].second)
            p.lineTo(path.last().first, path.last().second)
            dispatch(s, p, held.coerceIn(80, 3000))
        }
    }

    private fun dispatch(s: RemoteControlService, p: Path, durationMs: Long) {
        val g = GestureDescription.Builder().addStroke(GestureDescription.StrokeDescription(p, 0, durationMs)).build()
        s.dispatchGesture(g, null, null)
    }

    private fun scroll(ctx: Context, s: RemoteControlService, delta: Int) {
        // Wheel notches become short swipes under the pointer; bunched up so fast wheels feel smooth.
        scrollAcc += delta
        val now = SystemClock.uptimeMillis()
        if (now - lastScroll < 60 && abs(scrollAcc) < 360) return
        lastScroll = now
        val dm = screen(ctx)
        val dist = (scrollAcc / 120f) * dm.heightPixels * 0.12f
        scrollAcc = 0
        val y2 = (y + dist).coerceIn(1f, dm.heightPixels - 2f)
        val p = Path().apply { moveTo(x, y); lineTo(x, y2) }
        dispatch(s, p, 120)
    }

    private fun key(s: RemoteControlService, vk: Int) {
        when (vk) {
            KeyMap.VK_ESCAPE -> s.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK)
            KeyMap.VK_LWIN, 0x5C, KeyMap.VK_HOME -> s.performGlobalAction(AccessibilityService.GLOBAL_ACTION_HOME)
            KeyMap.VK_BACK -> editFocused(s) { it.dropLast(1) }
            KeyMap.VK_RETURN -> {
                val n = s.rootInActiveWindow?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
                if (n != null && android.os.Build.VERSION.SDK_INT >= 30) {
                    n.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_IME_ENTER.id)
                } else editFocused(s) { it + "\n" }
            }
        }
    }

    private fun type(s: RemoteControlService, text: String) = editFocused(s) { it + text }

    private fun editFocused(s: RemoteControlService, change: (String) -> String) {
        val n = s.rootInActiveWindow?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT) ?: return
        if (!n.isEditable) return
        val current = if (n.isShowingHintText) "" else (n.text?.toString() ?: "")
        val args = Bundle().apply { putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, change(current)) }
        n.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)
    }
}
