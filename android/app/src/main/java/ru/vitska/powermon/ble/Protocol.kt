package ru.vitska.powermon.ble

import java.util.UUID

/**
 * The wire contract, transcribed from CLI.md. Protocol 3.
 *
 * Nothing here touches Android APIs, so it is the part that can be reasoned about — and
 * if this file and CLI.md ever disagree, CLI.md is right.
 */
object Nus {
    val SERVICE: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    val RX: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")   // write
    val TX: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")   // notify

    const val NAME_PREFIX = "batmon"
    const val EOT = 0x04.toByte()

    /** Protocol this client was written against; `ver` reports the device's. */
    const val EXPECTED_PROTOCOL = 3
}

/** A completed command response: everything between the echo and the terminator. */
data class Response(
    val command: String,
    val lines: List<String>,
    val exit: Int,
) {
    val ok: Boolean get() = exit == 0
    val text: String get() = lines.joinToString("\n")
}

/** Telemetry records. One type per stream group, prefixed as CLI.md §5 describes. */
sealed interface Record {
    val ms: Long

    data class Fast(override val ms: Long, val volts: Double, val amps: Double) : Record

    data class Calc(
        override val ms: Long,
        val watts: Double,
        val socPct: Double,
        val chargeAh: Double,
        val state: String,
        val ocvV: Double,
        val peukert: Double,
    ) : Record

    data class Diag(
        override val ms: Long,
        val shuntMv: Double,
        val pga: String,
        val saturated: Boolean,
    ) : Record

    data class Env(
        override val ms: Long,
        val tempC: Double?,      // null when no sensor is fitted: CLI.md sends an empty field
        val humidPct: Double?,   // null on a BMP280, which has no humidity channel
        val pressHpa: Double?,
    ) : Record
}

/**
 * Splits the incoming byte stream into telemetry records and command responses.
 *
 * CLI.md offers the rule "while a command is in flight, everything up to the next 0x04
 * belongs to that command". Taken literally that mis-files stream records, because the
 * stream is asynchronous and physically interleaves with a slow command's output — and
 * `cal zero v` takes 68 seconds, which is a lot of telemetry to swallow.
 *
 * So this routes by PREFIX first and falls back to the in-flight response. That is
 * strictly more robust and agrees with CLI.md's own fallback: a line starting `f,` `c,`
 * `d,` `e,` or `#` is telemetry. Unknown prefixes are handed to the response, and
 * unknown RECORD types are dropped rather than treated as errors — CLI.md promises new
 * record types may appear without a protocol bump.
 */
class StreamParser(
    private val onRecord: (Record) -> Unit,
    private val onResponse: (Response) -> Unit,
    private val onLog: (String) -> Unit,
) {
    private val buf = StringBuilder()
    private var pending: MutableList<String>? = null
    private var pendingCommand: String = ""
    private var pendingExit: Int? = null

    /** True while a command's reply has not yet been terminated. */
    val busy: Boolean get() = pending != null

    fun expect(command: String) {
        pending = mutableListOf()
        pendingCommand = command
        pendingExit = null
    }

    fun feed(bytes: ByteArray) {
        for (b in bytes) {
            if (b == Nus.EOT) {
                finishResponse()
                continue
            }
            val c = b.toInt().toChar()
            if (c == '\n') {
                takeLine(buf.toString().trimEnd('\r'))
                buf.setLength(0)
            } else if (c != '\r') {
                buf.append(c)
                // A stream record can share a line with the local prompt on USB; over
                // BLE there is no prompt, but tolerate it anyway rather than lose the
                // first record after one.
                if (buf.length > 4096) buf.setLength(0)
            }
        }
    }

    private fun takeLine(raw: String) {
        val line = raw.removePrefix("batmon> ").trim()
        if (line.isEmpty()) return

        // Headers describe the schema we already know; surface them as log only.
        if (line.startsWith("#")) {
            onLog(line)
            return
        }

        parseRecord(line)?.let { onRecord(it); return }

        // Not telemetry, so it belongs to a command.
        val p = pending
        if (p == null) {
            onLog(line)                      // unsolicited: the greeting, a warning
            return
        }
        if (line.startsWith("> ")) return    // the echo; we already know what we sent
        val exit = line.removePrefix("exit ").toIntOrNull()
        if (line.startsWith("exit ") && exit != null) {
            pendingExit = exit
            return
        }
        p += line
    }

    private fun finishResponse() {
        val p = pending ?: return
        onResponse(Response(pendingCommand, p.toList(), pendingExit ?: -1))
        pending = null
        pendingCommand = ""
        pendingExit = null
    }

    private fun parseRecord(line: String): Record? {
        val f = line.split(',')
        if (f.size < 2) return null
        val ms = f[1].toLongOrNull() ?: return null

        // Empty fields mean "no value", never zero -- CLI.md is explicit that 0.00 C
        // would be indistinguishable from a real reading.
        fun num(i: Int): Double? = f.getOrNull(i)?.takeIf { it.isNotBlank() }?.toDoubleOrNull()

        return when (f[0]) {
            "f" -> Record.Fast(ms, num(2) ?: return null, num(3) ?: return null)
            "c" -> Record.Calc(
                ms,
                watts = num(2) ?: 0.0,
                socPct = num(3) ?: 0.0,
                chargeAh = num(4) ?: 0.0,
                state = f.getOrNull(5) ?: "UNKNOWN",
                ocvV = num(6) ?: 0.0,
                peukert = num(7) ?: 1.0,
            )
            "d" -> Record.Diag(
                ms,
                shuntMv = num(2) ?: 0.0,
                pga = f.getOrNull(3) ?: "",
                saturated = (f.getOrNull(4)?.trim() == "1"),
            )
            "e" -> Record.Env(ms, num(2), num(3), num(4))
            else -> null   // an unknown record type: drop it, do not fail
        }
    }
}

/** Micro-unit conversion. CLI.md §4: every argument is an integer in micro-units. */
object Micro {
    fun amps(a: Double): Long = Math.round(a * 1_000_000.0)
    fun volts(v: Double): Long = Math.round(v * 1_000_000.0)
    fun ampHours(ah: Double): Long = Math.round(ah * 1_000_000.0)
    fun ohms(r: Double): Long = Math.round(r * 1_000_000.0)
    /** Per mille, for `soc set`. */
    fun permille(pct: Double): Int = Math.round(pct * 10.0).toInt().coerceIn(0, 1000)
}

/**
 * How long to wait for a reply. CLI.md gives measured per-sample costs and recommends
 * `samples x per-sample x 2 + 2 s`; these are that formula applied to the defaults, with
 * the averaging commands rounded up generously. Getting this wrong is the difference
 * between "the device is slow" and "the client gave up on a working calibration".
 */
object Timeouts {
    private const val ONE_SENSOR_MS = 137L
    private const val BOTH_SENSORS_MS = 270L

    fun forCommand(cmd: String): Long {
        val w = cmd.trim().split(Regex("\\s+"))
        fun arg(i: Int, dflt: Long) = w.getOrNull(i)?.toLongOrNull() ?: dflt
        return when {
            w.getOrNull(0) == "cal" && w.getOrNull(1) == "zero" && w.getOrNull(2) == "i" ->
                arg(3, 256) * ONE_SENSOR_MS * 2 + 2_000
            w.getOrNull(0) == "cal" && w.getOrNull(1) == "zero" ->
                arg(3, 256) * BOTH_SENSORS_MS * 2 + 2_000
            w.getOrNull(0) == "cal" && (w.getOrNull(1) == "top" || w.getOrNull(1) == "vpath") ->
                arg(4, 64) * BOTH_SENSORS_MS * 2 + 2_000
            w.getOrNull(0) == "zero" -> arg(1, 256) * ONE_SENSOR_MS * 2 + 2_000
            w.getOrNull(0) == "detect" -> arg(1, 32) * BOTH_SENSORS_MS * 2 + 2_000
            w.getOrNull(0) == "read" -> 5_000
            else -> 4_000
        }
    }
}
