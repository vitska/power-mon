package ru.vitska.powermon.model

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import ru.vitska.powermon.ble.BatmonClient
import ru.vitska.powermon.ble.ConfigState
import ru.vitska.powermon.ble.DeviceStore
import ru.vitska.powermon.ble.FirmwareImage
import ru.vitska.powermon.ble.FirmwareReleases
import ru.vitska.powermon.ble.FirmwareTarget
import ru.vitska.powermon.ble.GATT_NOT_STARTED
import ru.vitska.powermon.ble.GATT_TIMEOUT
import ru.vitska.powermon.ble.OtaStatus
import ru.vitska.powermon.ble.OtaWriteError
import ru.vitska.powermon.ble.Release
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

/** The Firmware tab: what the board runs, what is published, and any update under way. */
data class FirmwareState(
    /** From `ota status`; null until read, or on firmware that lacks the command. */
    val ota: OtaStatus? = null,
    /** The board answered `ota` with "unknown command": it predates BLE updates. */
    val unsupported: Boolean = false,
    /** Which firmware the connected device runs: monitor or remote display. */
    val target: FirmwareTarget = FirmwareTarget.MONITOR,
    /** The newest release of [target]'s series. */
    val latest: Release? = null,
    val checking: Boolean = false,
    /** True once a check has completed, so "no release" can be told from "not asked". */
    val checked: Boolean = false,
    val updating: Boolean = false,
    /** 0..1 during the transfer, null otherwise. */
    val progress: Float? = null,
    /** What is happening now, in words. */
    val phase: String? = null,
    val error: String? = null,
    val notice: String? = null,
)

/** One row in the device picker: a board this phone knows about, has just seen, or both. */
data class DeviceEntry(
    val address: String,
    val name: String,
    val rssi: Int?,
    val known: Boolean,
    val connected: Boolean,
) {
    /** A remote display in update mode, not a monitor. */
    val isRemote: Boolean get() = name.startsWith(ru.vitska.powermon.ble.Nus.REMOTE_PREFIX)
}

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

    /**
     * What the device says its settings are. Re-read on connect and after every setter,
     * never carried across connections: another client may have changed any of it.
     */
    private val _config = MutableStateFlow(ConfigState.EMPTY)
    val config = _config.asStateFlow()

    private val _fw = MutableStateFlow(FirmwareState())
    val firmware = _fw.asStateFlow()

    /** Set just before rebooting into a new image: the version it should come back as. */
    private var expectAfterReboot: String? = null
    private var updateJob: Job? = null

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
     * Called once the Bluetooth permissions are actually held. Connects straight to the
     * board used last; if it is off, the client keeps retrying until it appears. Only a
     * fresh install, with nothing remembered, scans -- and then takes the first board
     * found, since there is no earlier choice to respect.
     */
    fun resumeLastOrScan() {
        if (client.link.value == Link.Ready) return
        knownRevision.value += 1
        val last = store.last
        if (last != null) {
            appendConsole("connecting to ${store.known()[last] ?: last}...")
            client.connect(last)
        } else {
            client.startScan(connectFirstFound = true)
        }
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
        _config.value = ConfigState.EMPTY
        _fw.value = FirmwareState(latest = _fw.value.latest, checked = _fw.value.checked)
        expectAfterReboot = null
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
        val target = FirmwareTarget.forDeviceName(client.deviceName.value)
        if (target != _fw.value.target) {
            // Another series: the release found for the previous device does not apply.
            _fw.value = _fw.value.copy(target = target, latest = null, checked = false)
        }
        if (target == FirmwareTarget.REMOTE) {
            // A remote display serves only `ver`, `ota` and `reboot`: no settings to
            // read and no telemetry to start.
            refreshOta()
            settleUpdate()
            return@launch
        }
        refreshConfig()
        refreshOta()
        settleUpdate()
        run("stream csv")
    }

    /**
     * Re-reads every setting. Cheap (one sub-100 ms command) and the only honest way to
     * show a current value: the alternative is echoing back what this app last wrote,
     * which is wrong the moment the device clamps a value or another client changes one.
     */
    suspend fun refreshConfig() {
        val r = client.send("config")
        _config.value = if (r != null && r.ok) ConfigState.parse(r.lines) else ConfigState.EMPTY
    }

    fun launchRefreshConfig() = viewModelScope.launch { refreshConfig() }

    // ------------------------------------------------------------------ firmware

    /** Re-reads `ota status`. "unknown command" (-2) means firmware before BLE updates. */
    suspend fun refreshOta() {
        val r = client.send("ota status") ?: return
        _fw.value = when {
            r.ok -> _fw.value.copy(ota = OtaStatus.parse(r.lines), unsupported = false)
            r.exit == -2 -> _fw.value.copy(ota = null, unsupported = true)
            else -> _fw.value
        }
    }

    /** Asks GitHub for the newest release. Off the main thread; errors land in [firmware]. */
    fun checkLatest() = viewModelScope.launch {
        _fw.value = _fw.value.copy(checking = true, error = null)
        val target = _fw.value.target
        val result = runCatching { withContext(Dispatchers.IO) { FirmwareReleases.latest(target) } }
        _fw.value = _fw.value.copy(
            checking = false,
            checked = result.isSuccess,
            latest = result.getOrNull(),
            error = result.exceptionOrNull()?.let { "release check failed: ${it.message}" },
        )
    }

    fun updateFromRelease() {
        val rel = _fw.value.latest ?: return
        startUpdate("downloading ${rel.tag}") {
            val bytes = withContext(Dispatchers.IO) { FirmwareReleases.download(rel) }
            FirmwareImage.parse(bytes, _fw.value.target)
        }
    }

    fun updateFromFile(bytes: ByteArray) = startUpdate("checking the file") {
        FirmwareImage.parse(bytes, _fw.value.target)
    }

    fun cancelUpdate() {
        updateJob?.cancel()
    }

    private fun startUpdate(first: String, load: suspend () -> FirmwareImage) {
        if (updateJob?.isActive == true) return
        updateJob = viewModelScope.launch {
            _fw.value = _fw.value.copy(
                updating = true, phase = first, progress = null, error = null, notice = null,
            )
            try {
                flash(load())
            } catch (e: CancellationException) {
                _fw.value = _fw.value.copy(error = "cancelled; the running firmware is untouched")
                withContext(NonCancellable) { client.send("ota abort") }
                throw e
            } catch (e: Exception) {
                _fw.value = _fw.value.copy(error = e.message ?: e.toString())
            } finally {
                client.fastLink(false)
                _fw.value = _fw.value.copy(updating = false, progress = null, phase = null)
            }
        }
    }

    /**
     * The whole update, CLI.md §6 "Firmware update": announce size and hash (the device
     * erases the spare slot), stream the image in acknowledged chunks each prefixed with
     * its offset, have the device verify it and select it for boot, then reboot.
     * Confirmation happens after reconnecting, in [settleUpdate].
     */
    private suspend fun flash(img: FirmwareImage) {
        if (client.link.value != Link.Ready) error("not connected")
        if (!client.otaSupported) {
            error("this firmware predates BLE updates; flash it once over USB (tools/flash.ps1)")
        }
        val size = img.bytes.size

        _fw.value = _fw.value.copy(phase = "erasing the spare slot for ${img.version}")
        val begin = run("ota begin $size ${img.sha256Hex}") ?: error("no reply to ota begin")
        if (!begin.ok) error(begin.text)

        client.fastLink(true)
        val chunk = client.otaChunk
        val started = System.currentTimeMillis()
        var off = 0
        while (off < size) {
            val n = minOf(chunk, size - off)
            val buf = ByteArray(4 + n)
            buf[0] = off.toByte(); buf[1] = (off shr 8).toByte()
            buf[2] = (off shr 16).toByte(); buf[3] = (off shr 24).toByte()
            System.arraycopy(img.bytes, off, buf, 4, n)

            var st = client.writeOta(buf)
            var tries = 0
            // Busy, or an acknowledgement that never came: the device accepts an exact
            // repeat of the last chunk, so resending is safe.
            while ((st == 0x83 || st == GATT_TIMEOUT || st == GATT_NOT_STARTED) && tries++ < 5) {
                delay(100)
                st = client.writeOta(buf)
            }
            if (st != 0) {
                client.send("ota abort")
                error("transfer stopped at $off of $size bytes: ${OtaWriteError.describe(st)}")
            }
            off += n
            val secs = (System.currentTimeMillis() - started) / 1000.0
            _fw.value = _fw.value.copy(
                progress = off.toFloat() / size,
                phase = "sending ${img.version}: ${off / 1024} of ${size / 1024} KB" +
                    if (secs > 1) ", %.1f KB/s".format(off / 1024.0 / secs) else "",
            )
        }
        client.fastLink(false)

        _fw.value = _fw.value.copy(progress = null, phase = "device is verifying the image")
        val end = run("ota end") ?: error("no reply to ota end")
        if (!end.ok) error(end.text)

        expectAfterReboot = img.version
        _fw.value = _fw.value.copy(phase = "restarting into ${img.version}")
        run("reboot")
        // The client reconnects on its own when the link drops; just wait for it.
        withTimeoutOrNull(5_000) { client.link.first { it != Link.Ready } }
        _fw.value = _fw.value.copy(phase = "waiting for the board to come back")
        withTimeoutOrNull(60_000) { client.link.first { it == Link.Ready } }
            ?: error("${img.version} was written, but the board did not reconnect within a " +
                "minute. If it never does, it rolls back on its own after ten minutes.")
    }

    /**
     * After a reconnect: if this app just flashed the board, check that it came back
     * running what was sent, and end its probation. The handshake that got us here is the
     * proof the new image works -- its radio and its console both answered.
     */
    private suspend fun settleUpdate() {
        val want = expectAfterReboot ?: return
        expectAfterReboot = null
        val ota = _fw.value.ota
        when {
            ota?.version != want -> _fw.value = _fw.value.copy(
                error = "the board came back running ${ota?.version ?: "?"}, not $want -- " +
                    "the update did not take, or was rolled back",
            )
            ota.onProbation -> {
                val r = run("ota confirm")
                refreshOta()
                _fw.value = _fw.value.copy(
                    notice = if (r?.ok == true) "updated to $want and confirmed"
                    else "updated to $want, but confirming failed: ${r?.text ?: "no reply"}",
                )
            }
            else -> _fw.value = _fw.value.copy(notice = "updated to $want")
        }
    }

    /** Keep the image on probation. */
    fun confirmFirmware() = viewModelScope.launch { run("ota confirm"); refreshOta() }

    /** Go back to the other slot's image. The board reboots, so the link drops. */
    fun rollbackFirmware() = viewModelScope.launch {
        val r = run("ota rollback")
        if (r?.ok == true) {
            // The board reboots; the client reconnects by itself.
            _fw.value = _fw.value.copy(notice = "rolling back; reconnecting")
        } else {
            _fw.value = _fw.value.copy(error = r?.text ?: "no reply to ota rollback")
        }
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
