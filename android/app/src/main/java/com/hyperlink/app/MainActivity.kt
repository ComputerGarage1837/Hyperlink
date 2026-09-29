package com.hyperlink.app

import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.InputType
import android.text.format.DateUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import kotlin.math.roundToInt

/** Home screen: saved devices with live status, hosts found nearby, updates and settings. */
class MainActivity : AppCompatActivity() {
    companion object {
        const val EXTRA_RESUME = "resume"
    }

    private sealed class Row {
        data class Header(val text: String) : Row()
        data class Device(val d: SavedDevice, val p: HostPresence?, val openHere: Int) : Row()
        data class Nearby(val p: HostPresence) : Row()
        data class Hint(val text: String) : Row()
    }

    private lateinit var settings: Settings
    private lateinit var presence: Presence
    private var found: Map<String, HostPresence> = emptyMap()
    private val adapter = Adapter()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        presence = Presence(this) { found = it; refresh() }

        val root = FrameLayout(this).apply {
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(0xFF0B0B0B.toInt(), 0xFF17140E.toInt()))
        }
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(header())
        val list = RecyclerView(this).apply {
            layoutManager = LinearLayoutManager(this@MainActivity)
            adapter = this@MainActivity.adapter
            clipToPadding = false
            setPadding(dp(12), 0, dp(12), dp(96))
        }
        column.addView(list, LinearLayout.LayoutParams(-1, 0, 1f))
        root.addView(column, FrameLayout.LayoutParams(-1, -1))

        val fab = ExtendedFloatingActionButton(this).apply {
            text = "Add device"
            setTextColor(0xFF0B0B0B.toInt())
            backgroundTintList = ColorStateList.valueOf(0xFFE3B341.toInt())
            setOnClickListener { editDevice(null, null) }
        }
        root.addView(fab, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.END).apply {
            setMargins(0, 0, dp(20), dp(24))
        })
        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            column.setPadding(bars.left, bars.top, bars.right, 0)
            (fab.layoutParams as FrameLayout.LayoutParams).bottomMargin = dp(24) + bars.bottom
            fab.requestLayout()
            insets
        }
        setContentView(root)
        ActiveSessions.onChange = { runOnUiThread { refresh() } }
        refresh()
        if (savedInstanceState == null && settings.checkUpdatesOnStart) Updater.check(this, userAsked = false)
        intent.getStringExtra(EXTRA_RESUME)?.let { SessionActivity.resume(this, it) }
        // The "Connected to …" notification that keeps sessions alive needs this on Android 13+.
        if (android.os.Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 1)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        intent.getStringExtra(EXTRA_RESUME)?.let { SessionActivity.resume(this, it) }
    }

    override fun onResume() {
        super.onResume()
        presence.targets = DeviceStore.all(this).flatMap { listOf(it.remoteAddress, it.address) }.filter { it.isNotEmpty() }
        presence.start()
        refresh()
    }

    override fun onPause() {
        super.onPause()
        presence.stop()
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    private fun header(): View {
        val row = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(20), dp(16), dp(12), dp(8))
        }
        row.addView(ImageView(this).apply { setImageResource(R.mipmap.ic_launcher) }, LinearLayout.LayoutParams(dp(40), dp(40)))
        row.addView(TextView(this).apply {
            text = "Hyperlink"
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 24f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(dp(12), 0, 0, 0)
        }, LinearLayout.LayoutParams(0, -2, 1f))
        row.addView(textButton("Update") { Updater.check(this, userAsked = true) })
        row.addView(textButton("Settings") { startActivity(Intent(this, SettingsActivity::class.java)) })
        return row
    }

    private fun textButton(label: String, onClick: () -> Unit) =
        MaterialButton(this, null, com.google.android.material.R.attr.borderlessButtonStyle).apply {
            text = label
            isAllCaps = false
            setTextColor(0xFFE3B341.toInt())
            setOnClickListener { onClick() }
        }

    private fun refresh() {
        val saved = DeviceStore.all(this)
        val rows = mutableListOf<Row>(Row.Header("My devices"))
        if (saved.isEmpty()) rows += Row.Hint("No devices yet. Install Hyperlink Host on your PC, then tap Add device " +
            "or pick it from the list below.")
        val matched = HashSet<String>()
        for (d in saved) {
            val p = presence.find(found, d.address, d.hostId)
            if (p != null) matched += p.address
            rows += Row.Device(d, p, ActiveSessions.count(d.id))
        }
        val nearby = found.values.filter { it.address !in matched && saved.none { s -> s.hostId.isNotEmpty() && s.hostId == it.hostId } }
        rows += Row.Header("Found on your network")
        if (nearby.isEmpty()) rows += Row.Hint("Looking for PCs running Hyperlink Host…")
        nearby.sortedBy { it.name.lowercase() }.forEach { rows += Row.Nearby(it) }
        adapter.submit(rows)
    }

    /** Returns to an open session for this device, or opens one. */
    private fun connect(d: SavedDevice, monitor: Int = -1) {
        if (monitor < 0 && SessionActivity.resume(this, d.id)) return
        SessionActivity.open(this, d.id, monitor)
    }

    private fun connectNewWindow(d: SavedDevice) = SessionActivity.open(this, d.id, -1)

    private fun editDevice(existing: SavedDevice?, from: HostPresence?) {
        fun field(hint: String, value: String, type: Int): Pair<TextInputLayout, TextInputEditText> {
            val til = TextInputLayout(this).apply { this.hint = hint }
            val et = TextInputEditText(til.context).apply { setText(value); inputType = type }
            til.addView(et)
            return til to et
        }
        val (nameL, name) = field("Name (anything you like)", existing?.name ?: from?.name ?: "", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS)
        val (addrL, addr) = field("Fallback address (optional: Tailscale or host name)", existing?.address ?: "", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI)
        val (pinL, pin) = field("PIN (shown on the PC)", existing?.pin ?: "", InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD)
        pinL.endIconMode = TextInputLayout.END_ICON_PASSWORD_TOGGLE
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
            addView(nameL); addView(addrL); addView(pinL)
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(if (existing == null) "Add device" else "Edit ${existing.name}")
            .setView(box)
            .setPositiveButton("Save", null)
            .setNegativeButton("Cancel", null)
            .show()
        dialog.getButton(android.content.DialogInterface.BUTTON_POSITIVE).setOnClickListener {
            val a = addr.text.toString().trim()
            val n = name.text.toString().trim().ifEmpty { from?.name ?: a }
            val knownId = existing?.hostId?.isNotEmpty() == true || from != null
            if (a.isEmpty() && !knownId) {
                addrL.error = "Pick it from Found on your network, or enter an address"
                return@setOnClickListener
            }
            val (host, port) = if (a.isEmpty()) "" to NativeClient.DEFAULT_PORT
                else a.split(":").let { it[0] to (it.getOrNull(1)?.toIntOrNull() ?: NativeClient.DEFAULT_PORT) }
            val d = (existing ?: SavedDevice(name = n, address = host)).copy(
                name = n, address = host, port = port, pin = pin.text.toString().trim(),
                hostId = from?.hostId ?: existing?.hostId ?: "",
            )
            DeviceStore.save(this, d)
            presence.targets = DeviceStore.all(this).flatMap { listOf(it.remoteAddress, it.address) }.filter { it.isNotEmpty() }
            dialog.dismiss()
            refresh()
        }
    }

    private fun deviceMenu(anchor: View, d: SavedDevice) {
        PopupMenu(this, anchor).apply {
            menu.add("Connect in a new window")
            menu.add("Edit name, address or PIN")
            menu.add("Remove")
            setOnMenuItemClickListener {
                when (it.title.toString()) {
                    "Connect in a new window" -> connectNewWindow(d)
                    "Edit name, address or PIN" -> editDevice(d, null)
                    "Remove" -> MaterialAlertDialogBuilder(this@MainActivity)
                        .setTitle("Remove ${d.name}?")
                        .setPositiveButton("Remove") { _, _ -> DeviceStore.delete(this@MainActivity, d.id); refresh() }
                        .setNegativeButton("Cancel", null).show()
                }
                true
            }
            show()
        }
    }

    // ------------------------------------------------------------------ list

    private inner class Adapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        private var rows = listOf<Row>()

        fun submit(r: List<Row>) {
            rows = r
            @Suppress("NotifyDataSetChanged")
            notifyDataSetChanged()
        }

        override fun getItemCount() = rows.size
        override fun getItemViewType(position: Int) = when (rows[position]) {
            is Row.Header -> 0
            is Row.Device -> 1
            is Row.Nearby -> 2
            is Row.Hint -> 3
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val v: View = when (viewType) {
                0 -> TextView(parent.context).apply {
                    setTextColor(0xFFB89A55.toInt())
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                    setTypeface(typeface, Typeface.BOLD)
                    setPadding(dp(8), dp(20), dp(8), dp(8))
                }
                3 -> TextView(parent.context).apply {
                    setTextColor(0xFFC9BFA8.toInt())
                    setPadding(dp(8), dp(4), dp(8), dp(8))
                }
                else -> MaterialCardView(parent.context).apply {
                    radius = dp(18).toFloat()
                    setCardBackgroundColor(0xFF1A1712.toInt())
                    strokeColor = 0x33E3B341
                    strokeWidth = dp(1)
                    cardElevation = 0f
                    useCompatPadding = false
                }
            }
            v.layoutParams = RecyclerView.LayoutParams(-1, -2).apply { if (viewType == 1 || viewType == 2) bottomMargin = dp(10) }
            return object : RecyclerView.ViewHolder(v) {}
        }

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            when (val row = rows[position]) {
                is Row.Header -> (holder.itemView as TextView).text = row.text.uppercase()
                is Row.Hint -> (holder.itemView as TextView).text = row.text
                is Row.Device -> bindDevice(holder.itemView as MaterialCardView, row)
                is Row.Nearby -> bindNearby(holder.itemView as MaterialCardView, row.p)
            }
        }
    }

    private fun statusDot(color: Int) = View(this).apply {
        background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(color) }
        layoutParams = LinearLayout.LayoutParams(dp(10), dp(10)).apply { marginEnd = dp(8) }
    }

    private fun bindDevice(card: MaterialCardView, row: Row.Device) {
        val d = row.d
        val p = row.p
        card.removeAllViews()
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(14), dp(10), dp(12))
        }
        val top = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        top.addView(TextView(this).apply {
            text = d.name
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 19f)
            setTypeface(typeface, Typeface.BOLD)
        }, LinearLayout.LayoutParams(0, -2, 1f))
        top.addView(textButton("⋮") { }.apply { setOnClickListener { deviceMenu(this, d) } })
        col.addView(top)
        col.addView(TextView(this).apply {
            text = buildString {
                if (p != null) append("${p.name}  ·  v${p.version}")
                else if (d.remoteAddress.isNotEmpty()) append("Reachable from anywhere via Tailscale when it's on")
                else if (d.address.isNotEmpty()) append("Fallback: ${d.address}")
                else append("Found automatically when it's on your network")
            }
            setTextColor(0xFFC9BFA8.toInt())
        })

        val (color, status) = when {
            p == null -> 0xFF6B7280.toInt() to ("Offline" + if (d.lastSeen > 0) " · last connected " +
                DateUtils.getRelativeTimeSpanString(d.lastSeen, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS) else "")
            p.clients > 0 -> 0xFFFBBF24.toInt() to "In use · ${p.clients} connected, ${p.streams} screen${if (p.streams == 1) "" else "s"} streaming"
            else -> 0xFF4ADE80.toInt() to "Online · ready"
        }
        val statusRow = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(8), 0, 0)
            addView(statusDot(color))
            addView(TextView(this@MainActivity).apply {
                text = status + if (row.openHere > 0) "  ·  open here (${row.openHere})" else ""
                setTextColor(Color.WHITE)
            })
        }
        col.addView(statusRow)
        val actions = LinearLayout(this).apply { setPadding(0, dp(10), 0, 0) }
        actions.addView(MaterialButton(this).apply {
            text = "Connect"
            isAllCaps = false
            setTextColor(0xFF0B0B0B.toInt())
            backgroundTintList = ColorStateList.valueOf(0xFFE3B341.toInt())
            setOnClickListener { connect(d) }
        })
        col.addView(actions)
        card.addView(col)
        card.setOnClickListener { connect(d) }
        card.setOnLongClickListener { deviceMenu(it, d); true }
    }

    private fun bindNearby(card: MaterialCardView, p: HostPresence) {
        card.removeAllViews()
        val row = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(18), dp(12), dp(10), dp(12))
        }
        row.addView(statusDot(if (p.clients > 0) 0xFFFBBF24.toInt() else 0xFF4ADE80.toInt()))
        row.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(this@MainActivity).apply {
                text = p.name
                setTextColor(Color.WHITE)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f)
            })
            addView(TextView(this@MainActivity).apply {
                text = "${p.address}  ·  v${p.version}" + if (p.pinRequired) "  ·  PIN needed" else ""
                setTextColor(0xFFC9BFA8.toInt())
            })
        }, LinearLayout.LayoutParams(0, -2, 1f))
        row.addView(textButton("Add") { editDevice(null, p) })
        card.addView(row)
        card.setOnClickListener { editDevice(null, p) }
        card.setOnLongClickListener(null)
    }
}
