package com.hyperlink.app

import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.MotionEvent
import android.view.ViewConfiguration
import kotlin.math.hypot
import kotlin.math.sqrt

/**
 * Turns touches on the monitor canvas into mouse input.
 *
 * Touchpad mode: drag to move the pointer, tap = click, two-finger tap = right-click,
 * double-tap and drag = drag, two-finger drag = scroll.
 * Touch screen mode: tap where you want to click, drag = drag, long-press = right-click,
 * two-finger drag = scroll.
 * Both: three-finger tap shows or hides the toolbar. Physical mice work directly.
 */
class TouchInput(
    private val canvas: MonitorCanvas,
    private val client: () -> NativeClient?,
    private val settings: Settings,
    private val onThreeFingerTap: () -> Unit,
) {
    private val slop = ViewConfiguration.get(canvas.context).scaledTouchSlop.toFloat()
    private val handler = Handler(Looper.getMainLooper())
    private val density = canvas.resources.displayMetrics.density

    private var downX = 0f
    private var downY = 0f
    private var lastX = 0f
    private var lastY = 0f
    private var downTime = 0L
    private var maxPointers = 0
    private var moved = false
    private var dragging = false
    private var scrolling = false
    private var longPressed = false
    private var lastTapUp = 0L
    private var dragArmed = false      // touchpad: second touch right after a tap
    private var scrollRemX = 0f
    private var scrollRemY = 0f
    private var relRemX = 0f
    private var relRemY = 0f
    private var mouseButtons = 0

    private val longPress = Runnable {
        if (!moved && maxPointers == 1) {
            longPressed = true
            if (!settings.trackpad) moveAbs(downX, downY)
            click(NativeClient.MOUSE_RIGHT)
        }
    }

    fun onTouch(e: MotionEvent): Boolean {
        if (e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) return onMouse(e)
        val c = client() ?: return true
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x; downY = e.y; lastX = e.x; lastY = e.y
                downTime = e.eventTime
                maxPointers = 1
                moved = false; dragging = false; scrolling = false; longPressed = false
                relRemX = 0f; relRemY = 0f
                dragArmed = settings.trackpad && e.eventTime - lastTapUp < 300
                handler.postDelayed(longPress, ViewConfiguration.getLongPressTimeout().toLong())
                if (!settings.trackpad) moveAbs(e.x, e.y)
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                maxPointers = maxOf(maxPointers, e.pointerCount)
                handler.removeCallbacks(longPress)
                if (dragging) { c.mouseButton(NativeClient.MOUSE_LEFT, false); dragging = false }
                lastX = centroidX(e); lastY = centroidY(e)
                scrollRemX = 0f; scrollRemY = 0f
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // Re-anchor on the remaining fingers so nothing jumps.
                lastX = centroidX(e, e.actionIndex); lastY = centroidY(e, e.actionIndex)
            }
            MotionEvent.ACTION_MOVE -> {
                if (e.pointerCount >= 2) {
                    val cx = centroidX(e)
                    val cy = centroidY(e)
                    if (!scrolling && hypot(cx - lastX, cy - lastY) > slop / 2) scrolling = true
                    if (scrolling) {
                        moved = true
                        // Natural scrolling: content follows the fingers. Sent in 1/120-notch units.
                        scrollRemY += (cy - lastY) * 4f / density
                        scrollRemX += -(cx - lastX) * 4f / density
                        val sy = scrollRemY.toInt()
                        val sx = scrollRemX.toInt()
                        if (sy != 0 || sx != 0) {
                            c.scroll(sy, sx)
                            scrollRemY -= sy; scrollRemX -= sx
                        }
                    }
                    lastX = cx; lastY = cy
                    return true
                }
                if (maxPointers > 1) return true
                if (!moved && hypot(e.x - downX, e.y - downY) > slop) {
                    moved = true
                    handler.removeCallbacks(longPress)
                    if (settings.trackpad && dragArmed) {
                        c.mouseButton(NativeClient.MOUSE_LEFT, true); dragging = true
                    } else if (!settings.trackpad && !longPressed) {
                        c.mouseButton(NativeClient.MOUSE_LEFT, true); dragging = true
                    }
                }
                if (moved) {
                    if (settings.trackpad) moveRel(e.x - lastX, e.y - lastY)
                    else moveAbs(e.x, e.y, clamp = true)
                }
                lastX = e.x; lastY = e.y
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                handler.removeCallbacks(longPress)
                val quick = e.eventTime - downTime < 300
                if (dragging) {
                    c.mouseButton(NativeClient.MOUSE_LEFT, false)
                    dragging = false
                } else if (e.actionMasked == MotionEvent.ACTION_UP && !moved && !longPressed && quick) {
                    when (maxPointers) {
                        1 -> {
                            if (!settings.trackpad) moveAbs(e.x, e.y)
                            click(NativeClient.MOUSE_LEFT)
                            lastTapUp = e.eventTime
                        }
                        2 -> click(NativeClient.MOUSE_RIGHT)
                        else -> onThreeFingerTap()
                    }
                }
                if (maxPointers > 1) lastTapUp = 0
            }
        }
        return true
    }

    /** Hover and wheel from a physical mouse. */
    fun onGenericMotion(e: MotionEvent): Boolean {
        if (!e.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) return false
        val c = client() ?: return false
        when (e.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE -> moveAbs(e.x, e.y)
            MotionEvent.ACTION_SCROLL -> {
                val v = (e.getAxisValue(MotionEvent.AXIS_VSCROLL) * 120).toInt()
                val h = (e.getAxisValue(MotionEvent.AXIS_HSCROLL) * 120).toInt()
                if (v != 0 || h != 0) c.scroll(v, h)
            }
            else -> return false
        }
        return true
    }

    private fun onMouse(e: MotionEvent): Boolean {
        val c = client() ?: return true
        moveAbs(e.x, e.y, clamp = true)
        val state = e.buttonState
        val map = listOf(
            MotionEvent.BUTTON_PRIMARY to NativeClient.MOUSE_LEFT,
            MotionEvent.BUTTON_SECONDARY to NativeClient.MOUSE_RIGHT,
            MotionEvent.BUTTON_TERTIARY to NativeClient.MOUSE_MIDDLE,
            MotionEvent.BUTTON_BACK to 4,
            MotionEvent.BUTTON_FORWARD to 5,
        )
        for ((bit, button) in map) {
            val now = state and bit != 0
            val was = mouseButtons and bit != 0
            if (now != was) c.mouseButton(button, now)
        }
        mouseButtons = state
        return true
    }

    private fun click(button: Int) {
        val c = client() ?: return
        c.mouseButton(button, true)
        c.mouseButton(button, false)
    }

    private fun moveAbs(x: Float, y: Float, clamp: Boolean = false) {
        val (m, nx, ny) = canvas.hit(x, y, clamp) ?: return
        client()?.mouseAbs(m.id, nx, ny)
    }

    private fun moveRel(dx: Float, dy: Float) {
        // Speed plus gentle acceleration, in host pixels.
        val dist = sqrt(dx * dx + dy * dy) / density
        val gain = settings.trackpadSpeed * (1f + minOf(dist, 40f) / 25f)
        relRemX += dx / density * gain * 1.5f
        relRemY += dy / density * gain * 1.5f
        val ix = relRemX.toInt()
        val iy = relRemY.toInt()
        if (ix != 0 || iy != 0) {
            client()?.mouseRel(ix, iy)
            relRemX -= ix; relRemY -= iy
        }
    }

    private fun centroidX(e: MotionEvent, skip: Int = -1): Float {
        var s = 0f; var n = 0
        for (i in 0 until e.pointerCount) if (i != skip) { s += e.getX(i); n++ }
        return if (n > 0) s / n else e.x
    }

    private fun centroidY(e: MotionEvent, skip: Int = -1): Float {
        var s = 0f; var n = 0
        for (i in 0 until e.pointerCount) if (i != skip) { s += e.getY(i); n++ }
        return if (n > 0) s / n else e.y
    }
}
