package ru.vitska.powermon.model

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import ru.vitska.powermon.ble.BatmonClient
import ru.vitska.powermon.ble.DeviceStore
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.ble.Nus
import ru.vitska.powermon.ble.Record
import ru.vitska.powermon.ble.Response

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

/** One row in the device picker: a board this phone knows about, has just seen, or both. */
data class DeviceEntry(
    val address: String,
    val name: String,
    val rssi: Int?,
    val known: Boolean,
    val connected: Boolean,
)

class MonitorViewModel(app: Application) : AndroidViewModel(app) {

    private val store = DeviceStore(app)
    val client = BatmonClient(app, viewModelScope)

    val link: StateFlow<Link> get() = client.link
    val deviceName: StateFlow<String?> get() = client.deviceName
    val deviceAddress: StateFlow<String?> get() = client.deviceAddress
    val scanning: StateFlow<Boolean> get() = client.scanning

    private val _tel = MutableStateFlow(Telemetry())
    val telemetry = _tel.asStateFlow()

    private val _shake = MutableStateFlow(Handshake())
    val handshake = _shake.asStateFlow()

    private val _console = MutableStateFlow(listOf<String>())
    val console = _console.asStateFlow()

    private val _busy = MutableStateFlow(false)
    val busy = _busy.asStateFlow()

    /** Bumped whenever the remembered set changes, to re-read it into [devices]. */
    private val knownRevision = MutableStateFlow(0)

    /**
     * The picker's contents: remembered boards merged with whatever the scan has turned
     * up, so a known board is listed (greyed, no signal) even before it answers a scan.
     */
    private val _devices = MutableStateFlow(emptyList<DeviceEntry>())
    val devices = _devices.asStateFlow()

    /** Rolling window of fast-record arrivals, for a real rate rather than a claimed one. */
    private val fastStamps = ArrayDeque<Long>()

    init {
        viewModelScope.launch { client.records.collect { r -> apply(r) } }
        viewModelScope.launch { client.log.collect { l -> appendConsole(l) } }
        viewModelScope.launch {
            client.link.collect { st -> if (st == Link.Ready) onReady() }
        }
        viewModelScope.launch {
            combine(
                client.found,
                knownRevision,
                client.deviceAddress,
            ) { found, _, current ->
                val known = store.known()
                val seen = found.associateBy { it.address.uppercase() }
                val all = (known.keys.map { it.uppercase() } + seen.keys).distinct()
                all.map { addr ->
                    val hit = seen[addr]
                    DeviceEntry(
                        address = addr,
                        name = hit?.name
                            ?: known.entries.firstOrNull { it.key.uppercase() == addr }?.value
                            ?: addr,
                        rssi = hit?.rssi,
                        known = known.keys.any { it.uppercase() == addr },
                        connected = current?.uppercase() == addr,
                    )
                }.sortedWith(
                    compareByDescending<DeviceEntry> { it.connected }
                        .thenByDescending { it.rssi != null }
                        .thenByDescending { it.rssi ?: Int.MIN_VALUE }
                        .thenBy { it.name }
                )
            }.collect { list -> _devices.value = list }
        }
    }

    // ------------------------------------------------------------------ devices

    /**
     * Called once the Bluetooth permissions are actually held. Reconnects to the board
     * used last, via a scan rather than a direct connect: a scan hit proves the board is
     * powered and in range, where a direct connect to an absent one just stalls until
     * the stack gives up.
     */
    fun resumeLastOrScan() {
        if (client.link.value == Link.Ready) return
        knownRevision.value += 1
        val last = store.last
        if (last != null) appendConsole("looking for ${store.known()[last] ?: last}...")
        // First run has no board to prefer, so the single board in range is the one
        // meant; once anything is remembered, switching is always an explicit choice.
        client.startScan(autoConnectTo = last, connectFirstFound = last == null)
    }

    fun scan() {
        knownRevision.value += 1
        client.startScan()
    }

    fun stopScan() = client.stopScan()

    /** Switch to another board. The old link is dropped inside the client. */
    fun connectTo(address: String, name: String? = null) {
        // Nothing from the previous board should survive the switch: a stale voltage
        // under a new device's name is worse than an empty panel.
        _tel.value = Telemetry()
        _shake.value = Handshake()
        fastStamps.clear()
        appendConsole("--- connecting to ${name ?: address}")
        client.connect(address)
    }

    fun forget(address: String) {
        store.forget(address)
        knownRevision.value += 1
    }

    // ------------------------------------------------------------------ telemetry

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
        // Remember it only now: a board that reached a usable link is worth reconnecting
        // to, whereas one that failed at discovery is not.
        client.deviceAddress.value?.let { addr ->
            store.remember(addr, client.deviceName.value ?: addr)
            store.last = addr
            knownRevision.value += 1
        }
        run("stream csv")
    }

    /** Runs a command, appends the transcript, and hands back the response. */
    suspend fun run(cmd: String): Response? {
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
    fun launchCommandWith(cmd: String, then: (Response?) -> Unit) =
        viewModelScope.launch { then(run(cmd)) }

    private fun appendConsole(line: String) {
        // Bounded: a 10 Hz stream would otherwise turn the transcript into a leak.
        _console.value = (_console.value + line).takeLast(400)
    }

    fun clearConsole() { _console.value = emptyList() }

    fun disconnect() = client.disconnect()
}
