package ru.vitska.powermon.ble

/**
 * One INA219 read directly, from the `raw` command (CLI.md, "raw per-sensor readings"):
 * whatever this specific chip sees and is set to convert it with, independent of which
 * role (current/voltage) the firmware has assigned it. The point is deciding that role
 * by eye — the same judgement `shunt loc` asks for, but with numbers instead of a guess.
 */
data class RawSensor(
    val present: Boolean,
    val role: String?,       // "current+voltage" | "current" | "voltage" | "idle"
    val busV: Double?,
    val side: String?,       // "positive" | "ground" | "unclear"
    val shuntMv: Double?,    // signed
    val currentA: Double?,   // through this chip's own conversion
    val pga: Int?,           // divisor, e.g. 8 for /8
    val rangeMv: Double?,
    val saturated: Boolean,
    val shuntUohm: Long?,
    val gainPpm: Long?,
    val offsetUa: Long?,
    val sign: String?,       // "normal" | "invert"
)

data class RawSensors(
    val pos: RawSensor,
    val neg: RawSensor,
    val shuntLoc: String?,
    val shuntUohm: Long?,
    val poles: String?,       // "ok" | "swapped" | "unclear" | "single"
    /** False on firmware before `raw` existed (exit -2, "unknown command"). */
    val supported: Boolean = true,
) {
    companion object {
        val UNSUPPORTED = RawSensors(
            pos = RawSensor(false, null, null, null, null, null, null, null, false, null, null, null, null),
            neg = RawSensor(false, null, null, null, null, null, null, null, false, null, null, null, null),
            shuntLoc = null, shuntUohm = null, poles = null, supported = false,
        )

        fun parse(lines: List<String>): RawSensors {
            val c = ConfigState.parse(lines)
            fun one(key: String) = RawSensor(
                present = c.bool("$key.present") ?: false,
                role = c.str("$key.role"),
                busV = c.micro("$key.bus_uv", 3)?.toDoubleOrNull(),
                side = c.str("$key.side"),
                shuntMv = c.long("$key.shunt_uv")?.let { it / 1000.0 },
                currentA = c.micro("$key.current_ua", 4)?.toDoubleOrNull(),
                pga = c.int("$key.pga"),
                rangeMv = c.long("$key.range_uv")?.let { it / 1000.0 },
                saturated = c.bool("$key.sat") ?: false,
                shuntUohm = c.long("$key.shunt_uohm"),
                gainPpm = c.long("$key.gain_ppm"),
                offsetUa = c.long("$key.offset_ua"),
                sign = c.str("$key.sign"),
            )
            return RawSensors(
                pos = one("pos"),
                neg = one("neg"),
                shuntLoc = c.str("shunt.loc"),
                shuntUohm = c.long("shunt.uohm"),
                poles = c.str("poles"),
            )
        }
    }
}
