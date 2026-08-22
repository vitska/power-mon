package ru.vitska.powermon.ble

import android.content.Context

/**
 * The boards this phone has talked to, and which one it used last.
 *
 * Addresses are the identity, not names: `batmon-DCFA` is derived from the MAC so it is
 * stable in practice, but two boards can be renamed and a name is not a key. The name is
 * cached only so the picker can list a known board before a scan has found it.
 */
class DeviceStore(context: Context) {

    private val prefs = context.getSharedPreferences("devices", Context.MODE_PRIVATE)

    /** Address of the last board that reached a usable link, or null on a fresh install. */
    var last: String?
        get() = prefs.getString(KEY_LAST, null)
        set(v) = prefs.edit().apply { if (v == null) remove(KEY_LAST) else putString(KEY_LAST, v) }
            .apply()

    /** Known boards as address to name, newest name winning. */
    fun known(): Map<String, String> =
        prefs.getStringSet(KEY_KNOWN, emptySet()).orEmpty()
            .mapNotNull { entry ->
                val i = entry.indexOf('\t')
                if (i <= 0) null else entry.substring(0, i) to entry.substring(i + 1)
            }
            .toMap()

    fun remember(address: String, name: String) {
        val merged = known().toMutableMap()
        merged[address] = name
        write(merged)
    }

    fun forget(address: String) {
        val merged = known().toMutableMap()
        merged.remove(address)
        write(merged)
        if (last == address) last = null
    }

    private fun write(map: Map<String, String>) {
        prefs.edit()
            .putStringSet(KEY_KNOWN, map.map { (a, n) -> "$a\t$n" }.toSet())
            .apply()
    }

    private companion object {
        const val KEY_LAST = "last_address"
        const val KEY_KNOWN = "known"
    }
}
