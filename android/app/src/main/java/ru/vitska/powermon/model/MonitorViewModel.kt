package ru.vitska.powermon.model

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import ru.vitska.powermon.ble.BatmonClient
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.ble.Nus
import ru.vitska.powermon.ble.Record

/** Everything the monitor screen shows, assembled from the four record types. */
data class Telemetry(
    val volts: Double? = null,
    val amps: Double? = null,
    val watts: Double? = null,
    val socPct: Double? = null,
    val chargeAh: Double? = null,
    val state: String = "—",
    val ocvV: Double? = null,
    val peukert: Double? = null,
    val shuntMv: Double? = null,
    val pga: String = "",
    val saturated: Boolean = false,
    val tempC: Double? = null,
    val humidPct: Double? = null,
    val pressHpa: Double? = null,
    /** Measured from record timestamps, not assumed — CLI.md guarantees no duplicates. */
    val fastHz: Double? = null,
    val lastFastMs: Long? = null,
)

data class Handshake(
    val protocol: Int? = null,
    val firmware: String = "",
    val mac: String = "",
    val mismatch: Boolean = false,
)

class MonitorViewModel(app: Application) : AndroidViewModel(app) {

    val client = BatmonClient(app, viewModelScope)

    val link: StateFlow<Link> get() = client.link
    val deviceName: StateFlow<String?> get() = client.deviceName

    private val _tel = MutableStateFlow(Telemetry())
    val telemetry = _tel.asStateFlow()

    private val _shake = MutableStateFlow(Handshake())
    val handshake = _shake.asStateFlow()

    private val _console = MutableStateFlow(listOf<String>())
    val console = _console.asStateFlow()

    private val _busy = MutableStateFlow(false)
    val busy = _busy.asStateFlow()

    /** Rolling window of fast-record arrivals, for a real rate rather than a claimed one. */
    private val fastStamps = ArrayDeque<Long>()

    init {
        viewModelScope.launch {
            client.records.collect { r -> apply(r) }
        }
        viewModelScope.launch {
            client.log.collect { l -> appendConsole(l) }
        }
        viewModelScope.launch {
            client.link.collect { st ->
                if (st == Link.Ready) onReady()
            }
        }
    }

    private fun apply(r: Record) {
        _tel.value = when (r) {
            is Record.Fast -> {
                // Rate from the device's own timestamps. Records are never duplicated,
                // so this is the true sample rate and not a guess about the sensor.
                fastStamps.addLast(r.ms)
                while (fastStamps.size > 16) fastStamps.removeFirst()
                val span = if (fastStamps.size >= 2)
                    (fastStamps.last() - fastStamps.first()).toDouble() else 0.0
                val hz = if (span > 0) (fastStamps.size - 1) * 1000.0 / span else null
                _tel.value.copy(volts = r.volts, amps = r.amps, fastHz = hz, lastFastMs = r.ms)
            }
            is Record.Calc -> _tel.value.copy(
                watts = r.watts, socPct = r.socPct, chargeAh = r.chargeAh,
                state = r.state, ocvV = r.ocvV, peukert = r.peukert,
            )
            is Record.Diag -> _tel.value.copy(
                shuntMv = r.shuntMv, pga = r.pga, saturated = r.saturated,
            )
            is Record.Env -> _tel.value.copy(
                tempC = r.tempC, humidPct = r.humidPct, pressHpa = r.pressHpa,
            )
        }
    }

    /**
     * CLI.md's recommended opening sequence: handshake, then read the state worth showing,
     * then start telemetry. Done once per connection, and never cached across one --
     * another client may have changed the configuration in between.
     */
    private fun onReady() = viewModelScope.launch {
        val ver = run("ver")
        if (ver != null) {
            val kv = ver.lines.mapNotNull {
                val p = it.split(' ', limit = 2)
                if (p.size == 2) p[0] to p[1] else null
            }.toMap()
            val proto = kv["protocol"]?.toIntOrNull()
            _shake.value = Handshake(
                protocol = proto,
                firmware = kv["firmware"] ?: "",
                mac = kv["mac"] ?: "",
                mismatch = proto != null && proto != Nus.EXPECTED_PROTOCOL,
            )
        }
        run("stream csv")
    }

    /** Runs a command, appends the transcript, and hands back the response. */
    suspend fun run(cmd: String): ru.vitska.powermon.ble.Response? {
        _busy.value = true
        appendConsole("> $cmd")
        val r = client.send(cmd)
        if (r == null) {
            appendConsole("(no reply — timed out)")
        } else {
            r.lines.forEach { appendConsole(it) }
            if (!r.ok) appendConsole("(exit ${r.exit})")
        }
        _busy.value = false
        return r
    }

    fun launchCommand(cmd: String) = viewModelScope.launch { run(cmd) }

    /** Same, but hands the response back so a screen can show it in place. */
    fun launchCommandWith(
        cmd: String,
        then: (ru.vitska.powermon.ble.Response?) -> Unit,
    ) = viewModelScope.launch { then(run(cmd)) }

    private fun appendConsole(line: String) {
        // Bounded: a 10 Hz stream would otherwise turn the transcript into a leak.
        _console.value = (_console.value + line).takeLast(400)
    }

    fun clearConsole() { _console.value = emptyList() }

    fun connect() = client.startScan()
    fun disconnect() = client.disconnect()
}
