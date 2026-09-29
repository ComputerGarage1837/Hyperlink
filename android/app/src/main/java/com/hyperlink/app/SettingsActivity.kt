package com.hyperlink.app

import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import kotlin.math.roundToInt

class SettingsActivity : AppCompatActivity() {
    private lateinit var s: Settings
    private lateinit var list: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        s = Settings(this)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        list = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8), dp(8), dp(8), dp(32))
        }
        val scroll = ScrollView(this).apply {
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(0xFF0E1026.toInt(), 0xFF141C45.toInt()))
            addView(list)
        }
        ViewCompat.setOnApplyWindowInsetsListener(scroll) { v, insets ->
            val b = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.setPadding(b.left, b.top, b.right, b.bottom)
            insets
        }
        setContentView(scroll)
        build()
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    private fun build() {
        list.removeAllViews()
        list.addView(TextView(this).apply {
            text = "Settings"
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 24f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(12), dp(12), dp(12), dp(8))
        })

        section("Picture")
        choice("Resolution", s.maxHeight,
            listOf(0 to "Match this screen (recommended)", -1 to "PC's full resolution", 2160 to "Up to 2160p",
                1440 to "Up to 1440p", 1080 to "Up to 1080p", 720 to "Up to 720p")) { s.maxHeight = it }
        choice("Frame rate", s.fps, listOf(144 to "144 fps", 120 to "120 fps", 90 to "90 fps", 60 to "60 fps", 30 to "30 fps"),
            note = "Capped at this screen's refresh rate. The PC's monitor must also run at least this fast.") { s.fps = it }
        choice("Bitrate", s.bitrateMbps, listOf(20, 40, 60, 80, 100, 150, 200).map { it to "$it Mb/s" },
            note = "Shared between screens when you view several at once.") { s.bitrateMbps = it }
        choice("Video codec", s.codec, listOf(-1 to "Automatic (HEVC when possible)", 1 to "HEVC (H.265)",
            0 to "H.264", 2 to "AV1 (newest GPUs and phones)")) { s.codec = it }
        choice("Error correction", s.fecPercent, listOf(10 to "10% (clean network)", 20 to "20% (default)",
            35 to "35% (busy Wi-Fi)", 50 to "50% (poor Wi-Fi)")) { s.fecPercent = it }

        section("Screens and input")
        choice("When connecting, show", if (s.startWithAllMonitors) 1 else 0,
            listOf(1 to "All screens together", 0 to "One screen (the main one)")) { s.startWithAllMonitors = it == 1 }
        choice("Touch works like", if (s.trackpad) 1 else 0,
            listOf(1 to "A touchpad (move the pointer)", 0 to "A touch screen (tap where you click)")) { s.trackpad = it == 1 }
        choice("Touchpad speed", (s.trackpadSpeed * 10).roundToInt(),
            listOf(8 to "Slow", 12 to "Medium", 16 to "Normal", 22 to "Fast", 30 to "Very fast")) { s.trackpadSpeed = it / 10f }
        toggle("Turn to landscape while connected", s.lockLandscape) { s.lockLandscape = it }
        toggle("Show performance stats", s.showStats) { s.showStats = it }
        text("This device's name", s.clientName) { s.clientName = it }

        section("Updates")
        toggle("Check for updates when the app opens", s.checkUpdatesOnStart) { s.checkUpdatesOnStart = it }
        row("Check for updates now", "You have version ${BuildConfig.VERSION_NAME}") { Updater.check(this, userAsked = true) }
    }

    private fun section(title: String) {
        list.addView(TextView(this).apply {
            text = title.uppercase()
            setTextColor(0xFF9AA6D8.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(12), dp(20), dp(12), dp(6))
        })
    }

    private fun row(title: String, value: String, onClick: () -> Unit): LinearLayout {
        val r = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(12), dp(12), dp(12))
            isClickable = true
            val ta = obtainStyledAttributes(intArrayOf(android.R.attr.selectableItemBackground))
            background = ta.getDrawable(0)
            ta.recycle()
            setOnClickListener { onClick() }
        }
        r.addView(TextView(this).apply {
            text = title
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
        })
        r.addView(TextView(this).apply {
            text = value
            setTextColor(0xFF3EE6FF.toInt())
        })
        list.addView(r)
        return r
    }

    private fun choice(title: String, current: Int, options: List<Pair<Int, String>>, note: String? = null, set: (Int) -> Unit) {
        val label = options.firstOrNull { it.first == current }?.second ?: options.first().second
        row(title, label + (note?.let { "\n$it" } ?: "")) {
            val idx = options.indexOfFirst { it.first == current }
            MaterialAlertDialogBuilder(this)
                .setTitle(title)
                .setSingleChoiceItems(options.map { it.second }.toTypedArray(), idx) { d, which ->
                    set(options[which].first)
                    d.dismiss()
                    build()
                }
                .show()
        }
    }

    private fun toggle(title: String, value: Boolean, set: (Boolean) -> Unit) {
        val r = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(12), dp(6), dp(12), dp(6))
        }
        r.addView(TextView(this).apply {
            text = title
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
        }, LinearLayout.LayoutParams(0, -2, 1f))
        r.addView(MaterialSwitch(this).apply {
            isChecked = value
            setOnCheckedChangeListener { _, c -> set(c) }
        })
        list.addView(r)
    }

    private fun text(title: String, value: String, set: (String) -> Unit) {
        row(title, value) {
            val til = TextInputLayout(this)
            val et = TextInputEditText(til.context).apply { setText(value); inputType = InputType.TYPE_CLASS_TEXT }
            til.addView(et)
            val box = FrameLayout(this).apply { setPadding(dp(24), dp(8), dp(24), 0); addView(til) }
            MaterialAlertDialogBuilder(this)
                .setTitle(title)
                .setView(box)
                .setPositiveButton("Save") { _, _ -> set(et.text.toString().trim()); build() }
                .setNegativeButton("Cancel", null)
                .show()
        }
    }
}
