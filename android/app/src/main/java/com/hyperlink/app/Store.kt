package com.hyperlink.app

import android.content.Context
import android.os.Build
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/** A host the user saved, with their own name and PIN for it. */
data class SavedDevice(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val address: String,
    val port: Int = NativeClient.DEFAULT_PORT,
    val pin: String = "",
    val hostId: String = "",       // learned from the host; lets us follow it to a new IP
    val remoteAddress: String = "", // the host's Tailscale address, learned automatically
    val lastSeen: Long = 0,
) {
    fun toJson(): JSONObject = JSONObject()
        .put("id", id).put("name", name).put("address", address).put("port", port)
        .put("pin", pin).put("hostId", hostId).put("remoteAddress", remoteAddress).put("lastSeen", lastSeen)

    companion object {
        fun fromJson(o: JSONObject) = SavedDevice(
            id = o.optString("id", UUID.randomUUID().toString()),
            name = o.optString("name"),
            address = o.optString("address"),
            port = o.optInt("port", NativeClient.DEFAULT_PORT),
            pin = o.optString("pin"),
            hostId = o.optString("hostId"),
            remoteAddress = o.optString("remoteAddress"),
            lastSeen = o.optLong("lastSeen"),
        )
    }
}

object DeviceStore {
    private const val PREFS = "devices"

    fun all(ctx: Context): List<SavedDevice> {
        val raw = ctx.getSharedPreferences(PREFS, 0).getString("list", "[]") ?: "[]"
        val arr = runCatching { JSONArray(raw) }.getOrDefault(JSONArray())
        return (0 until arr.length()).map { SavedDevice.fromJson(arr.getJSONObject(it)) }
    }

    fun get(ctx: Context, id: String) = all(ctx).firstOrNull { it.id == id }

    fun save(ctx: Context, d: SavedDevice) {
        val list = all(ctx).toMutableList()
        val i = list.indexOfFirst { it.id == d.id }
        if (i >= 0) list[i] = d else list.add(d)
        write(ctx, list)
    }

    fun delete(ctx: Context, id: String) = write(ctx, all(ctx).filter { it.id != id })

    private fun write(ctx: Context, list: List<SavedDevice>) {
        val arr = JSONArray()
        list.forEach { arr.put(it.toJson()) }
        ctx.getSharedPreferences(PREFS, 0).edit().putString("list", arr.toString()).apply()
    }
}

/** Streaming and app preferences. */
class Settings(ctx: Context) {
    private val p = ctx.getSharedPreferences("settings", 0)

    /** Longest edge limit for a single-monitor stream; 0 = the monitor's own resolution. */
    var maxHeight: Int
        get() = p.getInt("maxHeight", 0)
        set(v) = p.edit().putInt("maxHeight", v).apply()
    var fps: Int
        get() = p.getInt("fps", 120)
        set(v) = p.edit().putInt("fps", v).apply()
    var bitrateMbps: Int
        get() = p.getInt("bitrate", 60)
        set(v) = p.edit().putInt("bitrate", v).apply()
    /** -1 = automatic (HEVC if both sides can, else H.264). */
    var codec: Int
        get() = p.getInt("codec", -1)
        set(v) = p.edit().putInt("codec", v).apply()
    var fecPercent: Int
        get() = p.getInt("fec", 20)
        set(v) = p.edit().putInt("fec", v).apply()
    /** true = touchpad (relative), false = touch screen (tap where you want to click). */
    var trackpad: Boolean
        get() = p.getBoolean("trackpad", true)
        set(v) = p.edit().putBoolean("trackpad", v).apply()
    var trackpadSpeed: Float
        get() = p.getFloat("trackpadSpeed", 1.6f)
        set(v) = p.edit().putFloat("trackpadSpeed", v).apply()
    /** true = open sessions showing all monitors together; false = one monitor. */
    var startWithAllMonitors: Boolean
        get() = p.getBoolean("allMonitors", true)
        set(v) = p.edit().putBoolean("allMonitors", v).apply()
    /** Turn to landscape while connected (PC screens are wide). */
    var lockLandscape: Boolean
        get() = p.getBoolean("lockLandscape", true)
        set(v) = p.edit().putBoolean("lockLandscape", v).apply()
    fun hiddenScreens(deviceId: String): Set<Int> =
        (p.getString("hidden.$deviceId", "") ?: "").split(",").mapNotNull { it.toIntOrNull() }.toSet()
    fun setHiddenScreens(deviceId: String, ids: Set<Int>) =
        p.edit().putString("hidden.$deviceId", ids.joinToString(",")).apply()
    var showStats: Boolean
        get() = p.getBoolean("stats", false)
        set(v) = p.edit().putBoolean("stats", v).apply()
    var checkUpdatesOnStart: Boolean
        get() = p.getBoolean("checkUpdates", true)
        set(v) = p.edit().putBoolean("checkUpdates", v).apply()
    var skippedVersion: String
        get() = p.getString("skippedVersion", "") ?: ""
        set(v) = p.edit().putString("skippedVersion", v).apply()
    var clientName: String
        get() = p.getString("clientName", null) ?: "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}"
        set(v) = p.edit().putString("clientName", v).apply()
    /** PIN other devices need to connect to this phone. */
    var hostPin: String
        get() = p.getString("hostPin", null) ?: (100000 + java.util.Random().nextInt(900000)).toString().also {
            p.edit().putString("hostPin", it).apply()
        }
        set(v) = p.edit().putString("hostPin", v).apply()
    val clientId: String
        get() = p.getString("clientId", null) ?: UUID.randomUUID().toString().also {
            p.edit().putString("clientId", it).apply()
        }
}
