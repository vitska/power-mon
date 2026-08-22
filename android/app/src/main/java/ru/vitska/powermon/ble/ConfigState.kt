package ru.vitska.powermon.ble

/**
 * The device's settings as it reports them, from the `config` command.
 *
 * `config` prints one `key=value` per line in the same units the matching setter takes,
 * which is what makes showing a current value next to a control possible without a table
 * mapping one spelling to the other: `soc.cap_uah=44000000` is what `soc cap 44000000`
 * set. Anything not recognised is kept in [raw] and ignored — new keys appear without a
 * protocol bump, so an unknown key is a newer firmware, not an error.
 *
 * A firmware without `config` at all (protocol 3 before this command existed) answers
 * `exit -2`, and [supported] is then false: controls simply show no current value rather
 * than a wrong one.
 */
class ConfigState(val raw: Map<String, String>) {

    val supported: Boolean get() = raw.isNotEmpty()

    fun str(key: String): String? = raw[key]
    fun long(key: String): Long? = raw[key]?.toLongOrNull()
    fun int(key: String): Int? = raw[key]?.toIntOrNull()
    fun bool(key: String): Boolean? = raw[key]?.let { it == "1" }

    /** A micro-unit integer as a human number: `micro("soc.cap_uah", 1)` -> "44.0". */
    fun micro(key: String, dp: Int): String? =
        long(key)?.let { String.format("%.${dp}f", it / 1_000_000.0) }

    /** A micro-unit integer scaled to milli-units, for resistances quoted in mOhm. */
    fun milli(key: String, dp: Int): String? =
        long(key)?.let { String.format("%.${dp}f", it / 1_000.0) }

    /** Parts per million as a percentage error, which is how a gain is worth reading. */
    fun gainPct(key: String): String? =
        long(key)?.let { String.format("%+.3f %%", (it - 1_000_000) / 10_000.0) }

    companion object {
        val EMPTY = ConfigState(emptyMap())

        fun parse(lines: List<String>): ConfigState {
            val m = LinkedHashMap<String, String>()
            for (l in lines) {
                val i = l.indexOf('=')
                // Skip anything that is not key=value: a refusal, or prose from a
                // firmware that answered something else entirely.
                if (i <= 0) continue
                val k = l.substring(0, i).trim()
                if (k.isEmpty() || k.contains(' ')) continue
                m[k] = l.substring(i + 1).trim()
            }
            return ConfigState(m)
        }
    }
}
