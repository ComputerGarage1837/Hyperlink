package com.hyperlink.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** One host monitor on screen: a SurfaceView the decoder renders into, plus a label. */
class MonitorTile(ctx: Context, val monitor: Monitor) : FrameLayout(ctx) {
    val surface = SurfaceView(ctx)
    private val label = TextView(ctx).apply {
        setTextColor(Color.WHITE)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
        setPadding(dp(8), dp(3), dp(8), dp(3))
        background = GradientDrawable().apply { setColor(0xB30E1026.toInt()); cornerRadius = dp(10).toFloat() }
    }
    private val border = View(ctx)

    init {
        setBackgroundColor(Color.BLACK)
        addView(surface, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(border, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(label, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START)
            .apply { setMargins(dp(8), dp(8), 0, 0) })
        label.text = monitor.name + if (monitor.primary) "  •  main" else ""
    }

    fun setDecorations(showLabel: Boolean, highlighted: Boolean) {
        label.visibility = if (showLabel) VISIBLE else GONE
        border.background = if (showLabel) GradientDrawable().apply {
            setColor(Color.TRANSPARENT)
            setStroke(dp(if (highlighted) 2 else 1), if (highlighted) 0xFF3EE6FF.toInt() else 0x553EE6FF)
        } else null
    }

    fun setStatus(text: String?) {
        label.text = monitor.name + (if (monitor.primary) "  •  main" else "") + (text?.let { "  •  $it" } ?: "")
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).roundToInt()
}

/**
 * Lays out monitor tiles either all together, arranged the way they sit on the host's desktop,
 * or one monitor filling the view. Tiles keep the monitor's aspect ratio.
 */
class MonitorCanvas(ctx: Context) : ViewGroup(ctx) {
    val tiles = LinkedHashMap<Int, MonitorTile>()   // by monitor id
    var focusedId: Int? = null                        // null = all monitors
        private set
    var onTileTapped: ((Monitor) -> Unit)? = null
    var surfaceListener: SurfaceListener? = null
    var onLayoutChanged: (() -> Unit)? = null
    private val gap = (6 * resources.displayMetrics.density)
    private val rects = HashMap<Int, RectF>()

    interface SurfaceListener {
        fun onSurfaceReady(tile: MonitorTile, holder: SurfaceHolder)
        fun onSurfaceGone(tile: MonitorTile)
    }

    init {
        setBackgroundColor(0xFF05060F.toInt())
    }

    fun setMonitors(monitors: List<Monitor>) {
        tiles.values.forEach { removeView(it) }
        tiles.clear()
        for (m in monitors) {
            val t = MonitorTile(context, m)
            t.surface.holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceCreated(h: SurfaceHolder) {}
                override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, hgt: Int) {
                    surfaceListener?.onSurfaceReady(t, h)
                }
                override fun surfaceDestroyed(h: SurfaceHolder) {
                    surfaceListener?.onSurfaceGone(t)
                }
            })
            tiles[m.id] = t
            addView(t)
        }
        val f = focusedId
        if (f != null && f !in tiles) focusedId = null
        requestLayout()
    }

    fun showAll() {
        focusedId = null
        requestLayout()
    }

    fun focus(monitorId: Int) {
        focusedId = monitorId
        requestLayout()
    }

    val isAll get() = focusedId == null

    /** Screen rect of a tile (in this view's coordinates), if it's showing. */
    fun rectOf(monitorId: Int): RectF? = rects[monitorId]

    /** Which monitor is under a point, and where on it (0..65535). */
    fun hit(x: Float, y: Float, clamp: Boolean = false): Triple<Monitor, Int, Int>? {
        var best: Pair<Int, RectF>? = null
        for ((id, r) in rects) {
            if (r.contains(x, y)) { best = id to r; break }
        }
        if (best == null && clamp) {
            // Nearest tile: keeps drags that leave the picture on the monitor they started on.
            best = rects.minByOrNull { (_, r) ->
                val dx = max(0f, max(r.left - x, x - r.right))
                val dy = max(0f, max(r.top - y, y - r.bottom))
                dx * dx + dy * dy
            }?.toPair()
        }
        val (id, r) = best ?: return null
        val nx = ((x - r.left) / r.width()).coerceIn(0f, 1f)
        val ny = ((y - r.top) / r.height()).coerceIn(0f, 1f)
        val m = tiles[id]?.monitor ?: return null
        return Triple(m, (nx * 65535).roundToInt(), (ny * 65535).roundToInt())
    }

    override fun onMeasure(wSpec: Int, hSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(wSpec), MeasureSpec.getSize(hSpec))
        computeRects(measuredWidth.toFloat(), measuredHeight.toFloat())
        for ((id, t) in tiles) {
            val r = rects[id]
            if (r == null) {
                t.measure(MeasureSpec.makeMeasureSpec(0, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(0, MeasureSpec.EXACTLY))
            } else {
                t.measure(MeasureSpec.makeMeasureSpec(r.width().roundToInt(), MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(r.height().roundToInt(), MeasureSpec.EXACTLY))
            }
        }
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        for ((id, tile) in tiles) {
            val rect = rects[id]
            if (rect == null) {
                tile.visibility = GONE
                tile.layout(0, 0, 0, 0)
            } else {
                tile.visibility = VISIBLE
                tile.layout(rect.left.roundToInt(), rect.top.roundToInt(),
                    rect.left.roundToInt() + tile.measuredWidth, rect.top.roundToInt() + tile.measuredHeight)
                tile.setDecorations(showLabel = isAll && tiles.size > 1, highlighted = false)
            }
        }
        onLayoutChanged?.invoke()
    }

    private fun computeRects(w: Float, h: Float) {
        rects.clear()
        if (w <= 0 || h <= 0 || tiles.isEmpty()) return
        val focused = focusedId?.let { tiles[it] }
        if (focused != null || tiles.size == 1) {
            val m = (focused ?: tiles.values.first()).monitor
            val s = min(w / m.width, h / m.height)
            val tw = m.width * s
            val th = m.height * s
            rects[m.id] = RectF((w - tw) / 2, (h - th) / 2, (w + tw) / 2, (h + th) / 2)
            return
        }
        // All monitors, in their real arrangement, scaled to fit with a small gap between them.
        val ms = tiles.values.map { it.monitor }
        val minX = ms.minOf { it.x }.toFloat()
        val minY = ms.minOf { it.y }.toFloat()
        val maxX = ms.maxOf { it.x + it.width }.toFloat()
        val maxY = ms.maxOf { it.y + it.height }.toFloat()
        val pad = gap * 2
        val s = min((w - pad * 2) / (maxX - minX), (h - pad * 2) / (maxY - minY))
        val ox = (w - (maxX - minX) * s) / 2
        val oy = (h - (maxY - minY) * s) / 2
        for (m in ms) {
            val left = ox + (m.x - minX) * s
            val top = oy + (m.y - minY) * s
            rects[m.id] = RectF(left + gap / 2, top + gap / 2, left + m.width * s - gap / 2, top + m.height * s - gap / 2)
        }
    }

    override fun onInterceptTouchEvent(ev: MotionEvent) = true
}

/** Draws the host's mouse pointer on top of the video, positioned from the host's reports. */
class CursorOverlay(ctx: Context) : View(ctx) {
    private var bitmap: Bitmap? = null
    private var hotX = 0
    private var hotY = 0
    private var monitorId = -1
    private var nx = 0f
    private var ny = 0f
    private var visible = false
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    var canvasView: MonitorCanvas? = null

    fun setShape(w: Int, h: Int, hx: Int, hy: Int, argb: IntArray) {
        if (w <= 0 || h <= 0) return
        bitmap = Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888)
        hotX = hx
        hotY = hy
        invalidate()
    }

    fun setPosition(monitor: Int, x: Int, y: Int, vis: Boolean) {
        if (!vis && monitor != monitorId && visible) return  // another monitor hiding it: ignore
        monitorId = monitor
        nx = x / 65535f
        ny = y / 65535f
        visible = vis
        invalidate()
    }

    override fun onDraw(c: Canvas) {
        val bmp = bitmap ?: return
        if (!visible) return
        val cv = canvasView ?: return
        val r = cv.rectOf(monitorId) ?: return
        val m = cv.tiles[monitorId]?.monitor ?: return
        val scale = max(r.width() / m.width, 0.6f * resources.displayMetrics.density / 2f)
        val x = r.left + nx * r.width() - hotX * scale
        val y = r.top + ny * r.height() - hotY * scale
        c.save()
        c.clipRect(r)
        c.translate(x, y)
        c.scale(scale, scale)
        c.drawBitmap(bmp, 0f, 0f, paint)
        c.restore()
    }
}
